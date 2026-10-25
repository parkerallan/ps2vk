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
    PVK_FORMAT_CT32                 /**< GS PSMCT32 color image (RGBA, 8 bits each). */
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
 * @defgroup swapchain Swapchain
 * @brief The framebuffers shown on the TV, and presenting them.
 * @{
 */

/** @brief The framebuffers shown on the TV in turn. */
typedef struct PvkSwapchain_T     *PvkSwapchain;
/** @brief An image in GS VRAM, such as a swapchain framebuffer. */
typedef struct PvkImage_T         *PvkImage;

/** @brief Parameters for pvkCreateSwapchain(). */
typedef struct PvkSwapchainCreateInfo {
    uint32_t  width;      /**< Width in pixels, e.g. 640. */
    uint32_t  height;     /**< Height in pixels, e.g. 448. */
    PvkFormat format;     /**< Must be PVK_FORMAT_CT32. */
    uint32_t  imageCount; /**< Must be 2. */
} PvkSwapchainCreateInfo;

/**
 * @brief Allocates the swapchain framebuffers in GS VRAM and turns on video output.
 *
 * Image 0 is shown first, so the first image to render into is image 1.
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
/** @brief A vertex shader (VU1 code) plus GS state, created ahead of time. */
typedef struct PvkPipeline_T      *PvkPipeline;

/** @brief Shader stages. */
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

    int32_t         colorInputLocation;    /**< Fragment: location of the color input, or -1 if none. */
    uint32_t        gouraud;               /**< Fragment: GS PRIM.IIP (blend vertex colors). */
    uint32_t        textured;              /**< Fragment: GS PRIM.TME (always 0 for now). */
} PvkShaderCode;

/** @brief Parameters for pvkCreateShaderModule(). */
typedef struct PvkShaderModuleCreateInfo {
    const PvkShaderCode *code; /**< Shader generated by tools/shaderc. */
} PvkShaderModuleCreateInfo;

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

/** @brief Parameters for pvkCreateGraphicsPipeline(). */
typedef struct PvkGraphicsPipelineCreateInfo {
    PvkShaderModule                vertexShader;   /**< Vertex shader module. */
    PvkShaderModule                fragmentShader; /**< Fragment shader module. */
    uint32_t                       vertexStride;   /**< Bytes per vertex; must be 16 per shader input. */
    uint32_t                       attributeCount; /**< Number of entries in attributes. */
    const PvkVertexInputAttribute *attributes;     /**< One per vertex shader input. */
    PvkPrimitiveTopology           topology;       /**< Must be PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST. */
    PvkViewport                    viewport;       /**< Viewport, fixed for this pipeline. */
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
 * Checks that the shaders match each other and the vertex layout, and
 * precomputes the viewport transform and GS primitive settings.
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

/** @brief A rectangle in pixels. */
typedef struct PvkRect2D {
    int32_t  x;      /**< Left edge. */
    int32_t  y;      /**< Top edge. */
    uint32_t width;  /**< Width. */
    uint32_t height; /**< Height. */
} PvkRect2D;

/** @brief The image to render into and how to start it. */
typedef struct PvkRenderingAttachmentInfo {
    PvkImage            image;      /**< Target image, e.g. a swapchain image. */
    PvkAttachmentLoadOp loadOp;     /**< What to do with its contents first. */
    PvkClearColorValue  clearValue; /**< Color used when loadOp is CLEAR. */
} PvkRenderingAttachmentInfo;

/** @brief Parameters for pvkCmdBeginRendering(), like Vulkan 1.3 dynamic rendering. */
typedef struct PvkRenderingInfo {
    PvkRect2D                  renderArea;      /**< Area to render (and clear). */
    PvkRenderingAttachmentInfo colorAttachment; /**< The color image. */
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
 * @brief Starts rendering into an image, clearing it if requested.
 * @param cmd The command buffer.
 * @param info Target image, render area and load operation.
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
 * @brief Binds the vertex buffer for the following draws.
 * @param cmd The command buffer.
 * @param buffer The buffer holding the vertices.
 * @param offset Byte offset of the first vertex; must be a multiple of 16.
 */
void pvkCmdBindVertexBuffer(PvkCommandBuffer cmd, PvkBuffer buffer, size_t offset);

/**
 * @brief Draws vertices from the bound vertex buffer.
 *
 * Must be called between pvkCmdBeginRendering() and pvkCmdEndRendering(),
 * with a pipeline and vertex buffer bound. Only whole triangles are drawn.
 *
 * @param cmd The command buffer.
 * @param vertexCount Number of vertices to draw.
 * @param firstVertex Index of the first vertex.
 */
void pvkCmdDraw(PvkCommandBuffer cmd, uint32_t vertexCount, uint32_t firstVertex);

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
