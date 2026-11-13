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
| ✓ | Pipeline layouts: declare what resources a pipeline's shaders read | `VkPipelineLayout`, `VkPushConstantRange` | `PvkPipelineLayout` from push constant ranges and descriptor set layouts; `layout` in the pipeline create info, `pvkCmdPushConstants` takes the layout and stage flags | Per stage, a bit per 4-byte word of the 128-byte VU1 push range, plus copies of the set layouts; pipeline creation checks the vertex shader's block is covered and the fragment shader's sampler2D is a combined image sampler binding in its set layout, draws check the values were set through a layout with the same ranges |
| ✓ | Descriptor sets: bind a texture and constants in one call | `VkDescriptorSetLayout`, `VkDescriptorPool`, `VkDescriptorSet`, `vkUpdateDescriptorSets`, `vkCmdBindDescriptorSets` | Same names with `Pvk`/`pvk`; `PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, up to 4 sets of 8 bindings, no binding arrays; shaderc records the sampler's `set` as well as its `binding` | A set is a table of images and buffer ranges per binding, allocated in order from its pool and given back only by a pool reset; layouts are copied by value into pipeline layouts and sets, and compare by their bindings. The draw state write reads the bound set's image into `TEX0` when the draw is recorded; `TEX0.TFX` comes from the pipeline, so the texture registers stay with the draw state |
| ✓ | Uniform buffers: constant blocks such as camera matrix, lights | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`, `VkDescriptorBufferInfo` | `PVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`, `PvkDescriptorBufferInfo` (buffer, 16-byte-aligned offset, range or `PVK_WHOLE_SIZE`); the vertex shader's blocks are listed in `PvkShaderCode` with their set, binding, size and VU1 qword | 1 KB of VU1 memory between the push constants and the input area. Since shaders are compiled before any layout exists, shaderc fixes the addresses: a shader's blocks are packed in (set, binding) order, and the pipeline checks each is a uniform buffer binding for the vertex stage in the layout's set layout. The first draw after a pipeline or set bind sends every block as a DMA `REF` + VIF `UNPACK` straight from the buffer (no copy, so the buffer's contents at submit time are what the draw sees), after a VIF `FLUSH` so the running batch's prologue is not overwritten. Like push constant qwords, each block qword is loaded into a VF register right before its first use and the register is freed after its last, so a shader may read more of them than VU1 has free registers |
| ✓ | Push constants: small per-draw values written into the command list | `VkPushConstantRange`, `vkCmdPushConstants` | `pvkCmdPushConstants` | Inline `UNPACK` to VU1 qwords 4-11 (128 bytes), after a VIF `FLUSH` so a running batch is not overwritten; `pvkCmdDraw` checks the shader's block was set |
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
| ✓ | Triangle strips and fans: shared vertices, less data | `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP`, `..._TRIANGLE_FAN` | Same enum values | GS `PRIM` = strip/fan, so the GS joins the vertices itself; each VU1 batch after the first repeats the 2 vertices the next triangle shares with the previous batch (for a fan the hub and the last vertex, sent as two blocks), in plain and indexed draws. No primitive restart |
| ✓ | Points, lines, line strips | `VK_PRIMITIVE_TOPOLOGY_POINT_LIST`, `..._LINE_LIST`, `..._LINE_STRIP` | Same enum values, plus `PVK_PRIMITIVE_TOPOLOGY_SPRITE_LIST` for the GS's native rectangles (2 vertices = opposite corners) | GS `PRIM` = point/line/line strip/sprite; lists are cut to whole primitives per batch, a line strip batch repeats 1 vertex |
| ✓ | Indexed drawing: reuse vertices through an index list | `vkCmdBindIndexBuffer`, `vkCmdDrawIndexed`, `VK_INDEX_TYPE_UINT16` | `pvkCmdBindIndexBuffer`, `pvkCmdDrawIndexed` | A batch sends the run of vertices its indices span as one `UNPACK` (no copy) and the indices as `UNPACK S_16`, one qword each, copied into the chain; the batch header's `y` field points VU1 at the list and the generated loop fetches first vertex + index × qwords (integer adds, no multiply). Batches grow a triangle at a time while both fit; a triangle spanning too far is sent as three single-vertex unpacks |
| | Instanced drawing: same mesh many times in one call | `instanceCount` in `vkCmdDraw`, `VK_VERTEX_INPUT_RATE_INSTANCE` | `instanceCount` parameter, per-instance attributes | Per-instance data unpacked to VU1; VU1 loops the mesh per instance |
| | Indirect drawing: draw parameters read from a buffer | `vkCmdDrawIndirect`, `VkDrawIndirectCommand` | `pvkCmdDrawIndirect` | EE reads the parameters when the command buffer is built |
| | Multiple vertex buffers: attributes in separate buffers | `vkCmdBindVertexBuffers`, `VkVertexInputBindingDescription` | `pvkCmdBindVertexBuffers`, binding descriptions | One `UNPACK` per binding with VIF `STCYCL` to interleave in VU1 memory |
| | Compact vertex formats: 16/8-bit attributes save memory and DMA | `VK_FORMAT_R16G16B16_SNORM`, `R8G8B8A8_UNORM`, etc. | Matching `PvkFormat` values | VIF `UNPACK` `V3_16`, `V4_8` etc. expand to floats for free |
| ✓ | Back-face culling: skip triangles facing away | `VkPipelineRasterizationStateCreateInfo` `cullMode`, `frontFace` | `PvkPipelineRasterizationStateCreateInfo` (`cullMode`, `frontFace`, Vulkan's values) as `rasterization` in the pipeline create info; triangle topologies only, FRONT_AND_BACK skips the draw | The epilogue computes each triangle's signed area in screen space when its last vertex is transformed (the two previous positions stay in VF25/VF26, a fan keeps its hub) and sets that vertex's XYZ2 ADC bit when area × sign < 0, so the GS queues the vertex but draws nothing. A context qword says how the topology forms triangles and the batch header carries the sign, flipped per triangle for a strip's alternating winding; the test is skipped when culling is off |

## Render targets and output

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| ✓ | Depth buffer: near objects hide far ones | `VkImage` with `D16`/`D24`/`D32` format, `pDepthAttachment` in `VkRenderingInfo` | `PVK_FORMAT_Z32/Z24/Z16`, `pvkCreateImage` (depth formats only so far), `depthAttachment` in `PvkRenderingInfo` with a `PvkClearValue` union | GS `ZBUF` with `PSMZ16`/`PSMZ24`/`PSMZ32`, page aligned in VRAM; must be as wide as the framebuffer (one `FBW`); a clear is one sprite pass writing color, depth or both with the other masked (`FBMSK`/`ZMSK`) |
| ✓ | Depth test and write control | `VkPipelineDepthStencilStateCreateInfo` | `PvkPipelineDepthStencilStateCreateInfo` and `depthAttachmentFormat` in the pipeline; `PvkCompareOp` LESS, LESS_OR_EQUAL, ALWAYS, NEVER | GS `TEST.ZTE`/`ZTST`, `ZBUF.ZMSK`, sent before the first draw after a bind or begin; depth flipped because the GS only has ≥ and > tests, scaled to the format's range (Z32 stops below 2³¹ for `ftoi0`); the alpha test is off |
| ✓ | Images you can render into and sample: render to texture | `vkCreateImage` with usage `COLOR_ATTACHMENT` + `SAMPLED`, `vkCreateImageView` | `usage` in `PvkImageCreateInfo` (`PvkImageUsageFlagBits`, 0 = the format's usual use); a texture with `COLOR_ATTACHMENT_BIT` goes into `PvkRenderingAttachmentInfo` as a one-level `PvkImageView` and into a set as another view | Each level of such an image is page aligned (`FRAME.FBP` counts 8 KB pages where `TEX0.TBP` counts 256-byte blocks) and at least 64 wide (`FBW` units); `pvkCmdBeginRendering` sets `FRAME`/`SCISSOR` from the view's level and `TEX0` later reads the same address. The draw state before a sampling draw already carries `TEXFLUSH`, so a pass may sample what the previous pass of the same command buffer drew |
| | Load/store ops | `VkAttachmentLoadOp`, `VkAttachmentStoreOp` | `PvkAttachmentLoadOp`, `PvkAttachmentStoreOp` | Clear = sprite fill; load/store/don't care = nothing (GS draws straight into VRAM) |
| | Color write mask | `VkPipelineColorBlendAttachmentState.colorWriteMask` | Same field | GS `FRAME.FBMSK` |
| | Scissor rectangles | `VkPipelineViewportStateCreateInfo`, `vkCmdSetScissor` | Pipeline field + `pvkCmdSetScissor` | GS `SCISSOR` |
| ✓ | Video modes: NTSC, PAL, 480p | `VkSurfaceKHR`, `VK_KHR_display` display modes | `PvkDisplayMode` in `PvkSwapchainCreateInfo` (auto, NTSC, PAL, 480p) | `graph_set_mode`; interlaced modes are field mode and present only on even fields (`CSR.FIELD`), so both fields show one frame; 480p flips every vsync |
| | Triple buffering, immediate present | `VkPresentModeKHR` (`FIFO`, `MAILBOX`, `IMMEDIATE`), `minImageCount` | `PvkPresentMode`, `imageCount` up to 3 | 3 framebuffers; immediate = write `DISPFB` without waiting for vsync |
| | Custom resolutions and widescreen | `VkSwapchainCreateInfoKHR.imageExtent` | Swapchain width/height | Framebuffer size + GS `DISPLAY` magnification |
| | 16-bit framebuffers: half the VRAM | `VK_FORMAT_A1R5G5B5_UNORM_PACK16` | `PVK_FORMAT_CT16` | GS `PSMCT16` |
| | Framebuffer readback: screenshots, debugging | `vkCmdCopyImageToBuffer` | `pvkCmdCopyImageToBuffer` | GS local-to-host transfer (`BITBLTBUF`, `TRXDIR` = 1) |

## Blending

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| ✓ | Alpha blending: transparency, additive, subtractive | `VkPipelineColorBlendAttachmentState` (`blendEnable`, `VkBlendFactor`, `VkBlendOp`), `blendConstants` | `PvkPipelineColorBlendStateCreateInfo` in the pipeline create info; `PvkBlendFactor` ZERO, ONE and the `*_ALPHA` factors, `PvkBlendOp` ADD, SUBTRACT, REVERSE_SUBTRACT | GS `ALPHA`: (A − B) × C + D, `PRIM.ABE`. Factor combinations the GS can't express are rejected at pipeline creation. *No multiplicative blending: the GS has no color factor* |
| | Stencil test | `VkStencilOpState` in `VkPipelineDepthStencilStateCreateInfo` | Stencil fields, limited | GS destination alpha test `TEST.DATE`/`DATM` + `FBA`. *1-bit stand-in for a stencil buffer* |
| | Antialiased lines and edges | `VkPipelineRasterizationLineStateCreateInfoEXT` (smooth lines) | Pipeline flag | GS `PRIM.AA1` |

## Textures

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| ✓ | Texture upload | `vkCmdCopyBufferToImage`, `VkBufferImageCopy` | `pvkCmdCopyBufferToImage` with a `PvkBufferImageCopy` (buffer offset + rectangle); `pvkCreateImage` with `PVK_FORMAT_CT32` makes the texture (power of two, 8 to 1024) | `BITBLTBUF`/`TRXPOS`/`TRXREG`/`TRXDIR` by VIF `DIRECT`, then the pixels as GIF `IMAGE` packets streamed from the buffer by DMA `REF` tags (16K qwords each, the `DIRECT` code in the tag), outside rendering. Bound textures get `TEX0`/`TEX1`/`CLAMP`/`TEXFLUSH` with the draw state |
| ✓ | Texture formats | `VkFormat`: `R8G8B8A8_UNORM`, `R8G8B8_UNORM`, `A1R5G5B5_UNORM_PACK16` | `PVK_FORMAT_CT32/CT24/CT16`, plus PS2-only palettized `PVK_FORMAT_T8`/`T4` with a color table: a CT32 or CT16 image 256 or 16 wide and 1 high, paired with the texture as `colorTable` in `PvkDescriptorImageInfo` | GS `PSMCT32/24/16`, `PSMT8`/`PSMT4` with `TEX0.CBP`/`CPSM`, `CLD` = 1; the table is copied into the chain in CSM1 layout (index bits 3 and 4 swapped), `TEXA` gives CT24 and CT16 their alpha. A texture takes every VRAM block its rows touch |
| ✓ | Samplers: filtering and wrap modes | `VkSampler`, `vkCreateSampler`, `VkDescriptorImageInfo.sampler` | `PvkSampler`, `pvkCreateSampler` with `PvkFilter` (nearest, linear) for mag and min, `PvkSamplerAddressMode` per axis (repeat, clamp to edge), `sampler` in `PvkDescriptorImageInfo`, required | GS `TEX1.MMAG`/`MMIN` and `CLAMP.WMS`/`WMT`, written with `TEX0` in the draw state from the bound set's sampler. *Mirrored repeat, clamp to border, anisotropy and compare samplers are rejected at creation: the GS has none of them* |
| ✓ | Mipmaps and LOD control | `mipLevels`, `VkBufferImageCopy.imageSubresource.mipLevel`, `VkSamplerMipmapMode`, `minLod`/`maxLod`/`mipLodBias` | `mipLevels` in `PvkImageCreateInfo` (up to `PVK_MAX_MIP_LEVELS` = 7, each level at least 8 texels a side), `mipLevel` in `PvkBufferImageCopy`; `mipmapMode`, `mipLodBias`, `maxLod` (`PVK_LOD_CLAMP_NONE` for every level) in `PvkSamplerCreateInfo`, `minLod` must be 0 | Every level is its own block-aligned VRAM allocation; `TEX0` points at level 0 and `MIPTBP1`/`MIPTBP2` at levels 1 to 6, written with the draw state when the image has more than one. `TEX1.MXL` is the smaller of the image's last level and `maxLod`, `MMIN` becomes the mipmap variant when it is above 0. *The GS has no screen-space derivatives: `LCM` = 0, `L` = 0 and `K` = `mipLodBias` give LOD = log2(w) + bias, clamped to 0..`MXL`, so the bias must hold log2 of the texels per pixel at w = 1 (log2(texels / (world size × focal length)) in perspective), and the same LOD picks the mag filter below 0. No lower clamp, so `minLod` is rejected* |
| ✓ | Unnormalized coordinates: texture coordinates in texels, for sprite sheets and text | `VkSamplerCreateInfo.unnormalizedCoordinates` | `unnormalizedCoordinates` in `PvkSamplerCreateInfo`; the vertex shader's `vec2` output is then in texels. Vulkan's rules apply: clamp to edge on both axes, `magFilter` = `minFilter`, nearest mipmapping, `maxLod` 0 | GS `PRIM.FST` = 1 with `UV` instead of `ST`/`Q`, from the bound set's sampler when the draw is recorded: the batch header's `w` field carries the flag and the GIF tag names `UV`; the epilogue branches on it, stores the coordinate as 12.4 fixed point (`ftoi4`) and skips the divide by w. *No perspective correction* |
| ✓ | Image views: sampling a render target or one mip level | `VkImageView`, `vkCreateImageView` | `PvkImageView`, `pvkCreateImageView` with `baseMipLevel` and `levelCount` (`PVK_REMAINING_MIP_LEVELS`); `imageView` replaces the image in `PvkDescriptorImageInfo` and `PvkRenderingAttachmentInfo`, swapchain images get views too; the format must be the image's | A record of the image and level range. `TEX0` takes the base level's address, buffer width and log2 size (smaller for a higher base), `MIPTBP1`/`2` the levels after it and `MXL` the view's count, so one image is sampled from different levels by different sets; an attachment view names the level `FRAME` or `ZBUF` points at |

## Shader compiler (GLSL)

These are what `vkCreateShaderModule` / `pvkCreateShaderModule` must accept. The ps2vk side is `tools/shaderc` plus `PvkShaderCode`.

| Status | Feature | SPIR-V input | PS2 output |
| --- | --- | --- | --- |
| ✓ | Pass-through vertex shaders | `OpLoad`/`OpStore` of inputs and outputs | VU1 `MOVE` + generated prologue/epilogue |
| ✓ | `outColor = vColor` fragment shaders | `OpLoad`/`OpStore` | GS `PRIM.IIP` (Gouraud) |
| ✓ | Errors with line numbers and reasons | Any unsupported opcode | Build-time error |
| ✓ | Arithmetic | `OpFAdd`, `OpFSub`, `OpFMul`, `OpFDiv`, `OpFNegate`, `OpVectorTimesScalar`, float/vec4 constants | VU1 `ADD`, `SUB`, `MUL`, `MULx` broadcast, `DIV` per component through Q; constants loaded once with `LOI`. *Vertex shaders only: the GS can't compute per pixel* |
| ✓ | Swizzles, constructors, component access | `OpVectorShuffle`, `OpCompositeConstruct`, `OpCompositeExtract` | Composed into one per-field map at compile time: `v.w` and `v.xyz` are read in place (broadcast operands, write masks); anything else is a grouped masked `MOVE` or `ADD` from `VF00`, which also supplies 0 and 1 |
| ✓ | Matrices and matrix × vector | `OpMatrixTimesVector`, `OpTypeMatrix` | VU1 `MULA`/`MADDA`/`MADD` chain, one instruction per column, the vector's field broadcast; push constant and constant matrices, column-major with 16-byte columns (std430), so a mat4 is 4 push qwords and `m[c]` reads one |
| ✓ | Built-in functions | `OpExtInst` GLSL.std.450 (`FAbs`, `FMin`, `FMax`, `FClamp`, `FMix`, `Floor`, `Ceil`, `Trunc`, `Fract`, `Sqrt`, `InverseSqrt`, `Length`, `Normalize`, `Cross`, `Sin`, `Cos`, `Exp`, `Exp2`), `OpDot` | Per-field ones are one FMAC op (`ABS`, `MINI`, `MAX`, `MADD` for mix); floor/ceil add and subtract 1.5·2²³ (VU1 truncates, so |x| < 2²² only); dot folds a `MUL` with broadcast `ADD`s into x, length and normalize follow with `SQRT`/`RSQRT` in Q, cross is `OPMULA`/`OPMSUB`; sin/cos reduce the angle to ±π then `ESIN` per field (cos = sin(x + π/2)), exp/exp2 negate for `EEXP` (e⁻ˣ). *`pow`/`log` are rejected: no logarithm unit* |
| ✓ | Push constants | `OpVariable` `PushConstant`, `OpAccessChain` with constant indices, `OpLoad` | Each qword the shader reads is loaded (`LQ` from the push range) right before its first use, read in place and its register freed after its last use; float/vec2/vec3/vec4 members and arrays of them, 128 bytes max |
| ✓ | Uniform blocks | `OpVariable` `Uniform` with `Block`, `Binding` and `DescriptorSet` decorations, `OpAccessChain` with constant indices | The same members as push constant blocks, std140 (a vec3 and each float array element in a qword of their own), up to 8 blocks and 1 KB together; placed in VU1's uniform area in (set, binding) order and read like push constants: `LQ` before the first use, then in place. *Vertex shaders only: the GS can't read them* |
| | Uniform arrays with dynamic indexing | `OpAccessChain` with a variable index | VU1 integer registers as addresses |
| | Local variables and functions | `OpVariable` `Function`, `OpFunctionCall` | Inlined by `spirv-opt`, kept in registers |
| | `if`/`else` and loops | `OpBranch`, `OpBranchConditional`, `OpLoopMerge` | Unrolled, or VU1 `IBNE`/`IBEQ` branches |
| ✓ | Texture coordinate output | A `vec2` `Output` variable (a `vec4` output is the color) | Epilogue writes S/w, T/w, Q = 1/w in `VF19` and stores `ST`, `RGBAQ`, `XYZ2` (3 output qwords per vertex); `texcoordOutputLocation` in `PvkShaderCode`. *Fog output not yet* |
| ✓ | Texture sampling in fragment shaders | `OpImageSampleImplicitLod` of a `sampler2D` with the `vec2` input as is, optionally `OpFMul` by the color input | GS `TEX0.TFX` decal (`texture()` alone) or modulate (`texture() * vColor`); `textured`, `textureFunction`, `samplerBinding`, `texcoordInputLocation` in `PvkShaderCode`. *Highlight, `textureLod` and anything else per pixel are rejected.* The runtime rejects textured pipelines until texture upload and binding exist |
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
| ✓ | Triangle | Pipeline, vertex and index buffers, shaders, a mat4 push constant (the triangle circles the screen), NTSC display mode |
| ✓ | Textured cube | Depth buffer, back-face culling (clockwise front faces, since the projection flips y like Vulkan's), a 256-color PNG read with libpng (ps2sdk-ports) from `host:` and uploaded once as a T8 texture with its palette in a CT16 color table, decal sampling, a mat4 push constant, indexed drawing |
| ✓ | Lighting | A sphere lit by an orbiting directional light: camera and light in two uniform buffers of one set, the light's buffer rewritten every frame, the model rotation and material color as push constants. The vertex shader writes the normal·light dot product as a texture coordinate and the fragment shader modulates the material by a 256×8 ramp texture (ambient + clamped Lambert), so the GS clamps per pixel and the day/night line is a curve, not a per-triangle sawtooth |
| ✓ | Rain | A night rain scene, one topology per effect: sky and ground as a triangle strip, twinkling stars as points, the moon, its glow and its reflection as fans, 300 raindrops as a line list, splash ripples as line strips and splash droplets as sprites. The rain and droplets blend additively, the ripples with alpha; everything that moves is simulated on the EE and rewritten into one vertex buffer each frame |
| | Skinning | Bone matrices, animation |
| ✓ | Render to texture | Two passes in one command buffer: a cube with flat face colors spins fast inside a 256×256 CT32 color attachment image with its own Z16 depth buffer, then a second cube on screen is textured with that image, so every face is a window onto the first cube. One vertex shader (color and texture coordinate out) with two fragment shaders, two pipelines with the viewport and depth format of their target |
| | PBR materials | Material system, image-based lighting |
