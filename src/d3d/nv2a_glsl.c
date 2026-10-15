/*
 * NV2A vertex programs to GLSL. Instruction decode follows the macOS Halo
 * port (ChupathingyCE, CC0): each slot is four little-endian words, a MAC
 * op and an ILU op that run in parallel.
 */

#include "nv2a_glsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} Txt;

static void txt_add(Txt *t, const char *fmt, ...)
{
    va_list ap, aq;
    int n;

    va_start(ap, fmt);
    va_copy(aq, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(aq);
        return;
    }
    if (t->len + (size_t)n + 1 > t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 4096;
        while (nc < t->len + (size_t)n + 1)
            nc *= 2;
        t->buf = (char *)realloc(t->buf, nc);
        t->cap = nc;
    }
    if (t->buf) {
        vsnprintf(t->buf + t->len, t->cap - t->len, fmt, aq);
        t->len += (size_t)n;
    }
    va_end(aq);
}

static unsigned field(const uint32_t *insn, int word, int low, int bits)
{
    return (insn[word] >> low) & ((1u << bits) - 1);
}

enum {
    MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
    MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL
};

enum {
    ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT
};

enum {
    MUX_UNKNOWN, MUX_TEMP, MUX_INPUT, MUX_CONST
};

static const char *output_name(unsigned address)
{
    switch (address) {
    case 0: return "oPos";
    case 3: return "oD0";
    case 4: return "oD1";
    case 5: return "oFog";
    case 6: return "oPts";
    case 7: return "oB0";
    case 8: return "oB1";
    case 9: return "oT0";
    case 10: return "oT1";
    case 11: return "oT2";
    case 12: return "oT3";
    default: return "oUnused";
    }
}

static void write_mask(unsigned mask, char *out)
{
    int n = 0;
    if (mask & 8) out[n++] = 'x';
    if (mask & 4) out[n++] = 'y';
    if (mask & 2) out[n++] = 'z';
    if (mask & 1) out[n++] = 'w';
    out[n] = 0;
}

static void operand(Txt *t, const uint32_t *insn, char which, int relative)
{
    static const char sw[] = "xyzw";
    unsigned negate, swizzle[4], index, mux;

    switch (which) {
    case 'A':
        negate = field(insn, 1, 8, 1);
        swizzle[0] = field(insn, 1, 6, 2);
        swizzle[1] = field(insn, 1, 4, 2);
        swizzle[2] = field(insn, 1, 2, 2);
        swizzle[3] = field(insn, 1, 0, 2);
        index = field(insn, 2, 28, 4);
        mux = field(insn, 2, 26, 2);
        break;
    case 'B':
        negate = field(insn, 2, 25, 1);
        swizzle[0] = field(insn, 2, 23, 2);
        swizzle[1] = field(insn, 2, 21, 2);
        swizzle[2] = field(insn, 2, 19, 2);
        swizzle[3] = field(insn, 2, 17, 2);
        index = field(insn, 2, 13, 4);
        mux = field(insn, 2, 11, 2);
        break;
    default:
        negate = field(insn, 2, 10, 1);
        swizzle[0] = field(insn, 2, 8, 2);
        swizzle[1] = field(insn, 2, 6, 2);
        swizzle[2] = field(insn, 2, 4, 2);
        swizzle[3] = field(insn, 2, 2, 2);
        index = (field(insn, 2, 0, 2) << 2) | field(insn, 3, 30, 2);
        mux = field(insn, 3, 28, 2);
        break;
    }

    txt_add(t, "%s", negate ? "-" : "");
    switch (mux) {
    case MUX_TEMP:
        if (index == 12)
            txt_add(t, "oPos");
        else
            txt_add(t, "r%u", index);
        break;
    case MUX_INPUT:
        txt_add(t, "v%u", field(insn, 1, 9, 4));
        break;
    case MUX_CONST:
        if (relative)
            txt_add(t, "c[clamp(a0 + %u, 0, %d)]",
                    field(insn, 1, 13, 8), NV2A_GLSL_CONSTANTS - 1);
        else
            txt_add(t, "c[%u]", field(insn, 1, 13, 8));
        break;
    default:
        txt_add(t, "vec4(0.0)");
        break;
    }
    txt_add(t, ".%c%c%c%c",
            sw[swizzle[0]], sw[swizzle[1]], sw[swizzle[2]], sw[swizzle[3]]);
}

static const char prologue[] =
#if defined(__APPLE__)
    "#version 410 core\n"
#else
    "#version 330 core\n"
#endif
    "uniform vec4 c[192];\n"
    "uniform vec4 viewport_scale;\n"
    "uniform vec4 viewport_offset;\n"
    "uniform float point_size;\n"
    "uniform float screen_offset;\n"
    "out vec4 xD0;\n"
    "out vec4 xD1;\n"
    "out vec4 xB0;\n"
    "out vec4 xB1;\n"
    "out vec4 xT0;\n"
    "out vec4 xT1;\n"
    "out vec4 xT2;\n"
    "out vec4 xT3;\n"
    "out float xFog;\n"
    "invariant gl_Position;\n"
    "vec4 unpack_normpacked3(uint p)\n"
    "{\n"
    "  int x = int(p << 21) >> 21;\n"
    "  int y = int(p << 10) >> 21;\n"
    "  int z = int(p) >> 22;\n"
    "  return vec4(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1.0);\n"
    "}\n"
    "vec4 nv2a_rcc(float x)\n"
    "{\n"
    "  float r = 1.0 / x;\n"
    "  if (r > 0.0) r = clamp(r, 5.42101e-20, 1.884467e+19);\n"
    "  else r = clamp(r, -1.884467e+19, -5.42101e-20);\n"
    "  return vec4(r);\n"
    "}\n"
    "vec4 nv2a_exp(float x)\n"
    "{\n"
    "  return vec4(exp2(floor(x)), fract(x), exp2(x), 1.0);\n"
    "}\n"
    "vec4 nv2a_log(float x)\n"
    "{\n"
    "  x = abs(x);\n"
    "  if (x == 0.0) return vec4(-1.0e30, 1.0, -1.0e30, 1.0);\n"
    "  float e = floor(log2(x));\n"
    "  return vec4(e, x / exp2(e), log2(x), 1.0);\n"
    "}\n"
    "vec4 nv2a_lit(vec4 s)\n"
    "{\n"
    "  float specular = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0;\n"
    "  return vec4(1.0, max(s.x, 0.0), specular, 1.0);\n"
    "}\n";

char *nv2a_vertex_shader_to_glsl(const uint32_t *instructions,
                                 unsigned instruction_count,
                                 unsigned packed_attribute_mask)
{
    Txt text = { 0 };
    unsigned index;

    if (!instructions || instruction_count == 0)
        return NULL;

    txt_add(&text, "%s", prologue);
    for (index = 0; index < NV2A_GLSL_ATTRIBUTES; index++) {
        if (packed_attribute_mask & (1u << index))
            txt_add(&text, "layout(location = %u) in uint v%u_packed;\n", index, index);
        else
            txt_add(&text, "layout(location = %u) in vec4 v%u_in;\n", index, index);
    }

    txt_add(&text, "void main()\n{\n");
    for (index = 0; index < NV2A_GLSL_ATTRIBUTES; index++) {
        if (packed_attribute_mask & (1u << index))
            txt_add(&text, "\tvec4 v%u = unpack_normpacked3(v%u_packed);\n", index, index);
        else
            txt_add(&text, "\tvec4 v%u = v%u_in;\n", index, index);
    }
    txt_add(&text,
            "\tvec4 r0 = vec4(0.0), r1 = vec4(0.0), r2 = vec4(0.0), r3 = vec4(0.0);\n"
            "\tvec4 r4 = vec4(0.0), r5 = vec4(0.0), r6 = vec4(0.0), r7 = vec4(0.0);\n"
            "\tvec4 r8 = vec4(0.0), r9 = vec4(0.0), r10 = vec4(0.0), r11 = vec4(0.0);\n"
            "\tvec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "\tvec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "\tvec4 oB0 = vec4(0.0, 0.0, 0.0, 1.0), oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "\tvec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "\tvec4 oT2 = vec4(0.0, 0.0, 0.0, 1.0), oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "\tvec4 oFog = vec4(1.0), oPts = vec4(point_size), oUnused = vec4(0.0);\n"
            "\tint a0 = 0;\n"
            "\tvec4 A, B, C, mac, ilu;\n");

    for (index = 0; index < instruction_count; index++) {
        const uint32_t *insn = instructions + index * 4;
        unsigned mac = field(insn, 1, 21, 4);
        unsigned ilu = field(insn, 1, 25, 3);
        unsigned mac_mask = field(insn, 3, 24, 4);
        unsigned temporary = field(insn, 3, 20, 4);
        unsigned ilu_mask = field(insn, 3, 16, 4);
        unsigned output_mask = field(insn, 3, 12, 4);
        unsigned output_is_register = field(insn, 3, 11, 1);
        unsigned output_address = field(insn, 3, 3, 8);
        unsigned output_from_ilu = field(insn, 3, 2, 1);
        int relative = (int)field(insn, 3, 1, 1);
        char mask[5];

        txt_add(&text, "\t/* %u */\n", index);
        txt_add(&text, "\tA = "); operand(&text, insn, 'A', relative); txt_add(&text, ";\n");
        txt_add(&text, "\tB = "); operand(&text, insn, 'B', relative); txt_add(&text, ";\n");
        txt_add(&text, "\tC = "); operand(&text, insn, 'C', relative); txt_add(&text, ";\n");

        switch (mac) {
        case MAC_NOP: break;
        case MAC_MOV: txt_add(&text, "\tmac = A;\n"); break;
        case MAC_MUL: txt_add(&text, "\tmac = A * B;\n"); break;
        case MAC_ADD: txt_add(&text, "\tmac = A + C;\n"); break;
        case MAC_MAD: txt_add(&text, "\tmac = A * B + C;\n"); break;
        case MAC_DP3: txt_add(&text, "\tmac = vec4(dot(A.xyz, B.xyz));\n"); break;
        case MAC_DPH: txt_add(&text, "\tmac = vec4(dot(A.xyz, B.xyz) + B.w);\n"); break;
        case MAC_DP4: txt_add(&text, "\tmac = vec4(dot(A, B));\n"); break;
        case MAC_DST: txt_add(&text, "\tmac = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case MAC_MIN: txt_add(&text, "\tmac = min(A, B);\n"); break;
        case MAC_MAX: txt_add(&text, "\tmac = max(A, B);\n"); break;
        case MAC_SLT: txt_add(&text, "\tmac = vec4(lessThan(A, B));\n"); break;
        case MAC_SGE: txt_add(&text, "\tmac = vec4(greaterThanEqual(A, B));\n"); break;
        case MAC_ARL: txt_add(&text, "\tmac = A;\n"); break;
        default: txt_add(&text, "\tmac = vec4(0.0);\n"); break;
        }
        switch (ilu) {
        case ILU_NOP: break;
        case ILU_MOV: txt_add(&text, "\tilu = C;\n"); break;
        case ILU_RCP: txt_add(&text, "\tilu = vec4(1.0 / C.x);\n"); break;
        case ILU_RCC: txt_add(&text, "\tilu = nv2a_rcc(C.x);\n"); break;
        case ILU_RSQ: txt_add(&text, "\tilu = vec4(inversesqrt(abs(C.x)));\n"); break;
        case ILU_EXP: txt_add(&text, "\tilu = nv2a_exp(C.x);\n"); break;
        case ILU_LOG: txt_add(&text, "\tilu = nv2a_log(C.x);\n"); break;
        case ILU_LIT: txt_add(&text, "\tilu = nv2a_lit(C);\n"); break;
        default: txt_add(&text, "\tilu = vec4(0.0);\n"); break;
        }

        if (mac == MAC_ARL) {
            txt_add(&text, "\ta0 = int(floor(mac.x + 0.001));\n");
        } else if (mac != MAC_NOP && mac_mask) {
            write_mask(mac_mask, mask);
            if (temporary == 12)
                txt_add(&text, "\toPos.%s = mac.%s;\n", mask, mask);
            else
                txt_add(&text, "\tr%u.%s = mac.%s;\n", temporary, mask, mask);
        }
        if (ilu != ILU_NOP && ilu_mask) {
            unsigned ilu_temporary = mac != MAC_NOP ? 1 : temporary;
            write_mask(ilu_mask, mask);
            if (ilu_temporary == 12)
                txt_add(&text, "\toPos.%s = ilu.%s;\n", mask, mask);
            else
                txt_add(&text, "\tr%u.%s = ilu.%s;\n", ilu_temporary, mask, mask);
        }
        if (output_mask && (output_from_ilu ? ilu : mac) != 0) {
            const char *source = output_from_ilu ? "ilu" : "mac";
            write_mask(output_mask, mask);
            if (output_is_register)
                txt_add(&text, "\t%s.%s = %s.%s;\n",
                        output_name(output_address), mask, source, mask);
        }
        if (field(insn, 3, 0, 1))
            break;
    }

    /* Apple GL 4.1 and GL 3.3 have no glClipControl. Y stays up and depth
     * is -1..1 unless the shader puts them back. */
    txt_add(&text,
            "\tvec3 scale = vec3(viewport_scale.x != 0.0 ? viewport_scale.x : 1.0,\n"
            "\t\tviewport_scale.y != 0.0 ? viewport_scale.y : 1.0,\n"
            "\t\tviewport_scale.z != 0.0 ? viewport_scale.z : 1.0);\n"
            "\tvec3 ndc = (vec3(oPos.xy + vec2(0.5 + screen_offset, 0.5), oPos.z) - viewport_offset.xyz) / scale;\n"
            "\tgl_Position = vec4(ndc * oPos.w, oPos.w);\n"
            "\tgl_Position.y = -gl_Position.y;\n"
            "\tgl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n"
            "\tgl_PointSize = oPts.x;\n"
            "\txD0 = clamp(oD0, 0.0, 1.0);\n"
            "\txD1 = clamp(oD1, 0.0, 1.0);\n"
            "\txB0 = clamp(oB0, 0.0, 1.0);\n"
            "\txB1 = clamp(oB1, 0.0, 1.0);\n"
            "\txT0 = oT0;\n"
            "\txT1 = oT1;\n"
            "\txT2 = oT2;\n"
            "\txT3 = oT3;\n"
            "\txFog = oFog.x;\n"
            "}\n");
    return text.buf;
}
