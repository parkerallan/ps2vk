/**
 * @file ps2vk.h
 * @brief ps2vk - a Vulkan-like graphics API for the PlayStation 2.
 *
 *
 * Names follow Vulkan where the concept is the same; the "pvk" prefix is a
 * placeholder.
 */
#ifndef PS2VK_H
#define PS2VK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/**
 * @defgroup common Results and formats
 * @brief Result codes and data formats used throughout the API.
 * @{
 */

/** @brief Result codes. Values match Vulkan's where the meaning is the same. */
typedef enum PvkResult {
    PVK_SUCCESS                        = 0,           /**< The call succeeded. */
    PVK_ERROR_OUT_OF_HOST_MEMORY       = -1,          /**< EE main RAM allocation failed. */
    PVK_ERROR_OUT_OF_DEVICE_MEMORY     = -2,          /**< Not enough GS VRAM. */
    PVK_ERROR_INITIALIZATION_FAILED    = -3,          /**< Hardware setup failed. */
    PVK_ERROR_FEATURE_NOT_PRESENT      = -8,          /**< The PS2 or ps2vk doesn't support this yet. */
    PVK_ERROR_FORMAT_NOT_SUPPORTED     = -11,         /**< The format can't be used here. */
    PVK_ERROR_INVALID_SHADER           = -1000012000, /**< The shader code is missing or broken. */
    PVK_ERROR_VALIDATION_FAILED        = -1000011001  /**< Invalid use of the API; details are printed. */
} PvkResult;

/** @brief Data formats for vertex attributes and images. */
typedef enum PvkFormat {
    PVK_FORMAT_UNDEFINED = 0,       /**< No format. */
    PVK_FORMAT_R32G32B32A32_SFLOAT, /**< Vertex attribute: 4 floats. */
    PVK_FORMAT_CT32,                /**< GS PSMCT32 color image (RGBA, 8 bits each). */
    PVK_FORMAT_Z32,                 /**< GS PSMZ32 depth image (like VK_FORMAT_D32). */
    PVK_FORMAT_Z24,                 /**< GS PSMZ24 depth image (like VK_FORMAT_X8_D24). */
    PVK_FORMAT_Z16                  /**< GS PSMZ16 depth image (like VK_FORMAT_D16). */
} PvkFormat;

/** @} */

/* ======================================================================== */
/**
 * @defgroup device Instance and device
 * @brief Starting ps2vk and getting the graphics hardware ready.
 * @{
 */

/** @brief The connection to ps2vk. */
typedef struct PvkInstance_T      *PvkInstance;
/** @brief The PS2 graphics hardware (EE DMA, VU1, GS). There is only one. */
typedef struct PvkDevice_T        *PvkDevice;
/** @brief Where command buffers are submitted. The device has exactly one. */
typedef struct PvkQueue_T         *PvkQueue;

/** @brief Parameters for pvkCreateInstance(). */
typedef struct PvkInstanceCreateInfo {
    const char *applicationName; /**< Application name (informational). */
} PvkInstanceCreateInfo;

/** @brief Parameters for pvkCreateDevice(). May be NULL. */
typedef struct PvkDeviceCreateInfo {
    uint32_t reserved; /**< Unused; set to 0. */
} PvkDeviceCreateInfo;

/**
 * @brief Creates the ps2vk instance.
 * @param info Instance parameters.
 * @param[out] instance The new instance.
 * @return PVK_SUCCESS or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateInstance(const PvkInstanceCreateInfo *info, PvkInstance *instance);

/**
 * @brief Destroys the instance.
 * @param instance Instance to destroy.
 */
void      pvkDestroyInstance(PvkInstance instance);

/**
 * @brief Prepares the graphics hardware: resets the GIF and sets up the GIF and VIF1 DMA channels.
 * @param instance The instance.
 * @param info Device parameters, or NULL.
 * @param[out] device The new device.
 * @return PVK_SUCCESS or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateDevice(PvkInstance instance, const PvkDeviceCreateInfo *info, PvkDevice *device);

/**
 * @brief Waits for the device to go idle, then destroys it.
 * @param device Device to destroy.
 */
void      pvkDestroyDevice(PvkDevice device);

/**
 * @brief Gets the device's queue.
 * @param device The device.
 * @param[out] queue The queue.
 */
void      pvkGetDeviceQueue(PvkDevice device, PvkQueue *queue);

/**
 * @brief Waits until all submitted work has finished.
 * @param device The device.
 * @return PVK_SUCCESS.
 */
PvkResult pvkDeviceWaitIdle(PvkDevice device);

/** @} */

/* ======================================================================== */
/**
 * @defgroup images Images
 * @brief Pixel memory in GS VRAM: framebuffers, depth buffers and textures.
 * @{
 */

/** @brief An image in GS VRAM: a swapchain framebuffer, a depth buffer or a texture. */
typedef struct PvkImage_T         *PvkImage;

/** @brief Parameters for pvkCreateImage(). */
typedef struct PvkImageCreateInfo {
    uint32_t  width;  /**< Width in pixels; a depth buffer must be as wide as the framebuffer it is used with, a texture a power of two from 8 to 1024. */
    uint32_t  height; /**< Height in pixels; a texture a power of two from 8 to 1024. */
    PvkFormat format; /**< PVK_FORMAT_CT32 for a texture, or PVK_FORMAT_Z32, PVK_FORMAT_Z24 or PVK_FORMAT_Z16 for a depth buffer. */
} PvkImageCreateInfo;

/**
 * @brief Allocates an image in GS VRAM.
 *
 * A PVK_FORMAT_CT32 image is a texture: fill it with pvkCmdCopyBufferToImage()
 * and sample it through a descriptor set (see @ref descriptors). Depth formats make depth buffers.
 * Framebuffers come from the swapchain. GS VRAM is handed out in creation
 * order and never reclaimed, so create images once, after the swapchain.
 *
 * @param device The device.
 * @param info Image parameters.
 * @param[out] image The new image.
 * @return PVK_SUCCESS, PVK_ERROR_FORMAT_NOT_SUPPORTED, PVK_ERROR_OUT_OF_DEVICE_MEMORY
 *         or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateImage(PvkDevice device, const PvkImageCreateInfo *info, PvkImage *image);

/**
 * @brief Destroys an image created with pvkCreateImage(). Its VRAM stays allocated.
 * @param device The device.
 * @param image Image to destroy.
 */
void      pvkDestroyImage(PvkDevice device, PvkImage image);

/** @} */

/* ======================================================================== */
/**
 * @defgroup swapchain Swapchain
 * @brief The framebuffers shown on the TV, and presenting them.
 * @{
 */

/** @brief The framebuffers shown on the TV in turn. */
typedef struct PvkSwapchain_T     *PvkSwapchain;

/**
 * @brief The video mode the swapchain is shown in, like a VK_KHR_display mode.
 *
 * Interlaced modes show one framebuffer for both fields of a picture, so they
 * present at most 30 (NTSC) or 25 (PAL) frames a second. Progressive 480p
 * presents up to 60 and needs a display that accepts it, which PCSX2 and
 * component or HDMI outputs do.
 */
typedef enum PvkDisplayMode {
    PVK_DISPLAY_MODE_AUTO = 0, /**< The console's region: NTSC or PAL, interlaced. */
    PVK_DISPLAY_MODE_NTSC,     /**< 480i: 640x448, 60 fields a second. */
    PVK_DISPLAY_MODE_PAL,      /**< 576i: 640x512, 50 fields a second. */
    PVK_DISPLAY_MODE_480P      /**< 480p: 640x448 progressive, 60 frames a second. */
} PvkDisplayMode;

/** @brief Parameters for pvkCreateSwapchain(). */
typedef struct PvkSwapchainCreateInfo {
    uint32_t       width;       /**< Width in pixels, e.g. 640. */
    uint32_t       height;      /**< Height in pixels, e.g. 448. */
    PvkFormat      format;      /**< Must be PVK_FORMAT_CT32. */
    uint32_t       imageCount;  /**< Must be 2. */
    PvkDisplayMode displayMode; /**< Video mode; 0 is PVK_DISPLAY_MODE_AUTO. */
} PvkSwapchainCreateInfo;

/**
 * @brief Allocates the swapchain framebuffers in GS VRAM and turns on video output.
 *
 * Image 0 is shown first, so the first image to render into is image 1. The
 * video mode is set from PvkSwapchainCreateInfo::displayMode.
 *
 * @param device The device.
 * @param info Swapchain parameters.
 * @param[out] swapchain The new swapchain.
 * @return PVK_SUCCESS, PVK_ERROR_FORMAT_NOT_SUPPORTED, PVK_ERROR_FEATURE_NOT_PRESENT,
 *         PVK_ERROR_OUT_OF_DEVICE_MEMORY or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateSwapchain(PvkDevice device, const PvkSwapchainCreateInfo *info, PvkSwapchain *swapchain);

/**
 * @brief Destroys the swapchain.
 * @param device The device.
 * @param swapchain Swapchain to destroy.
 */
void      pvkDestroySwapchain(PvkDevice device, PvkSwapchain swapchain);

/**
 * @brief Gets the swapchain images.
 *
 * Call with images = NULL to get the count, then again with an array of that size.
 *
 * @param device The device.
 * @param swapchain The swapchain.
 * @param[in,out] count Size of images on input; number written on output.
 * @param[out] images Image handles, or NULL.
 * @return PVK_SUCCESS.
 */
PvkResult pvkGetSwapchainImages(PvkDevice device, PvkSwapchain swapchain, uint32_t *count, PvkImage *images);

/**
 * @brief Returns the index of the image to render into next (the one not on screen).
 * @param device The device.
 * @param swapchain The swapchain.
 * @param[out] imageIndex Index into the images from pvkGetSwapchainImages().
 * @return PVK_SUCCESS.
 */
PvkResult pvkAcquireNextImage(PvkDevice device, PvkSwapchain swapchain, uint32_t *imageIndex);

/**
 * @brief Shows an image on the TV.
 *
 * Waits for rendering to finish, waits for vsync, then points the display at the image.
 *
 * @param queue The queue.
 * @param swapchain The swapchain.
 * @param imageIndex Image to show.
 * @return PVK_SUCCESS.
 */
PvkResult pvkQueuePresent(PvkQueue queue, PvkSwapchain swapchain, uint32_t imageIndex);

/** @} */

/* ======================================================================== */
/**
 * @defgroup buffers Buffers
 * @brief Memory in EE main RAM that the graphics hardware reads, such as vertex data.
 * @{
 */

/** @brief A block of EE main RAM that DMA reads directly, e.g. vertex data. */
typedef struct PvkBuffer_T        *PvkBuffer;

/** @brief Parameters for pvkCreateBuffer(). */
typedef struct PvkBufferCreateInfo {
    size_t size; /**< Size in bytes; rounded up to a multiple of 16. */
} PvkBufferCreateInfo;

/**
 * @brief Allocates a buffer in EE main RAM, 64-byte aligned.
 * @param device The device.
 * @param info Buffer parameters.
 * @param[out] buffer The new buffer.
 * @return PVK_SUCCESS or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateBuffer(PvkDevice device, const PvkBufferCreateInfo *info, PvkBuffer *buffer);

/**
 * @brief Frees a buffer. It must not be in use by a submission.
 * @param device The device.
 * @param buffer Buffer to free.
 */
void      pvkDestroyBuffer(PvkDevice device, PvkBuffer buffer);

/**
 * @brief Gets a CPU pointer to the buffer's memory.
 *
 * Writes don't need flushing: the CPU cache is written back at pvkQueueSubmit().
 *
 * @param device The device.
 * @param buffer The buffer.
 * @param[out] data Pointer to the buffer's memory.
 * @return PVK_SUCCESS.
 */
PvkResult pvkMapMemory(PvkDevice device, PvkBuffer buffer, void **data);

/**
 * @brief Ends CPU access to the buffer. Currently does nothing.
 * @param device The device.
 * @param buffer The buffer.
 */
void      pvkUnmapMemory(PvkDevice device, PvkBuffer buffer);

/** @} */

/* ======================================================================== */
/**
 * @defgroup pipelines Shaders and pipelines
 * @brief Compiled GLSL shaders and the pipelines built from them.
 * @{
 */

/** @brief A compiled shader (see PvkShaderCode). */
typedef struct PvkShaderModule_T  *PvkShaderModule;
/** @brief Declares what a pipeline's shaders read from outside the vertex buffer (see PvkPipelineLayoutCreateInfo). */
typedef struct PvkPipelineLayout_T *PvkPipelineLayout;
/** @brief The shape of a descriptor set: which bindings it has and what each holds (see @ref descriptors). */
typedef struct PvkDescriptorSetLayout_T *PvkDescriptorSetLayout;
/** @brief A vertex shader (VU1 code) plus GS state, created ahead of time. */
typedef struct PvkPipeline_T      *PvkPipeline;

/** @brief Shader stages. The values are bits, so they can be ORed into a stage mask. */
typedef enum PvkShaderStage {
    PVK_SHADER_STAGE_VERTEX   = 1, /**< Runs on VU1. */
    PVK_SHADER_STAGE_FRAGMENT = 2  /**< Becomes GS settings. */
} PvkShaderStage;

/** @brief How vertices are grouped into primitives. */
typedef enum PvkPrimitiveTopology {
    PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 0 /**< Every 3 vertices form a triangle. */
} PvkPrimitiveTopology;

/** @brief Most vertex inputs a vertex shader can have. */
#define PVK_MAX_VERTEX_INPUTS 8

/** @brief Size of the push constant range, in bytes (Vulkan's minimum). */
#define PVK_MAX_PUSH_CONSTANT_BYTES 128

/** @brief Most descriptor sets a pipeline layout can have and a command buffer can bind (Vulkan's minimum). */
#define PVK_MAX_DESCRIPTOR_SETS 4

/** @brief Most uniform blocks one vertex shader can declare. */
#define PVK_MAX_UNIFORM_BLOCKS 8

/** @brief VU1 memory set aside for a vertex shader's uniform blocks together, in bytes. */
#define PVK_MAX_UNIFORM_BYTES 1024

/**
 * @brief A uniform block of a vertex shader and where VU1 keeps it.
 *
 * shaderc places a shader's blocks one after another, in (set, binding)
 * order, in the PVK_MAX_UNIFORM_BYTES of VU1 memory reserved for them; the
 * draw fills each from the buffer in the bound descriptor set.
 */
typedef struct PvkShaderUniformBlock {
    uint32_t set;     /**< Descriptor set index of the block (0 when the GLSL names none). */
    uint32_t binding; /**< Its binding number. */
    uint32_t qword;   /**< First qword of the block in the uniform area. */
    uint32_t bytes;   /**< Size of the block, std140. */
} PvkShaderUniformBlock;

/**
 * @brief A compiled shader, as generated by tools/shaderc.
 *
 * Applications never fill this in by hand: shaderc emits a .c file defining
 * one of these, and the application passes its address to
 * pvkCreateShaderModule().
 */
typedef struct PvkShaderCode {
    PvkShaderStage stage;                  /**< Vertex or fragment. */
    const char    *name;                   /**< Source file name, for error messages. */

    const uint32_t *vu1CodeStart;          /**< Vertex: start of the VU1 microcode, linked into the ELF. */
    const uint32_t *vu1CodeEnd;            /**< Vertex: end of the VU1 microcode. */
    uint32_t        inputCount;            /**< Vertex: number of inputs, each one vec4. */
    uint32_t        inputLocations[PVK_MAX_VERTEX_INPUTS]; /**< Vertex: input locations, in the order VU1 reads them. */
    int32_t         colorOutputLocation;   /**< Vertex: location of the color output, or -1 if none. */
    int32_t         texcoordOutputLocation;/**< Vertex: location of the vec2 texture coordinate output, or -1 if none. When set, VU1 emits ST, RGBAQ, XYZ2 per vertex instead of RGBAQ, XYZ2. */
    uint32_t        pushConstantBytes;     /**< Vertex: size of its push constant block, 0 if it has none. */
    uint32_t        uniformBlockCount;     /**< Vertex: number of entries in uniformBlocks. */
    PvkShaderUniformBlock uniformBlocks[PVK_MAX_UNIFORM_BLOCKS]; /**< Vertex: its uniform blocks, in (set, binding) order. */

    int32_t         colorInputLocation;   /**< Fragment: location of the color input it uses, or -1 if none. */
    int32_t         texcoordInputLocation; /**< Fragment: location of the vec2 input it samples with, or -1 if it does not sample. */
    uint32_t        gouraud;               /**< Fragment: GS PRIM.IIP (blend vertex colors). */
    uint32_t        textured;              /**< Fragment: GS PRIM.TME (reads a texture). */
    uint32_t        textureFunction;       /**< Fragment: GS TEX0.TFX, 0 = modulate (texture * color), 1 = decal (texture only). */
    int32_t         samplerBinding;        /**< Fragment: the sampler2D's binding, or -1 if it has none. */
    int32_t         samplerSet;            /**< Fragment: the sampler2D's descriptor set (0 when the GLSL names none), or -1 if it has no sampler. */
} PvkShaderCode;

/** @brief Parameters for pvkCreateShaderModule(). */
typedef struct PvkShaderModuleCreateInfo {
    const PvkShaderCode *code; /**< Shader generated by tools/shaderc. */
} PvkShaderModuleCreateInfo;

/** @brief Part of the push constant range that one or more shader stages read, like VkPushConstantRange. */
typedef struct PvkPushConstantRange {
    uint32_t stageFlags; /**< PvkShaderStage values ORed together. Only the vertex stage can read push constants on the PS2. */
    uint32_t offset;     /**< First byte of the range; a multiple of 4. */
    uint32_t size;       /**< Bytes in the range; a multiple of 4, offset + size at most PVK_MAX_PUSH_CONSTANT_BYTES. */
} PvkPushConstantRange;

/**
 * @brief Parameters for pvkCreatePipelineLayout().
 *
 * The push constant ranges may not overlap. Set layout i describes the
 * descriptor set bound at index i with pvkCmdBindDescriptorSets().
 */
typedef struct PvkPipelineLayoutCreateInfo {
    uint32_t                      setLayoutCount;         /**< Number of entries in setLayouts, at most PVK_MAX_DESCRIPTOR_SETS. */
    const PvkDescriptorSetLayout *setLayouts;             /**< The descriptor set layouts, in set index order, or NULL for none. */
    uint32_t                      pushConstantRangeCount; /**< Number of entries in pushConstantRanges. */
    const PvkPushConstantRange   *pushConstantRanges;     /**< The push constant ranges, or NULL for none. */
} PvkPipelineLayoutCreateInfo;

/**
 * @brief Creates a pipeline layout.
 *
 * The layout says which bytes of the 128-byte push constant range each shader
 * stage may read, which on the PS2 is the VU1 memory pvkCmdPushConstants()
 * writes, and which descriptor sets the shaders expect. A pipeline's vertex
 * shader must have its push constant block inside the layout's vertex ranges,
 * pvkCmdPushConstants() may only write bytes the layout gives the stages it
 * names, a fragment shader's sampler2D must be a combined image sampler
 * binding in the set layout at its set index, and each of the vertex shader's
 * uniform blocks must be a uniform buffer binding, for the vertex stage, in
 * the set layout at its set index. The set layouts are copied, so they may be
 * destroyed afterwards.
 *
 * @param device The device.
 * @param info Layout parameters.
 * @param[out] layout The new layout.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreatePipelineLayout(PvkDevice device, const PvkPipelineLayoutCreateInfo *info, PvkPipelineLayout *layout);

/**
 * @brief Destroys a pipeline layout. Pipelines created with it stay valid.
 * @param device The device.
 * @param layout Layout to destroy.
 */
void      pvkDestroyPipelineLayout(PvkDevice device, PvkPipelineLayout layout);

/** @brief One vertex attribute in the vertex buffer. */
typedef struct PvkVertexInputAttribute {
    uint32_t  location; /**< Shader input location it feeds. */
    PvkFormat format;   /**< Must be PVK_FORMAT_R32G32B32A32_SFLOAT. */
    uint32_t  offset;   /**< Bytes from the start of the vertex; input i must be at i * 16. */
} PvkVertexInputAttribute;

/** @brief Maps clip space to framebuffer pixels, as in Vulkan (y points down). */
typedef struct PvkViewport {
    float x;        /**< Left edge in pixels. */
    float y;        /**< Top edge in pixels. */
    float width;    /**< Width in pixels. */
    float height;   /**< Height in pixels. */
    float minDepth; /**< Depth for clip-space z = 0, usually 0. */
    float maxDepth; /**< Depth for clip-space z = 1, usually 1. */
} PvkViewport;

/**
 * @brief Depth comparisons. Values match Vulkan's VkCompareOp.
 *
 * The GS only compares with >= and >, so ps2vk stores depth flipped (near is
 * large) and supports the comparisons that map onto that: LESS,
 * LESS_OR_EQUAL, ALWAYS and NEVER. The others are rejected at pipeline creation.
 */
typedef enum PvkCompareOp {
    PVK_COMPARE_OP_NEVER            = 0, /**< Nothing passes. GS ZTST = 0. */
    PVK_COMPARE_OP_LESS             = 1, /**< Nearer wins. GS ZTST = GREATER on flipped depth. */
    PVK_COMPARE_OP_EQUAL            = 2, /**< Not supported. */
    PVK_COMPARE_OP_LESS_OR_EQUAL    = 3, /**< Nearer or equal wins. GS ZTST = GEQUAL on flipped depth. */
    PVK_COMPARE_OP_GREATER          = 4, /**< Not supported. */
    PVK_COMPARE_OP_NOT_EQUAL        = 5, /**< Not supported. */
    PVK_COMPARE_OP_GREATER_OR_EQUAL = 6, /**< Not supported. */
    PVK_COMPARE_OP_ALWAYS           = 7  /**< Everything passes. GS ZTST = 1. */
} PvkCompareOp;

/**
 * @brief Depth test and write control, like VkPipelineDepthStencilStateCreateInfo.
 *
 * All zeros means no depth test and no depth writes, which is what a pipeline
 * without a depth attachment needs. There is no stencil buffer on the GS.
 */
typedef struct PvkPipelineDepthStencilStateCreateInfo {
    uint32_t     depthTestEnable;  /**< Nonzero to compare against the depth attachment. GS TEST.ZTE/ZTST. */
    uint32_t     depthWriteEnable; /**< Nonzero to write passing depths; ignored when the test is off. GS ZBUF.ZMSK. */
    PvkCompareOp depthCompareOp;   /**< Comparison when the test is on. */
} PvkPipelineDepthStencilStateCreateInfo;

/**
 * @brief Blend factors. Values match Vulkan's VkBlendFactor.
 *
 * The GS blends as (A - B) * C + D, where A, B and D are the source color,
 * the destination color or zero and C is the source alpha, the destination
 * alpha or a fixed value. So a factor must be an alpha, one minus an alpha,
 * zero or one, both factors must use the same alpha, and color factors
 * (SRC_COLOR, DST_COLOR, ...) are rejected at pipeline creation.
 */
typedef enum PvkBlendFactor {
    PVK_BLEND_FACTOR_ZERO                     = 0,  /**< 0. */
    PVK_BLEND_FACTOR_ONE                      = 1,  /**< 1. */
    PVK_BLEND_FACTOR_SRC_COLOR                = 2,  /**< Not supported. */
    PVK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR      = 3,  /**< Not supported. */
    PVK_BLEND_FACTOR_DST_COLOR                = 4,  /**< Not supported. */
    PVK_BLEND_FACTOR_ONE_MINUS_DST_COLOR      = 5,  /**< Not supported. */
    PVK_BLEND_FACTOR_SRC_ALPHA                = 6,  /**< The fragment's alpha. GS ALPHA.C = As. */
    PVK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA      = 7,  /**< 1 - the fragment's alpha. */
    PVK_BLEND_FACTOR_DST_ALPHA                = 8,  /**< The framebuffer's alpha. GS ALPHA.C = Ad. */
    PVK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA      = 9,  /**< 1 - the framebuffer's alpha. */
    PVK_BLEND_FACTOR_CONSTANT_COLOR           = 10, /**< Not supported. */
    PVK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR = 11, /**< Not supported. */
    PVK_BLEND_FACTOR_CONSTANT_ALPHA           = 12, /**< blendConstants[3]. GS ALPHA.C = FIX. */
    PVK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA = 13  /**< 1 - blendConstants[3]. */
} PvkBlendFactor;

/** @brief Blend operations. Values match Vulkan's VkBlendOp. */
typedef enum PvkBlendOp {
    PVK_BLEND_OP_ADD              = 0, /**< src * srcFactor + dst * dstFactor. */
    PVK_BLEND_OP_SUBTRACT         = 1, /**< src * srcFactor - dst * dstFactor. */
    PVK_BLEND_OP_REVERSE_SUBTRACT = 2, /**< dst * dstFactor - src * srcFactor. */
    PVK_BLEND_OP_MIN              = 3, /**< Not supported. */
    PVK_BLEND_OP_MAX              = 4  /**< Not supported. */
} PvkBlendOp;

/**
 * @brief Blending of the color attachment, like VkPipelineColorBlendAttachmentState.
 *
 * The equation src * srcColorBlendFactor (op) dst * dstColorBlendFactor is
 * rewritten into the GS's (A - B) * C + D at pipeline creation; combinations
 * that do not fit are rejected with PVK_ERROR_FEATURE_NOT_PRESENT. The usual
 * ones do: alpha blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD), additive
 * (SRC_ALPHA or ONE, ONE, ADD) and subtractive (SRC_ALPHA, ONE,
 * REVERSE_SUBTRACT). The GS has no alpha blending: the attachment's alpha is
 * always the fragment's alpha. All zeros means no blending.
 */
typedef struct PvkPipelineColorBlendAttachmentState {
    uint32_t       blendEnable;         /**< Nonzero to blend. GS PRIM.ABE. */
    PvkBlendFactor srcColorBlendFactor; /**< Factor on the fragment's color. */
    PvkBlendFactor dstColorBlendFactor; /**< Factor on the framebuffer's color. */
    PvkBlendOp     colorBlendOp;        /**< How the two products combine. */
} PvkPipelineColorBlendAttachmentState;

/** @brief Blend state of a pipeline, like VkPipelineColorBlendStateCreateInfo. */
typedef struct PvkPipelineColorBlendStateCreateInfo {
    PvkPipelineColorBlendAttachmentState attachment;        /**< Blending of the single color attachment. */
    float                                blendConstants[4]; /**< Only [3] is used, by the CONSTANT_ALPHA factors, as GS ALPHA.FIX (0..1, 1 = 128). */
} PvkPipelineColorBlendStateCreateInfo;

/** @brief Parameters for pvkCreateGraphicsPipeline(). */
typedef struct PvkGraphicsPipelineCreateInfo {
    PvkShaderModule                vertexShader;   /**< Vertex shader module. */
    PvkShaderModule                fragmentShader; /**< Fragment shader module. */
    PvkPipelineLayout              layout;         /**< Layout whose vertex push constant ranges cover the vertex shader's block. */
    uint32_t                       vertexStride;   /**< Bytes per vertex; must be 16 per shader input. */
    uint32_t                       attributeCount; /**< Number of entries in attributes. */
    const PvkVertexInputAttribute *attributes;     /**< One per vertex shader input. */
    PvkPrimitiveTopology           topology;       /**< Must be PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST. */
    PvkViewport                    viewport;       /**< Viewport, fixed for this pipeline. */
    PvkFormat                      depthAttachmentFormat; /**< Format of the depth attachment the pipeline draws with, or PVK_FORMAT_UNDEFINED for none (like VkPipelineRenderingCreateInfo). Sets the depth range the viewport maps to. */
    PvkPipelineDepthStencilStateCreateInfo depthStencil; /**< Depth test and write control. */
    PvkPipelineColorBlendStateCreateInfo   colorBlend;   /**< Blending with the color attachment. */
} PvkGraphicsPipelineCreateInfo;

/**
 * @brief Wraps a shader compiled by tools/shaderc.
 * @param device The device.
 * @param info Shader module parameters.
 * @param[out] module The new shader module.
 * @return PVK_SUCCESS, PVK_ERROR_INVALID_SHADER or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateShaderModule(PvkDevice device, const PvkShaderModuleCreateInfo *info, PvkShaderModule *module);

/**
 * @brief Destroys a shader module.
 * @param device The device.
 * @param module Shader module to destroy.
 */
void      pvkDestroyShaderModule(PvkDevice device, PvkShaderModule module);

/**
 * @brief Creates a graphics pipeline.
 *
 * Checks that the shaders match each other, the vertex layout and the
 * pipeline layout, and precomputes the viewport transform, GS primitive
 * settings, depth test and blend equation. A depth test needs a
 * depthAttachmentFormat, the comparison must be one the GS can do (see
 * PvkCompareOp) and the blend factors must fit the GS equation (see
 * PvkPipelineColorBlendAttachmentState).
 *
 * @param device The device.
 * @param info Pipeline parameters.
 * @param[out] pipeline The new pipeline.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED, PVK_ERROR_FORMAT_NOT_SUPPORTED,
 *         PVK_ERROR_FEATURE_NOT_PRESENT or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateGraphicsPipeline(PvkDevice device, const PvkGraphicsPipelineCreateInfo *info, PvkPipeline *pipeline);

/**
 * @brief Destroys a pipeline.
 * @param device The device.
 * @param pipeline Pipeline to destroy.
 */
void      pvkDestroyPipeline(PvkDevice device, PvkPipeline pipeline);

/** @} */

/* ======================================================================== */
/**
 * @defgroup descriptors Descriptor sets
 * @brief Bundles of resources a pipeline's shaders read, bound in one call.
 *
 * A descriptor set layout names the bindings a set has; a pipeline layout lists
 * the set layouts a pipeline expects; sets are allocated from a pool, filled
 * with pvkUpdateDescriptorSets() and bound with pvkCmdBindDescriptorSets().
 * A binding holds a combined image sampler, the texture a fragment shader's
 * sampler2D reads, or a uniform buffer, the memory behind a vertex shader's
 * uniform block. On the PS2 the bound set's image is written into GS TEX0
 * with the draw state, when the command is recorded; its buffers are sent to
 * VU1 memory ahead of the first draw after a bind, by DMA straight from the
 * buffer when the command buffer is submitted.
 * @{
 */

/** @brief A pool that descriptor sets are allocated from. */
typedef struct PvkDescriptorPool_T      *PvkDescriptorPool;
/** @brief A set of resources, filled in with pvkUpdateDescriptorSets(). */
typedef struct PvkDescriptorSet_T       *PvkDescriptorSet;

/** @brief What a descriptor binding holds. Values match Vulkan's VkDescriptorType. */
typedef enum PvkDescriptorType {
    PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1, /**< A PVK_FORMAT_CT32 image a sampler2D reads: GS TEX0. */
    PVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER         = 6  /**< Part of a PvkBuffer a vertex shader's uniform block reads: VIF UNPACK into VU1 memory. */
} PvkDescriptorType;

/** @brief Most bindings in one descriptor set layout. */
#define PVK_MAX_DESCRIPTOR_BINDINGS 8

/** @brief One binding of a descriptor set layout, like VkDescriptorSetLayoutBinding. */
typedef struct PvkDescriptorSetLayoutBinding {
    uint32_t          binding;         /**< The binding number the shader's layout(binding = n) names. */
    PvkDescriptorType descriptorType;  /**< What it holds. */
    uint32_t          descriptorCount; /**< Must be 1: binding arrays are not supported. */
    uint32_t          stageFlags;      /**< PvkShaderStage values ORed together; a combined image sampler needs PVK_SHADER_STAGE_FRAGMENT, a uniform buffer PVK_SHADER_STAGE_VERTEX. */
} PvkDescriptorSetLayoutBinding;

/** @brief Parameters for pvkCreateDescriptorSetLayout(). */
typedef struct PvkDescriptorSetLayoutCreateInfo {
    uint32_t                             bindingCount; /**< Number of entries in bindings, at most PVK_MAX_DESCRIPTOR_BINDINGS. */
    const PvkDescriptorSetLayoutBinding *bindings;     /**< The bindings, each with a distinct binding number; any order. */
} PvkDescriptorSetLayoutCreateInfo;

/**
 * @brief Creates a descriptor set layout.
 *
 * Two layouts with the same bindings are interchangeable: a set allocated
 * with one may be used with a pipeline layout created from the other.
 *
 * @param device The device.
 * @param info Layout parameters.
 * @param[out] layout The new layout.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED, PVK_ERROR_FEATURE_NOT_PRESENT
 *         or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateDescriptorSetLayout(PvkDevice device, const PvkDescriptorSetLayoutCreateInfo *info,
                                       PvkDescriptorSetLayout *layout);

/**
 * @brief Destroys a descriptor set layout. Pipeline layouts and sets made with it stay valid.
 * @param device The device.
 * @param layout Layout to destroy.
 */
void      pvkDestroyDescriptorSetLayout(PvkDevice device, PvkDescriptorSetLayout layout);

/** @brief How many descriptors of one type a pool holds, like VkDescriptorPoolSize. */
typedef struct PvkDescriptorPoolSize {
    PvkDescriptorType type;            /**< The descriptor type. */
    uint32_t          descriptorCount; /**< Descriptors of that type the pool can hand out in total. */
} PvkDescriptorPoolSize;

/** @brief Parameters for pvkCreateDescriptorPool(). */
typedef struct PvkDescriptorPoolCreateInfo {
    uint32_t                     maxSets;       /**< Most sets allocated from the pool at once. */
    uint32_t                     poolSizeCount; /**< Number of entries in poolSizes. */
    const PvkDescriptorPoolSize *poolSizes;     /**< Descriptors available per type. */
} PvkDescriptorPoolCreateInfo;

/**
 * @brief Creates a descriptor pool. Its sets' memory is allocated here, once.
 * @param device The device.
 * @param info Pool parameters.
 * @param[out] pool The new pool.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateDescriptorPool(PvkDevice device, const PvkDescriptorPoolCreateInfo *info, PvkDescriptorPool *pool);

/**
 * @brief Destroys a pool and every set allocated from it.
 * @param device The device.
 * @param pool Pool to destroy.
 */
void      pvkDestroyDescriptorPool(PvkDevice device, PvkDescriptorPool pool);

/**
 * @brief Returns all of a pool's sets to it. The set handles become invalid.
 * @param device The device.
 * @param pool The pool.
 * @return PVK_SUCCESS.
 */
PvkResult pvkResetDescriptorPool(PvkDevice device, PvkDescriptorPool pool);

/** @brief Parameters for pvkAllocateDescriptorSets(). */
typedef struct PvkDescriptorSetAllocateInfo {
    PvkDescriptorPool             descriptorPool;     /**< Pool to allocate from. */
    uint32_t                      descriptorSetCount; /**< Number of sets, one per entry in setLayouts. */
    const PvkDescriptorSetLayout *setLayouts;         /**< The layout of each set. */
} PvkDescriptorSetAllocateInfo;

/**
 * @brief Allocates descriptor sets from a pool.
 *
 * Fails, allocating none, when the pool has too few sets or descriptors left.
 * Sets are freed only by resetting or destroying the pool. A new set's
 * bindings are empty until pvkUpdateDescriptorSets() fills them.
 *
 * @param device The device.
 * @param info The pool and the layouts.
 * @param[out] sets One handle per layout.
 * @return PVK_SUCCESS or PVK_ERROR_OUT_OF_HOST_MEMORY (the pool is exhausted).
 */
PvkResult pvkAllocateDescriptorSets(PvkDevice device, const PvkDescriptorSetAllocateInfo *info, PvkDescriptorSet *sets);

/** @brief The image a combined image sampler binding reads, like VkDescriptorImageInfo. */
typedef struct PvkDescriptorImageInfo {
    PvkImage image; /**< A PVK_FORMAT_CT32 image: a texture or a swapchain image. Sampled bilinear, no mipmaps, repeating, until samplers exist. */
} PvkDescriptorImageInfo;

/** @brief The rest of a buffer, as the range of a PvkDescriptorBufferInfo. */
#define PVK_WHOLE_SIZE (~(size_t)0)

/** @brief The part of a buffer a uniform buffer binding reads, like VkDescriptorBufferInfo. */
typedef struct PvkDescriptorBufferInfo {
    PvkBuffer buffer; /**< The buffer. */
    size_t    offset; /**< Byte offset of the block's first byte; a multiple of 16. */
    size_t    range;  /**< Bytes from offset the block may read, or PVK_WHOLE_SIZE; at least the shader's block size. */
} PvkDescriptorBufferInfo;

/** @brief One binding to fill in with pvkUpdateDescriptorSets(), like VkWriteDescriptorSet. */
typedef struct PvkWriteDescriptorSet {
    PvkDescriptorSet               dstSet;         /**< The set to write. */
    uint32_t                       dstBinding;     /**< The binding number within it. */
    PvkDescriptorType              descriptorType; /**< Must match the binding's type in the set's layout. */
    const PvkDescriptorImageInfo  *imageInfo;      /**< The image, for a combined image sampler. */
    const PvkDescriptorBufferInfo *bufferInfo;     /**< The buffer range, for a uniform buffer. */
} PvkWriteDescriptorSet;

/**
 * @brief Writes resources into descriptor set bindings.
 *
 * A set may be rewritten at any time; draws recorded before the write keep
 * what was bound when they were recorded, since the command buffer reads the
 * set at pvkCmdDraw() time. A uniform buffer's contents, though, are read by
 * DMA when the command buffer is submitted, so the values in the buffer at
 * submit time are what the draws see, as in Vulkan. The data is laid out as
 * in the shader's block (std140): a mat4 is four 16-byte columns, a vec3 and
 * a float array element each take a 16-byte slot.
 *
 * @param device The device.
 * @param writeCount Number of entries in writes.
 * @param writes The bindings to fill and what to put in them.
 */
void      pvkUpdateDescriptorSets(PvkDevice device, uint32_t writeCount, const PvkWriteDescriptorSet *writes);

/** @} */

/* ======================================================================== */
/**
 * @defgroup commands Command buffers
 * @brief Recording drawing commands into a list the hardware runs later.
 * @{
 */

/** @brief A recorded list of commands: a VIF1 DMA chain in main RAM. */
typedef struct PvkCommandBuffer_T *PvkCommandBuffer;

/** @brief What happens to an image's contents when rendering begins. */
typedef enum PvkAttachmentLoadOp {
    PVK_ATTACHMENT_LOAD_OP_LOAD      = 0, /**< Keep the existing contents. */
    PVK_ATTACHMENT_LOAD_OP_CLEAR     = 1, /**< Fill with the clear color. */
    PVK_ATTACHMENT_LOAD_OP_DONT_CARE = 2  /**< Contents are undefined. */
} PvkAttachmentLoadOp;

/** @brief Parameters for pvkCreateCommandBuffer(). May be NULL. */
typedef struct PvkCommandBufferCreateInfo {
    uint32_t maxQwords; /**< Size of the recorded DMA chain in 16-byte qwords; 0 = 4096 (64 KB). */
} PvkCommandBufferCreateInfo;

/** @brief A clear color. */
typedef struct PvkClearColorValue {
    float float32[4]; /**< Red, green, blue, alpha, each 0 to 1. */
} PvkClearColorValue;

/** @brief A clear depth. */
typedef struct PvkClearDepthStencilValue {
    float    depth;   /**< Depth to fill with, 0 to 1; usually 1 (far). */
    uint32_t stencil; /**< Unused: the GS has no stencil buffer. */
} PvkClearDepthStencilValue;

/** @brief The value an attachment is cleared to; which member is used depends on the attachment. */
typedef union PvkClearValue {
    PvkClearColorValue        color;        /**< For the color attachment. */
    PvkClearDepthStencilValue depthStencil; /**< For the depth attachment. */
} PvkClearValue;

/** @brief A rectangle in pixels. */
typedef struct PvkRect2D {
    int32_t  x;      /**< Left edge. */
    int32_t  y;      /**< Top edge. */
    uint32_t width;  /**< Width. */
    uint32_t height; /**< Height. */
} PvkRect2D;

/** @brief An image to render into and how to start it. */
typedef struct PvkRenderingAttachmentInfo {
    PvkImage            image;      /**< Target image: a swapchain image, or a depth image; NULL for no attachment. */
    PvkAttachmentLoadOp loadOp;     /**< What to do with its contents first. */
    PvkClearValue       clearValue; /**< Value used when loadOp is CLEAR. */
} PvkRenderingAttachmentInfo;

/**
 * @brief Parameters for pvkCmdBeginRendering(), like Vulkan 1.3 dynamic rendering.
 *
 * The depth attachment is optional (image NULL). When present it must be as
 * wide as the color attachment, since the GS gives both buffers one width, and
 * the pipelines drawn with it must name its format in depthAttachmentFormat.
 */
typedef struct PvkRenderingInfo {
    PvkRect2D                  renderArea;      /**< Area to render (and clear). */
    PvkRenderingAttachmentInfo colorAttachment; /**< The color image. */
    PvkRenderingAttachmentInfo depthAttachment; /**< The depth image, or image NULL for none. */
} PvkRenderingInfo;

/**
 * @brief Creates a command buffer.
 * @param device The device.
 * @param info Command buffer parameters, or NULL for the default size.
 * @param[out] cmd The new command buffer.
 * @return PVK_SUCCESS or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateCommandBuffer(PvkDevice device, const PvkCommandBufferCreateInfo *info, PvkCommandBuffer *cmd);

/**
 * @brief Destroys a command buffer. It must not be in use by a submission.
 * @param device The device.
 * @param cmd Command buffer to destroy.
 */
void      pvkDestroyCommandBuffer(PvkDevice device, PvkCommandBuffer cmd);

/**
 * @brief Starts recording, discarding anything recorded before.
 * @param cmd The command buffer.
 * @return PVK_SUCCESS.
 */
PvkResult pvkBeginCommandBuffer(PvkCommandBuffer cmd);

/**
 * @brief Finishes recording.
 * @param cmd The command buffer.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED if any command failed to record.
 */
PvkResult pvkEndCommandBuffer(PvkCommandBuffer cmd);

/**
 * @brief Starts rendering into a color image and optionally a depth image, clearing them if requested.
 *
 * Sets GS FRAME, ZBUF and XYOFFSET. A clear is a sprite fill over the render
 * area; depth and color are cleared in one pass when both ask for it.
 *
 * @param cmd The command buffer.
 * @param info Target images, render area and load operations.
 */
void pvkCmdBeginRendering(PvkCommandBuffer cmd, const PvkRenderingInfo *info);

/**
 * @brief Ends rendering into the current image.
 * @param cmd The command buffer.
 */
void pvkCmdEndRendering(PvkCommandBuffer cmd);

/**
 * @brief Binds a pipeline for the following draws.
 * @param cmd The command buffer.
 * @param pipeline The pipeline.
 */
void pvkCmdBindPipeline(PvkCommandBuffer cmd, PvkPipeline pipeline);

/**
 * @brief Sets push constants for the following draws.
 *
 * The bytes written must lie in the layout's ranges for every stage in
 * stageFlags. The data is copied, so it may change after this call. The
 * values stay set across pvkCmdBindPipeline() as long as the new pipeline's
 * layout has the same push constant ranges; pvkCmdDraw() fails if the vertex
 * shader reads bytes that were not set with a matching layout.
 *
 * @param cmd The command buffer.
 * @param layout Pipeline layout the ranges come from.
 * @param stageFlags PvkShaderStage values ORed together; the stages that read the bytes.
 * @param offset Byte offset into the push constant range; a multiple of 4.
 * @param size Bytes to write; a multiple of 4, offset + size at most PVK_MAX_PUSH_CONSTANT_BYTES.
 * @param data The values, laid out as in the shader's block (std430): a mat4 is
 *             its four columns, 16 bytes each, column-major as GLSL stores it.
 */
void pvkCmdPushConstants(PvkCommandBuffer cmd, PvkPipelineLayout layout, uint32_t stageFlags,
                         uint32_t offset, uint32_t size, const void *data);

/**
 * @brief Binds the vertex buffer for the following draws.
 * @param cmd The command buffer.
 * @param buffer The buffer holding the vertices.
 * @param offset Byte offset of the first vertex; must be a multiple of 16.
 */
void pvkCmdBindVertexBuffer(PvkCommandBuffer cmd, PvkBuffer buffer, size_t offset);

/** @brief Size of the indices in an index buffer. */
typedef enum PvkIndexType {
    PVK_INDEX_TYPE_UINT16 = 0 /**< 16-bit indices, the only size VU1 reads. */
} PvkIndexType;

/**
 * @brief Binds the index buffer for the following pvkCmdDrawIndexed() calls.
 *
 * The indices are read by the CPU when a draw is recorded and copied into the
 * command buffer, so the index buffer may change after the draw.
 *
 * @param cmd The command buffer.
 * @param buffer The buffer holding the indices.
 * @param offset Byte offset of the first index; must be a multiple of 2.
 * @param type Must be PVK_INDEX_TYPE_UINT16.
 */
void pvkCmdBindIndexBuffer(PvkCommandBuffer cmd, PvkBuffer buffer, size_t offset, PvkIndexType type);

/** @brief A region copied by pvkCmdCopyBufferToImage() (like VkBufferImageCopy). */
typedef struct PvkBufferImageCopy {
    size_t    bufferOffset; /**< Byte offset of the first pixel in the buffer; a multiple of 16. */
    PvkRect2D rect;         /**< Destination rectangle in the image. The buffer holds its rows back to back, 4 bytes per pixel. */
} PvkBufferImageCopy;

/**
 * @brief Uploads pixels from a buffer into a PVK_FORMAT_CT32 image.
 *
 * A GS host-to-local transfer (BITBLTBUF, TRXPOS, TRXREG, TRXDIR, then the
 * pixels as a GIF IMAGE packet). The pixels stream straight from the buffer
 * by DMA when the command buffer is submitted, so the buffer must stay
 * unchanged until then. Must be called outside pvkCmdBeginRendering() /
 * pvkCmdEndRendering(). The region must hold a multiple of 4 pixels.
 *
 * @param cmd The command buffer.
 * @param buffer Source pixels, RGBA with 8 bits each (GS PSMCT32: alpha 0x80 is opaque).
 * @param image Destination texture.
 * @param region What to copy and where.
 */
void pvkCmdCopyBufferToImage(PvkCommandBuffer cmd, PvkBuffer buffer, PvkImage image, const PvkBufferImageCopy *region);

/**
 * @brief Binds descriptor sets for the following draws.
 *
 * Set i of the array goes to set index firstSet + i, replacing whatever was
 * bound there. A draw reads the sets at the indices the fragment shader's
 * sampler2D and the vertex shader's uniform blocks name; each such set's
 * layout must have the same bindings as the pipeline layout's set layout at
 * that index, and the bindings must have been written. Texture coordinates
 * run 0..1 over the bound image. Sets may be bound before or after the
 * pipeline. The first draw after a bind of sets or a pipeline sends the
 * uniform blocks' buffers to VU1 again, after waiting for the running batch.
 *
 * @param cmd The command buffer.
 * @param layout The pipeline layout the sets are used with (for the sets' expected layouts).
 * @param firstSet Set index of the first set.
 * @param setCount Number of sets; firstSet + setCount at most PVK_MAX_DESCRIPTOR_SETS.
 * @param sets The sets.
 */
void pvkCmdBindDescriptorSets(PvkCommandBuffer cmd, PvkPipelineLayout layout, uint32_t firstSet,
                              uint32_t setCount, const PvkDescriptorSet *sets);

/**
 * @brief Draws vertices from the bound vertex buffer.
 *
 * Must be called between pvkCmdBeginRendering() and pvkCmdEndRendering(),
 * with a pipeline and vertex buffer bound. The pipeline's depthAttachmentFormat
 * must match the depth attachment (or its absence). Only whole triangles are drawn.
 *
 * @param cmd The command buffer.
 * @param vertexCount Number of vertices to draw.
 * @param firstVertex Index of the first vertex.
 */
void pvkCmdDraw(PvkCommandBuffer cmd, uint32_t vertexCount, uint32_t firstVertex);

/**
 * @brief Draws vertices from the bound vertex buffer in the order the bound
 *        index buffer gives.
 *
 * Needs what pvkCmdDraw() needs plus an index buffer. The vertices a batch
 * refers to are sent to VU1 as one block, so index lists that stay local in
 * the vertex buffer (a mesh in draw order) use the least DMA; VU1 then fetches
 * each vertex by its index. Only whole triangles are drawn.
 *
 * @param cmd The command buffer.
 * @param indexCount Number of indices to draw.
 * @param firstIndex Position of the first index in the index buffer.
 * @param vertexOffset Added to every index before it selects a vertex.
 */
void pvkCmdDrawIndexed(PvkCommandBuffer cmd, uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset);

/** @} */

/* ======================================================================== */
/**
 * @defgroup sync Submission and synchronization
 * @brief Sending command buffers to the hardware and waiting for them.
 * @{
 */

/** @brief Lets the CPU wait until a submission has finished drawing. */
typedef struct PvkFence_T         *PvkFence;

/** @brief Parameters for pvkCreateFence(). May be NULL. */
typedef struct PvkFenceCreateInfo {
    uint32_t signaled; /**< Nonzero to create the fence already signaled. */
} PvkFenceCreateInfo;

/**
 * @brief Sends a command buffer to the hardware.
 *
 * Writes the CPU cache back to RAM and starts DMA. Only one submission can be
 * in flight, so this first waits for the previous one.
 *
 * @param queue The queue.
 * @param cmd A command buffer that has finished recording.
 * @param fence Fence to signal when drawing finishes, or NULL.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED if the command buffer is
 *         still recording or failed to record.
 */
PvkResult pvkQueueSubmit(PvkQueue queue, PvkCommandBuffer cmd, PvkFence fence);

/**
 * @brief Creates a fence.
 * @param device The device.
 * @param info Fence parameters, or NULL for an unsignaled fence.
 * @param[out] fence The new fence.
 * @return PVK_SUCCESS or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateFence(PvkDevice device, const PvkFenceCreateInfo *info, PvkFence *fence);

/**
 * @brief Destroys a fence, waiting first if its submission is still running.
 * @param device The device.
 * @param fence Fence to destroy.
 */
void      pvkDestroyFence(PvkDevice device, PvkFence fence);

/**
 * @brief Sets fences back to unsignaled.
 * @param device The device.
 * @param count Number of fences.
 * @param fences The fences.
 * @return PVK_SUCCESS.
 */
PvkResult pvkResetFences(PvkDevice device, uint32_t count, const PvkFence *fences);

/**
 * @brief Waits until the fences' submissions have finished drawing.
 * @param device The device.
 * @param count Number of fences.
 * @param fences The fences.
 * @return PVK_SUCCESS.
 */
PvkResult pvkWaitForFences(PvkDevice device, uint32_t count, const PvkFence *fences);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PS2VK_H */
