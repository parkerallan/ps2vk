/*
 * VU1 data memory layout shared by the ps2vk runtime and tools/shaderc.
 * Addresses are in qwords (16 bytes); VU1 data memory holds 1024.
 *
 *   0   context: viewport scale, viewport offset, color scale (per pipeline)
 *   4   push constants: 8 qwords = 128 bytes, written by pvkCmdPushConstants
 *   16  input:   GIF tag for this batch, a count qword, then vertices, then
 *       for an indexed batch the 16-bit indices, one per qword (VIF UNPACK S_16)
 *   512 output:  GIF tag, then per vertex RGBAQ, XYZ2 (sent with XGKICK), or
 *       ST, RGBAQ, XYZ2 when the vertex shader writes a texture coordinate
 */
#ifndef PS2VK_VU1_LAYOUT_H
#define PS2VK_VU1_LAYOUT_H

#define PVK_VU1_CONTEXT_ADDR      0
#define PVK_VU1_CTX_VIEWPORT_SCALE  0
#define PVK_VU1_CTX_VIEWPORT_OFFSET 1
#define PVK_VU1_CTX_COLOR_SCALE     2
#define PVK_VU1_CONTEXT_QWORDS    3

#define PVK_VU1_PUSH_ADDR         4
#define PVK_VU1_PUSH_QWORDS       8   /* PVK_MAX_PUSH_CONSTANT_BYTES / 16 */

#define PVK_VU1_INPUT_ADDR        16
#define PVK_VU1_INPUT_GIFTAG      0
#define PVK_VU1_INPUT_COUNT       1   /* x: vertices to process; y: qword offset from the
                                         input base of the index list, 0 = vertices in order */
#define PVK_VU1_INPUT_HEADER      2   /* qwords before the first vertex */

#define PVK_VU1_OUTPUT_ADDR       512
#define PVK_VU1_OUTPUT_PER_VERTEX 2   /* RGBAQ, XYZ2 */
#define PVK_VU1_OUTPUT_PER_VERTEX_TEXTURED 3 /* ST, RGBAQ, XYZ2 */

/* VF register a textured vertex shader builds its ST (S/w, T/w, Q = 1/w) output in. */
#define PVK_VU1_TEXCOORD_REG      19

#define PVK_VU1_MEMORY_QWORDS     1024

#endif /* PS2VK_VU1_LAYOUT_H */
