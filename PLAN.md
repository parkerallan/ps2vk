# ps2vk feature plan

Research and findings on everything ps2vk could support, grouped by area. Each item lists:

- **Vulkan**: the Vulkan object, function or state it corresponds to ("none" when Vulkan leaves it to shader or application code).
- **ps2vk**: what to add to the ps2vk API (`Pvk*` types, `pvk*` functions, enum values). "Internal" means no API change.
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
| | Dynamic state: change viewport/scissor/fog/blend constant while recording | `VkPipelineDynamicStateCreateInfo`, `vkCmdSetViewport`, `vkCmdSetScissor`, `vkCmdSetBlendConstants` | `pvkCmdSetViewport`, `pvkCmdSetScissor`, `pvkCmdSetBlendConstants`, `pvkCmdSetFogColor` | Viewport: VU1 context write; scissor `SCISSOR`; blend `ALPHA.FIX`; fog `FOGCOL` |
| | Multiple frames in flight: CPU prepares the next frame while the GS draws | Per-frame command buffers + fences (pattern) | Allow several in-flight submissions | Needs interrupt-driven fences (next row) |
| | Interrupt-driven fences: hardware notifies the CPU instead of polling | `VkFence` | Internal change to `PvkFence` | GS `SIGNAL` with a label per fence, `INTC_GS` handler, EE semaphore (as ps2gl does) |
| | Semaphores: order work between queues without the CPU | `VkSemaphore` | `PvkSemaphore` | VIF `MSKPATH3` + `FLUSHA` between the graphics and transfer queues |
| | Pipeline barriers: later work waits for earlier work | `vkCmdPipelineBarrier`, `VkImageMemoryBarrier` | `pvkCmdPipelineBarrier` | VIF `FLUSH`/`FLUSHE`/`FLUSHA`, GS `TEXFLUSH` |
| | Transfer queue: upload textures while drawing continues | `VkQueue` with `VK_QUEUE_TRANSFER_BIT` | Second `PvkQueue` | DMA channel 2 straight to the GIF (PATH3) |
| | Queries: GPU timing for profiling | `VkQueryPool`, `vkCmdWriteTimestamp`, `vkGetQueryPoolResults` | `PvkQueryPool`, `pvkCmdWriteTimestamp` | EE cycle counter read when a GS `SIGNAL` interrupt fires. *No occlusion queries in hardware* |
| | Pipeline cache: skip re-uploading shaders already in VU1 | `VkPipelineCache` | `PvkPipelineCache` | Track which programs are resident in VU1's 16 KB program memory; skip `MPG` |
| | Validation: check every call for misuse | Validation layers, `VK_EXT_debug_utils` messenger | Debug build of the library + error callback | Checks in the library, compiled out of release builds |
| | Debug names and labels | `vkSetDebugUtilsObjectNameEXT`, `vkCmdBeginDebugUtilsLabelEXT` | `pvkSetObjectName`, `pvkCmdBeginLabel` | Names in error messages; VIF `MARK` codes visible in PCSX2 dumps |

## Drawing

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| ✓ | Triangle lists | `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` | `PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` | GS `PRIM` = triangle |
| | Triangle strips and fans: shared vertices, less data | `..._TRIANGLE_STRIP`, `..._TRIANGLE_FAN` | Same enum values | GS `PRIM` = strip/fan; VU1 batches overlap by 2 vertices |
| | Points, lines, line strips | `..._POINT_LIST`, `..._LINE_LIST`, `..._LINE_STRIP` | Same enum values | GS `PRIM` = point/line/line strip |
| | Sprites: native GS rectangles for 2D, UI, particles | None | `PVK_PRIMITIVE_TOPOLOGY_SPRITE_LIST` (extension) | GS `PRIM` = sprite (2 corners per rectangle) |
| | Indexed drawing: reuse vertices through an index list | `vkCmdBindIndexBuffer`, `vkCmdDrawIndexed`, `VK_INDEX_TYPE_UINT16` | `pvkCmdBindIndexBuffer`, `pvkCmdDrawIndexed` | Indices and vertices unpacked to VU1, which fetches by index (ps2gl's indexed renderer) |
| | Instanced drawing: same mesh many times in one call | `instanceCount`, `VK_VERTEX_INPUT_RATE_INSTANCE`, `gl_InstanceIndex` | `instanceCount` parameter, per-instance attributes | Per-instance data unpacked to VU1; VU1 loops the mesh per instance |
| | Indirect drawing: draw parameters read from a buffer | `vkCmdDrawIndirect`, `VkDrawIndirectCommand` | `pvkCmdDrawIndirect` | EE reads the parameters when the command buffer is built |
| | Multiple vertex buffers: attributes in separate buffers | `vkCmdBindVertexBuffers`, `VkVertexInputBindingDescription` | `pvkCmdBindVertexBuffers`, binding descriptions | One `UNPACK` per binding with VIF `STCYCL` to interleave in VU1 memory |
| | Compact vertex formats: 16/8-bit attributes save memory and DMA | `VK_FORMAT_R16G16B16_SNORM`, `R8G8B8A8_UNORM`, etc. | Matching `PvkFormat` values | VIF `UNPACK` `V3_16`, `V4_8` etc. expand to floats for free |
| | Clipping: triangles crossing the screen edge draw correctly | Built into the rasterizer (clip space) | Internal | Clipper in the VU1 shader epilogue (ps2gl `clip_cull.i`) |
| | Guard-band clipping: only clip triangles far off screen | Built in | Internal | GS 4096×4096 coordinate space + VU1 `CLIP` test |
| | Back-face culling: skip triangles facing away | `VkPipelineRasterizationStateCreateInfo` `cullMode`, `frontFace` | Same fields in pipeline create info | VU1 epilogue checks winding and sets the XYZ2 "don't draw" bit |
| | Double-buffered VU1 memory: DMA loads the next batch while VU1 works | None (driver internal) | Internal | VIF `BASE`/`OFFSET`, VU1 `XTOP`, `MSCNT` |
| | Large draws without stalls | None (driver internal) | Internal | Continuous batching using the double buffer above |

## Render targets and output

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Depth buffer: near objects hide far ones | `VkImage` with `D16`/`D24`/`D32` format, `pDepthAttachment` in `VkRenderingInfo` | Depth `PvkFormat`s, depth attachment in `PvkRenderingInfo` | GS `ZBUF` with `PSMZ16`/`PSMZ24`/`PSMZ32` |
| | Depth test and write control | `VkPipelineDepthStencilStateCreateInfo` | `PvkPipelineDepthStencilStateCreateInfo` | GS `TEST.ZTE`/`ZTST`, `ZBUF.ZMSK`; depth flipped because the GS only has ≥ and > tests |
| | Render to texture: draw into an image, then sample it | `VkImage` usage `COLOR_ATTACHMENT` + `SAMPLED`, `VkImageView` | `PvkImageCreateInfo` usage flags, `PvkImageView` | `FRAME` points at the texture's VRAM; `TEX0` later reads the same address |
| | Multiple render passes per frame | Several `vkCmdBeginRendering` blocks | Already supported by the API shape | New `FRAME`/`ZBUF` per pass |
| | Load/store ops | `VkAttachmentLoadOp`, `VkAttachmentStoreOp` | `PvkAttachmentLoadOp`, `PvkAttachmentStoreOp` | Clear = sprite fill; load/store/don't care = nothing (GS draws straight into VRAM) |
| | Color write mask | `VkPipelineColorBlendAttachmentState.colorWriteMask` | Same field | GS `FRAME.FBMSK` |
| | Scissor rectangles | `VkPipelineViewportStateCreateInfo`, `vkCmdSetScissor` | Pipeline field + `pvkCmdSetScissor` | GS `SCISSOR` |
| | Video modes: NTSC, PAL, 480p | `VkSurfaceKHR`, `VK_KHR_display` display modes | `PvkSwapchainCreateInfo` video mode field | `SetGsCrt` / `graph_set_mode`, GS `SMODE`/`DISPLAY` |
| | Field rendering: half-height buffers for interlaced TVs, saves VRAM | None | Swapchain flag | Half-height framebuffers, `XYOFFSET` shifted half a line on odd fields |
| | Triple buffering, immediate present | `VkPresentModeKHR` (`FIFO`, `MAILBOX`, `IMMEDIATE`), `minImageCount` | `PvkPresentMode`, `imageCount` up to 3 | 3 framebuffers; immediate = write `DISPFB` without waiting for vsync |
| | Custom resolutions and widescreen | `VkSwapchainCreateInfoKHR.imageExtent` | Swapchain width/height | Framebuffer size + GS `DISPLAY` magnification; widescreen via projection aspect |
| | 16-bit framebuffers: half the VRAM | `VK_FORMAT_A1R5G5B5_UNORM_PACK16` | `PVK_FORMAT_CT16` | GS `PSMCT16` |
| | Framebuffer readback: screenshots, debugging | `vkCmdCopyImageToBuffer` | `pvkCmdCopyImageToBuffer` | GS local-to-host transfer (`BITBLTBUF`, `TRXDIR` = 1) |

## Blending and fixed-function stages

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Alpha blending: transparency | `VkPipelineColorBlendAttachmentState` (`blendEnable`, factors, ops) | `PvkPipelineColorBlendState` | GS `ALPHA`: (A − B) × C + D, `PRIM.ABE`. Vulkan factor combinations the GS can't express are rejected at pipeline creation |
| | Additive, multiplicative, subtractive presets | Blend factor combinations | Preset values | Fixed `ALPHA` settings |
| | Alpha test: cut-out shapes (leaves, fences) | None (GLSL `discard`) | From the fragment shader via shaderc | GS `TEST.ATE`/`ATST`/`AFAIL` |
| | Destination alpha test: masking by a framebuffer bit | `VkStencilOpState` (stencil) | `PvkPipelineDepthStencilStateCreateInfo` stencil fields, limited | GS `TEST.DATE`/`DATM` + `FBA`. *1-bit stand-in for a stencil buffer* |
| | Fog | None (GLSL `mix`) | From the fragment shader via shaderc | `PRIM.FGE`, `FOGCOL`, fog value written by VU1 |
| | Dithering: less banding at 16-bit | None (driver) | Pipeline flag | GS `DTHE`, `DIMX` |
| | Color clamping | Implicit for UNORM formats | Internal | GS `COLCLAMP` |
| | Antialiased lines and edges | `VkPipelineRasterizationLineStateCreateInfoEXT` (smooth lines) | Pipeline flag | GS `PRIM.AA1` |

## Textures

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Texture upload | `vkCmdCopyBufferToImage`, `VkBufferImageCopy` | `pvkCmdCopyBufferToImage` | GIF `IMAGE` transfer with `BITBLTBUF`/`TRXPOS`/`TRXREG`/`TRXDIR` |
| | 32/24/16-bit color formats | `VK_FORMAT_R8G8B8A8_UNORM`, `R8G8B8_UNORM`, `A1R5G5B5_UNORM_PACK16` | `PVK_FORMAT_CT32/CT24/CT16` | GS `PSMCT32/24/16` |
| | Palettized 8-bit/4-bit textures with color tables | None in core Vulkan | `PVK_FORMAT_T8`/`T4` + a CLUT image in the descriptor | GS `PSMT8`/`PSMT4`, `TEX0.CBP`/`CPSM`/`CSM`/`CLD` |
| | Samplers: filtering and wrap modes | `VkSampler`, `VkSamplerCreateInfo` | `PvkSampler`, `PvkSamplerCreateInfo` | GS `TEX1.MMAG`/`MMIN`, `CLAMP.WMS`/`WMT` (repeat, clamp, region) |
| | Mipmaps and LOD control | `mipLevels`, `VkSamplerMipmapMode`, `minLod`/`maxLod`/`mipLodBias` | Same fields | GS `MIPTBP1`/`MIPTBP2`, `TEX1.MXL`/`L`/`K` |
| | Texture functions: how texture and vertex color combine | None (fragment shader math) | From the fragment shader via shaderc | GS `TEX0.TFX` (modulate, decal, highlight, highlight2), `TEX0.TCC` |
| | Perspective-correct texturing | Default behaviour | Internal | VU1 outputs `STQ` (s/w, t/w, 1/w), `PRIM.FST` = 0 |
| | VRAM allocator | `VkDeviceMemory` from a `DEVICE_LOCAL` heap | `PvkDeviceMemory` heap 1 | Page/block allocator over the 4 MB GS memory |
| | Texture streaming: keep textures in RAM, upload when used | None | Streamed-image extension (image + shared VRAM slot) | Upload into the slot before the draw + `TEXFLUSH` |
| | Texture atlases | None (application) | None (tools) | Packing done by the texture converter |
| | Automatic texture cache flushing | Implicit in barriers | Internal | GS `TEXFLUSH` emitted by barriers and uploads |
| | Render targets sampled as textures | `VkImageView` in a descriptor | `PvkImageView` | `TEX0` at the render target's VRAM address |
| | Environment / sphere mapping | Shader (`reflect` + cubemap) | Shader + texture | VU1 computes sphere-map UVs. *GS has no cubemaps* |
| | Projected textures | Shader | Shader + texture | VU1 computes `STQ` from the projector matrix |
| | Animated textures and palette animation | Descriptor updates / application | Descriptor update, CLUT upload | Re-upload the texture or just its CLUT |

## Shader compiler (GLSL)

Vulkan object for all of these: `VkShaderModule` (SPIR-V). The ps2vk side is `tools/shaderc` plus `PvkShaderCode`.

| Status | Feature | SPIR-V input | PS2 output |
| --- | --- | --- | --- |
| ✓ | Pass-through vertex shaders | `OpLoad`/`OpStore` of inputs and outputs | VU1 `MOVE` + generated prologue/epilogue |
| ✓ | `outColor = vColor` fragment shaders | `OpLoad`/`OpStore` | GS `PRIM.IIP` (Gouraud) |
| ✓ | Errors with line numbers and reasons | Any unsupported opcode | Build-time error |
| | Arithmetic | `OpFAdd`, `OpFSub`, `OpFMul`, `OpFDiv`, `OpFNegate`, `OpVectorTimesScalar` | VU1 `ADD`, `SUB`, `MUL`, `DIV` (Q register) |
| | Swizzles, constructors, component access | `OpVectorShuffle`, `OpCompositeConstruct`, `OpCompositeExtract` | VU1 field masks (`.xyz`), `MR32`, broadcast operands |
| | Matrices and matrix × vector | `OpMatrixTimesVector`, `OpTypeMatrix` | VU1 `MULA`/`MADDA`/`MADD` chain (4 instructions) |
| | Built-in functions | `OpExtInst` GLSL.std.450 (`Normalize`, `Pow`, `Sin`, …), `OpDot` | VU1 `DIV`/`SQRT`/`RSQRT`, EFU (`ESIN`, `EEXP`, `ERSQRT`, …) |
| | Uniform blocks and push constants | `OpVariable` `Uniform`/`PushConstant` | Loads from the VU1 context area defined by the pipeline layout |
| | Uniform arrays with dynamic indexing (bones) | `OpAccessChain` with a variable index | VU1 integer registers as addresses |
| | Local variables and functions | `OpVariable` `Function`, `OpFunctionCall` | Inlined by `spirv-opt`, kept in registers |
| | `if`/`else` and loops | `OpBranch`, `OpBranchConditional`, `OpLoopMerge` | Unrolled, or VU1 `IBNE`/`IBEQ` branches |
| | Texture coordinate and fog outputs | Extra `Output` variables | `ST`/`Q` and `FOG` in the GIF packet |
| | Fragment patterns: texture only, texture × color, highlight | `OpImageSampleImplicitLod` (+ `OpFMul`) | GS `TEX0.TFX` decal / modulate / highlight |
| | `discard` → alpha test | `OpKill` / `OpDemoteToHelperInvocation` | GS `TEST.ATE` |
| | `mix(fogColor, c, f)` → fog | `OpExtInst` `FMix` | GS `PRIM.FGE` + `FOGCOL` |
| | Register allocation across 32 VU1 registers | — | VF00–VF31, VI00–VI15 |
| | Instruction scheduling | — | Pair upper/lower instructions, hide latencies (openvcl or built in) |
| | Generated double-buffering and clipping | — | `XTOP`-relative addressing, clipper in the epilogue |
| | Runtime shader files | `vkCreateShaderModule` from file data | `.pvs` file loaded by `pvkCreateShaderModule` |
| | `spirv-opt` in the build | — | Inlining, loop unrolling, constant folding before shaderc |
| | Shader reflection | Like SPIRV-Reflect | Interface data in `PvkShaderCode` used for pipeline validation |
| | Compute shaders for VU0 | `ExecutionModel GLCompute` | VU0 microcode (see VU0) |

## Lighting

Vulkan has no lighting objects: lighting is shader code reading light data from uniform buffers. ps2vk follows the same model. *The GS can't run code per pixel, so lighting is computed per vertex on VU1.*

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Ambient light | Shader + uniform buffer | Shader + uniform buffer | VU1 vertex shader |
| | Directional lights (sun) | Shader + uniform buffer | Shader + uniform buffer | VU1 dot product per vertex |
| | Point lights with attenuation | Shader + uniform buffer | Shader + uniform buffer | VU1 per vertex |
| | Spot lights | Shader + uniform buffer | Shader + uniform buffer | VU1 per vertex |
| | Diffuse and specular | Shader | Shader | VU1 per vertex, Gouraud-interpolated by the GS |
| | Two-sided lighting | Shader (`gl_FrontFacing`) | Shader | VU1 lights both sides; winding picks the result |
| | Vertex colors and baked lighting | Vertex attribute | Vertex attribute | Color passed through; no runtime cost |
| | Lightmaps | Second texture sampled in the shader | Multipass helper | Second draw with the lightmap and multiplicative blending (one texture per draw on the GS) |
| | Spherical harmonics / hemisphere ambient | Shader + uniform buffer | Shader + uniform buffer | VU1 per vertex |
| | Cel / toon shading | Shader | Shader + lookup texture | Lighting value becomes a texture coordinate into a ramp texture |
| | Rim lighting | Shader | Shader | VU1 per vertex |
| | Specular via environment maps | Shader + cubemap | Shader + texture | Additive pass with sphere-mapped texture |
| | Blob shadows | Application | Application | Darkened textured quad under the object |
| | Projected shadow textures | Render to texture + shader | Render to texture + projected texture | Silhouette drawn to a texture, projected onto receivers |
| | Shadow volumes | Stencil (`VkStencilOpState`) | Stencil fields (limited) | *Destination alpha (`DATE`/`FBA`) in place of a stencil buffer* |
| | Shadow maps | Depth image + compare sampler (`compareEnable`) | Not directly | *Approximated with render-to-texture and palette tricks; no depth comparison on the GS* |

## Materials and PBR

Vulkan has no material objects: materials are pipelines, descriptor sets and uniform buffers chosen by the application. *The GS can't shade per pixel, so PBR is approximated per vertex and with extra texture passes.*

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Material parameters (base color, metallic, roughness, emissive) | Uniform buffer + textures | Uniform buffer + textures | VU1 context constants |
| | Per-vertex PBR lighting (GGX, Lambert) | Fragment shader | Vertex shader | VU1 per vertex |
| | Image-based lighting | Prefiltered cubemaps + BRDF lookup | Prefiltered sphere maps | One mip level per roughness, chosen per draw |
| | Fresnel | Shader | Shader | VU1 per vertex |
| | Emissive and glow passes | Shader / extra pass | Extra pass | Additive blending pass |
| | Normal-map approximation | Normal map sampled in the fragment shader | Not directly | *Baked per-vertex normals, or emboss multipass* |
| | Material system | None (application/engine) | Optional helper library | Picks pipeline, descriptor sets and passes per material |

## Animation

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Skeletal skinning on VU1 | Vertex shader + bone matrices in a uniform/storage buffer | Vertex shader + uniform buffer | Bone matrices in VU1 context memory; joint indices/weights as vertex attributes |
| | Skinning on VU0 | Compute shader (`vkCmdDispatch`) | Compute on VU0 | VU0 transforms vertices into a buffer VU1 then draws |
| | Morph targets / blend shapes | Extra vertex attributes or buffers | Multiple vertex bindings | VU1 blends attributes per vertex |
| | Keyframe interpolation | None (application) | Optional helper library | EE or VU0 math |
| | Animation blending and layering | None (application) | Optional helper library | EE or VU0 math |
| | Vertex animation (grass, flags) | Vertex shader + time push constant | Vertex shader + push constant | VU1 per vertex |
| | UV scrolling | Push constant offset | Push constant | VU1 adds an offset to `ST` |

## Effects

Vulkan has no effect objects; effects are built from the features above. The "Built from" column lists what each needs.

| Status | Feature | Built from | PS2 implementation |
| --- | --- | --- | --- |
| | Particles and billboards | Sprites, compute or vertex shader, blending | VU1 expands one point to a camera-facing sprite |
| | Trails and ribbons | Triangle strips, dynamic vertex buffers | Strips updated each frame from the EE |
| | Bloom | Render to texture, blending | Downsample with bilinear sprites, blur passes, additive blend back |
| | Blur and depth of field | Render to texture, blending | Blurred downsampled copies blended by depth |
| | Motion blur | Render to texture, blending | Previous frame blended into the current one |
| | Color grading | Render to texture, palettized textures | Framebuffer reinterpreted as 8-bit indices through a CLUT |
| | Fades and overlays | Sprites, blending | Full-screen sprite |
| | Planar reflections | Render to texture | Scene drawn mirrored into a texture, then mapped onto the surface |
| | Water and refraction | Render to texture, projected textures, UV animation | Distorted texture coordinates computed on VU1 |
| | Fog volumes | Blending, depth | Extra blended geometry |
| | Lens flares | Sprites, blending | Additive sprites along the sun-to-centre line |
| | Screen distortion / heat haze | Render to texture | Framebuffer resampled through a warped sprite grid |

## Geometry and scene

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Frustum culling | None (application or compute) | Optional helper / VU0 compute | Bounding-volume tests on EE or VU0 before drawing |
| | Level of detail | None (application) | Optional helper | Mesh chosen by distance |
| | Terrain | None (application) | Optional helper | Strips with distance-based detail |
| | Procedural geometry on VU1 | Mesh shaders (`VK_EXT_mesh_shader`) | Vertex shader that emits extra vertices | VU1 writes more output vertices than it reads |
| | Subdivision / tessellation | Tessellation control and evaluation shaders | VU1 shader extension | VU1 subdivides triangles before the epilogue |
| | Skybox / sky dome | None (application) | Sample | Drawn first with depth writes off |
| | Text and 2D/UI | None (application) | Optional helper | Textured sprites from a font texture |

## VU0

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Compute queue | `VkQueue` with `VK_QUEUE_COMPUTE_BIT`, `vkCreateComputePipelines`, `vkCmdDispatch` | Compute `PvkQueue`, `PvkComputePipeline`, `pvkCmdDispatch` | VU0 micro mode fed by DMA channel 0 (VIF0) |
| | Storage buffers for compute results | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | `PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | VU0 data memory copied back to EE RAM |
| | Math library (matrices, vectors, quaternions) | None (like GLM) | Optional helper library | VU0 macro-mode instructions from the EE |
| | Culling, skinning, particle simulation | Compute dispatches | `pvkCmdDispatch` with shaders | VU0 compute shaders |

## Performance

| Status | Feature | Vulkan | ps2vk | PS2 implementation |
| --- | --- | --- | --- | --- |
| | Reusing recorded command lists across frames | Reusable / secondary command buffers | Same | DMA chains resent unchanged |
| | Building command lists in fast memory | None (driver) | Internal | EE scratchpad RAM (16 KB) |
| | Batching and state sorting | None (application) | Optional helper | Sort draws by pipeline and texture |
| | Minimal GS state changes | None (driver) | Internal | Command buffer tracks GS register state, skips repeats |
| | Texture uploads overlapped with drawing | Transfer queue + semaphores | Transfer queue + semaphores | PATH3 uploads with `MSKPATH3` ordering |
| | Profiling | `VkQueryPool` timestamps, debug utils | Query pools, debug labels | EE cycle counter, PCSX2 profiler |
| | Benchmark samples | None | Samples | Fixed scenes timed per frame |

## Tools and assets

These run on the PC and produce data for ps2vk objects.

| Status | Tool | Produces data for |
| --- | --- | --- |
| | Texture converter: PNG → PS2 formats, palettes, mipmaps | `PvkImage` |
| | Mesh converter: glTF/OBJ → ps2vk vertex formats, strips | `PvkBuffer` (vertex and index data) |
| | Animation converter: glTF skins and clips | Bone matrices for uniform buffers |
| | Debug text overlay | Sprites + font texture |
| | GS dump support for PCSX2 debugging | — |
| | Automated tests in PCSX2 | — |
| | CI build | — |

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

## Platform

| Status | Item | Notes |
| --- | --- | --- |
| | Testing on real PS2 hardware | The emulator doesn't catch every hardware difference |
| | PAL and progressive output on hardware | Confirm the video modes on real TVs |
