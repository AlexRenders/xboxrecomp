/*
 * d3d8_gl.c - OpenGL 3.3 backend for the D3D8 compat layer (Linux)
 *
 * The Windows build keeps using D3D11 (d3d8_device.c / d3d8_resources.c /
 * d3d8_states.c / d3d8_combiners.c / d3d8_vsh.c / d3d8_shaders.c). The
 * CMake split picks this file on POSIX.
 *
 * This is the FIRST cut of the real OpenGL backend:
 *   - SDL2 window + GL 3.3 core context
 *   - All Xbox D3D8 COM interfaces wired up (every vtable slot has a body)
 *   - Real Clear, Present, BeginScene/EndScene, viewport, scissor
 *   - Render-state caching with a small "apply" path for depth/blend/cull/etc.
 *   - CreateTexture / CreateVertexBuffer / CreateIndexBuffer with shadow
 *     memory + a GL handle ready for upload
 *   - DrawPrimitive(UP) / DrawIndexedPrimitive(UP) with a fixed GLSL program
 *     supporting position + diffuse + UV (the FVF set the recompiled RW
 *     driver actually emits at this point)
 *   - NV2A vertex-shader bytecode to GLSL (nv2a_glsl.c), used when the
 *     title calls D3DDevice_CreateVertexShader
 *
 * Register-combiner pixel programs are still the fixed modulate. Halo's
 * combiner translator indexes a private state array, not these D3DRS values.
 */

#include "d3d8_xbox.h"
#include "d3d8_fvf.h"
#include "nv2a_glsl.h"

#include <SDL.h>
#include <epoxy/gl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#ifndef D3D_OK
#define D3D_OK ((HRESULT)0)
#endif
#ifndef D3DERR_INVALIDCALL
#define D3DERR_INVALIDCALL ((HRESULT)0x8876086CL)
#endif

/* ======================================================================== */
/* Globals                                                                  */
/* ======================================================================== */

#define MAX_RS    256

/* Window title for the SDL window, configurable per title via
 * xbox_D3D8SetWindowTitle(). Default is a generic name. */
static const char *g_window_title = "Xbox Game";

void xbox_D3D8SetWindowTitle(const char *title)
{
    g_window_title = (title && title[0]) ? title : "Xbox Game";
}
#define MAX_TSS   33
#define MAX_TSU   8     /* texture stages */

typedef struct {
    /* Host */
    SDL_Window      *window;
    SDL_GLContext    glctx;
    int              backbuf_w;
    int              backbuf_h;

    /* D3D state cache */
    DWORD            rs[MAX_RS];
    DWORD            tss[MAX_TSU][MAX_TSS];
    D3DMATRIX        m_world, m_view, m_proj;
    D3DVIEWPORT8     viewport;
    D3DCOLOR         tex_factor;

    /* Current bindings */
    IDirect3DBaseTexture8 *textures[MAX_TSU];
    IDirect3DVertexBuffer8 *vb;
    UINT             vb_stride;
    UINT             vb_offset;
    IDirect3DIndexBuffer8 *ib;
    UINT             ib_base;
    DWORD            current_fvf;
    DWORD            current_vsh;
    DWORD            current_psh;

    /* Lighting */
    D3DMATERIAL8     material;
    D3DLIGHT8        lights[8];
    BOOL             light_enabled[8];

    /* GL draw resources */
    GLuint           vao;
    GLuint           stream_vbo;
    GLuint           stream_ibo;
    GLsizeiptr       stream_vbo_size;
    GLsizeiptr       stream_ibo_size;

    /* GL fixed shader program */
    GLuint           prog;
    GLint            u_mvp;
    GLint            u_use_tex;
    GLint            u_use_xform;
    GLint            u_tex0;

    float            vsc[NV2A_GLSL_CONSTANTS][4];
} D3DGL;

#define NV2A_VS_SLOTS 64

typedef struct {
    int      used;
    uint32_t *code;
    unsigned insn_count;
    GLuint   prog;
    int      compile_failed;
    GLint    u_c, u_scale, u_offset, u_point, u_screen, u_tex, u_use_tex;
} Nv2aSlot;

static Nv2aSlot g_vs[NV2A_VS_SLOTS];

static D3DGL g;

/* ======================================================================== */
/* Tiny helpers                                                             */
/* ======================================================================== */

static void mat4_identity(D3DMATRIX *m)
{
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f;
}

static void mat4_mul(D3DMATRIX *r, const D3DMATRIX *a, const D3DMATRIX *b)
{
    D3DMATRIX t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t.m[i][j] = a->m[i][0]*b->m[0][j] + a->m[i][1]*b->m[1][j]
                      + a->m[i][2]*b->m[2][j] + a->m[i][3]*b->m[3][j];
    *r = t;
}

static GLenum gl_prim(D3DPRIMITIVETYPE t, UINT prim_count, UINT *out_vcount)
{
    switch (t) {
    case D3DPT_POINTLIST:     *out_vcount = prim_count;       return GL_POINTS;
    case D3DPT_LINELIST:      *out_vcount = prim_count * 2;   return GL_LINES;
    case D3DPT_LINESTRIP:     *out_vcount = prim_count + 1;   return GL_LINE_STRIP;
    case D3DPT_TRIANGLELIST:  *out_vcount = prim_count * 3;   return GL_TRIANGLES;
    case D3DPT_TRIANGLESTRIP: *out_vcount = prim_count + 2;   return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN:   *out_vcount = prim_count + 2;   return GL_TRIANGLE_FAN;
    default:                  *out_vcount = prim_count * 3;   return GL_TRIANGLES;
    }
}

static GLenum gl_blend(DWORD b)
{
    switch (b) {
    case D3DBLEND_ZERO:         return GL_ZERO;
    case D3DBLEND_ONE:          return GL_ONE;
    case D3DBLEND_SRCCOLOR:     return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return GL_SRC_ALPHA_SATURATE;
    default:                    return GL_ONE;
    }
}

static GLenum gl_cmp(DWORD f)
{
    switch (f) {
    case D3DCMP_NEVER:        return GL_NEVER;
    case D3DCMP_LESS:         return GL_LESS;
    case D3DCMP_EQUAL:        return GL_EQUAL;
    case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
    case D3DCMP_GREATER:      return GL_GREATER;
    case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
    case D3DCMP_ALWAYS:       return GL_ALWAYS;
    default:                  return GL_LESS;
    }
}

/* ======================================================================== */
/* Fixed GLSL program (position + diffuse + UV, optional MVP transform)     */
/* ======================================================================== */

static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos;\n"
    "layout(location=1) in vec4 a_color;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "uniform int  u_use_xform;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  if (u_use_xform != 0) {\n"
    "    gl_Position = u_mvp * vec4(a_pos.xyz, 1.0);\n"
    "  } else {\n"
    "    /* XYZRHW: a_pos.w is RHW; emit clip-space directly. The caller is\n"
    "       expected to feed already-clip-space coords. */\n"
    "    gl_Position = a_pos;\n"
    "  }\n"
    "  v_color = a_color;\n"
    "  v_uv    = a_uv;\n"
    "}\n";

static const char *FS_SRC =
    "#version 330 core\n"
    "in  vec4 v_color;\n"
    "in  vec2 v_uv;\n"
    "uniform int       u_use_tex;\n"
    "uniform sampler2D u_tex0;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  vec4 c = v_color;\n"
    "  if (u_use_tex != 0) c *= texture(u_tex0, v_uv);\n"
    "  frag_color = c;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log), &n, log);
        fprintf(stderr, "[d3d8_gl] shader compile failed: %.*s\n", n, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log), &n, log);
        fprintf(stderr, "[d3d8_gl] program link failed: %.*s\n", n, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void init_gl_pipeline(void)
{
    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);

    glGenBuffers(1, &g.stream_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.stream_vbo);
    g.stream_vbo_size = 1 << 20;
    glBufferData(GL_ARRAY_BUFFER, g.stream_vbo_size, NULL, GL_STREAM_DRAW);

    glGenBuffers(1, &g.stream_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.stream_ibo);
    g.stream_ibo_size = 1 << 18;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, g.stream_ibo_size, NULL, GL_STREAM_DRAW);

    GLuint vs = compile_shader(GL_VERTEX_SHADER,   VS_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC);
    g.prog = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    g.u_mvp       = glGetUniformLocation(g.prog, "u_mvp");
    g.u_use_tex   = glGetUniformLocation(g.prog, "u_use_tex");
    g.u_use_xform = glGetUniformLocation(g.prog, "u_use_xform");
    g.u_tex0      = glGetUniformLocation(g.prog, "u_tex0");
}

/* ======================================================================== */
/* Apply current render-state cache to GL                                   */
/* ======================================================================== */

static void apply_render_states(void)
{
    /* Depth */
    if (g.rs[D3DRS_ZENABLE])       glEnable(GL_DEPTH_TEST);
    else                            glDisable(GL_DEPTH_TEST);
    glDepthMask(g.rs[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
    glDepthFunc(gl_cmp(g.rs[D3DRS_ZFUNC]));

    /* Blend */
    if (g.rs[D3DRS_ALPHABLENDENABLE]) {
        glEnable(GL_BLEND);
        glBlendFunc(gl_blend(g.rs[D3DRS_SRCBLEND]),
                    gl_blend(g.rs[D3DRS_DESTBLEND]));
    } else {
        glDisable(GL_BLEND);
    }

    /* Cull */
    switch (g.rs[D3DRS_CULLMODE]) {
    case D3DCULL_NONE: glDisable(GL_CULL_FACE); break;
    case D3DCULL_CW:   glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(GL_BACK); break;
    case D3DCULL_CCW:  glEnable(GL_CULL_FACE); glFrontFace(GL_CW);  glCullFace(GL_BACK); break;
    }

    /* Color write */
    DWORD cw = g.rs[D3DRS_COLORWRITEENABLE];
    glColorMask((cw & 1) != 0, (cw & 2) != 0, (cw & 4) != 0, (cw & 8) != 0);
}

/* ======================================================================== */
/* VB / IB / Texture wrapper structs (defined here, not in d3d8_internal.h) */
/* ======================================================================== */

typedef struct {
    IDirect3DVertexBuffer8 iface;
    LONG     ref;
    GLuint   gl_buf;
    UINT     size;
    DWORD    fvf;
    DWORD    usage;
    BYTE    *sys_mem;
    BOOL     dirty;
} GLVertexBuffer;

typedef struct {
    IDirect3DIndexBuffer8 iface;
    LONG     ref;
    GLuint   gl_buf;
    UINT     size;
    D3DFORMAT format;
    BYTE    *sys_mem;
    BOOL     dirty;
} GLIndexBuffer;

typedef struct {
    IDirect3DTexture8 iface;
    LONG     ref;
    GLuint   gl_tex;
    UINT     width, height, levels;
    D3DFORMAT format;
    BYTE    *sys_mem;        /* Level 0 staging */
    UINT     pitch;
    BOOL     dirty;
} GLTexture;

typedef struct {
    IDirect3DSurface8 iface;
    LONG     ref;
    UINT     width, height;
    D3DFORMAT format;
    /* Surfaces are minimal stubs in the first cut. */
} GLSurface;

/* ======================================================================== */
/* IDirect3DVertexBuffer8                                                   */
/* ======================================================================== */

static HRESULT __stdcall vb_QueryInterface(IDirect3DVertexBuffer8 *self, const IID *iid, void **pp)
{ (void)self;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall vb_AddRef(IDirect3DVertexBuffer8 *self)
{ GLVertexBuffer *v=(GLVertexBuffer*)self; return (ULONG)__atomic_add_fetch(&v->ref,1,__ATOMIC_SEQ_CST); }
static ULONG __stdcall vb_Release(IDirect3DVertexBuffer8 *self)
{
    GLVertexBuffer *v=(GLVertexBuffer*)self;
    LONG r=__atomic_sub_fetch(&v->ref,1,__ATOMIC_SEQ_CST);
    if (r<=0) { if (v->gl_buf) glDeleteBuffers(1,&v->gl_buf); free(v->sys_mem); free(v); }
    return (ULONG)r;
}
static HRESULT __stdcall vb_GetDevice(IDirect3DVertexBuffer8 *s, IDirect3DDevice8 **pp)
{ (void)s; *pp=xbox_GetD3DDevice(); return D3D_OK; }
static DWORD __stdcall vb_SetPriority(IDirect3DVertexBuffer8 *s, DWORD p) { (void)s;(void)p; return 0; }
static DWORD __stdcall vb_GetPriority(IDirect3DVertexBuffer8 *s) { (void)s; return 0; }
static void  __stdcall vb_PreLoad(IDirect3DVertexBuffer8 *s) { (void)s; }
static DWORD __stdcall vb_GetType(IDirect3DVertexBuffer8 *s) { (void)s; return 0; }
static HRESULT __stdcall vb_Lock(IDirect3DVertexBuffer8 *self, UINT off, UINT sz, BYTE **pp, DWORD flags)
{
    GLVertexBuffer *v=(GLVertexBuffer*)self; (void)flags;
    if (!v->sys_mem) return D3DERR_INVALIDCALL;
    if (sz == 0) sz = v->size - off;
    *pp = v->sys_mem + off;
    v->dirty = TRUE;
    return D3D_OK;
}
static HRESULT __stdcall vb_Unlock(IDirect3DVertexBuffer8 *self) { (void)self; return D3D_OK; }
static HRESULT __stdcall vb_GetDesc(IDirect3DVertexBuffer8 *s, void *p) { (void)s;(void)p; return D3D_OK; }

static const IDirect3DVertexBuffer8Vtbl g_vb_vtbl = {
    vb_QueryInterface, vb_AddRef, vb_Release,
    vb_GetDevice, vb_SetPriority, vb_GetPriority, vb_PreLoad, vb_GetType,
    vb_Lock, vb_Unlock, vb_GetDesc,
};

/* ======================================================================== */
/* IDirect3DIndexBuffer8                                                    */
/* ======================================================================== */

static HRESULT __stdcall ib_QueryInterface(IDirect3DIndexBuffer8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall ib_AddRef(IDirect3DIndexBuffer8 *s)
{ GLIndexBuffer *v=(GLIndexBuffer*)s; return (ULONG)__atomic_add_fetch(&v->ref,1,__ATOMIC_SEQ_CST); }
static ULONG __stdcall ib_Release(IDirect3DIndexBuffer8 *s)
{
    GLIndexBuffer *v=(GLIndexBuffer*)s;
    LONG r=__atomic_sub_fetch(&v->ref,1,__ATOMIC_SEQ_CST);
    if (r<=0) { if (v->gl_buf) glDeleteBuffers(1,&v->gl_buf); free(v->sys_mem); free(v); }
    return (ULONG)r;
}
static HRESULT __stdcall ib_GetDevice(IDirect3DIndexBuffer8 *s, IDirect3DDevice8 **pp)
{ (void)s; *pp=xbox_GetD3DDevice(); return D3D_OK; }
static DWORD __stdcall ib_SetPriority(IDirect3DIndexBuffer8 *s, DWORD p) { (void)s;(void)p; return 0; }
static DWORD __stdcall ib_GetPriority(IDirect3DIndexBuffer8 *s) { (void)s; return 0; }
static void  __stdcall ib_PreLoad(IDirect3DIndexBuffer8 *s) { (void)s; }
static DWORD __stdcall ib_GetType(IDirect3DIndexBuffer8 *s) { (void)s; return 0; }
static HRESULT __stdcall ib_Lock(IDirect3DIndexBuffer8 *self, UINT off, UINT sz, BYTE **pp, DWORD flags)
{
    GLIndexBuffer *v=(GLIndexBuffer*)self; (void)flags;
    if (!v->sys_mem) return D3DERR_INVALIDCALL;
    if (sz == 0) sz = v->size - off;
    *pp = v->sys_mem + off;
    v->dirty = TRUE;
    return D3D_OK;
}
static HRESULT __stdcall ib_Unlock(IDirect3DIndexBuffer8 *s) { (void)s; return D3D_OK; }
static HRESULT __stdcall ib_GetDesc(IDirect3DIndexBuffer8 *s, void *p) { (void)s;(void)p; return D3D_OK; }

static const IDirect3DIndexBuffer8Vtbl g_ib_vtbl = {
    ib_QueryInterface, ib_AddRef, ib_Release,
    ib_GetDevice, ib_SetPriority, ib_GetPriority, ib_PreLoad, ib_GetType,
    ib_Lock, ib_Unlock, ib_GetDesc,
};

/* ======================================================================== */
/* IDirect3DTexture8                                                        */
/* ======================================================================== */

static HRESULT __stdcall tex_QueryInterface(IDirect3DTexture8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall tex_AddRef(IDirect3DTexture8 *s)
{ GLTexture *t=(GLTexture*)s; return (ULONG)__atomic_add_fetch(&t->ref,1,__ATOMIC_SEQ_CST); }
static ULONG __stdcall tex_Release(IDirect3DTexture8 *s)
{
    GLTexture *t=(GLTexture*)s;
    LONG r=__atomic_sub_fetch(&t->ref,1,__ATOMIC_SEQ_CST);
    if (r<=0) { if (t->gl_tex) glDeleteTextures(1,&t->gl_tex); free(t->sys_mem); free(t); }
    return (ULONG)r;
}
static HRESULT __stdcall tex_GetDevice(IDirect3DTexture8 *s, IDirect3DDevice8 **pp)
{ (void)s; *pp=xbox_GetD3DDevice(); return D3D_OK; }
static DWORD __stdcall tex_SetPriority(IDirect3DTexture8 *s, DWORD p) { (void)s;(void)p; return 0; }
static DWORD __stdcall tex_GetPriority(IDirect3DTexture8 *s) { (void)s; return 0; }
static void  __stdcall tex_PreLoad(IDirect3DTexture8 *s) { (void)s; }
static DWORD __stdcall tex_GetType(IDirect3DTexture8 *s) { (void)s; return 0; }
static DWORD __stdcall tex_GetLevelCount(IDirect3DTexture8 *s)
{ GLTexture *t=(GLTexture*)s; return t->levels; }
static HRESULT __stdcall tex_GetLevelDesc(IDirect3DTexture8 *s, UINT lvl, D3DSURFACE_DESC *pD)
{
    GLTexture *t=(GLTexture*)s;
    if (!pD || lvl>=t->levels) return D3DERR_INVALIDCALL;
    memset(pD,0,sizeof(*pD));
    pD->Format = t->format;
    pD->Width  = t->width  >> lvl;  if (pD->Width  == 0) pD->Width  = 1;
    pD->Height = t->height >> lvl;  if (pD->Height == 0) pD->Height = 1;
    return D3D_OK;
}
static HRESULT __stdcall tex_GetSurfaceLevel(IDirect3DTexture8 *s, UINT lvl, IDirect3DSurface8 **pp)
{ (void)s;(void)lvl; if(pp) *pp=NULL; return D3D_OK; }
static HRESULT __stdcall tex_LockRect(IDirect3DTexture8 *self, UINT lvl, D3DLOCKED_RECT *pLR,
                                       const RECT *r, DWORD flags)
{
    GLTexture *t=(GLTexture*)self; (void)lvl;(void)r;(void)flags;
    if (!pLR || !t->sys_mem) return D3DERR_INVALIDCALL;
    pLR->Pitch = (INT)t->pitch;
    pLR->pBits = t->sys_mem;
    t->dirty = TRUE;
    return D3D_OK;
}
static HRESULT __stdcall tex_UnlockRect(IDirect3DTexture8 *self, UINT lvl)
{
    GLTexture *t=(GLTexture*)self; (void)lvl;
    if (t->dirty && t->gl_tex) {
        glBindTexture(GL_TEXTURE_2D, t->gl_tex);
        /* First cut: assume BGRA8 layout (matches D3DFMT_A8R8G8B8/X8R8G8B8). */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     t->width, t->height, 0,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, t->sys_mem);
        t->dirty = FALSE;
    }
    return D3D_OK;
}

static const IDirect3DTexture8Vtbl g_tex_vtbl = {
    tex_QueryInterface, tex_AddRef, tex_Release,
    tex_GetDevice, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType,
    tex_GetLevelCount,
    tex_GetLevelDesc, tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect,
};

/* ======================================================================== */
/* IDirect3DSurface8 (minimal stub)                                         */
/* ======================================================================== */

static HRESULT __stdcall sf_QueryInterface(IDirect3DSurface8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall sf_AddRef(IDirect3DSurface8 *s)
{ GLSurface *t=(GLSurface*)s; return (ULONG)__atomic_add_fetch(&t->ref,1,__ATOMIC_SEQ_CST); }
static ULONG __stdcall sf_Release(IDirect3DSurface8 *s)
{
    GLSurface *t=(GLSurface*)s;
    LONG r=__atomic_sub_fetch(&t->ref,1,__ATOMIC_SEQ_CST);
    if (r<=0) free(t);
    return (ULONG)r;
}
static HRESULT __stdcall sf_GetDevice(IDirect3DSurface8 *s, IDirect3DDevice8 **pp)
{ (void)s; *pp=xbox_GetD3DDevice(); return D3D_OK; }
static HRESULT __stdcall sf_GetDesc(IDirect3DSurface8 *s, D3DSURFACE_DESC *pD)
{
    GLSurface *t=(GLSurface*)s;
    if (!pD) return D3DERR_INVALIDCALL;
    memset(pD,0,sizeof(*pD));
    pD->Format=t->format; pD->Width=t->width; pD->Height=t->height;
    return D3D_OK;
}
static HRESULT __stdcall sf_LockRect(IDirect3DSurface8 *s, D3DLOCKED_RECT *pL, const RECT *r, DWORD f)
{ (void)s;(void)pL;(void)r;(void)f; return D3DERR_INVALIDCALL; }
static HRESULT __stdcall sf_UnlockRect(IDirect3DSurface8 *s) { (void)s; return D3D_OK; }

static const IDirect3DSurface8Vtbl g_sf_vtbl = {
    sf_QueryInterface, sf_AddRef, sf_Release,
    sf_GetDevice, sf_GetDesc, sf_LockRect, sf_UnlockRect,
};

/* ======================================================================== */
/* IDirect3DDevice8                                                         */
/* ======================================================================== */

static IDirect3DDevice8 g_device;   /* singleton, vtable set at init */

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *s) { (void)s; return 1; }
static ULONG __stdcall dev_Release(IDirect3DDevice8 *s) { (void)s; return 0; }

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *s, IDirect3D8 **pp)
{ (void)s; (void)pp; return D3D_OK; }
static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *s, void *pC) { (void)s;(void)pC; return D3D_OK; }
static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *s, void *pM) { (void)s;(void)pM; return D3D_OK; }
static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *s, void *pP) { (void)s;(void)pP; return D3D_OK; }

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *s, D3DPRESENT_PARAMETERS *pPP)
{ (void)s;(void)pPP; return D3D_OK; }

static void gl_bind_thread(void);

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *s, const RECT *src, const RECT *dst,
                                     HWND hwnd, void *dirty)
{
    (void)s;(void)src;(void)dst;(void)hwnd;(void)dirty;
    gl_bind_thread();
    if (g.window)
        SDL_GL_SwapWindow(g.window);
    /* Pump events so the window stays responsive. Quit closes the window
     * but leaves the process running until the game's loop notices. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            fprintf(stderr, "[d3d8_gl] window close requested\n");
        }
    }
    return D3D_OK;
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *s, INT i, DWORD t,
                                            IDirect3DSurface8 **pp)
{ (void)s;(void)i;(void)t; if(pp) *pp=NULL; return D3D_OK; }

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *s) { (void)s; return D3D_OK; }
static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *s)   { (void)s; return D3D_OK; }

/* The window is created on the main thread. Draws and presents run on the
 * title thread, so that thread has to hold the context. */
static void gl_bind_thread(void)
{
    if (g.window && g.glctx)
        SDL_GL_MakeCurrent(g.window, g.glctx);
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *s, DWORD count, const D3DRECT *rects,
                                    DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    (void)s; (void)count; (void)rects;
    gl_bind_thread();
    GLbitfield m = 0;
    if (flags & D3DCLEAR_TARGET) {
        float a = ((color >> 24) & 0xFF) / 255.0f;
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float gr= ((color >>  8) & 0xFF) / 255.0f;
        float b = ((color      ) & 0xFF) / 255.0f;
        glClearColor(r, gr, b, a);
        m |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & D3DCLEAR_ZBUFFER)  { glClearDepth(z);           m |= GL_DEPTH_BUFFER_BIT;   }
    if (flags & D3DCLEAR_STENCIL)  { glClearStencil((GLint)stencil); m |= GL_STENCIL_BUFFER_BIT; }
    if (m) {
        glDepthMask(GL_TRUE);         /* depth writes must be enabled to clear */
        glColorMask(1,1,1,1);
        glDisable(GL_SCISSOR_TEST);
        glClear(m);
        apply_render_states();        /* restore caller's masks */
    }
    return D3D_OK;
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *s, D3DTRANSFORMSTATETYPE st,
                                          const D3DMATRIX *m)
{
    (void)s; if (!m) return D3DERR_INVALIDCALL;
    switch (st) {
    case D3DTS_WORLD:      g.m_world = *m; break;
    case D3DTS_VIEW:       g.m_view  = *m; break;
    case D3DTS_PROJECTION: g.m_proj  = *m; break;
    default: break;
    }
    return D3D_OK;
}
static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *s, D3DTRANSFORMSTATETYPE st,
                                          D3DMATRIX *m)
{
    (void)s; if (!m) return D3DERR_INVALIDCALL;
    switch (st) {
    case D3DTS_WORLD:      *m = g.m_world; break;
    case D3DTS_VIEW:       *m = g.m_view;  break;
    case D3DTS_PROJECTION: *m = g.m_proj;  break;
    default: mat4_identity(m); break;
    }
    return D3D_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *s, D3DRENDERSTATETYPE st, DWORD v)
{
    (void)s;
    if ((unsigned)st < MAX_RS) g.rs[st] = v;
    if (st == D3DRS_TEXTUREFACTOR) g.tex_factor = v;
    return D3D_OK;
}
static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *s, D3DRENDERSTATETYPE st, DWORD *pv)
{
    (void)s; if (!pv) return D3DERR_INVALIDCALL;
    *pv = ((unsigned)st < MAX_RS) ? g.rs[st] : 0;
    return D3D_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *s, DWORD st,
                                                  D3DTEXTURESTAGESTATETYPE ty, DWORD v)
{
    (void)s;
    if (st < MAX_TSU && (unsigned)ty < MAX_TSS) g.tss[st][ty] = v;
    return D3D_OK;
}
static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *s, DWORD st,
                                                  D3DTEXTURESTAGESTATETYPE ty, DWORD *pv)
{
    (void)s; if (!pv) return D3DERR_INVALIDCALL;
    *pv = (st < MAX_TSU && (unsigned)ty < MAX_TSS) ? g.tss[st][ty] : 0;
    return D3D_OK;
}

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *s, DWORD st, IDirect3DBaseTexture8 *t)
{ (void)s; if (st < MAX_TSU) g.textures[st] = t; return D3D_OK; }
static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *s, DWORD st, IDirect3DBaseTexture8 **pp)
{ (void)s; if (!pp) return D3DERR_INVALIDCALL; *pp = (st < MAX_TSU) ? g.textures[st] : NULL; return D3D_OK; }

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *s, UINT sn,
                                             IDirect3DVertexBuffer8 *pVB, UINT stride)
{ (void)s;(void)sn; g.vb = pVB; g.vb_stride = stride; g.vb_offset = 0; return D3D_OK; }
static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *s, UINT sn,
                                             IDirect3DVertexBuffer8 **pp, UINT *st)
{ (void)s;(void)sn; if(pp) *pp=g.vb; if(st) *st=g.vb_stride; return D3D_OK; }
static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *s, IDirect3DIndexBuffer8 *pIB, UINT base)
{ (void)s; g.ib = pIB; g.ib_base = base; return D3D_OK; }
static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *s, IDirect3DIndexBuffer8 **pp, UINT *pb)
{ (void)s; if(pp) *pp=g.ib; if(pb) *pb=g.ib_base; return D3D_OK; }

/* Vertex layout setup for the FVF the recompiled RW driver actually emits at
 * this point: XYZ (or XYZRHW) + DIFFUSE + optional TEX1. */
static void setup_fvf_attribs(DWORD fvf, UINT stride)
{
    GLboolean xyzrhw = d3d8_fvf_transformed(fvf) ? GL_TRUE : GL_FALSE;
    GLboolean has_diff = (fvf & D3DFVF_DIFFUSE) ? GL_TRUE : GL_FALSE;
    GLboolean has_tex  = ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) > 0;
    UINT off = 0;
    /* Position: 3 floats (XYZ) or 4 floats (XYZRHW) */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, xyzrhw ? 4 : 3, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(uintptr_t)off);
    off = d3d8_fvf_position_bytes(fvf);
    if (fvf & D3DFVF_NORMAL) off += 12;
    /* Diffuse: D3DCOLOR (BGRA byte order, normalised to 0..1) */
    if (has_diff) {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                              (const void *)(uintptr_t)off);
        off += 4;
    } else {
        glDisableVertexAttribArray(1);
        glVertexAttrib4f(1, 1.f, 1.f, 1.f, 1.f);
    }
    /* Specular */
    if (fvf & D3DFVF_SPECULAR) off += 4;
    /* Texcoord 0 */
    if (has_tex) {
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              (const void *)(uintptr_t)off);
    } else {
        glDisableVertexAttribArray(2);
        glVertexAttrib2f(2, 0.f, 0.f);
    }
}

static void bind_texture0_to(GLint loc_tex, GLint loc_use)
{
    int use_tex = 0;
    GLTexture *t = (GLTexture *)g.textures[0];
    if (t && t->gl_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t->gl_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_REPEAT);
        use_tex = 1;
    }
    if (loc_tex >= 0)
        glUniform1i(loc_tex, 0);
    if (loc_use >= 0)
        glUniform1i(loc_use, use_tex);
}

static void bind_texture0(void)
{
    bind_texture0_to(g.u_tex0, g.u_use_tex);
}

/* Apple's GL waits inside glBufferSubData when the previous frame is still
 * reading that buffer. A fresh allocation does not. */
static void stream_upload(GLenum target, GLuint buf, GLsizeiptr *cap,
                          GLsizeiptr size, const void *data)
{
    glBindBuffer(target, buf);
    glBufferData(target, size, data, GL_STREAM_DRAW);
    *cap = size;
}

static const char *NV2A_FS_SRC =
#if defined(__APPLE__)
    "#version 410 core\n"
#else
    "#version 330 core\n"
#endif
    "in vec4 xD0;\n"
    "in vec4 xT0;\n"
    "uniform int u_use_tex;\n"
    "uniform sampler2D u_tex0;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  vec4 c = xD0;\n"
    "  if (u_use_tex != 0) c *= texture(u_tex0, xT0.xy);\n"
    "  frag_color = c;\n"
    "}\n";

static Nv2aSlot *nv2a_slot(DWORD handle)
{
    unsigned i;
    if (!(handle & 0x80000000u))
        return NULL;
    i = handle & 0x7fffffffu;
    if (i == 0 || i > NV2A_VS_SLOTS)
        return NULL;
    if (!g_vs[i - 1].used)
        return NULL;
    return &g_vs[i - 1];
}

static int nv2a_ensure(Nv2aSlot *s)
{
    char *src;
    GLuint vs, fs;

    if (s->prog || s->compile_failed)
        return s->prog != 0;
    src = nv2a_vertex_shader_to_glsl(s->code, s->insn_count, 0);
    if (!src) {
        s->compile_failed = 1;
        return 0;
    }
    vs = compile_shader(GL_VERTEX_SHADER, src);
    free(src);
    fs = compile_shader(GL_FRAGMENT_SHADER, NV2A_FS_SRC);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        s->compile_failed = 1;
        return 0;
    }
    s->prog = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!s->prog) {
        s->compile_failed = 1;
        return 0;
    }
    s->u_c = glGetUniformLocation(s->prog, "c");
    s->u_scale = glGetUniformLocation(s->prog, "viewport_scale");
    s->u_offset = glGetUniformLocation(s->prog, "viewport_offset");
    s->u_point = glGetUniformLocation(s->prog, "point_size");
    s->u_screen = glGetUniformLocation(s->prog, "screen_offset");
    s->u_tex = glGetUniformLocation(s->prog, "u_tex0");
    s->u_use_tex = glGetUniformLocation(s->prog, "u_use_tex");
    return 1;
}

/* 1 when an NV2A program is bound. 0 falls back to the fixed FVF shader. */
static int bind_nv2a_program(void)
{
    Nv2aSlot *s = nv2a_slot(g.current_vsh);
    D3DVIEWPORT8 vp;
    float scale[4], offset[4];

    if (!s || !nv2a_ensure(s))
        return 0;
    glUseProgram(s->prog);
    if (s->u_c >= 0)
        glUniform4fv(s->u_c, NV2A_GLSL_CONSTANTS, &g.vsc[0][0]);
    vp = g.viewport;
    if (vp.Width == 0 || vp.Height == 0) {
        vp.X = 0;
        vp.Y = 0;
        vp.Width = (DWORD)g.backbuf_w;
        vp.Height = (DWORD)g.backbuf_h;
        vp.MinZ = 0.f;
        vp.MaxZ = 1.f;
    }
    scale[0] = vp.Width * 0.5f;
    scale[1] = vp.Height * 0.5f;
    scale[2] = (vp.MaxZ != vp.MinZ) ? (vp.MaxZ - vp.MinZ) : 1.f;
    scale[3] = 1.f;
    offset[0] = vp.X + vp.Width * 0.5f;
    offset[1] = vp.Y + vp.Height * 0.5f;
    offset[2] = vp.MinZ;
    offset[3] = 0.f;
    if (s->u_scale >= 0) glUniform4fv(s->u_scale, 1, scale);
    if (s->u_offset >= 0) glUniform4fv(s->u_offset, 1, offset);
    if (s->u_point >= 0) glUniform1f(s->u_point, 1.f);
    if (s->u_screen >= 0) glUniform1f(s->u_screen, 0.f);
    bind_texture0_to(s->u_tex, s->u_use_tex);
    return 1;
}

static void update_mvp(DWORD fvf)
{
    D3DMATRIX wv, mvp;
    mat4_mul(&wv, &g.m_world, &g.m_view);
    mat4_mul(&mvp, &wv, &g.m_proj);
    /* GL is column-major; D3D MATRIX is row-major. Tell GL to transpose. */
    glUniformMatrix4fv(g.u_mvp, 1, GL_TRUE, (const GLfloat *)mvp.m);
    glUniform1i(g.u_use_xform, d3d8_fvf_transformed(fvf) ? 0 : 1);
}

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt,
                                              UINT prim_count, const void *verts, UINT stride)
{
    (void)s;
    if (!verts || prim_count == 0 || stride == 0) return D3D_OK;
    gl_bind_thread();
    UINT vcount = 0;
    GLenum mode = gl_prim(pt, prim_count, &vcount);
    GLsizeiptr size = (GLsizeiptr)vcount * stride;
    int nv2a = bind_nv2a_program();
    if (!nv2a)
        glUseProgram(g.prog);
    glBindVertexArray(g.vao);
    stream_upload(GL_ARRAY_BUFFER, g.stream_vbo, &g.stream_vbo_size, size, verts);
    setup_fvf_attribs(g.current_fvf, stride);
    if (!nv2a) {
        bind_texture0();
        update_mvp(g.current_fvf);
    }
    apply_render_states();
    glDrawArrays(mode, 0, (GLsizei)vcount);
    return D3D_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt,
        UINT min_vi, UINT nv, UINT prim_count, const void *idx, D3DFORMAT idx_fmt,
        const void *verts, UINT stride)
{
    (void)s; (void)min_vi;
    if (!verts || !idx || prim_count == 0) return D3D_OK;
    gl_bind_thread();
    UINT icount = 0;
    GLenum mode = gl_prim(pt, prim_count, &icount);
    GLenum itype = (idx_fmt == D3DFMT_INDEX32) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
    GLsizeiptr ibytes = (GLsizeiptr)icount * ((itype == GL_UNSIGNED_INT) ? 4 : 2);
    GLsizeiptr vbytes = (GLsizeiptr)nv * stride;
    int nv2a = bind_nv2a_program();
    if (!nv2a)
        glUseProgram(g.prog);
    glBindVertexArray(g.vao);
    stream_upload(GL_ARRAY_BUFFER, g.stream_vbo, &g.stream_vbo_size, vbytes, verts);
    stream_upload(GL_ELEMENT_ARRAY_BUFFER, g.stream_ibo, &g.stream_ibo_size, ibytes, idx);
    setup_fvf_attribs(g.current_fvf, stride);
    if (!nv2a) {
        bind_texture0();
        update_mvp(g.current_fvf);
    }
    apply_render_states();
    glDrawElements(mode, (GLsizei)icount, itype, 0);
    return D3D_OK;
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt,
                                            UINT start, UINT prim_count)
{
    (void)s;
    GLVertexBuffer *vb = (GLVertexBuffer *)g.vb;
    if (!vb || !vb->sys_mem) return D3D_OK;
    UINT vcount = 0;
    GLenum mode = gl_prim(pt, prim_count, &vcount);
    const BYTE *base = vb->sys_mem + start * g.vb_stride;
    return dev_DrawPrimitiveUP(s, pt, prim_count, base, g.vb_stride);
    (void)mode; (void)vcount;
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt,
        UINT min_vi, UINT nv, UINT start_idx, UINT prim_count)
{
    (void)s;
    GLVertexBuffer *vb = (GLVertexBuffer *)g.vb;
    GLIndexBuffer  *ib = (GLIndexBuffer  *)g.ib;
    if (!vb || !ib || !vb->sys_mem || !ib->sys_mem) return D3D_OK;
    D3DFORMAT ifmt = ib->format;
    UINT istride = (ifmt == D3DFMT_INDEX32) ? 4 : 2;
    const BYTE *iptr = ib->sys_mem + start_idx * istride;
    return dev_DrawIndexedPrimitiveUP(s, pt, min_vi, nv, prim_count,
                                       iptr, ifmt, vb->sys_mem, g.vb_stride);
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *s, UINT w, UINT h, UINT lvls,
        DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture8 **pp)
{
    (void)s; (void)usage; (void)pool;
    if (!pp) return D3DERR_INVALIDCALL;
    GLTexture *t = (GLTexture *)calloc(1, sizeof(*t));
    t->iface.lpVtbl = &g_tex_vtbl;
    t->ref = 1; t->width = w; t->height = h; t->format = fmt;
    t->levels = lvls ? lvls : 1;
    t->pitch = w * 4;                              /* assume 32bpp for now */
    t->sys_mem = (BYTE *)calloc(1, t->pitch * h);
    glGenTextures(1, &t->gl_tex);
    *pp = &t->iface;
    return D3D_OK;
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *s, UINT len, DWORD usage,
                                                 DWORD fvf, D3DPOOL pool,
                                                 IDirect3DVertexBuffer8 **pp)
{
    (void)s; (void)pool;
    if (!pp) return D3DERR_INVALIDCALL;
    GLVertexBuffer *v = (GLVertexBuffer *)calloc(1, sizeof(*v));
    v->iface.lpVtbl = &g_vb_vtbl;
    v->ref = 1; v->size = len; v->fvf = fvf; v->usage = usage;
    v->sys_mem = (BYTE *)calloc(1, len ? len : 1);
    glGenBuffers(1, &v->gl_buf);
    *pp = &v->iface;
    return D3D_OK;
}

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *s, UINT len, DWORD usage,
                                                D3DFORMAT fmt, D3DPOOL pool,
                                                IDirect3DIndexBuffer8 **pp)
{
    (void)s; (void)usage; (void)pool;
    if (!pp) return D3DERR_INVALIDCALL;
    GLIndexBuffer *v = (GLIndexBuffer *)calloc(1, sizeof(*v));
    v->iface.lpVtbl = &g_ib_vtbl;
    v->ref = 1; v->size = len; v->format = fmt;
    v->sys_mem = (BYTE *)calloc(1, len ? len : 1);
    glGenBuffers(1, &v->gl_buf);
    *pp = &v->iface;
    return D3D_OK;
}

static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *s, UINT w, UINT h,
        D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, BOOL lockable, IDirect3DSurface8 **pp)
{
    (void)s; (void)ms; (void)lockable;
    if (!pp) return D3DERR_INVALIDCALL;
    GLSurface *t = (GLSurface *)calloc(1, sizeof(*t));
    t->iface.lpVtbl = &g_sf_vtbl;
    t->ref = 1; t->width = w; t->height = h; t->format = fmt;
    *pp = &t->iface;
    return D3D_OK;
}
static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *s, UINT w, UINT h,
        D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8 **pp)
{ return dev_CreateRenderTarget(s, w, h, fmt, ms, FALSE, pp); }

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *s, IDirect3DSurface8 *rt,
                                              IDirect3DSurface8 *zs)
{ (void)s;(void)rt;(void)zs; return D3D_OK; }
static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *s, IDirect3DSurface8 **pp)
{ (void)s; if(pp) *pp=NULL; return D3D_OK; }
static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *s, IDirect3DSurface8 **pp)
{ (void)s; if(pp) *pp=NULL; return D3D_OK; }

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *s, const D3DVIEWPORT8 *vp)
{
    (void)s; if (!vp) return D3DERR_INVALIDCALL;
    g.viewport = *vp;
    glViewport((GLint)vp->X, (GLint)(g.backbuf_h - vp->Y - vp->Height),
               (GLsizei)vp->Width, (GLsizei)vp->Height);
    glDepthRange(vp->MinZ, vp->MaxZ);
    return D3D_OK;
}
static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *s, D3DVIEWPORT8 *vp)
{ (void)s; if (!vp) return D3DERR_INVALIDCALL; *vp = g.viewport; return D3D_OK; }

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *s, const D3DMATERIAL8 *m)
{ (void)s; if (m) g.material = *m; return D3D_OK; }
static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *s, D3DMATERIAL8 *m)
{ (void)s; if (m) *m = g.material; return D3D_OK; }
static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *s, DWORD i, const D3DLIGHT8 *l)
{ (void)s; if (i < 8 && l) g.lights[i] = *l; return D3D_OK; }
static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *s, DWORD i, D3DLIGHT8 *l)
{ (void)s; if (i < 8 && l) *l = g.lights[i]; return D3D_OK; }
static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *s, DWORD i, BOOL en)
{ (void)s; if (i < 8) g.light_enabled[i] = en; return D3D_OK; }

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *s, DWORD h)
{
    (void)s; g.current_vsh = h;
    /* Handle is either a real shader ID (high bit set) or an FVF code. */
    if (!(h & 0x80000000u)) g.current_fvf = h;
    return D3D_OK;
}
static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *s, DWORD *p)
{ (void)s; if (p) *p = g.current_vsh; return D3D_OK; }
static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *s, INT reg,
                                                      const void *p, DWORD n)
{
    (void)s;
    if (!p || reg < 0 || (DWORD)reg >= NV2A_GLSL_CONSTANTS)
        return D3DERR_INVALIDCALL;
    if ((DWORD)reg + n > NV2A_GLSL_CONSTANTS)
        n = NV2A_GLSL_CONSTANTS - (DWORD)reg;
    memcpy(g.vsc[reg], p, n * sizeof(g.vsc[0]));
    return D3D_OK;
}

HRESULT D3DDevice_CreateVertexShader(const DWORD *declaration, const DWORD *function,
                                     DWORD *handle, DWORD usage)
{
    unsigned slot, insns, i;

    (void)declaration;
    (void)usage;
    if (!handle || !function)
        return D3DERR_INVALIDCALL;
    slot = NV2A_VS_SLOTS;
    for (i = 0; i < NV2A_VS_SLOTS; i++) {
        if (!g_vs[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == NV2A_VS_SLOTS)
        return D3DERR_INVALIDCALL;
    insns = 0;
    for (i = 0; i < 136; i++) {
        insns++;
        if (function[i * 4 + 3] & 1)
            break;
    }
    g_vs[slot].code = (uint32_t *)malloc(insns * 4 * sizeof(uint32_t));
    if (!g_vs[slot].code)
        return D3DERR_INVALIDCALL;
    memcpy(g_vs[slot].code, function, insns * 4 * sizeof(uint32_t));
    g_vs[slot].insn_count = insns;
    g_vs[slot].used = 1;
    g_vs[slot].prog = 0;
    g_vs[slot].compile_failed = 0;
    *handle = 0x80000000u | (slot + 1);
    return D3D_OK;
}
static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *s, DWORD h)
{ (void)s; g.current_psh = h; return D3D_OK; }
static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *s, DWORD *p)
{ (void)s; if (p) *p = g.current_psh; return D3D_OK; }
static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *s, INT reg,
                                                     const void *p, DWORD n)
{ (void)s;(void)reg;(void)p;(void)n; return D3D_OK; }

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *s, DWORD f, const D3DGAMMARAMP *r)
{ (void)s;(void)f;(void)r; }
static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *s, D3DGAMMARAMP *r)
{ (void)s;(void)r; }

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *s, DWORD pn, const void *e)
{ (void)s;(void)pn;(void)e; return D3D_OK; }

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *s, DWORD c, DWORD **pp)
{ (void)s;(void)c; if (pp) *pp = NULL; return D3D_OK; }
static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *s, DWORD *p)
{ (void)s;(void)p; return D3D_OK; }

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *s, DWORD flags)
{ return dev_Present(s, NULL, NULL, NULL, NULL); (void)flags; }

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface, dev_AddRef, dev_Release,
    dev_GetDirect3D, dev_GetDeviceCaps, dev_GetDisplayMode, dev_GetCreationParameters,
    dev_Reset, dev_Present, dev_GetBackBuffer,
    dev_BeginScene, dev_EndScene, dev_Clear,
    dev_SetTransform, dev_GetTransform,
    dev_SetRenderState, dev_GetRenderState,
    dev_SetTextureStageState, dev_GetTextureStageState,
    dev_SetTexture, dev_GetTexture,
    dev_SetStreamSource, dev_GetStreamSource,
    dev_SetIndices, dev_GetIndices,
    dev_DrawPrimitive, dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP, dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture, dev_CreateVertexBuffer, dev_CreateIndexBuffer,
    dev_CreateRenderTarget, dev_CreateDepthStencilSurface,
    dev_SetRenderTarget, dev_GetRenderTarget, dev_GetDepthStencilSurface,
    dev_SetViewport, dev_GetViewport,
    dev_SetMaterial, dev_GetMaterial,
    dev_SetLight, dev_GetLight, dev_LightEnable,
    dev_SetVertexShader, dev_GetVertexShader, dev_SetVertexShaderConstant,
    dev_SetPixelShader, dev_GetPixelShader, dev_SetPixelShaderConstant,
    dev_SetGammaRamp, dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush, dev_EndPush,
    dev_Swap,
};

/* ======================================================================== */
/* IDirect3D8 (factory)                                                     */
/* ======================================================================== */

static HRESULT __stdcall d3d_QueryInterface(IDirect3D8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall d3d_AddRef(IDirect3D8 *s)  { (void)s; return 1; }
static ULONG __stdcall d3d_Release(IDirect3D8 *s) { (void)s; return 0; }

static HRESULT __stdcall d3d_CreateDevice(IDirect3D8 *s, UINT adapter, DWORD devtype,
        HWND hwnd, DWORD flags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **pp)
{
    (void)s; (void)adapter; (void)devtype; (void)hwnd; (void)flags;
    if (!pp || !pPP) return D3DERR_INVALIDCALL;

    /* GL 4.1 is the newest Apple ships. Everywhere else stays on 3.3. */
    if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_InitSubSystem(SDL_INIT_VIDEO);

    g.backbuf_w = (int)pPP->BackBufferWidth;
    g.backbuf_h = (int)pPP->BackBufferHeight;
    if (g.backbuf_w <= 0) g.backbuf_w = 640;
    if (g.backbuf_h <= 0) g.backbuf_h = 480;

#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,  SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,  24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    g.window = SDL_CreateWindow(g_window_title,
                                SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                g.backbuf_w, g.backbuf_h,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g.window) {
        fprintf(stderr, "[d3d8_gl] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return D3DERR_INVALIDCALL;
    }
    g.glctx = SDL_GL_CreateContext(g.window);
    if (!g.glctx) {
        fprintf(stderr, "[d3d8_gl] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g.window);
        g.window = NULL;
        return D3DERR_INVALIDCALL;
    }
    SDL_GL_MakeCurrent(g.window, g.glctx);
#if defined(__APPLE__)
    /* SDL's swap wait needs the main-thread run loop. The title draws on
     * another thread, so vsync here stalls the wrong one. The host loop
     * presents the window. */
    SDL_GL_SetSwapInterval(0);
#else
    SDL_GL_SetSwapInterval(1);
#endif

    fprintf(stderr, "[d3d8_gl] GL %s / GLSL %s\n",
            glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    init_gl_pipeline();

    /* Reasonable default states */
    mat4_identity(&g.m_world);
    mat4_identity(&g.m_view);
    mat4_identity(&g.m_proj);
    g.rs[D3DRS_ZENABLE]            = 1;
    g.rs[D3DRS_ZWRITEENABLE]       = 1;
    g.rs[D3DRS_ZFUNC]              = D3DCMP_LESSEQUAL;
    g.rs[D3DRS_ALPHABLENDENABLE]   = 0;
    g.rs[D3DRS_SRCBLEND]           = D3DBLEND_SRCALPHA;
    g.rs[D3DRS_DESTBLEND]          = D3DBLEND_INVSRCALPHA;
    g.rs[D3DRS_CULLMODE]           = D3DCULL_CCW;
    g.rs[D3DRS_COLORWRITEENABLE]   = 0x0F;

    glViewport(0, 0, g.backbuf_w, g.backbuf_h);

    g_device.lpVtbl = &g_device_vtbl;
    *pp = &g_device;
    return D3D_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d_QueryInterface, d3d_AddRef, d3d_Release,
    d3d_CreateDevice,
};

static IDirect3D8 g_d3d8 = { &g_d3d8_vtbl };

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    return &g_d3d8;
}

/* Size the title asked for. The main thread applies it; the game thread
 * only stores it. */
static atomic_uint g_req_w;
static atomic_uint g_req_h;
static atomic_int g_req_gen;
static int g_applied_gen;
static int g_host_loop;

static void host_note_size(unsigned width, unsigned height)
{
    if (width < 16 || width > 1920)
        width = 640;
    if (height < 16 || height > 1080)
        height = 480;
    atomic_store_explicit(&g_req_w, width, memory_order_relaxed);
    atomic_store_explicit(&g_req_h, height, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_req_gen, 1, memory_order_release);
}

static void host_present_clear(void)
{
    if (!g.window || !g.glctx)
        return;
    SDL_GL_MakeCurrent(g.window, g.glctx);
    glClearColor(0.04f, 0.05f, 0.07f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    SDL_GL_SwapWindow(g.window);
}

static int host_create_window(unsigned width, unsigned height)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice8 *dev = NULL;
    IDirect3D8 *d3d;
    HRESULT hr;

    if (g.window)
        return 1;
    host_note_size(width, height);
    width = atomic_load_explicit(&g_req_w, memory_order_relaxed);
    height = atomic_load_explicit(&g_req_h, memory_order_relaxed);

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;

    d3d = xbox_Direct3DCreate8(0);
    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 1, NULL, 0, &pp, &dev);
    fprintf(stderr, "[d3d8_gl] host device %ux%u hr=0x%08X\n",
            width, height, (unsigned)hr);
    if (hr != D3D_OK || !g.window)
        return 0;
    host_present_clear();
    SDL_RaiseWindow(g.window);
    g_applied_gen = atomic_load_explicit(&g_req_gen, memory_order_acquire);
    return 1;
}

void d3d8_HostOpenWindow(void)
{
    host_create_window(640, 480);
}

void d3d8_HostCreateDevice(unsigned width, unsigned height)
{
    host_note_size(width, height);
    fprintf(stderr, "[d3d8_gl] title wants %ux%u\n",
            atomic_load_explicit(&g_req_w, memory_order_relaxed),
            atomic_load_explicit(&g_req_h, memory_order_relaxed));
    /* The window belongs to the thread that is pumping it. */
#if defined(__APPLE__)
    if (!pthread_main_np())
        return;
#endif
    if (!g.window)
        host_create_window(width, height);
}

static void host_apply_size(void)
{
    int gen = atomic_load_explicit(&g_req_gen, memory_order_acquire);
    unsigned w, h;

    if (gen == g_applied_gen || !g.window)
        return;
    w = atomic_load_explicit(&g_req_w, memory_order_relaxed);
    h = atomic_load_explicit(&g_req_h, memory_order_relaxed);
    g_applied_gen = gen;
    if ((int)w == g.backbuf_w && (int)h == g.backbuf_h)
        return;
    g.backbuf_w = (int)w;
    g.backbuf_h = (int)h;
    SDL_SetWindowSize(g.window, g.backbuf_w, g.backbuf_h);
    gl_bind_thread();
    glViewport(0, 0, g.backbuf_w, g.backbuf_h);
    SDL_GL_MakeCurrent(g.window, NULL);
    fprintf(stderr, "[d3d8_gl] window resized to %ux%u\n", w, h);
}

static void host_pump_events(void)
{
    SDL_Event ev;
    static int close_logged;

    if (!g.window)
        return;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT ||
            (ev.type == SDL_WINDOWEVENT &&
             ev.window.event == SDL_WINDOWEVENT_CLOSE)) {
            if (!close_logged) {
                fprintf(stderr, "[d3d8_gl] close ignored; window stays up\n");
                close_logged = 1;
            }
            SDL_ShowWindow(g.window);
            SDL_RaiseWindow(g.window);
        }
    }
}

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern int xbox_FramebufferWindowQuery(uint32_t *va, uint32_t *pitch);

static GLuint s_scan_prog, s_scan_tex, s_scan_vao;

static void host_scanout(void)
{
    uint32_t va, pitch, width, height, sum, i;
    const uint8_t *src;
    static uint32_t logged_va;

    if (!g.window || !g.glctx)
        return;
    if (!xbox_FramebufferWindowQuery(&va, &pitch) || pitch < 8)
        return;
    width = pitch / 4u;
    if (width < 16 || width > 1920)
        return;
    height = 480;
    src = (const uint8_t *)((uintptr_t)va + (uintptr_t)xbox_GetMemoryOffset());
    if (va != logged_va) {
        uint32_t nonzero = 0;
        sum = 0;
        for (i = 0; i < pitch * height; i++) {
            sum += src[i];
            nonzero += src[i] != 0;
        }
        fprintf(stderr, "[d3d8_gl] scanout va=0x%08X %ux%u sum=%u nonzero=%u\n",
                va, width, height, sum, nonzero);
        logged_va = va;
    }
    gl_bind_thread();
    if (!s_scan_prog) {
        static const char *vs_src =
            "#version 330 core\n"
            "layout(location=0) in vec2 a_pos;\n"
            "layout(location=1) in vec2 a_uv;\n"
            "out vec2 v_uv;\n"
            "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
        static const char *fs_src =
            "#version 330 core\n"
            "in vec2 v_uv;\n"
            "uniform sampler2D u_tex;\n"
            "out vec4 frag;\n"
            "void main() { frag = texture(u_tex, v_uv); }\n";
        static const float quad[] = {
            -1.f, -1.f, 0.f, 1.f,
             1.f, -1.f, 1.f, 1.f,
            -1.f,  1.f, 0.f, 0.f,
             1.f,  1.f, 1.f, 0.f,
        };
        GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
        GLuint vbo = 0;
        s_scan_prog = (vs && fs) ? link_program(vs, fs) : 0;
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        if (!s_scan_prog)
            return;
        glGenVertexArrays(1, &s_scan_vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(s_scan_vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
        glGenTextures(1, &s_scan_tex);
        glBindTexture(GL_TEXTURE_2D, s_scan_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g.backbuf_w > 0 ? g.backbuf_w : (GLsizei)width,
                     g.backbuf_h > 0 ? g.backbuf_h : (GLsizei)height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(s_scan_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scan_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, src);
    glUniform1i(glGetUniformLocation(s_scan_prog, "u_tex"), 0);
    glBindVertexArray(s_scan_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    SDL_GL_SwapWindow(g.window);
    SDL_GL_MakeCurrent(g.window, NULL);
}

void d3d8_HostMainLoop(volatile int *done)
{
    if (!g.window)
        host_create_window(640, 480);
    g_host_loop = 1;
    fprintf(stderr, "[d3d8_gl] main thread owns the window\n");
    /* Present is the only swap. Clearing here erased whatever the title drew.
     * The context is released so the title thread can bind it. */
    if (g.window && g.glctx)
        SDL_GL_MakeCurrent(g.window, NULL);
    while (!done || !*done) {
        host_pump_events();
        host_apply_size();
        host_scanout();
        SDL_Delay(16);
    }
    g_host_loop = 0;
}

void d3d8_HostPump(void)
{
    if (!g.window || g_host_loop)
        return;
    /* macOS delivers window events on the thread that created it. */
#if defined(__APPLE__)
    if (!pthread_main_np())
        return;
#endif
    host_pump_events();
}

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return &g_device;
}

void d3d8_PresentFrame(void)
{
    dev_Present(&g_device, NULL, NULL, NULL, NULL);
}

/* Alias used by recomp_manual.c via d3d8_internal.h on both backends. */
IDirect3DDevice8 *d3d8_GetDevice(void)
{
    return &g_device;
}

/* Used by nv2a_pb_replay to skip Present when it owns the frame. The
 * Windows backend defines this in d3d8_device.c; we mirror it here. */
volatile int g_suppress_present = 0;
