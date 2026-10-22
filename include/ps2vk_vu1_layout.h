/*
 * VU1 data memory layout shared by the ps2vk runtime and tools/shaderc.
 * Addresses are in qwords (16 bytes); VU1 data memory holds 1024.
 *
 *   0   context: viewport scale, viewport offset, color scale
 *   8   input:   GIF tag for this batch, vertex count (x), then vertices
 *   512 output:  GIF tag, then per vertex RGBAQ, XYZ2 (sent with XGKICK)
 */
#ifndef PS2VK_VU1_LAYOUT_H
#define PS2VK_VU1_LAYOUT_H

#define PVK_VU1_CONTEXT_ADDR      0
#define PVK_VU1_CTX_VIEWPORT_SCALE  0
#define PVK_VU1_CTX_VIEWPORT_OFFSET 1
#define PVK_VU1_CTX_COLOR_SCALE     2
#define PVK_VU1_CONTEXT_QWORDS    3

#define PVK_VU1_INPUT_ADDR        8
#define PVK_VU1_INPUT_GIFTAG      0
#define PVK_VU1_INPUT_COUNT       1
#define PVK_VU1_INPUT_HEADER      2   /* qwords before the first vertex */

#define PVK_VU1_OUTPUT_ADDR       512
#define PVK_VU1_OUTPUT_PER_VERTEX 2   /* RGBAQ, XYZ2 */

#define PVK_VU1_MEMORY_QWORDS     1024

#endif /* PS2VK_VU1_LAYOUT_H */
