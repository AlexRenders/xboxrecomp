/**
 * NV2A vertex-shader microcode to GLSL.
 *
 * Adapted from the macOS port of Halo: Combat Evolved (ChupathingyCE,
 * port/linux/src/nv2a_vsh.c, CC0). The instruction encoding is the GPU's,
 * not that game's. The generated shader undoes the Xbox screen-space
 * position and flips Y / depth, because GL 3.3 and Apple's GL 4.1 have
 * no glClipControl.
 */
#ifndef XBOXRECOMP_NV2A_GLSL_H
#define XBOXRECOMP_NV2A_GLSL_H

#include <stdint.h>

#define NV2A_GLSL_CONSTANTS 192
#define NV2A_GLSL_ATTRIBUTES 16

/* Caller frees the returned string. NULL if the program is empty. */
char *nv2a_vertex_shader_to_glsl(const uint32_t *instructions,
                                 unsigned instruction_count,
                                 unsigned packed_attribute_mask);

#endif
