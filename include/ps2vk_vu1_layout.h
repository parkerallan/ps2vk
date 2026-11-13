/*
 * VU1 data memory layout shared by the ps2vk runtime and tools/shaderc.
 * Addresses are in qwords (16 bytes); VU1 data memory holds 1024.
 *
 *   0   context: viewport scale, viewport offset, color scale, culling (per pipeline)
 *   4   push constants: 8 qwords = 128 bytes, written by pvkCmdPushConstants
 *   16  uniform buffers: 64 qwords = 1 KB; the vertex shader's uniform blocks,
 *       placed by shaderc in (set, binding) order, each filled from the buffer
 *       in the bound descriptor set before the first draw after a bind
 *   80  input:   GIF tag for this batch, a count qword, then vertices, then
 *       for an indexed batch the 16-bit indices, one per qword (VIF UNPACK S_16)
 *   512 output:  GIF tag, then per vertex RGBAQ, XYZ2 (sent with XGKICK), or
 *       ST (UV for a sampler with unnormalized coordinates), RGBAQ, XYZ2 when
 *       the vertex shader writes a texture coordinate
 */
#ifndef PS2VK_VU1_LAYOUT_H
#define PS2VK_VU1_LAYOUT_H

#define PVK_VU1_CONTEXT_ADDR      0
#define PVK_VU1_CTX_VIEWPORT_SCALE  0
#define PVK_VU1_CTX_VIEWPORT_OFFSET 1
#define PVK_VU1_CTX_COLOR_SCALE     2
#define PVK_VU1_CTX_CULL            3 /* x: winding sign flip per triangle (-1 strip, 1 else);
                                         y: int, 1 for a fan; z: int, vertices per triangle
                                         (3 list, 1 strip/fan), 0 = no culling; w: int 0x8000 (ADC) */
#define PVK_VU1_CONTEXT_QWORDS    4

#define PVK_VU1_PUSH_ADDR         4
#define PVK_VU1_PUSH_QWORDS       8   /* PVK_MAX_PUSH_CONSTANT_BYTES / 16 */

#define PVK_VU1_UNIFORM_ADDR      16
#define PVK_VU1_UNIFORM_QWORDS    64  /* PVK_MAX_UNIFORM_BYTES / 16 */

#define PVK_VU1_INPUT_ADDR        80
#define PVK_VU1_INPUT_GIFTAG      0
#define PVK_VU1_INPUT_COUNT       1   /* x: vertices to process; y: qword offset from the
                                         input base of the index list, 0 = vertices in order;
                                         z: float, winding sign of the batch's first triangle
                                         (signed area * z < 0 = cull), VU1 flips it per triangle;
                                         w: int, 1 = store UV (texels, 12.4) instead of ST and Q */
#define PVK_VU1_INPUT_HEADER      2   /* qwords before the first vertex */

#define PVK_VU1_OUTPUT_ADDR       512
#define PVK_VU1_OUTPUT_PER_VERTEX 2   /* RGBAQ, XYZ2 */
#define PVK_VU1_OUTPUT_PER_VERTEX_TEXTURED 3 /* ST, RGBAQ, XYZ2 */

/* VF register a textured vertex shader builds its ST (S/w, T/w, Q = 1/w) or UV output in. */
#define PVK_VU1_TEXCOORD_REG      19

#define PVK_VU1_MEMORY_QWORDS     1024

#endif /* PS2VK_VU1_LAYOUT_H */
