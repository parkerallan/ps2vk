# ps2vk feature plan

The Vulkan API functions and objects to implement in ps2vk, grouped by area. Each item lists:

- **Vulkan**: the Vulkan object, function or state it corresponds to.
- **ps2vk**: what to add to the ps2vk API (`Pvk*` types, `pvk*` functions, enum values).
- **PS2 implementation**: how it maps onto the hardware (EE, DMA, VIF, VU1, GIF, GS).

Status: ✓ = implemented. *Italic notes* mark features the PS2 can't do natively and how they're approximated.

## Core API

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| ✓ | Instance, device, queue | `VkInstance`, `VkDevice`, `VkQueue` | `PvkInstance`, `PvkDevice`, `PvkQueue` | GIF reset, DMA channel 1 (VIF1) and 2 (GIF) setup |
| ✓ | Swapchain, present at vsync | `VkSwapchainKHR`, `vkQueuePresentKHR` | `PvkSwapchain`, `pvkQueuePresent` | 2 framebuffers in GS VRAM, `DISPFB` switched after vsync |
| ✓ | Buffers in main RAM | `VkBuffer`, `vkMapMemory` | `PvkBuffer`, `pvkMapMemory` | 64-byte-aligned EE RAM, cache written back at submit |
| ✓ | Shader modules, graphics pipeline | `VkShaderModule`, `vkCreateGraphicsPipelines` | `PvkShaderModule`, `PvkPipeline` | VU1 microcode + GS `PRIM` value + VU1 context constants |
| ✓ | Command buffers | `VkCommandBuffer` | `PvkCommandBuffer` | VIF1 DMA chain built with libpacket2 |
| ✓ | Dynamic rendering with clear | `vkCmdBeginRendering` | `pvkCmdBeginRendering` | GS `FRAME`/`ZBUF`/`XYOFFSET`/`SCISSOR` via VIF `DIRECT`, clear with sprites |
| ✓ | Fences | `VkFence` | `PvkFence` | GS `FINISH` register, CPU polls `CSR` |
| | Hardware capability queries: what the PS2 supports, so apps check instead of guess | `VkPhysicalDevice`, `vkGetPhysicalDeviceProperties/Features`, `VkPhysicalDeviceLimits` | `PvkPhysicalDevice`, `pvkGetPhysicalDeviceProperties` | Constants: 4 MB VRAM, 1024×1024 max texture, 7 mip levels, 16 KB VU1 memory |
| | Separate memory allocation: allocate once, place many buffers/images in it | `VkDeviceMemory`, `vkAllocateMemory`, `vkBindBufferMemory`, `vkBindImageMemory` | `PvkDeviceMemory`, `pvkAllocateMemory`, `pvkBind*Memory` | Heap 0 = EE RAM, heap 1 = GS VRAM (8 KB pages) |
| | Command pools: cheap creation and reset of many command buffers | `VkCommandPool`, `vkAllocateCommandBuffers`, `vkResetCommandPool` | `PvkCommandPool` | One memory arena that packet2 chains are carved from |
| | Secondary command buffers: record once, replay every frame | `VK_COMMAND_BUFFER_LEVEL_SECONDARY`, `vkCmdExecuteCommands` | `pvkCmdExecuteCommands` | DMA `CALL` tag into the recorded chain, `RET` at its end |
| | Pipeline layouts: declare what resources a pipeline's shaders read | `VkPipelineLayout` | `PvkPipelineLayout` | Fixes the VU1 context-memory layout and which GS texture registers are used |
| | Descriptor sets: bind a texture and constants in one call | `VkDescriptorSetLayout`, `VkDescriptorPool`, `VkDescriptorSet`, `vkUpdateDescriptorSets`, `vkCmdBindDescriptorSets` | Same names with `Pvk`/`pvk` | Prebuilt packet: `UNPACK` constants to VU1 + `DIRECT` GS `TEX0`/`TEX1`/`CLAMP`; bound with a DMA `CALL` |
| | Uniform buffers: constant blocks such as camera matrix, lights | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | `PVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | VIF `UNPACK` into the VU1 context area |
| | Push constants: small per-draw values written into the command list | `VkPushConstantRange`, `vkCmdPushConstants` | `pvkCmdPushConstants` | Inline `UNPACK` to a reserved VU1 range |
| | Specialization constants: one GLSL shader, several baked variants | `VkSpecializationInfo` | `PvkSpecializationInfo` | shaderc emits variants, or constants patched at pipeline creation |
| | Dynamic state: change viewport/scissor/blend constant while recording | `VkPipelineDynamicStateCreateInfo`, `vkCmdSetViewport`, `vkCmdSetScissor`, `vkCmdSetBlendConstants` | `pvkCmdSetViewport`, `pvkCmdSetScissor`, `pvkCmdSetBlendConstants` | Viewport: VU1 context write; scissor `SCISSOR`; blend `ALPHA.FIX` |
| | Interrupt-driven fences: hardware notifies the CPU instead of polling; allows multiple frames in flight | `VkFence`, `vkWaitForFences` | `PvkFence`, `pvkWaitForFences` | GS `SIGNAL` with a label per fence, `INTC_GS` handler, EE semaphore (as ps2gl does) |
| | Semaphores: order work between queues without the CPU | `VkSemaphore` | `PvkSemaphore` | VIF `MSKPATH3` + `FLUSHA` between the graphics and transfer queues |
| | Pipeline barriers: later work waits for earlier work | `vkCmdPipelineBarrier`, `VkImageMemoryBarrier` | `pvkCmdPipelineBarrier` | VIF `FLUSH`/`FLUSHE`/`FLUSHA`, GS `TEXFLUSH` |
| | Transfer queue: upload textures while drawing continues | `VkQueue` with `VK_QUEUE_TRANSFER_BIT` | Second `PvkQueue` | DMA channel 2 straight to the GIF (PATH3) |
| | Queries: GPU timing for profiling | `VkQueryPool`, `vkCmdWriteTimestamp`, `vkGetQueryPoolResults` | `PvkQueryPool`, `pvkCmdWriteTimestamp` | EE cycle counter read when a GS `SIGNAL` interrupt fires. *No occlusion queries in hardware* |
| | Pipeline cache: skip re-uploading shaders already in VU1 | `VkPipelineCache` | `PvkPipelineCache` | Track which programs are resident in VU1's 16 KB program memory; skip `MPG` |
| | Validation: report misuse of the API | `vkCreateDebugUtilsMessengerEXT` | `pvkCreateDebugMessenger` | Checks in the library, compiled out of release builds |
| | Debug names and labels | `vkSetDebugUtilsObjectNameEXT`, `vkCmdBeginDebugUtilsLabelEXT` | `pvkSetObjectName`, `pvkCmdBeginLabel` | Names in error messages; VIF `MARK` codes visible in PCSX2 dumps |

## Drawing

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| ✓ | Triangle lists | `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` | `PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` | GS `PRIM` = triangle |
| | Triangle strips and fans: shared vertices, less data | `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP`, `..._TRIANGLE_FAN` | Same enum values | GS `PRIM` = strip/fan; VU1 batches overlap by 2 vertices |
| | Points, lines, line strips | `VK_PRIMITIVE_TOPOLOGY_POINT_LIST`, `..._LINE_LIST`, `..._LINE_STRIP` | Same enum values, plus `PVK_PRIMITIVE_TOPOLOGY_SPRITE_LIST` for the GS's native rectangles | GS `PRIM` = point/line/line strip/sprite |
| | Indexed drawing: reuse vertices through an index list | `vkCmdBindIndexBuffer`, `vkCmdDrawIndexed`, `VK_INDEX_TYPE_UINT16` | `pvkCmdBindIndexBuffer`, `pvkCmdDrawIndexed` | Indices and vertices unpacked to VU1, which fetches by index (ps2gl's indexed renderer) |
| | Instanced drawing: same mesh many times in one call | `instanceCount` in `vkCmdDraw`, `VK_VERTEX_INPUT_RATE_INSTANCE` | `instanceCount` parameter, per-instance attributes | Per-instance data unpacked to VU1; VU1 loops the mesh per instance |
| | Indirect drawing: draw parameters read from a buffer | `vkCmdDrawIndirect`, `VkDrawIndirectCommand` | `pvkCmdDrawIndirect` | EE reads the parameters when the command buffer is built |
| | Multiple vertex buffers: attributes in separate buffers | `vkCmdBindVertexBuffers`, `VkVertexInputBindingDescription` | `pvkCmdBindVertexBuffers`, binding descriptions | One `UNPACK` per binding with VIF `STCYCL` to interleave in VU1 memory |
| | Compact vertex formats: 16/8-bit attributes save memory and DMA | `VK_FORMAT_R16G16B16_SNORM`, `R8G8B8A8_UNORM`, etc. | Matching `PvkFormat` values | VIF `UNPACK` `V3_16`, `V4_8` etc. expand to floats for free |
| | Back-face culling: skip triangles facing away | `VkPipelineRasterizationStateCreateInfo` `cullMode`, `frontFace` | Same fields in pipeline create info | VU1 epilogue checks winding and sets the XYZ2 "don't draw" bit |

## Render targets and output

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Depth buffer: near objects hide far ones | `VkImage` with `D16`/`D24`/`D32` format, `pDepthAttachment` in `VkRenderingInfo` | Depth `PvkFormat`s, depth attachment in `PvkRenderingInfo` | GS `ZBUF` with `PSMZ16`/`PSMZ24`/`PSMZ32` |
| | Depth test and write control | `VkPipelineDepthStencilStateCreateInfo` | `PvkPipelineDepthStencilStateCreateInfo` | GS `TEST.ZTE`/`ZTST`, `ZBUF.ZMSK`; depth flipped because the GS only has ≥ and > tests |
| | Images you can render into and sample: render to texture | `vkCreateImage` with usage `COLOR_ATTACHMENT` + `SAMPLED`, `vkCreateImageView` | `pvkCreateImage`, `pvkCreateImageView` | `FRAME` points at the texture's VRAM; `TEX0` later reads the same address |
| | Load/store ops | `VkAttachmentLoadOp`, `VkAttachmentStoreOp` | `PvkAttachmentLoadOp`, `PvkAttachmentStoreOp` | Clear = sprite fill; load/store/don't care = nothing (GS draws straight into VRAM) |
| | Color write mask | `VkPipelineColorBlendAttachmentState.colorWriteMask` | Same field | GS `FRAME.FBMSK` |
| | Scissor rectangles | `VkPipelineViewportStateCreateInfo`, `vkCmdSetScissor` | Pipeline field + `pvkCmdSetScissor` | GS `SCISSOR` |
| | Video modes: NTSC, PAL, 480p | `VkSurfaceKHR`, `VK_KHR_display` display modes | `PvkSwapchainCreateInfo` video mode field | `SetGsCrt` / `graph_set_mode`, GS `SMODE`/`DISPLAY` |
| | Triple buffering, immediate present | `VkPresentModeKHR` (`FIFO`, `MAILBOX`, `IMMEDIATE`), `minImageCount` | `PvkPresentMode`, `imageCount` up to 3 | 3 framebuffers; immediate = write `DISPFB` without waiting for vsync |
| | Custom resolutions and widescreen | `VkSwapchainCreateInfoKHR.imageExtent` | Swapchain width/height | Framebuffer size + GS `DISPLAY` magnification |
| | 16-bit framebuffers: half the VRAM | `VK_FORMAT_A1R5G5B5_UNORM_PACK16` | `PVK_FORMAT_CT16` | GS `PSMCT16` |
| | Framebuffer readback: screenshots, debugging | `vkCmdCopyImageToBuffer` | `pvkCmdCopyImageToBuffer` | GS local-to-host transfer (`BITBLTBUF`, `TRXDIR` = 1) |

## Blending

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Alpha blending: transparency, additive, multiplicative | `VkPipelineColorBlendAttachmentState` (`blendEnable`, `VkBlendFactor`, `VkBlendOp`) | `PvkPipelineColorBlendState` | GS `ALPHA`: (A − B) × C + D, `PRIM.ABE`. Vulkan factor combinations the GS can't express are rejected at pipeline creation |
| | Stencil test | `VkStencilOpState` in `VkPipelineDepthStencilStateCreateInfo` | Stencil fields, limited | GS destination alpha test `TEST.DATE`/`DATM` + `FBA`. *1-bit stand-in for a stencil buffer* |
| | Antialiased lines and edges | `VkPipelineRasterizationLineStateCreateInfoEXT` (smooth lines) | Pipeline flag | GS `PRIM.AA1` |

## Textures

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Texture upload | `vkCmdCopyBufferToImage`, `VkBufferImageCopy` | `pvkCmdCopyBufferToImage` | GIF `IMAGE` transfer with `BITBLTBUF`/`TRXPOS`/`TRXREG`/`TRXDIR` |
| | Texture formats | `VkFormat`: `R8G8B8A8_UNORM`, `R8G8B8_UNORM`, `A1R5G5B5_UNORM_PACK16` | `PVK_FORMAT_CT32/CT24/CT16`, plus PS2-only palettized `PVK_FORMAT_T8`/`T4` with a color table image | GS `PSMCT32/24/16`, `PSMT8`/`PSMT4` with `TEX0.CBP`/`CPSM`/`CSM`/`CLD` |
| | Samplers: filtering and wrap modes | `VkSampler`, `vkCreateSampler` | `PvkSampler`, `pvkCreateSampler` | GS `TEX1.MMAG`/`MMIN`, `CLAMP.WMS`/`WMT` (repeat, clamp, region) |
| | Mipmaps and LOD control | `mipLevels`, `VkSamplerMipmapMode`, `minLod`/`maxLod`/`mipLodBias` | Same fields | GS `MIPTBP1`/`MIPTBP2`, `TEX1.MXL`/`L`/`K` |
| | Image views: sampling a render target or one mip level | `VkImageView`, `vkCreateImageView` | `PvkImageView`, `pvkCreateImageView` | `TEX0` at the image's VRAM address and format |

## Shader compiler (GLSL)

These are what `vkCreateShaderModule` / `pvkCreateShaderModule` must accept. The ps2vk side is `tools/shaderc` plus `PvkShaderCode`.

| Status | Feature | SPIR-V input | PS2 output |
| --- | --- | --- | --- |
| ✓ | Pass-through vertex shaders | `OpLoad`/`OpStore` of inputs and outputs | VU1 `MOVE` + generated prologue/epilogue |
| ✓ | `outColor = vColor` fragment shaders | `OpLoad`/`OpStore` | GS `PRIM.IIP` (Gouraud) |
| ✓ | Errors with line numbers and reasons | Any unsupported opcode | Build-time error |
| ✓ | Arithmetic | `OpFAdd`, `OpFSub`, `OpFMul`, `OpFDiv`, `OpFNegate`, `OpVectorTimesScalar`, float/vec4 constants | VU1 `ADD`, `SUB`, `MUL`, `MULx` broadcast, `DIV` per component through Q; constants loaded once with `LOI`. *Vertex shaders only: the GS can't compute per pixel* |
| | Swizzles, constructors, component access | `OpVectorShuffle`, `OpCompositeConstruct`, `OpCompositeExtract` | VU1 field masks (`.xyz`), `MR32`, broadcast operands |
| | Matrices and matrix × vector | `OpMatrixTimesVector`, `OpTypeMatrix` | VU1 `MULA`/`MADDA`/`MADD` chain (4 instructions) |
| | Built-in functions | `OpExtInst` GLSL.std.450 (`Normalize`, `Pow`, `Sin`, …), `OpDot` | VU1 `DIV`/`SQRT`/`RSQRT`, EFU (`ESIN`, `EEXP`, `ERSQRT`, …) |
| | Uniform blocks and push constants | `OpVariable` `Uniform`/`PushConstant` | Loads from the VU1 context area defined by the pipeline layout |
| | Uniform arrays with dynamic indexing | `OpAccessChain` with a variable index | VU1 integer registers as addresses |
| | Local variables and functions | `OpVariable` `Function`, `OpFunctionCall` | Inlined by `spirv-opt`, kept in registers |
| | `if`/`else` and loops | `OpBranch`, `OpBranchConditional`, `OpLoopMerge` | Unrolled, or VU1 `IBNE`/`IBEQ` branches |
| | Texture coordinate and fog outputs | Extra `Output` variables | `ST`/`Q` and `FOG` in the GIF packet |
| | Texture sampling in fragment shaders | `OpImageSampleImplicitLod` (+ `OpFMul`) | GS `TEX0.TFX` decal / modulate / highlight |
| | `discard` | `OpKill` / `OpDemoteToHelperInvocation` | GS alpha test `TEST.ATE` |
| | Fog via `mix` | `OpExtInst` `FMix` | GS `PRIM.FGE` + `FOGCOL` |
| | Runtime shader files | `vkCreateShaderModule` from file data | `.pvs` file loaded by `pvkCreateShaderModule` |
| | Compute shaders | `ExecutionModel GLCompute` | VU0 microcode (see VU0) |

## Shader stages

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Mesh shaders: shaders that generate geometry | `VK_EXT_mesh_shader` | Vertex shader that emits extra vertices | VU1 writes more output vertices than it reads |
| | Tessellation: subdivide triangles | Tessellation control and evaluation shaders | VU1 shader extension | VU1 subdivides triangles before the epilogue |

## VU0

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Compute queue and pipelines | `VkQueue` with `VK_QUEUE_COMPUTE_BIT`, `vkCreateComputePipelines`, `vkCmdDispatch` | Compute `PvkQueue`, `pvkCreateComputePipeline`, `pvkCmdDispatch` | VU0 micro mode fed by DMA channel 0 (VIF0) |
| | Storage buffers for compute results | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | `PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | VU0 data memory copied back to EE RAM |

## Samples

| Status | Sample | Exercises |
| --- | --- | --- |
| ✓ | Clear | Swapchain, rendering, fences |
| ✓ | Triangle | Pipeline, vertex buffer, shaders |
| | Textured cube | Depth buffer, textures, uniforms, indexed drawing |
| | Lighting | Light uniforms, per-vertex lighting |
| | Skinning | Bone matrices, animation |
| | Particles | Sprites, blending |
| | Render to texture | Render targets sampled as textures |
| | PBR materials | Material system, image-based lighting |
