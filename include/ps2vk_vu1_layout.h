/*
 * VU1 data memory layout shared by the ps2vk runtime and tools/shaderc.
 * Addresses are in qwords (16 bytes); VU1 data memory holds 1024.
 *
 *   0   context: viewport scale, viewport offset, color scale, culling (per pipeline)
 *   4   push constants: 8 qwords = 128 bytes, written by pvkCmdPushConstants
 *   12  frame: a GIF tag, GS FRAME_1 and FRAME_2 (A+D), filled by pvkCmdBeginRendering;
 *       a color write mask change rewrites an FBMSK word and XGKICKs the packet
 *   16  uniform buffers: 64 qwords = 1 KB; the vertex shader's uniform blocks,
 *       placed by shaderc in (set, binding) order, each filled from the buffer
 *       in the bound descriptor set before the first draw after a bind
 *   80  input:   a 4-qword header, then vertices (one qword per vertex-rate
 *       shader input; a few unused ones may come first and last, as the VIF
 *       unpacks whole 16-byte units of each binding), for an indexed batch the
 *       16-bit indices, one per qword (VIF UNPACK S_16), then the instances
 *       (one qword per instance-rate input, aligned the same way)
 *   512 output:  per instance a GIF tag, then per vertex RGBAQ, XYZ2, or ST (UV
 *       for a sampler with unnormalized coordinates), RGBAQ, XYZ2 when the
 *       vertex shader writes a texture coordinate; one XGKICK sends them all
 *
 * A compute dispatch lays the memory out its own way; the next draw sends the
 * graphics layout again.
 *
 *   0   dispatch header: 4 qwords written by pvkCmdDispatch
 *   4   push constants, as above
 *   12  invocation: 3 qwords the microcode writes before each invocation, then
 *       1 more header qword
 *   16  uniform buffers: the compute shader's uniform blocks, placed as above
 *   16 + the qwords those take: storage blocks, one after another in (set,
 *       binding) order, each the bound buffer range in whole qwords
 *
 * A mesh draw keeps qwords 0 to 79 as above; the task shader's uniform blocks
 * are packed against the end of the uniform area.
 *
 *   80  mesh header: the 8 header and invocation qwords of a compute dispatch
 *   88  GIF tag of a kick, state, culling sign, SetMeshOutputsEXT counts
 *   92  task header: the same 8 qwords, the next work group, the groups left
 *   102 task payload, then the mesh and the task shader's storage blocks
 *   top the mesh shader's outputs: a record per vertex (gl_Position, color,
 *       texture coordinate, fog: a qword each), a record per primitive (indices
 *       in x, y, z, gl_CullPrimitiveEXT in w; a per-primitive color), and two
 *       GIF packets kicked in turn
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

#define PVK_VU1_FRAME_ADDR        12
#define PVK_VU1_FRAME_QWORDS      3

#define PVK_VU1_UNIFORM_ADDR      16
#define PVK_VU1_UNIFORM_QWORDS    64  /* PVK_MAX_UNIFORM_BYTES / 16 */

#define PVK_VU1_INPUT_ADDR        80
#define PVK_VU1_INPUT_GIFTAG      0   /* with EOP; VU1 clears EOP on every instance's copy but the last */
#define PVK_VU1_INPUT_COUNT       1   /* x: vertices per instance; y: qword offset from the input
                                         base of the index list, 0 = vertices in order; z: float,
                                         winding sign of the current triangle (signed area * z < 0
                                         = cull), VU1 flips it per triangle; w: int, 1 = store UV
                                         (texels, 12.4) instead of ST and Q */
#define PVK_VU1_INPUT_FIRST       2   /* x: int, qword offset from the input base of the first
                                         vertex; y: float, winding sign of each instance's first
                                         triangle; z: int, instances; w: int, qword offset from
                                         the input base of the first instance */
#define PVK_VU1_INPUT_STRIDE      3   /* x: int, qwords per vertex; y: int, qwords per instance;
                                         z: float, gl_InstanceIndex of the current instance */
#define PVK_VU1_INPUT_HEADER      4   /* qwords before the first unpacked vertex */

#define PVK_VU1_OUTPUT_ADDR       512
#define PVK_VU1_OUTPUT_PER_VERTEX 2   /* RGBAQ, XYZ2; one more each for ST and for FOG */

/* VF register a textured vertex shader builds its ST (S/w, T/w, Q = 1/w) or UV output in. */
#define PVK_VU1_TEXCOORD_REG      19

#define PVK_VU1_MEMORY_QWORDS     1024

#define PVK_VU1_COMPUTE_GROUPS    0   /* x, y, z: floats, gl_NumWorkGroups */
#define PVK_VU1_COMPUTE_COUNTS    1   /* x, y, z: ints, work groups along each axis */
#define PVK_VU1_COMPUTE_BASES     2   /* x..w: ints, first qword of each storage block */
#define PVK_VU1_COMPUTE_LAST      3   /* x..w: floats, qwords from the first to the last element
                                         of each block's runtime array */
#define PVK_VU1_COMPUTE_WORKGROUP 12  /* x, y, z: floats, gl_WorkGroupID */
#define PVK_VU1_COMPUTE_LOCAL     13  /* x, y, z: floats, gl_LocalInvocationID; w: gl_LocalInvocationIndex */
#define PVK_VU1_COMPUTE_GLOBAL    14  /* x, y, z: floats, gl_GlobalInvocationID */
#define PVK_VU1_COMPUTE_LENGTHS   15  /* x..w: floats, elements in each block's runtime array */
#define PVK_VU1_STORAGE_ADDR      16  /* plus the qwords of the shader's uniform blocks */
#define PVK_VU1_STORAGE_BASE_REG  9   /* VI09..VI12 hold the storage blocks' first qwords */

/* Mesh and task header qwords, in the order of the compute ones above. */
#define PVK_VU1_HEADER_GROUPS     0
#define PVK_VU1_HEADER_COUNTS     1
#define PVK_VU1_HEADER_BASES      2
#define PVK_VU1_HEADER_LAST       3
#define PVK_VU1_HEADER_WORKGROUP  4
#define PVK_VU1_HEADER_LOCAL      5
#define PVK_VU1_HEADER_GLOBAL     6
#define PVK_VU1_HEADER_LENGTHS    7
#define PVK_VU1_HEADER_QWORDS     8

#define PVK_VU1_MESH_ADDR         80
#define PVK_VU1_MESH_GIFTAG       88  /* NLOOP 0 with EOP; a kick writes its vertex count */
#define PVK_VU1_MESH_STATE        89  /* ints. x: micro address to return to, 0 = stop; y: 1 = UV, not ST;
                                         z: 1 = cull by winding; w: 1 = the task group emitted mesh tasks */
#define PVK_VU1_MESH_CULL         90  /* z: float, winding sign (signed area * z < 0 = cull) */
#define PVK_VU1_MESH_OUTPUTS      91  /* ints. x: vertices, y: primitives; w: GIF packet the next kick fills */
#define PVK_VU1_TASK_ADDR         92
#define PVK_VU1_TASK_NEXT         100 /* x, y, z: floats, gl_WorkGroupID of the next task work group */
#define PVK_VU1_TASK_LEFT         101 /* x, y, z: ints, task work groups left along each axis */
#define PVK_VU1_PAYLOAD_ADDR      102
#define PVK_VU1_MESH_ENTRY        1   /* pair a mesh program starts at alone; pair 0 is the task's entry */
#define PVK_VU1_MESH_KICK_QWORDS  61  /* most qwords of one kicked GIF packet */

/* VU1 micro memory, in instruction pairs; the last 4 hold the routine that XGKICKs the frame packet. */
#define PVK_VU1_MICRO_PAIRS       2048
#define PVK_VU1_FRAME_ROUTINE     2044

#endif /* PS2VK_VU1_LAYOUT_H */
