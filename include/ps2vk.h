/**
 * @file ps2vk.h
 * @brief ps2vk - a Vulkan-like graphics API for the PlayStation 2.
 *
 *
 * Functions conceptually follow Vulkan, but instead use the "pvk" prefix.
 *
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

/** @brief Result codes. */
typedef enum PvkResult {
    PVK_SUCCESS                        = 0,           /**< The call succeeded. */
    PVK_NOT_READY                      = 1,           /**< pvkAcquireNextImage() with timeout 0: no image is free and none will be until a present. */
    PVK_TIMEOUT                        = 2,           /**< pvkAcquireNextImage() with a timeout: the same, so waiting would not help. */
    PVK_INCOMPLETE                     = 5,           /**< The array given was too small for every result. */
    PVK_ERROR_OUT_OF_HOST_MEMORY       = -1,          /**< EE main RAM allocation failed. */
    PVK_ERROR_OUT_OF_DEVICE_MEMORY     = -2,          /**< Not enough GS VRAM. */
    PVK_ERROR_INITIALIZATION_FAILED    = -3,          /**< Hardware setup failed. */
    PVK_ERROR_FEATURE_NOT_PRESENT      = -8,          /**< The PS2 or ps2vk doesn't support this yet. */
    PVK_ERROR_FORMAT_NOT_SUPPORTED     = -11,         /**< The format can't be used here. */
    PVK_ERROR_INVALID_SHADER           = -1000012000, /**< The shader code is missing or broken. */
    PVK_ERROR_VALIDATION_FAILED        = -1000011001  /**< Invalid use of the API; details are printed. */
} PvkResult;

/**
 * @brief Data formats for vertex attributes and images.
 *
 * Vertex attribute formats are a vec4 of floats, or 1 to 4
 * components of 32-bit floats, 16-bit or 8-bit integers (see
 * PvkVertexInputBinding for how they are laid out). The shader always sees a
 * vec4: missing components read 0, a missing w reads 1; UNORM/SNORM integers
 * are scaled to 0..1 / -1..1 and USCALED/SSCALED are the integer's value.
 * Image formats are the GS's.
 *
 * A vertex is read by DMA for every draw, and VU1 holds about 430 qwords of
 * input per batch however the attributes are stored, so a smaller attribute
 * format means less main RAM, less DMA time per frame and nothing else: the
 * VIF widens the values on the way in and VU1 scales them with two or three
 * instructions per attribute. Each 8- or 16-bit attribute is a binding of its
 * own (see PvkVertexInputBinding).
 *
 * | Format | Bytes per vertex | VIF unpack | Shader sees | What you get |
 * | --- | --- | --- | --- | --- |
 * | PVK_FORMAT_R32G32B32A32_SFLOAT | 16 | V4-32 | the floats | Exact values; several may share one binding. Use when a vertex is rewritten by the CPU every frame, or when precision matters: positions of a large mesh, skinning weights that must sum to 1. |
 * | PVK_FORMAT_R32G32B32_SFLOAT, R32G32_SFLOAT, R32_SFLOAT | 12, 8, 4 | V3-32, V2-32, S-32 | the floats, w = 1 | Same precision without the padding of a vec4. Positions, 2D texture coordinates and scalars that need the full float range. |
 * | PVK_FORMAT_R16G16B16A16_SNORM, R16G16B16_SNORM, R16G16_SNORM, R16_SNORM | 8, 6, 4, 2 | V4-16, V3-16, V2-16, S-16 | -1..1 | 1/32767 steps, invisible for anything within a unit range. Normals, tangents, and the positions of a model that fits in a ±1 box (put the model's size in its matrix). |
 * | PVK_FORMAT_R16G16B16A16_UNORM, R16G16B16_UNORM, R16G16_UNORM, R16_UNORM | 8, 6, 4, 2 | V4-16, V3-16, V2-16, S-16 | 0..1 | 1/65535 steps. Texture coordinates for any texture the PS2 can hold (1024 texels at most, so 64 steps per texel), heights and other 0..1 fields. |
 * | PVK_FORMAT_R16G16B16A16_SSCALED/USCALED and the 3, 2, 1 component forms | 8, 6, 4, 2 | V4-16, V3-16, V2-16, S-16 | the integer | -32768..32767 or 0..65535 as a float. Positions on a world grid with the unit in a push constant or uniform, which is how PS2 games stored level geometry; indices such as bone numbers. |
 * | PVK_FORMAT_R8G8B8A8_SNORM, R8G8B8_SNORM, R8G8_SNORM, R8_SNORM | 4, 3, 2, 1 | V4-8, V3-8, V2-8, S-8 | -1..1 | 1/127 steps, enough for a normal when lighting is per vertex (the GS interpolates colors, not normals, so the shader's lighting result is what is smoothed). |
 * | PVK_FORMAT_R8G8B8A8_UNORM, R8G8B8_UNORM, R8G8_UNORM, R8_UNORM | 4, 3, 2, 1 | V4-8, V3-8, V2-8, S-8 | 0..1 | 1/255 steps: the GS itself only keeps 8 bits per color channel, so vertex colors lose nothing. Colors, blend weights, ambient occlusion. |
 * | PVK_FORMAT_R8G8B8A8_SSCALED/USCALED and the 3, 2, 1 component forms | 4, 3, 2, 1 | V4-8, V3-8, V2-8, S-8 | the integer | -128..127 or 0..255 as a float. Bone indices, material or palette numbers, anything the shader uses as a count. |
 *
 * The practical rule: colors as R8G8B8A8_UNORM, normals and texture
 * coordinates as 16-bit, positions as 16-bit when the model fits a known box
 * and float otherwise.
 */
typedef enum PvkFormat {
    PVK_FORMAT_UNDEFINED = 0,       /**< No format. */
    PVK_FORMAT_R32G32B32A32_SFLOAT, /**< Vertex attribute: 4 floats. */
    PVK_FORMAT_CT32,                /**< GS PSMCT32 color image (RGBA, 8 bits each; alpha 0x80 is opaque). */
    PVK_FORMAT_CT24,                /**< GS PSMCT24 texture or color table (RGB, 8 bits each, 3 bytes per texel). Reads as opaque. */
    PVK_FORMAT_CT16,                /**< GS PSMCT16 texture or color table (16 bits per texel: R in bits 0-4, G 5-9, B 10-14, alpha bit 15). */
    PVK_FORMAT_T8,                  /**< GS PSMT8 texture: one byte per texel, an index into a 256-entry color table (PS2 only). */
    PVK_FORMAT_T4,                  /**< GS PSMT4 texture: 4 bits per texel, two per byte with the first in the low bits, an index into a 16-entry color table (PS2 only). */
    PVK_FORMAT_Z32,                 /**< GS PSMZ32 depth image. */
    PVK_FORMAT_Z24,                 /**< GS PSMZ24 depth image. */
    PVK_FORMAT_Z16,                 /**< GS PSMZ16 depth image. */
    PVK_FORMAT_R32G32B32_SFLOAT,    /**< Vertex attribute: 3 floats (VIF UNPACK V3-32). */
    PVK_FORMAT_R32G32_SFLOAT,       /**< Vertex attribute: 2 floats (V2-32). */
    PVK_FORMAT_R32_SFLOAT,          /**< Vertex attribute: 1 float (S-32). */
    PVK_FORMAT_R16G16B16A16_UNORM,  /**< Vertex attribute: 4 unsigned 16-bit integers, 0..65535 read as 0..1 (V4-16). */
    PVK_FORMAT_R16G16B16A16_SNORM,  /**< Vertex attribute: 4 signed 16-bit integers, -32767..32767 read as -1..1, -32768 as -1. */
    PVK_FORMAT_R16G16B16A16_USCALED,/**< Vertex attribute: 4 unsigned 16-bit integers read as their value. */
    PVK_FORMAT_R16G16B16A16_SSCALED,/**< Vertex attribute: 4 signed 16-bit integers read as their value. */
    PVK_FORMAT_R16G16B16_UNORM,     /**< Vertex attribute: 3 unsigned 16-bit integers (V3-16). */
    PVK_FORMAT_R16G16B16_SNORM,     /**< Vertex attribute: 3 signed 16-bit integers. */
    PVK_FORMAT_R16G16B16_USCALED,   /**< Vertex attribute: 3 unsigned 16-bit integers read as their value. */
    PVK_FORMAT_R16G16B16_SSCALED,   /**< Vertex attribute: 3 signed 16-bit integers read as their value. */
    PVK_FORMAT_R16G16_UNORM,        /**< Vertex attribute: 2 unsigned 16-bit integers (V2-16). */
    PVK_FORMAT_R16G16_SNORM,        /**< Vertex attribute: 2 signed 16-bit integers. */
    PVK_FORMAT_R16G16_USCALED,      /**< Vertex attribute: 2 unsigned 16-bit integers read as their value. */
    PVK_FORMAT_R16G16_SSCALED,      /**< Vertex attribute: 2 signed 16-bit integers read as their value. */
    PVK_FORMAT_R16_UNORM,           /**< Vertex attribute: 1 unsigned 16-bit integer (S-16). */
    PVK_FORMAT_R16_SNORM,           /**< Vertex attribute: 1 signed 16-bit integer. */
    PVK_FORMAT_R16_USCALED,         /**< Vertex attribute: 1 unsigned 16-bit integer read as its value. */
    PVK_FORMAT_R16_SSCALED,         /**< Vertex attribute: 1 signed 16-bit integer read as its value. */
    PVK_FORMAT_R8G8B8A8_UNORM,      /**< Vertex attribute: 4 unsigned bytes, 0..255 read as 0..1 (V4-8). */
    PVK_FORMAT_R8G8B8A8_SNORM,      /**< Vertex attribute: 4 signed bytes, -127..127 read as -1..1, -128 as -1. */
    PVK_FORMAT_R8G8B8A8_USCALED,    /**< Vertex attribute: 4 unsigned bytes read as their value. */
    PVK_FORMAT_R8G8B8A8_SSCALED,    /**< Vertex attribute: 4 signed bytes read as their value. */
    PVK_FORMAT_R8G8B8_UNORM,        /**< Vertex attribute: 3 unsigned bytes (V3-8). */
    PVK_FORMAT_R8G8B8_SNORM,        /**< Vertex attribute: 3 signed bytes. */
    PVK_FORMAT_R8G8B8_USCALED,      /**< Vertex attribute: 3 unsigned bytes read as their value. */
    PVK_FORMAT_R8G8B8_SSCALED,      /**< Vertex attribute: 3 signed bytes read as their value. */
    PVK_FORMAT_R8G8_UNORM,          /**< Vertex attribute: 2 unsigned bytes (V2-8). */
    PVK_FORMAT_R8G8_SNORM,          /**< Vertex attribute: 2 signed bytes. */
    PVK_FORMAT_R8G8_USCALED,        /**< Vertex attribute: 2 unsigned bytes read as their value. */
    PVK_FORMAT_R8G8_SSCALED,        /**< Vertex attribute: 2 signed bytes read as their value. */
    PVK_FORMAT_R8_UNORM,            /**< Vertex attribute: 1 unsigned byte (S-8). */
    PVK_FORMAT_R8_SNORM,            /**< Vertex attribute: 1 signed byte. */
    PVK_FORMAT_R8_USCALED,          /**< Vertex attribute: 1 unsigned byte read as its value. */
    PVK_FORMAT_R8_SSCALED           /**< Vertex attribute: 1 signed byte read as its value. */
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
/** @brief The graphics hardware as it can be queried before a device exists. The instance has exactly one. */
typedef struct PvkPhysicalDevice_T *PvkPhysicalDevice;
/** @brief The PS2 graphics hardware (EE DMA, VU1, GS). There is only one. */
typedef struct PvkDevice_T        *PvkDevice;
/**
 * @brief Where command buffers are submitted: one per queue family the device was created with.
 *
 * The graphics queue (family 0) sends its chains down DMA channel 1 through
 * VIF1 and PATH2; the transfer queue (family 1) sends its chains down DMA
 * channel 2 straight to the GIF through PATH3, so a texture upload runs
 * while VIF1 and VU1 feed draws. The GIF takes PATH2 and PATH3 packets in
 * turn, so the two queues interleave at GIF packet boundaries.
 */
typedef struct PvkQueue_T         *PvkQueue;
/**
 * @brief Lets the CPU wait until a submission has finished drawing, or an acquired swapchain image is free.
 *
 * Each submission ends with a GS FINISH and a GS SIGNAL whose ID names its
 * fence. The SIGNAL raises INTC_GS; the handler marks the fence signaled and
 * releases an EE semaphore, so a waiting thread sleeps instead of polling.
 * A fence given to pvkAcquireNextImage() is signaled the same way by the
 * vblank handler when the image leaves the screen.
 */
typedef struct PvkFence_T         *PvkFence;
/**
 * @brief Orders one queue operation after another without the CPU.
 *
 * Binary: a submission or an acquire signals it, and one later submission or
 * present waits for it, which resets it. The wait holds the waiting
 * operation back, not the CPU: a submission waiting on one stays in the
 * queue's ring and is started by an interrupt handler the moment the signal
 * arrives, the INTC_GS SIGNAL handler for a submission's signal or the
 * INTC_VBLANK_S handler for an acquire's; a present waiting on one is a
 * DISPFB write the vblank handler holds until the signal.
 */
typedef struct PvkSemaphore_T     *PvkSemaphore;

/** @brief Parameters for pvkCreateInstance(). */
typedef struct PvkInstanceCreateInfo {
    const char *applicationName; /**< Application name (informational). */
} PvkInstanceCreateInfo;

/** @brief What kind of device a physical device is. */
typedef enum PvkPhysicalDeviceType {
    PVK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1 /**< The GS shares the console with the EE. */
} PvkPhysicalDeviceType;

/** @brief Length of PvkPhysicalDeviceProperties::deviceName. */
#define PVK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256

/**
 * @brief Most mip levels a texture can have: the GS caps it at 7 (TEX1.MXL is 3
 * bits) whatever the size.
 */
#define PVK_MAX_MIP_LEVELS 7

/**
 * @brief What the hardware can do.
 *
 * The library checks against these same values, so a creation call that
 * stays within them does not fail for size.
 *
 * | Limit | Value | Where it comes from |
 * | --- | --- | --- |
 * | maxImageDimension2D | 1024 | GS TEX0.TW/TH are log2 sizes up to 10 |
 * | maxMipLevels (PS2 only) | PVK_MAX_MIP_LEVELS | TEX1.MXL |
 * | maxUniformBufferRange | PVK_MAX_UNIFORM_BYTES | the VU1 uniform area |
 * | maxPushConstantsSize | PVK_MAX_PUSH_CONSTANT_BYTES | 8 qwords of VU1 memory |
 * | maxBoundDescriptorSets | PVK_MAX_DESCRIPTOR_SETS | the set table in the command buffer |
 * | maxDescriptorSetBindings (PS2 only) | PVK_MAX_DESCRIPTOR_BINDINGS | the binding table of a set |
 * | maxPerStageDescriptorSamplers, maxPerStageDescriptorSampledImages | 1 | the GS has one TEX0 |
 * | maxPerStageDescriptorUniformBuffers | PVK_MAX_UNIFORM_BLOCKS | shaderc's block table |
 * | maxVertexInputAttributes | PVK_MAX_VERTEX_INPUTS | one VU1 input qword each |
 * | maxVertexInputBindings | PVK_MAX_VERTEX_BINDINGS | one DMA REF per binding per batch |
 * | maxViewports | 1 | one XYOFFSET/SCISSOR pair |
 * | maxViewportDimensions | 2048 x 2048 | the 4096 primitive space minus the origin margin |
 * | viewportBoundsRange | -1024 to 3071 | XYOFFSET puts pixel 0 at 1024 of 4096 |
 * | maxFramebufferWidth, maxFramebufferHeight | 2048 | FRAME.FBW in 64-pixel units, DISPLAY height |
 * | maxColorAttachments | 1 | one FRAME register |
 * | maxDrawIndirectCount | 4294967295 | the EE reads each command when the draw is recorded |
 * | minUniformBufferOffsetAlignment, minMemoryMapAlignment | 16, 64 | a VIF UNPACK qword, a cache line |
 * | maxSamplerLodBias | 127.9375 | TEX1.K is signed 7.4 fixed point |
 * | maxSamplerAnisotropy | 1 | none |
 * | pointSizeRange, lineWidthRange | 1 to 1 | GS points and lines are one pixel |
 * | subPixelPrecisionBits | 4 | XYZ2 is 12.4 fixed point |
 * | timestampPeriod | 1e9 / 294912000 ns | the EE COP0 Count register, read when the SIGNAL interrupt fires |
 * | timestampComputeAndGraphics | 1 | a GS SIGNAL goes through PATH2 and PATH3 alike |
 */
typedef struct PvkPhysicalDeviceLimits {
    uint32_t maxImageDimension2D;                 /**< Widest and tallest texture, in texels. */
    uint32_t maxMipLevels;                        /**< PS2 only: most mip levels of a texture. */
    uint32_t maxUniformBufferRange;               /**< Largest uniform buffer range, in bytes. */
    uint32_t maxPushConstantsSize;                /**< Bytes of push constants. */
    uint32_t maxBoundDescriptorSets;              /**< Descriptor sets a pipeline layout has and a command buffer binds. */
    uint32_t maxDescriptorSetBindings;            /**< PS2 only: bindings in one descriptor set layout. */
    uint32_t maxPerStageDescriptorSamplers;       /**< Samplers one shader stage reads. */
    uint32_t maxPerStageDescriptorUniformBuffers; /**< Uniform buffers one shader stage reads. */
    uint32_t maxPerStageDescriptorSampledImages;  /**< Sampled images one shader stage reads. */
    uint32_t maxVertexInputAttributes;            /**< Vertex attributes a pipeline declares. */
    uint32_t maxVertexInputBindings;              /**< Vertex buffer bindings a pipeline declares. */
    uint32_t maxViewports;                        /**< Viewports a pipeline has. */
    uint32_t maxViewportDimensions[2];            /**< Widest and tallest viewport, in pixels. */
    float    viewportBoundsRange[2];              /**< Lowest and highest viewport corner coordinate. */
    uint32_t maxFramebufferWidth;                 /**< Widest color or depth attachment. */
    uint32_t maxFramebufferHeight;                /**< Tallest color or depth attachment. */
    uint32_t maxColorAttachments;                 /**< Color attachments of one rendering pass. */
    uint32_t maxDrawIndirectCount;                /**< Largest drawCount of pvkCmdDrawIndirect() and pvkCmdDrawIndexedIndirect(). */
    size_t   minUniformBufferOffsetAlignment;     /**< Alignment of PvkDescriptorBufferInfo::offset. */
    size_t   minMemoryMapAlignment;               /**< Alignment of what pvkMapMemory() returns. */
    float    maxSamplerLodBias;                   /**< Largest magnitude of PvkSamplerCreateInfo::mipLodBias. */
    float    maxSamplerAnisotropy;                /**< Largest anisotropy; 1 means none. */
    float    pointSizeRange[2];                   /**< Smallest and largest point size, in pixels. */
    float    lineWidthRange[2];                   /**< Thinnest and widest line, in pixels. */
    uint32_t subPixelPrecisionBits;               /**< Fraction bits of a rasterized position. */
    float    timestampPeriod;                     /**< Nanoseconds per timestamp tick: the EE clock, 1e9 / 294912000. */
    uint32_t timestampComputeAndGraphics;         /**< 1: every queue family writes timestamps. */
} PvkPhysicalDeviceLimits;

/** @brief Who and what the physical device is. */
typedef struct PvkPhysicalDeviceProperties {
    uint32_t              apiVersion;    /**< The ps2vk API version, PVK_API_VERSION_1_0. */
    uint32_t              driverVersion; /**< The library version. */
    uint32_t              vendorID;      /**< 0x054C: Sony. */
    uint32_t              deviceID;      /**< 0: the Graphics Synthesizer. */
    PvkPhysicalDeviceType deviceType;    /**< PVK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU. */
    char                  deviceName[PVK_MAX_PHYSICAL_DEVICE_NAME_SIZE]; /**< "PlayStation 2 Graphics Synthesizer". */
    PvkPhysicalDeviceLimits limits;      /**< What it can do. */
} PvkPhysicalDeviceProperties;

/** @brief The API version pvkGetPhysicalDeviceProperties() reports: major 1, minor 0, patch 0, packed as major << 22 | minor << 12 | patch. */
#define PVK_API_VERSION_1_0 (1u << 22)

/**
 * @brief Optional device behaviour an application turns on.
 *
 * pvkGetPhysicalDeviceFeatures() says which the hardware supports; every
 * feature is off unless the application enables it in PvkDeviceCreateInfo,
 * and pvkCreateDevice() fails with PVK_ERROR_FEATURE_NOT_PRESENT for one the
 * hardware lacks. With robustBufferAccess a vertex shader's run-time index
 * into a block array (see PvkShaderUniformBlock) is clamped to the array, so
 * an index past the end reads the last element; the PS2 pays 3 to 4 VU1
 * instructions per indexed read for it. Off, such a read takes whatever VU1
 * memory follows the array, undefined. With multiDrawIndirect an indirect
 * draw takes a drawCount above 1; with drawIndirectFirstInstance an indirect
 * command's firstInstance may be nonzero. The EE reads every command, so both
 * cost nothing on the PS2.
 */
typedef struct PvkPhysicalDeviceFeatures {
    uint32_t robustBufferAccess;       /**< Nonzero: run-time indices into block arrays are clamped. */
    uint32_t multiDrawIndirect;        /**< Nonzero: pvkCmdDrawIndirect() and pvkCmdDrawIndexedIndirect() take a drawCount above 1. */
    uint32_t drawIndirectFirstInstance; /**< Nonzero: an indirect command's firstInstance may be nonzero. */
} PvkPhysicalDeviceFeatures;

/** @brief Properties of a memory heap. */
typedef enum PvkMemoryHeapFlagBits {
    PVK_MEMORY_HEAP_DEVICE_LOCAL_BIT = 1 /**< GS VRAM: only the GS reads and writes it. */
} PvkMemoryHeapFlagBits;

/** @brief One pool of memory. */
typedef struct PvkMemoryHeap {
    size_t   size;  /**< Bytes in the heap. */
    uint32_t flags; /**< PvkMemoryHeapFlagBits. */
} PvkMemoryHeap;

/** @brief Most heaps pvkGetPhysicalDeviceMemoryProperties() lists. */
#define PVK_MAX_MEMORY_HEAPS 2

/** @brief How memory of a type behaves. */
typedef enum PvkMemoryPropertyFlagBits {
    PVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT  = 1, /**< GS VRAM: the GS reads and writes it, the CPU can't map it. */
    PVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  = 2, /**< pvkMapMemory() works on it. */
    PVK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 4  /**< Mapped writes need no flush: the data cache is written back at pvkQueueSubmit(). */
} PvkMemoryPropertyFlagBits;

/** @brief One kind of memory an allocation can be. */
typedef struct PvkMemoryType {
    uint32_t propertyFlags; /**< PvkMemoryPropertyFlagBits. */
    uint32_t heapIndex;     /**< The heap it comes from. */
} PvkMemoryType;

/** @brief Most memory types pvkGetPhysicalDeviceMemoryProperties() lists. */
#define PVK_MAX_MEMORY_TYPES 2

/**
 * @brief The device's memory.
 *
 * Heap 0 is EE main RAM, heap 1 is GS VRAM, both whole. Type 0 is heap 0,
 * host visible and coherent: buffers live there. Type 1 is heap 1, device
 * local: images live there. PvkMemoryRequirements::memoryTypeBits names the
 * type a resource takes.
 */
typedef struct PvkPhysicalDeviceMemoryProperties {
    uint32_t      memoryTypeCount;                   /**< Types listed. */
    PvkMemoryType memoryTypes[PVK_MAX_MEMORY_TYPES]; /**< The types. */
    uint32_t      memoryHeapCount;                   /**< Heaps listed. */
    PvkMemoryHeap memoryHeaps[PVK_MAX_MEMORY_HEAPS]; /**< The heaps. */
} PvkPhysicalDeviceMemoryProperties;

/** @brief What a queue family's queues can do. */
typedef enum PvkQueueFlagBits {
    PVK_QUEUE_GRAPHICS_BIT = 0x00000001, /**< Draws: pvkCmdBeginRendering() and everything inside it. */
    PVK_QUEUE_TRANSFER_BIT = 0x00000004  /**< Copies: pvkCmdCopyBufferToImage(). A graphics family has it too. */
} PvkQueueFlagBits;
/** @brief Bitmask of PvkQueueFlagBits. */
typedef uint32_t PvkQueueFlags;

/** @brief A size in three dimensions. */
typedef struct PvkExtent3D {
    uint32_t width;  /**< Width. */
    uint32_t height; /**< Height. */
    uint32_t depth;  /**< Depth. */
} PvkExtent3D;

/**
 * @brief One queue family: a kind of queue and how many of it the device has.
 *
 * Family 0 is the graphics queue: VIF1 and PATH2, GRAPHICS and TRANSFER.
 * Family 1 is the transfer queue: the GIF DMA channel and PATH3, TRANSFER
 * only. Each has one queue; family 0 alone presents.
 */
typedef struct PvkQueueFamilyProperties {
    PvkQueueFlags queueFlags;                  /**< PvkQueueFlagBits ORed together. */
    uint32_t      queueCount;                  /**< Queues in the family: 1. */
    uint32_t      timestampValidBits;          /**< 32: the EE COP0 Count register pvkCmdWriteTimestamp() reads. */
    PvkExtent3D   minImageTransferGranularity; /**< (1, 1, 1): copies are limited by format and qword size only, see pvkCmdCopyBufferToImage(). */
} PvkQueueFamilyProperties;

/** @brief Most queue families the device has. */
#define PVK_MAX_QUEUE_FAMILIES 2

/** @brief One queue family to create queues of, for PvkDeviceCreateInfo. */
typedef struct PvkDeviceQueueCreateInfo {
    uint32_t     queueFamilyIndex; /**< 0 for the graphics queue, 1 for the transfer queue. */
    uint32_t     queueCount;       /**< Queues of the family: 1. */
    const float *queuePriorities;  /**< One priority per queue, or NULL; the hardware has none to set. */
} PvkDeviceQueueCreateInfo;

/** @brief Parameters for pvkCreateDevice(). */
typedef struct PvkDeviceCreateInfo {
    uint32_t                        queueCreateInfoCount; /**< Number of entries in queueCreateInfos, at least 1. */
    const PvkDeviceQueueCreateInfo *queueCreateInfos;     /**< The queue families to create queues of, each family once. */
    const PvkPhysicalDeviceFeatures *enabledFeatures;     /**< Features to turn on, or NULL for none. */
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
 * @brief Lists the physical devices: the one PS2.
 * @param instance The instance.
 * @param[in,out] count With @p devices NULL, receives how many there are (1); otherwise how many fit, then how many were written.
 * @param[out] devices The devices, or NULL to ask for the count.
 * @return PVK_SUCCESS, or PVK_INCOMPLETE when @p count was 0 with @p devices given.
 */
PvkResult pvkEnumeratePhysicalDevices(PvkInstance instance, uint32_t *count, PvkPhysicalDevice *devices);

/**
 * @brief Reports what the hardware is and its limits.
 * @param physicalDevice The physical device.
 * @param[out] properties Filled in.
 */
void      pvkGetPhysicalDeviceProperties(PvkPhysicalDevice physicalDevice, PvkPhysicalDeviceProperties *properties);

/**
 * @brief Reports which features the hardware supports, each nonzero if it does.
 * @param physicalDevice The physical device.
 * @param[out] features Filled in.
 */
void      pvkGetPhysicalDeviceFeatures(PvkPhysicalDevice physicalDevice, PvkPhysicalDeviceFeatures *features);

/**
 * @brief Reports the memory types and heaps.
 * @param physicalDevice The physical device.
 * @param[out] properties Filled in.
 */
void      pvkGetPhysicalDeviceMemoryProperties(PvkPhysicalDevice physicalDevice, PvkPhysicalDeviceMemoryProperties *properties);

/**
 * @brief Reports the queue families: the graphics queue (family 0) and the transfer queue (family 1).
 * @param physicalDevice The physical device.
 * @param[in,out] count With @p properties NULL, receives how many there are (2); otherwise how many fit, then how many were written.
 * @param[out] properties The families, or NULL to ask for the count.
 */
void      pvkGetPhysicalDeviceQueueFamilyProperties(PvkPhysicalDevice physicalDevice, uint32_t *count, PvkQueueFamilyProperties *properties);

/**
 * @brief Prepares the graphics hardware: resets the GIF, sets up the VIF1 DMA channel and, when family 1 is asked for, the GIF DMA channel.
 * @param physicalDevice The physical device from pvkEnumeratePhysicalDevices().
 * @param info The queue families to create queues of and the features to enable.
 * @param[out] device The new device.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED if no queue is asked for, a family is unknown or asked for twice, or a queueCount is not 1; PVK_ERROR_FEATURE_NOT_PRESENT for an enabled feature the hardware lacks; or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateDevice(PvkPhysicalDevice physicalDevice, const PvkDeviceCreateInfo *info, PvkDevice *device);

/**
 * @brief Waits for the device to go idle, then destroys it.
 * @param device Device to destroy.
 */
void      pvkDestroyDevice(PvkDevice device);

/**
 * @brief Gets one of the device's queues.
 * @param device The device.
 * @param queueFamilyIndex A family named in the device's PvkDeviceCreateInfo: 0 graphics, 1 transfer.
 * @param queueIndex 0: each family has one queue.
 * @param[out] queue The queue, or NULL if the family was not asked for at pvkCreateDevice() or the index is not 0.
 */
void      pvkGetDeviceQueue(PvkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, PvkQueue *queue);

/**
 * @brief Waits until all submitted work has finished.
 * @param device The device.
 * @return PVK_SUCCESS.
 */
PvkResult pvkDeviceWaitIdle(PvkDevice device);

/** @} */

/* ======================================================================== */
/**
 * @defgroup memory Device memory
 * @brief Blocks of EE RAM or GS VRAM that buffers and images are placed in.
 *
 * A buffer or image is created empty and then bound to a part
 * of a PvkDeviceMemory allocation at an offset. One allocation holds as many
 * resources as fit, so an application lays its memory out itself: a frame's
 * buffers back to back in one EE RAM block, the depth buffer and textures
 * packed into the VRAM left after the framebuffers, or two resources used at
 * different times on the same bytes. pvkGetBufferMemoryRequirements() and
 * pvkGetImageMemoryRequirements() say how much a resource needs, at what
 * alignment and from which memory type.
 *
 * | Type | Heap | Memory | Alignment of an allocation | Resources |
 * | --- | --- | --- | --- | --- |
 * | 0 | 0 | EE main RAM, host visible and coherent | 64 bytes | Buffers |
 * | 1 | 1 | GS VRAM, device local | 8 KB (a GS page) | Images |
 *
 * VRAM offsets are bytes; the GS addresses words, 256-byte blocks (textures)
 * and 8 KB pages (framebuffers, depth buffers), which the requirements'
 * alignment reflects. The swapchain's framebuffers take VRAM of their own
 * when it is created, before any allocation; pvkGetPhysicalDeviceMemoryProperties()
 * reports the whole heap.
 * @{
 */

/** @brief A block of EE RAM or GS VRAM. */
typedef struct PvkDeviceMemory_T  *PvkDeviceMemory;

/** @brief The rest of a buffer or allocation, as a range or mapping size. */
#define PVK_WHOLE_SIZE (~(size_t)0)

/** @brief What a resource needs from an allocation. */
typedef struct PvkMemoryRequirements {
    size_t   size;           /**< Bytes the resource takes from its bind offset on. */
    size_t   alignment;      /**< The bind offset is a multiple of this. */
    uint32_t memoryTypeBits; /**< Bit i set: memory type i may hold it. */
} PvkMemoryRequirements;

/** @brief Parameters for pvkAllocateMemory(). */
typedef struct PvkMemoryAllocateInfo {
    size_t   allocationSize;  /**< Bytes to allocate; VRAM is taken in whole 8 KB pages. */
    uint32_t memoryTypeIndex; /**< Index into PvkPhysicalDeviceMemoryProperties::memoryTypes. */
} PvkMemoryAllocateInfo;

/**
 * @brief Allocates a block of memory from a heap.
 *
 * Type 0 memory is EE RAM from the C heap, 64-byte aligned. Type 1 memory is
 * GS VRAM, page aligned, taken from the part of VRAM the swapchain does not
 * use; it comes back to the heap with pvkFreeMemory(). Resources are placed
 * in it with pvkBindBufferMemory() and pvkBindImageMemory().
 *
 * @param device The device.
 * @param info Size and memory type.
 * @param[out] memory The new allocation.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED for a type the device lacks
 *         or a zero size, PVK_ERROR_OUT_OF_DEVICE_MEMORY when the heap has no
 *         room, or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkAllocateMemory(PvkDevice device, const PvkMemoryAllocateInfo *info, PvkDeviceMemory *memory);

/**
 * @brief Frees an allocation. The resources bound to it must not be used again, and no submission may still read it.
 * @param device The device.
 * @param memory Allocation to free; NULL does nothing.
 */
void      pvkFreeMemory(PvkDevice device, PvkDeviceMemory memory);

/**
 * @brief Gets a CPU pointer into a host visible allocation.
 *
 * Writes need no flushing: the data cache is written back at pvkQueueSubmit().
 * Only type 0 (EE RAM) memory can be mapped; a submission may read the
 * memory while it is mapped, so the pointer is good until pvkFreeMemory().
 *
 * @param device The device.
 * @param memory The allocation.
 * @param offset First byte mapped.
 * @param size Bytes mapped from offset on, or PVK_WHOLE_SIZE for the rest; 0 is an error.
 * @param[out] data Pointer to the memory at offset.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED for device local memory or
 *         a range outside the allocation.
 */
PvkResult pvkMapMemory(PvkDevice device, PvkDeviceMemory memory, size_t offset, size_t size, void **data);

/**
 * @brief Ends CPU access to an allocation. Currently does nothing.
 * @param device The device.
 * @param memory The allocation.
 */
void      pvkUnmapMemory(PvkDevice device, PvkDeviceMemory memory);

/** @} */

/* ======================================================================== */
/**
 * @defgroup images Images
 * @brief Pixel memory in GS VRAM: framebuffers, depth buffers and textures.
 *
 * The choice is almost entirely about GS VRAM, which is 4 MB shared by
 * framebuffers, depth buffer and every texture. A 640×448 CT32 framebuffer
 * pair plus a Z24 buffer already takes about 3.4 MB, so textures fight over
 * what is left. Smaller texel formats also move less data through the DMA on
 * upload and through the texture cache while drawing, which is why real PS2
 * games used 8-bit and 4-bit textures almost everywhere.
 *
 * | Format | Bits per texel | 256×256 texture | What you get |
 * | --- | --- | --- | --- |
 * | PVK_FORMAT_CT32 | 32 | 256 KB | Full RGB plus an 8-bit alpha. Use when you need smooth alpha gradients or exact color, such as a lighting ramp or a UI element with soft edges. |
 * | PVK_FORMAT_CT24 | 24 | 192 KB | Same RGB, no alpha; the GS reads it as opaque. Use for opaque full-color images that must not be quantized: photographs, pre-rendered backdrops, video frames and skyboxes. |
 * | PVK_FORMAT_CT16 | 16 | 128 KB | 5 bits per channel and a 1-bit alpha. Use for full-color textures with cut-out transparency: foliage, fences, grates, hair cards and particle sprites with hard edges. |
 * | PVK_FORMAT_T8 | 8 | 64 KB + 1 KB table | Each texel indexes a 256-entry table, and alpha comes from the table: a CT32 table gives every entry an 8-bit alpha, a CT16 table a 1-bit one, so T8 with a CT32 table does soft gradients at a quarter of CT32's size. Use for character skins, environment and terrain textures, props and anything painted: the bulk of a scene's textures. Swapping tables recolors a texture for free. |
 * | PVK_FORMAT_T4 | 4 | 32 KB + 64 B table | 16 colors, each with the alpha of its table entry (8-bit from a CT32 table, 1-bit from CT16). For sprites, fonts, decals, UI, and anything flat-shaded. Eight times smaller than CT32, so a whole sprite sheet fits where one CT32 texture would. |
 *
 * Color tables are separate images so several textures share one, and so the
 * same texture can be drawn with different palettes, which is how games did
 * team colors, damage states and day/night tints with no extra texture memory.
 *
 * The practical rule: start with T8, drop to T4 for flat art, go up to CT16
 * only when a palette visibly fails, and reserve CT32 for the few textures
 * where alpha gradients or exact values matter.
 *
 * A texture created with PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT can also be
 * rendered into: pvkCmdBeginRendering() points GS FRAME at its VRAM instead
 * of a swapchain image's, and a later draw, after a pvkCmdPipelineBarrier(),
 * samples the same VRAM through TEX0, which is how the PS2 did mirrors, in-game screens and post effects.
 * Such an image gets page-aligned VRAM (FRAME addresses in 8 KB pages, where
 * a plain texture only needs 256-byte blocks) and must be at least 64 wide,
 * the unit of FRAME.FBW. Everything that reads or renders an image goes
 * through a PvkImageView; a view picks the mip levels used.
 * @{
 */

/** @brief An image in GS VRAM: a swapchain framebuffer, a depth buffer, a texture or a render target. */
typedef struct PvkImage_T         *PvkImage;
/** @brief A way of using an image: the levels a texture is sampled through, or the one level rendered into. */
typedef struct PvkImageView_T     *PvkImageView;

/** @brief What an image is used for. */
typedef enum PvkImageUsageFlagBits {
    PVK_IMAGE_USAGE_TRANSFER_DST_BIT             = 0x02, /**< Filled by pvkCmdCopyBufferToImage(): a texture or color table. */
    PVK_IMAGE_USAGE_SAMPLED_BIT                  = 0x04, /**< Read through a combined image sampler binding: GS TEX0. */
    PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT         = 0x10, /**< Rendered into as the color attachment: GS FRAME. Page-aligned VRAM, PVK_FORMAT_CT32/CT24/CT16, at least 64 wide. */
    PVK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = 0x20  /**< Rendered into as the depth attachment: GS ZBUF. */
} PvkImageUsageFlagBits;

/**
 * @brief Which queue families may use a buffer or image at once.
 *
 * GS VRAM and EE RAM are read the same way from both queues, so the mode
 * changes no bytes; with EXCLUSIVE, passing a resource from one queue to the
 * other is written as a pair of pvkCmdPipelineBarrier() ownership transfers.
 */
typedef enum PvkSharingMode {
    PVK_SHARING_MODE_EXCLUSIVE  = 0, /**< One queue family at a time; a barrier naming both families hands it over. */
    PVK_SHARING_MODE_CONCURRENT = 1  /**< The families in queueFamilyIndices, with no handover. */
} PvkSharingMode;

/** @brief Parameters for pvkCreateImage(). */
typedef struct PvkImageCreateInfo {
    uint32_t  width;     /**< Width in pixels; a depth buffer must be as wide as the color attachment it is used with, a texture a power of two from 8 to 1024 (64 or more when it is also a color attachment), a color table 16 or 256 (its entries). */
    uint32_t  height;    /**< Height in pixels; a texture a power of two from 8 to 1024, a color table 1. */
    uint32_t  mipLevels; /**< Texture only: number of mip levels, 0 or 1 for the base level alone, up to PVK_MAX_MIP_LEVELS. Each level halves the one before and must stay at least 8 on each side. */
    PvkFormat format;    /**< PVK_FORMAT_CT32, PVK_FORMAT_CT24, PVK_FORMAT_CT16, PVK_FORMAT_T8 or PVK_FORMAT_T4 for a texture; PVK_FORMAT_CT32 or PVK_FORMAT_CT16 for a color table; PVK_FORMAT_Z32, PVK_FORMAT_Z24 or PVK_FORMAT_Z16 for a depth buffer. */
    uint32_t  usage;     /**< PvkImageUsageFlagBits ORed together, or 0 for the format's usual use: TRANSFER_DST and SAMPLED for a texture, TRANSFER_DST for a color table, DEPTH_STENCIL_ATTACHMENT for a depth format. */
    PvkSharingMode  sharingMode;           /**< PVK_SHARING_MODE_EXCLUSIVE or PVK_SHARING_MODE_CONCURRENT. */
    uint32_t        queueFamilyIndexCount; /**< CONCURRENT only: number of entries in queueFamilyIndices, at least 2. */
    const uint32_t *queueFamilyIndices;    /**< CONCURRENT only: the families, each once. */
} PvkImageCreateInfo;

/**
 * @brief Creates an image: a texture, color table, render target or depth buffer, to be placed in GS VRAM.
 *
 * A color format image at least 8 texels on each side is a texture: fill
 * each of its mip levels with pvkCmdCopyBufferToImage() and sample it
 * through a descriptor set (see @ref descriptors). Every level gets its own
 * VRAM, addressed by GS TEX0 and MIPTBP1/MIPTBP2 when the texture is bound.
 * With PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT in usage each level is page
 * aligned so it can be rendered into as well (see @ref images).
 * A PVK_FORMAT_CT32 or PVK_FORMAT_CT16 image 16 or 256 wide and 1 high is
 * a color table: the palette a PVK_FORMAT_T4 or PVK_FORMAT_T8 texture's
 * indices pick colors from (GS TEX0.CBP/CPSM, loaded in CSM1 layout). A table
 * is paired with a texture in its PvkDescriptorImageInfo, so one table can
 * serve several textures and a texture can switch tables; a 256-entry table
 * also serves a PVK_FORMAT_T4 texture, which reads its first 16 entries.
 * Depth formats make depth buffers. Framebuffers come from the swapchain.
 * The image has no VRAM until pvkBindImageMemory() places it in a type 1
 * allocation; views of it and sets that hold it come after that.
 *
 * @param device The device.
 * @param info Image parameters.
 * @param[out] image The new image.
 * @return PVK_SUCCESS, PVK_ERROR_FORMAT_NOT_SUPPORTED, PVK_ERROR_VALIDATION_FAILED
 *         for a usage the format can't have, PVK_ERROR_OUT_OF_DEVICE_MEMORY or
 *         PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateImage(PvkDevice device, const PvkImageCreateInfo *info, PvkImage *image);

/**
 * @brief Destroys an image created with pvkCreateImage(). Its VRAM stays with the allocation it was bound to.
 * @param device The device.
 * @param image Image to destroy.
 */
void      pvkDestroyImage(PvkDevice device, PvkImage image);

/**
 * @brief Reports the VRAM an image needs.
 *
 * Each mip level is its own GS texture, so size covers every level laid out
 * in order, each at the image's alignment: 8 KB pages for a depth buffer or
 * color attachment (GS FRAME and ZBUF address pages), 256-byte blocks for a
 * texture or color table (GS TEX0 and CBP address blocks). A texture takes
 * every block its rows touch in the GS's swizzled layout. memoryTypeBits is
 * type 1.
 *
 * @param device The device.
 * @param image The image.
 * @param[out] requirements Filled in.
 */
void      pvkGetImageMemoryRequirements(PvkDevice device, PvkImage image, PvkMemoryRequirements *requirements);

/**
 * @brief Places an image in a type 1 (GS VRAM) allocation.
 *
 * Level 0 starts at offset; the other levels follow at the alignment
 * pvkGetImageMemoryRequirements() reports. An image is bound once.
 *
 * @param device The device.
 * @param image The image, not yet bound.
 * @param memory A type 1 allocation.
 * @param offset Byte offset in the allocation, a multiple of the image's alignment.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED when the image is bound
 *         already, the memory is not VRAM, the offset is misaligned or the image
 *         does not fit.
 */
PvkResult pvkBindImageMemory(PvkDevice device, PvkImage image, PvkDeviceMemory memory, size_t offset);

/** @brief Every level from baseMipLevel on, as the levelCount of a PvkImageViewCreateInfo. */
#define PVK_REMAINING_MIP_LEVELS (~0u)
/** @brief Every array layer from baseArrayLayer on. */
#define PVK_REMAINING_ARRAY_LAYERS (~0u)

/** @brief How an image is being used; named in barriers. The GS reads VRAM the same way in every layout, so a change moves no bytes. */
typedef enum PvkImageLayout {
    PVK_IMAGE_LAYOUT_UNDEFINED                        = 0, /**< Contents unknown: a new image, or one whose contents may be discarded. */
    PVK_IMAGE_LAYOUT_GENERAL                          = 1, /**< Any use. */
    PVK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL         = 2, /**< Rendered into through GS FRAME. */
    PVK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3, /**< Rendered into through GS ZBUF. */
    PVK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL         = 5, /**< Sampled through GS TEX0. */
    PVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL             = 7, /**< Written by pvkCmdCopyBufferToImage(). */
    PVK_IMAGE_LAYOUT_PRESENT_SRC                      = 1000001002 /**< Shown through GS DISPFB. */
} PvkImageLayout;

/** @brief Which part of an image's pixels. */
typedef enum PvkImageAspectFlagBits {
    PVK_IMAGE_ASPECT_COLOR_BIT = 0x1, /**< The color of a CT32/CT24/CT16/T8/T4 image. */
    PVK_IMAGE_ASPECT_DEPTH_BIT = 0x2  /**< The depth of a Z32/Z24/Z16 image. */
} PvkImageAspectFlagBits;
/** @brief Bitmask of PvkImageAspectFlagBits. */
typedef uint32_t PvkImageAspectFlags;

/** @brief A range of an image's mip levels. */
typedef struct PvkImageSubresourceRange {
    PvkImageAspectFlags aspectMask;     /**< PVK_IMAGE_ASPECT_COLOR_BIT for a color image, PVK_IMAGE_ASPECT_DEPTH_BIT for a depth image. */
    uint32_t            baseMipLevel;   /**< First level. */
    uint32_t            levelCount;     /**< Levels from baseMipLevel on, or PVK_REMAINING_MIP_LEVELS. */
    uint32_t            baseArrayLayer; /**< 0: an image has one layer. */
    uint32_t            layerCount;     /**< 1, or PVK_REMAINING_ARRAY_LAYERS. */
} PvkImageSubresourceRange;

/** @brief Parameters for pvkCreateImageView(). */
typedef struct PvkImageViewCreateInfo {
    PvkImage  image;        /**< The image viewed. */
    PvkFormat format;       /**< The image's format, or PVK_FORMAT_UNDEFINED for it; the GS reads VRAM in the format it was written in. */
    uint32_t  baseMipLevel; /**< First mip level of the view; level 0 is the image's full size. */
    uint32_t  levelCount;   /**< Levels from baseMipLevel on, or PVK_REMAINING_MIP_LEVELS; 1 for a view rendered into. */
} PvkImageViewCreateInfo;

/**
 * @brief Creates a view of an image: the mip levels a binding samples or the level rendered into.
 *
 * A view of a texture goes into a PvkDescriptorImageInfo: TEX0 then points at
 * its base level (a smaller TW/TH when it is not level 0) and MIPTBP1/2 at the
 * levels after it, so one image can be sampled at full size by one set and
 * from a lower level by another. A view of one level of a color attachment
 * image, a swapchain image or a depth image goes into a
 * PvkRenderingAttachmentInfo. Views cost nothing on the PS2: they are a
 * record of the image and level range, read when a set is written or
 * rendering begins.
 *
 * @param device The device.
 * @param info The image and its levels.
 * @param[out] view The new view.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED for levels the image lacks or
 *         a format other than the image's, or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateImageView(PvkDevice device, const PvkImageViewCreateInfo *info, PvkImageView *view);

/**
 * @brief Destroys an image view. Sets that hold it must not be drawn with afterwards.
 * @param device The device.
 * @param view View to destroy.
 */
void      pvkDestroyImageView(PvkDevice device, PvkImageView view);

/** @} */

/* ======================================================================== */
/**
 * @defgroup samplers Samplers
 * @brief How a texture is read: filtering between texels and wrapping past its edges.
 *
 * A sampler goes into a combined image sampler binding next to the image
 * (PvkDescriptorImageInfo). On the PS2 its filters become GS TEX1.MMAG/MMIN
 * and its address modes CLAMP.WMS/WMT, written with the draw state. The GS
 * has no mirrored repeat, border color, anisotropy or depth comparison;
 * only repeat and clamp to edge exist.
 *
 * Mip level selection is TEX1.MXL/MMIN/K. The GS has no screen-space
 * derivatives: it computes LOD = log2(w) + mipLodBias from the vertex's
 * clip w (the Q it stores per vertex is 1/w), then clamps it to 0..MXL. So
 * mipLodBias carries the scale: log2 of the texels one pixel covers at
 * w = 1, which is log2(texture texels / (world size × focal length in
 * pixels)) for a perspective projection. The same LOD picks magFilter when it
 * is below 0 and minFilter otherwise. minLod must be 0 (the GS cannot clamp
 * from below); maxLod lowers MXL below the image's last level.
 *
 * With unnormalizedCoordinates the vertex shader's vec2 output is in texels
 * rather than 0 to 1, for sprite sheets and text. Draws with such a sampler
 * set GS PRIM.FST and store a UV register (12.4 fixed point) in place of
 * ST and Q: VU1 skips the divide by w, so the texture is not perspective
 * correct. The sampler must then clamp to edge on both axes,
 * use one filter for both magFilter and minFilter, nearest mipmapping and
 * maxLod 0.
 * @{
 */

/** @brief Rules for reading a texture, created with pvkCreateSampler(). */
typedef struct PvkSampler_T *PvkSampler;

/** @brief Texel filtering. */
typedef enum PvkFilter {
    PVK_FILTER_NEAREST = 0, /**< The nearest texel. GS MMAG/MMIN = NEAREST. */
    PVK_FILTER_LINEAR  = 1  /**< The 4 surrounding texels blended (bilinear). GS MMAG/MMIN = LINEAR. */
} PvkFilter;

/** @brief Mipmap selection. */
typedef enum PvkSamplerMipmapMode {
    PVK_SAMPLER_MIPMAP_MODE_NEAREST = 0, /**< Pick one level. GS MMIN = *_MIPMAP_NEAREST. */
    PVK_SAMPLER_MIPMAP_MODE_LINEAR  = 1  /**< Blend two levels (trilinear with PVK_FILTER_LINEAR). GS MMIN = *_MIPMAP_LINEAR. */
} PvkSamplerMipmapMode;

/** @brief A maxLod that never clamps: every level of the image is used. */
#define PVK_LOD_CLAMP_NONE 1000.0f

/** @brief What a coordinate outside 0 to 1 reads. */
typedef enum PvkSamplerAddressMode {
    PVK_SAMPLER_ADDRESS_MODE_REPEAT               = 0, /**< Tile the texture. GS WMS/WMT = REPEAT. */
    PVK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT      = 1, /**< Not supported. */
    PVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE        = 2, /**< Stretch the edge texel outward. GS WMS/WMT = CLAMP. */
    PVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER      = 3, /**< Not supported. */
    PVK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE = 4  /**< Not supported. */
} PvkSamplerAddressMode;

/** @brief Parameters for pvkCreateSampler(). All zeros is nearest filtering, repeat, the base level only. */
typedef struct PvkSamplerCreateInfo {
    PvkFilter             magFilter;    /**< Filter when a texel covers more than a pixel (zoomed in). GS TEX1.MMAG. */
    PvkFilter             minFilter;    /**< Filter when a pixel covers more than a texel (zoomed out). GS TEX1.MMIN. */
    PvkSamplerMipmapMode  mipmapMode;   /**< How levels are picked when the image has more than one and maxLod allows it. GS TEX1.MMIN. */
    PvkSamplerAddressMode addressModeU; /**< Wrapping of the s coordinate. GS CLAMP.WMS. */
    PvkSamplerAddressMode addressModeV; /**< Wrapping of the t coordinate. GS CLAMP.WMT. */
    float                 mipLodBias;   /**< Added to log2(w) to get the LOD (see @ref samplers); -128 to 127.9375, rounded to 1/16. GS TEX1.K. */
    float                 minLod;       /**< Must be 0: the GS has no lower LOD clamp. */
    float                 maxLod;       /**< Highest level used: 0 is the base level alone, PVK_LOD_CLAMP_NONE every level. The whole part becomes GS TEX1.MXL, capped at the image's last level. */
    uint32_t              unnormalizedCoordinates; /**< Nonzero: texture coordinates are in texels, not 0 to 1 (see @ref samplers). GS PRIM.FST with UV. */
} PvkSamplerCreateInfo;

/**
 * @brief Creates a sampler.
 * @param device The device.
 * @param info Filters and address modes.
 * @param[out] sampler The new sampler.
 * @return PVK_SUCCESS, PVK_ERROR_FEATURE_NOT_PRESENT for an address mode the GS
 *         lacks or a minLod other than 0, PVK_ERROR_VALIDATION_FAILED for a
 *         value outside the enums, a mipLodBias outside TEX1.K's range or
 *         unnormalizedCoordinates with filters, mipmapping, maxLod or address
 *         modes not allowed with it, or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateSampler(PvkDevice device, const PvkSamplerCreateInfo *info, PvkSampler *sampler);

/**
 * @brief Destroys a sampler. Descriptor sets that hold it must not be drawn with afterwards.
 * @param device The device.
 * @param sampler Sampler to destroy.
 */
void      pvkDestroySampler(PvkDevice device, PvkSampler sampler);

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
 * @brief Picks the image to render into next and arranges a signal for when it is free.
 *
 * The images are taken in turn, so the index is known at once and the call
 * never sleeps. The image is free when DISPFB no longer points at it and no
 * queued present will point it there; the semaphore and fence are signaled
 * at that moment by the INTC_VBLANK_S handler that performs the flip, or
 * before the call returns if the image is free already. One acquire can be
 * outstanding per image.
 *
 * @param device The device.
 * @param swapchain The swapchain.
 * @param timeout Nanoseconds to wait for an index. An index is always known
 *                at once or never until a present, so the call never sleeps;
 *                the value only picks the result for the latter case.
 * @param semaphore Semaphore to signal when the image is free, or NULL.
 * @param fence Unsignaled fence not in use, to signal when the image is free, or NULL.
 * @param[out] imageIndex Index into the images from pvkGetSwapchainImages().
 * @return PVK_SUCCESS; PVK_NOT_READY (timeout 0) or PVK_TIMEOUT (any other
 *         timeout) if the image is on screen and no present is queued to
 *         replace it, so nothing would ever signal; or
 *         PVK_ERROR_VALIDATION_FAILED if semaphore and fence are both NULL,
 *         the semaphore is signaled or has a signal pending, the fence is
 *         signaled or in use, or the image is acquired already.
 */
PvkResult pvkAcquireNextImage(PvkDevice device, PvkSwapchain swapchain, uint64_t timeout, PvkSemaphore semaphore,
                              PvkFence fence, uint32_t *imageIndex);

/** @brief Parameters for pvkQueuePresent(). */
typedef struct PvkPresentInfo {
    uint32_t            waitSemaphoreCount; /**< Number of entries in waitSemaphores, at most 4. */
    const PvkSemaphore *waitSemaphores;     /**< Semaphores that must be signaled before the flip; each is reset by the wait. */
    uint32_t            swapchainCount;     /**< Number of entries in swapchains, imageIndices and results: 1, the device has one swapchain. */
    const PvkSwapchain *swapchains;         /**< The device's swapchain. */
    const uint32_t     *imageIndices;       /**< Image to show. */
    PvkResult          *results;            /**< Written with the result for each swapchain, or NULL. */
} PvkPresentInfo;

/**
 * @brief Queues an image to be shown on the TV.
 *
 * Returns at once. At the first vertical blank (even field in an interlaced
 * mode) at which its wait semaphores are all signaled, the INTC_VBLANK_S
 * handler points DISPFB at the image; the image that leaves the screen is
 * then free for the acquire waiting on it. Up to 2 presents can be queued,
 * and the call sleeps while there are that many.
 *
 * @param queue The graphics queue (family 0); the transfer queue does not present.
 * @param info What to show and what to wait for.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED if the queue is not
 *         the graphics queue, swapchainCount is
 *         not 1, the swapchain is not the device's, the image index is out
 *         of range, there are more than 4 wait semaphores, or a wait
 *         semaphore has no signal pending.
 */
PvkResult pvkQueuePresent(PvkQueue queue, const PvkPresentInfo *info);

/** @} */

/* ======================================================================== */
/**
 * @defgroup buffers Buffers
 * @brief Ranges of EE main RAM that the graphics hardware reads, such as vertex data.
 * @{
 */

/** @brief A range of EE main RAM that DMA reads directly, e.g. vertex data; bound to type 0 memory. */
typedef struct PvkBuffer_T        *PvkBuffer;

/** @brief Parameters for pvkCreateBuffer(). */
typedef struct PvkBufferCreateInfo {
    size_t          size;                  /**< Size in bytes; rounded up to a multiple of 16. */
    PvkSharingMode  sharingMode;           /**< PVK_SHARING_MODE_EXCLUSIVE or PVK_SHARING_MODE_CONCURRENT. */
    uint32_t        queueFamilyIndexCount; /**< CONCURRENT only: number of entries in queueFamilyIndices, at least 2. */
    const uint32_t *queueFamilyIndices;    /**< CONCURRENT only: the families, each once. */
} PvkBufferCreateInfo;

/**
 * @brief Creates a buffer. It has no memory until pvkBindBufferMemory() places it in a type 0 allocation.
 * @param device The device.
 * @param info Buffer parameters.
 * @param[out] buffer The new buffer.
 * @return PVK_SUCCESS or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateBuffer(PvkDevice device, const PvkBufferCreateInfo *info, PvkBuffer *buffer);

/**
 * @brief Destroys a buffer. Its memory stays with the allocation it was bound to.
 * @param device The device.
 * @param buffer Buffer to destroy.
 */
void      pvkDestroyBuffer(PvkDevice device, PvkBuffer buffer);

/**
 * @brief Reports the memory a buffer needs.
 *
 * size is the buffer's size rounded up to 16 plus 48 bytes: a vertex binding
 * is read in whole 16-byte units, up to 48 bytes past its last vertex.
 * alignment is 64 (a cache line; the DMA reads qwords), memoryTypeBits is type 0.
 *
 * @param device The device.
 * @param buffer The buffer.
 * @param[out] requirements Filled in.
 */
void      pvkGetBufferMemoryRequirements(PvkDevice device, PvkBuffer buffer, PvkMemoryRequirements *requirements);

/**
 * @brief Places a buffer in a type 0 (EE RAM) allocation. A buffer is bound once.
 * @param device The device.
 * @param buffer The buffer, not yet bound.
 * @param memory A type 0 allocation.
 * @param offset Byte offset in the allocation, a multiple of 64.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED when the buffer is bound
 *         already, the memory is VRAM, the offset is misaligned or the buffer
 *         does not fit.
 */
PvkResult pvkBindBufferMemory(PvkDevice device, PvkBuffer buffer, PvkDeviceMemory memory, size_t offset);

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
/** @brief Holds the VU1 programs of the pipelines created through it, so pipelines with the same program share one copy (see PvkPipelineCacheCreateInfo). */
typedef struct PvkPipelineCache_T *PvkPipelineCache;

/** @brief Shader stages. The values are bits, so they can be ORed into a stage mask. */
typedef enum PvkShaderStage {
    PVK_SHADER_STAGE_VERTEX   = 1, /**< Runs on VU1. */
    PVK_SHADER_STAGE_FRAGMENT = 2  /**< Becomes GS settings. */
} PvkShaderStage;

/**
 * @brief How vertices are grouped into primitives.
 *
 * Each value is a GS primitive type, so the GS assembles the shapes itself.
 * A draw is split into VU1 batches; a strip or fan batch repeats the vertices
 * the next primitive shares with the previous batch (2 for a triangle strip,
 * the hub and the last vertex for a fan, 1 for a line strip), so the join is
 * seamless. Lists are cut to whole primitives, strips and fans draw nothing
 * with fewer vertices than one primitive. No primitive restart index.
 */
typedef enum PvkPrimitiveTopology {
    PVK_PRIMITIVE_TOPOLOGY_POINT_LIST     = 0, /**< Every vertex is a point (GS PRIM point). */
    PVK_PRIMITIVE_TOPOLOGY_LINE_LIST      = 1, /**< Every 2 vertices form a line. */
    PVK_PRIMITIVE_TOPOLOGY_LINE_STRIP     = 2, /**< Each vertex after the first makes a line with the previous one. */
    PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST  = 3, /**< Every 3 vertices form a triangle. */
    PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 4, /**< Each vertex after the second makes a triangle with the previous two. */
    PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN   = 5, /**< Each vertex after the second makes a triangle with the previous one and the first. */
    PVK_PRIMITIVE_TOPOLOGY_SPRITE_LIST    = 6  /**< PS2 only: every 2 vertices are opposite corners of an axis-aligned rectangle (GS PRIM sprite). The rectangle takes the second vertex's color and depth; texture coordinates interpolate between the corners. */
} PvkPrimitiveTopology;

/** @brief Most vertex inputs a vertex shader can have. */
#define PVK_MAX_VERTEX_INPUTS 8

/** @brief Most vertex buffer bindings a pipeline can declare and a command buffer can bind. */
#define PVK_MAX_VERTEX_BINDINGS 8

/** @brief Size of the push constant range, in bytes. */
#define PVK_MAX_PUSH_CONSTANT_BYTES 128

/** @brief Most descriptor sets a pipeline layout can have and a command buffer can bind. */
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
 *
 * A block, push constant or uniform, holds float, vec2, vec3, vec4, int and
 * matrix members and arrays of vectors or matrices, column-major. An array
 * element or matrix column may be picked at run time by an int member of a
 * block, gl_InstanceIndex or int() of a float, one such index per read. VU1
 * forms the address in an integer register, so the elements must be whole
 * 16-byte qwords (vec4, matrices, or any std140 array) and a vector's
 * component cannot be picked. The index is clamped to the array only when
 * the device has PvkPhysicalDeviceFeatures::robustBufferAccess.
 */
typedef struct PvkShaderUniformBlock {
    uint32_t set;     /**< Descriptor set index of the block (0 when the GLSL names none). */
    uint32_t binding; /**< Its binding number. */
    uint32_t qword;   /**< First qword of the block in the uniform area. */
    uint32_t bytes;   /**< Size of the block, std140. */
} PvkShaderUniformBlock;

/** @brief What a specialization constant of a vertex shader holds. VU1 computes with floats, so the pipeline converts the value to one. */
typedef enum PvkSpecConstantType {
    PVK_SPEC_CONSTANT_TYPE_FLOAT = 0, /**< GLSL float: the 4 bytes are the float. */
    PVK_SPEC_CONSTANT_TYPE_INT   = 1, /**< GLSL int: a signed 32-bit integer. */
    PVK_SPEC_CONSTANT_TYPE_UINT  = 2, /**< GLSL uint: an unsigned 32-bit integer. */
    PVK_SPEC_CONSTANT_TYPE_BOOL  = 3  /**< GLSL bool: 32 bits, 0 = false, anything else = true. */
} PvkSpecConstantType;

/**
 * @brief A specialization constant of a vertex shader (layout(constant_id = n) in the GLSL) and where it sits in the VU1 code.
 *
 * shaderc loads the constant with an LOI instruction whose 32-bit immediate is
 * the default from the GLSL. The microcode in the ELF keeps that default;
 * pvkCreateGraphicsPipeline() writes the value the stage's
 * PvkSpecializationInfo gives it over the immediate in the pipeline's own copy
 * of the code, as the float VU1 computes with. A vector constant has one entry
 * per specialized component. Only vertex shaders have specialization
 * constants: the fragment stage becomes GS settings, with no code to rewrite,
 * so shaderc rejects one there.
 */
typedef struct PvkShaderSpecConstant {
    uint32_t            constantID; /**< The constant_id. */
    const uint32_t     *vu1Word;    /**< The LOI immediate holding its default, in the microcode. */
    PvkSpecConstantType type;       /**< How to read the 4 bytes the application gives. */
} PvkShaderSpecConstant;

/**
 * @brief A compiled shader, as generated by tools/shaderc.
 *
 * shaderc simplifies the SPIR-V with spirv-opt before compiling it. Calls to the
 * shader's own functions and its local variables disappear into plain code
 * working on VU1 registers, and a loop that always runs the same number of
 * times is written out as repeated code instead of a loop. if/else, ?: and
 * loops whose number of times depends on the data become VU1 branches. A
 * specialization constant keeps its default in the code (see
 * PvkShaderSpecConstant); a branch or loop count that depends on one stays a
 * branch, since its value is only known when the pipeline is created.
 *
 * Applications never fill this in by hand: shaderc emits a .c file defining
 * one of these, and the application passes its address to
 * pvkCreateShaderModule().
 */
typedef struct PvkShaderCode {
    PvkShaderStage stage;                  /**< Vertex or fragment. */
    const char    *name;                   /**< Source file name, for error messages. */
    const char    *entryPoint;             /**< Name of its entry point, as the GLSL declares it (main). */

    const uint32_t *vu1CodeStart;          /**< Vertex: start of the VU1 microcode, linked into the ELF. */
    const uint32_t *vu1CodeEnd;            /**< Vertex: end of the VU1 microcode. */
    const uint32_t *vu1Convert;            /**< Vertex: the input conversion block in the microcode, preceded by one load per input: a branch, its delay slot, then 4 instructions per input (itof0 with loi scale, muli with loi -1, maxi, mul by VF00). pvkCreateGraphicsPipeline() rewrites the block in its copy of the code for each attribute's format and points each load at the input's qword of the vertex or the instance. */
    uint32_t        vu1ClampCount;         /**< Vertex: number of entries in vu1Clamps. */
    const uint32_t *const *vu1Clamps;      /**< Vertex: the run-time index clamps in the microcode, 3 instruction pairs each, NOP'd in the pipeline's copy unless the device has robustBufferAccess. */
    uint32_t        vu1SpecConstantCount;  /**< Vertex: number of entries in vu1SpecConstants. */
    const PvkShaderSpecConstant *vu1SpecConstants; /**< Vertex: its specialization constants, each the LOI immediate in the microcode that holds its default; pvkCreateGraphicsPipeline() overwrites the immediate in its copy of the code with the value the stage's PvkSpecializationInfo gives it. */
    uint32_t        inputCount;            /**< Vertex: number of inputs, each one vec4. */
    uint32_t        inputLocations[PVK_MAX_VERTEX_INPUTS]; /**< Vertex: input locations, in the order VU1 reads them. */
    int32_t         colorOutputLocation;   /**< Vertex: location of the color output, or -1 if none. */
    int32_t         texcoordOutputLocation;/**< Vertex: location of the vec2 texture coordinate output, or -1 if none. When set, VU1 emits ST (or UV for a sampler with unnormalizedCoordinates), RGBAQ, XYZ2 per vertex instead of RGBAQ, XYZ2. */
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

/** @brief Part of the push constant range that one or more shader stages read. */
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

/** @brief How often a vertex buffer binding advances. */
typedef enum PvkVertexInputRate {
    PVK_VERTEX_INPUT_RATE_VERTEX   = 0, /**< One element per vertex. */
    PVK_VERTEX_INPUT_RATE_INSTANCE = 1  /**< One element per instance of the draw: every vertex of an instance reads the same values. */
} PvkVertexInputRate;

/**
 * @brief A vertex buffer binding: one stream of vertices.
 *
 * The VIF expands each binding into VU1 memory with one UNPACK format, so a
 * binding is a tightly packed array in one format: its attributes share the
 * format, feed consecutive shader inputs and lie one after another in the
 * vertex, and the stride is their total size. Attributes of float vec4s may
 * share a binding; an attribute of any other format is a binding of its own
 * (its stride is the format's size: 16-bit RGB normals are 6 bytes, 8-bit RGBA
 * colors 4). So a mesh with mixed formats keeps one array per format, bound
 * with pvkCmdBindVertexBuffers().
 *
 * A binding with PVK_VERTEX_INPUT_RATE_INSTANCE holds one element per
 * instance of a draw instead of per vertex: the draw's instanceCount and
 * firstInstance pick which, and VU1 reads the element once per instance while
 * it runs the vertices again. gl_InstanceIndex is firstInstance plus the
 * instance's number; VU1 keeps it as a float, so a shader reads
 * it only as float(gl_InstanceIndex).
 */
typedef struct PvkVertexInputBinding {
    uint32_t           binding;   /**< Binding number, below PVK_MAX_VERTEX_BINDINGS. */
    uint32_t           stride;    /**< Bytes per element in this binding: the attributes' format size times their count. */
    PvkVertexInputRate inputRate; /**< Per vertex or per instance. */
} PvkVertexInputBinding;

/** @brief One vertex attribute. */
typedef struct PvkVertexInputAttribute {
    uint32_t  location; /**< Shader input location it feeds. */
    uint32_t  binding;  /**< The PvkVertexInputBinding it is read from. */
    PvkFormat format;   /**< A vertex attribute format (PVK_FORMAT_R32G32B32A32_SFLOAT to PVK_FORMAT_R8_SSCALED). */
    uint32_t  offset;   /**< Bytes from the start of the vertex in its binding: the n-th attribute of a binding, in shader input order, is at n times the format's size. */
} PvkVertexInputAttribute;

/** @brief A rectangle in pixels. */
typedef struct PvkRect2D {
    int32_t  x;      /**< Left edge. */
    int32_t  y;      /**< Top edge. */
    uint32_t width;  /**< Width. */
    uint32_t height; /**< Height. */
} PvkRect2D;

/** @brief Maps clip space to framebuffer pixels (y points down). */
typedef struct PvkViewport {
    float x;        /**< Left edge in pixels. */
    float y;        /**< Top edge in pixels. */
    float width;    /**< Width in pixels. */
    float height;   /**< Height in pixels. */
    float minDepth; /**< Depth for clip-space z = 0, usually 0. */
    float maxDepth; /**< Depth for clip-space z = 1, usually 1. */
} PvkViewport;

/**
 * @brief Depth comparisons.
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
 * @brief Depth test and write control.
 *
 * All zeros means no depth test and no depth writes, which is what a pipeline
 * without a depth attachment needs. There is no stencil buffer on the GS.
 */
typedef struct PvkPipelineDepthStencilStateCreateInfo {
    uint32_t     depthTestEnable;  /**< Nonzero to compare against the depth attachment. GS TEST.ZTE/ZTST. */
    uint32_t     depthWriteEnable; /**< Nonzero to write passing depths; ignored when the test is off. GS ZBUF.ZMSK. */
    PvkCompareOp depthCompareOp;   /**< Comparison when the test is on. */
} PvkPipelineDepthStencilStateCreateInfo;

/** @brief Which triangles a pipeline discards. */
typedef enum PvkCullMode {
    PVK_CULL_MODE_NONE           = 0, /**< Draw every triangle. */
    PVK_CULL_MODE_FRONT_BIT      = 1, /**< Discard front-facing triangles. */
    PVK_CULL_MODE_BACK_BIT       = 2, /**< Discard back-facing triangles. */
    PVK_CULL_MODE_FRONT_AND_BACK = 3  /**< Discard all triangles: the draw is skipped. */
} PvkCullMode;

/** @brief Which winding is front-facing, in framebuffer space (y down). */
typedef enum PvkFrontFace {
    PVK_FRONT_FACE_COUNTER_CLOCKWISE = 0, /**< Positive signed area is front-facing. */
    PVK_FRONT_FACE_CLOCKWISE         = 1  /**< Negative signed area is front-facing. */
} PvkFrontFace;

/**
 * @brief Culling.
 *
 * All zeros means no culling and counter-clockwise front faces. Only triangle
 * topologies cull; points, lines and sprites ignore it. The GS cannot cull, so
 * the vertex shader's epilogue computes each triangle's signed area in screen
 * space when its last vertex is transformed and, for a culled one, sets the
 * ADC bit of that vertex's XYZ2 so the GS does not draw it. A strip's
 * alternating winding and a fan's shared hub are followed per batch.
 */
typedef struct PvkPipelineRasterizationStateCreateInfo {
    PvkCullMode  cullMode;  /**< Triangles to discard. */
    PvkFrontFace frontFace; /**< Winding of a front-facing triangle. */
} PvkPipelineRasterizationStateCreateInfo;

/**
 * @brief Blend factors.
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

/** @brief Blend operations. */
typedef enum PvkBlendOp {
    PVK_BLEND_OP_ADD              = 0, /**< src * srcFactor + dst * dstFactor. */
    PVK_BLEND_OP_SUBTRACT         = 1, /**< src * srcFactor - dst * dstFactor. */
    PVK_BLEND_OP_REVERSE_SUBTRACT = 2, /**< dst * dstFactor - src * srcFactor. */
    PVK_BLEND_OP_MIN              = 3, /**< Not supported. */
    PVK_BLEND_OP_MAX              = 4  /**< Not supported. */
} PvkBlendOp;

/**
 * @brief Blending of the color attachment.
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

/** @brief Blend state of a pipeline. */
typedef struct PvkPipelineColorBlendStateCreateInfo {
    PvkPipelineColorBlendAttachmentState attachment;        /**< Blending of the single color attachment. */
    float                                blendConstants[4]; /**< Only [3] is used, by the CONSTANT_ALPHA factors, as GS ALPHA.FIX (0..1, 1 = 128). Ignored when PVK_DYNAMIC_STATE_BLEND_CONSTANTS is dynamic. */
} PvkPipelineColorBlendStateCreateInfo;

/**
 * @brief Pipeline state that is set while recording instead of at creation.
 *
 * The three the GS can change between draws. A pipeline that lists one
 * ignores the value in its create info; the matching pvkCmdSet*() must have
 * been called in the command buffer before a draw with it.
 */
typedef enum PvkDynamicState {
    PVK_DYNAMIC_STATE_VIEWPORT        = 0, /**< PvkGraphicsPipelineCreateInfo::viewport is set by pvkCmdSetViewport(). */
    PVK_DYNAMIC_STATE_SCISSOR         = 1, /**< PvkGraphicsPipelineCreateInfo::scissor is set by pvkCmdSetScissor(). */
    PVK_DYNAMIC_STATE_BLEND_CONSTANTS = 4  /**< PvkPipelineColorBlendStateCreateInfo::blendConstants is set by pvkCmdSetBlendConstants(). */
} PvkDynamicState;

/** @brief Which state a pipeline leaves dynamic. All zeros means none. */
typedef struct PvkPipelineDynamicStateCreateInfo {
    uint32_t               dynamicStateCount; /**< Number of entries in dynamicStates. */
    const PvkDynamicState *dynamicStates;     /**< The states set while recording. */
} PvkPipelineDynamicStateCreateInfo;

/** @brief Where one specialization constant's value lies in PvkSpecializationInfo::data. */
typedef struct PvkSpecializationMapEntry {
    uint32_t constantID; /**< The constant_id of the specialization constant it sets. */
    uint32_t offset;     /**< Byte offset of the value in PvkSpecializationInfo::data. */
    size_t   size;       /**< Bytes of the value: 4, the size of every specialization constant type (see PvkSpecConstantType). */
} PvkSpecializationMapEntry;

/**
 * @brief Values for a shader's specialization constants, applied when the pipeline is created.
 *
 * Each map entry names a constant_id and where its value lies in data. A
 * constant the entries do not name keeps the default from the GLSL; an entry
 * naming a constant_id the shader does not have is ignored. Every entry must
 * lie within data, and an entry that names one of the shader's constants must
 * be 4 bytes. The value is read as the constant's GLSL type: float, int, uint,
 * or bool as 32 bits where 0 is false.
 */
typedef struct PvkSpecializationInfo {
    uint32_t                         mapEntryCount; /**< Number of entries in mapEntries. */
    const PvkSpecializationMapEntry *mapEntries;    /**< The entries, or NULL for none. */
    size_t                           dataSize;      /**< Bytes in data. */
    const void                      *data;          /**< The values the entries point into. */
} PvkSpecializationInfo;

/**
 * @brief One shader stage of a graphics pipeline.
 */
typedef struct PvkPipelineShaderStageCreateInfo {
    PvkShaderStage               stage;              /**< PVK_SHADER_STAGE_VERTEX or PVK_SHADER_STAGE_FRAGMENT; the module's stage. */
    PvkShaderModule              module;             /**< The shader for that stage. */
    const char                  *name;               /**< Entry point to run: the module's PvkShaderCode::entryPoint. */
    const PvkSpecializationInfo *specializationInfo; /**< Values for the shader's specialization constants, or NULL to keep every default. Only a vertex shader has any. */
} PvkPipelineShaderStageCreateInfo;

/**
 * @brief Parameters for pvkCreatePipelineCache().
 *
 * A pipeline's VU1 program is its vertex shader's code with the vertex input
 * formats and specialization constants patched in. Pipelines created through
 * one cache whose patched programs come out identical share one copy of the
 * program, and pvkCmdBindPipeline() skips the upload to VU1 program memory when
 * the program the command buffer last uploaded is that same copy. A program
 * stays in EE RAM for as long as the cache or a pipeline refers to it, so a
 * cache may be destroyed before the pipelines made with it. There is no
 * program data to carry between runs (every program is patched from code in
 * the ELF), so initialData is accepted and ignored, as Vulkan allows for
 * cache data an implementation does not recognize.
 */
typedef struct PvkPipelineCacheCreateInfo {
    uint32_t    flags;           /**< 0. */
    size_t      initialDataSize; /**< Bytes in initialData, or 0. */
    const void *initialData;     /**< Ignored. */
} PvkPipelineCacheCreateInfo;

/** @brief Parameters for pvkCreateGraphicsPipeline(). */
typedef struct PvkGraphicsPipelineCreateInfo {
    uint32_t                       stageCount;     /**< Number of entries in stages: 2. */
    const PvkPipelineShaderStageCreateInfo *stages; /**< One PVK_SHADER_STAGE_VERTEX and one PVK_SHADER_STAGE_FRAGMENT stage, in either order. */
    PvkPipelineLayout              layout;         /**< Layout whose vertex push constant ranges cover the vertex shader's block. */
    uint32_t                       bindingCount;   /**< Number of entries in bindings. */
    const PvkVertexInputBinding   *bindings;       /**< The vertex buffer bindings the attributes come from. */
    uint32_t                       attributeCount; /**< Number of entries in attributes. */
    const PvkVertexInputAttribute *attributes;     /**< One per vertex shader input. */
    PvkPrimitiveTopology           topology;       /**< How the vertices of a draw are grouped into primitives. */
    PvkViewport                    viewport;       /**< Viewport, unless PVK_DYNAMIC_STATE_VIEWPORT is dynamic. */
    PvkRect2D                      scissor;        /**< Pixels the draws may write, clipped to the attachment, unless PVK_DYNAMIC_STATE_SCISSOR is dynamic; a zero width or height means the whole attachment level, which a secondary command buffer recorded with PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT does not know, so its draws need a real rectangle. GS SCISSOR. */
    PvkFormat                      depthAttachmentFormat; /**< Format of the depth attachment the pipeline draws with, or PVK_FORMAT_UNDEFINED for none. Sets the depth range the viewport maps to. */
    PvkPipelineRasterizationStateCreateInfo rasterization; /**< Back-face culling. */
    PvkPipelineDepthStencilStateCreateInfo depthStencil; /**< Depth test and write control. */
    PvkPipelineColorBlendStateCreateInfo   colorBlend;   /**< Blending with the color attachment. */
    PvkPipelineDynamicStateCreateInfo      dynamicState; /**< State set while recording instead of here. */
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
 * Checks that stages hold one vertex and one fragment shader, that they
 * match each other, the vertex layout and the pipeline layout, and precomputes the viewport transform, GS primitive
 * settings, culling, depth test and blend equation. A depth test needs a
 * depthAttachmentFormat, the comparison must be one the GS can do (see
 * PvkCompareOp) and the blend factors must fit the GS equation (see
 * PvkPipelineColorBlendAttachmentState). The states listed in dynamicState
 * are read from the command buffer instead (see PvkDynamicState). Every
 * vertex shader input needs an attribute in a declared binding (see
 * PvkVertexInputBinding); the pipeline
 * keeps its own copy of the VU1 code with the integer-to-float conversion
 * each input's format needs patched in, so float inputs cost nothing, and
 * each input's load pointed at its binding's per-vertex or per-instance
 * stream. The vertex stage's specialization constants are written into that
 * copy too (see PvkSpecializationInfo). When cache is not NULL and already
 * holds a program identical to that copy, the pipeline shares the cached one
 * (see PvkPipelineCacheCreateInfo).
 *
 * @param device The device.
 * @param cache Cache the pipeline's program is looked up in and added to, or NULL.
 * @param info Pipeline parameters.
 * @param[out] pipeline The new pipeline.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED, PVK_ERROR_FORMAT_NOT_SUPPORTED,
 *         PVK_ERROR_FEATURE_NOT_PRESENT, PVK_ERROR_INVALID_SHADER or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateGraphicsPipeline(PvkDevice device, PvkPipelineCache cache, const PvkGraphicsPipelineCreateInfo *info,
                                    PvkPipeline *pipeline);

/**
 * @brief Creates a pipeline cache.
 * @param device The device.
 * @param info Cache parameters. May be NULL.
 * @param[out] cache The new cache, empty.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreatePipelineCache(PvkDevice device, const PvkPipelineCacheCreateInfo *info, PvkPipelineCache *cache);

/**
 * @brief Destroys a pipeline cache. Pipelines created through it stay valid.
 * @param device The device.
 * @param cache Cache to destroy.
 */
void      pvkDestroyPipelineCache(PvkDevice device, PvkPipelineCache cache);

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

/** @brief What a descriptor binding holds. */
typedef enum PvkDescriptorType {
    PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1, /**< An image view (and its color table) a sampler2D reads: GS TEX0. */
    PVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER         = 6  /**< Part of a PvkBuffer a vertex shader's uniform block reads: VIF UNPACK into VU1 memory. */
} PvkDescriptorType;

/** @brief Most bindings in one descriptor set layout. */
#define PVK_MAX_DESCRIPTOR_BINDINGS 8

/** @brief One binding of a descriptor set layout. */
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

/** @brief How many descriptors of one type a pool holds. */
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

/** @brief The image and sampler a combined image sampler binding reads. */
typedef struct PvkDescriptorImageInfo {
    PvkSampler   sampler;    /**< How the image is read (see @ref samplers). */
    PvkImageView imageView;  /**< A view of an image with PVK_IMAGE_USAGE_SAMPLED_BIT: a texture, a render target or a swapchain image. Its base level is what texture coordinates 0 to 1 cover. */
    PvkImage     colorTable; /**< For a PVK_FORMAT_T8 or PVK_FORMAT_T4 image, the color table its indices read (256 entries, or 16 for T4); NULL for other formats. */
} PvkDescriptorImageInfo;

/** @brief The part of a buffer a uniform buffer binding reads. */
typedef struct PvkDescriptorBufferInfo {
    PvkBuffer buffer; /**< The buffer. */
    size_t    offset; /**< Byte offset of the block's first byte; a multiple of 16. */
    size_t    range;  /**< Bytes from offset the block may read, or PVK_WHOLE_SIZE; at least the shader's block size. */
} PvkDescriptorBufferInfo;

/** @brief One binding to fill in with pvkUpdateDescriptorSets(). */
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
 * submit time are what the draws see. The data is laid out as
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

/** @brief A recorded list of commands: a DMA chain in main RAM, for VIF1 (graphics family) or the GIF channel (transfer family). */
typedef struct PvkCommandBuffer_T *PvkCommandBuffer;

/**
 * @brief What pvkCmdBeginRendering() does with the pixels an attachment already holds.
 *
 * A rendering pass is every draw between pvkCmdBeginRendering() and
 * pvkCmdEndRendering(). An attachment is an image the pass draws into: the
 * color image or the depth image. The application picks one load op per
 * attachment.
 *
 * | Value     | What ps2vk sends to the GS                   | Pixels in the image when the first draw runs                       |
 * | --------- | -------------------------------------------- | ------------------------------------------------------------------ |
 * | LOAD      | Nothing                                      | The pixels the image held before the pass                          |
 * | CLEAR     | Sprites that fill renderArea with clearValue | clearValue inside renderArea, the old pixels outside renderArea    |
 * | DONT_CARE | Nothing                                      | Not specified: the application promises to draw over every pixel   |
 * | NONE      | Nothing                                      | Not specified: the application promises the pass draws no pixel    |
 *
 * The GS writes each pixel into VRAM when the pixel is drawn, so keeping the
 * old pixels costs nothing and only CLEAR sends data.
 */
typedef enum PvkAttachmentLoadOp {
    PVK_ATTACHMENT_LOAD_OP_LOAD      = 0,         /**< Keep the pixels the image held before the pass. */
    PVK_ATTACHMENT_LOAD_OP_CLEAR     = 1,         /**< Fill renderArea with clearValue. */
    PVK_ATTACHMENT_LOAD_OP_DONT_CARE = 2,         /**< The old pixels are not needed. */
    PVK_ATTACHMENT_LOAD_OP_NONE      = 1000400000 /**< The pass does not use the image. */
} PvkAttachmentLoadOp;

/**
 * @brief Whether the application needs an attachment's pixels after pvkCmdEndRendering().
 *
 * The application picks one store op per attachment.
 *
 * | Value     | What ps2vk sends to the GS | Pixels in the image after the pass                                 |
 * | --------- | -------------------------- | ------------------------------------------------------------------ |
 * | STORE     | Nothing                    | The pixels the pass drew                                           |
 * | DONT_CARE | Nothing                    | Not specified: the application promises never to read the pixels   |
 * | NONE      | Nothing                    | The pixels the image held before the pass, when the pass drew none |
 *
 * STORE is required for an image that is presented with pvkQueuePresent(),
 * sampled as a texture, or drawn on by a later pass with
 * PVK_ATTACHMENT_LOAD_OP_LOAD. The GS writes each pixel into VRAM when the
 * pixel is drawn, so nothing is left to save when the pass ends and no
 * store op sends data.
 */
typedef enum PvkAttachmentStoreOp {
    PVK_ATTACHMENT_STORE_OP_STORE     = 0,         /**< The pixels the pass drew are needed after the pass. */
    PVK_ATTACHMENT_STORE_OP_DONT_CARE = 1,         /**< The pixels are not needed after the pass. */
    PVK_ATTACHMENT_STORE_OP_NONE      = 1000301000 /**< The pass does not draw into the image. */
} PvkAttachmentStoreOp;

/**
 * @brief The memory command buffers are recorded into.
 *
 * A pool is one 64-byte-aligned block of main RAM. pvkBeginCommandBuffer()
 * gives the command buffer the free space at the top of the block and
 * pvkEndCommandBuffer() keeps only what was recorded, so command buffers of
 * one pool are recorded one after another: a second pvkBeginCommandBuffer()
 * while another of the pool's command buffers is recording fails.
 * pvkResetCommandPool() frees every recording at once by rewinding the block;
 * a command buffer recorded before the reset must be recorded again before it
 * is submitted. Recording a command buffer a second time without a pool reset
 * needs PVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; its old recording's
 * space is reused when it is the topmost, taken again from the top otherwise.
 */
typedef struct PvkCommandPool_T *PvkCommandPool;

/** @brief Flags for PvkCommandPoolCreateInfo. */
typedef enum PvkCommandPoolCreateFlagBits {
    PVK_COMMAND_POOL_CREATE_TRANSIENT_BIT            = 1, /**< Command buffers are re-recorded often. Accepted, changes nothing. */
    PVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 2  /**< pvkBeginCommandBuffer() may be called again on a recorded command buffer without pvkResetCommandPool(). */
} PvkCommandPoolCreateFlagBits;

/** @brief Flags for pvkResetCommandPool(). */
typedef enum PvkCommandPoolResetFlagBits {
    PVK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT = 1 /**< Accepted, changes nothing: the block is never shrunk. */
} PvkCommandPoolResetFlagBits;

/**
 * @brief Parameters for pvkCreateCommandPool().
 *
 * A pool's command buffers are submitted to the queue of its family. Those of
 * the graphics family (0) are VIF1 chains and take every command; those of
 * the transfer family (1) are GIF channel chains, which carry no VIF codes,
 * so they take pvkCmdCopyBufferToImage(), pvkCmdPipelineBarrier() and
 * pvkCmdExecuteCommands() only.
 */
typedef struct PvkCommandPoolCreateInfo {
    uint32_t flags;            /**< PvkCommandPoolCreateFlagBits, or 0. */
    uint32_t queueFamilyIndex; /**< Family of the queue the pool's command buffers are submitted to: 0 graphics, 1 transfer. */
    uint32_t maxQwords;        /**< Size of the block in 16-byte qwords, shared by all of the pool's recordings; 0 = 4096 (64 KB). */
} PvkCommandPoolCreateInfo;

/**
 * @brief Level of a command buffer: primary or secondary.
 *
 * A primary command buffer is submitted to a queue with pvkQueueSubmit(). A
 * secondary command buffer is never submitted; a primary runs it with
 * pvkCmdExecuteCommands(), as many times as it likes, so work recorded once
 * can be replayed every frame. On the PS2 the primary's DMA chain reaches the
 * secondary's with a CALL tag and the secondary's ends in a RET tag, so
 * nothing is copied.
 *
 * A secondary inherits no state from the primary: its pipeline, descriptor
 * sets, vertex buffers and push constants are bound inside it. One recorded
 * with PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT draws into the
 * attachments of the primary's pvkCmdBeginRendering(), knowing only their
 * depth format from PvkCommandBufferInheritanceInfo, so its pipelines need a
 * scissor rectangle: there is no attachment for a zero one to stand for.
 */
typedef enum PvkCommandBufferLevel {
    PVK_COMMAND_BUFFER_LEVEL_PRIMARY   = 0, /**< Submitted to a queue with pvkQueueSubmit(). */
    PVK_COMMAND_BUFFER_LEVEL_SECONDARY = 1  /**< Run from a primary command buffer with pvkCmdExecuteCommands(). */
} PvkCommandBufferLevel;

/** @brief Parameters for pvkAllocateCommandBuffers(). */
typedef struct PvkCommandBufferAllocateInfo {
    PvkCommandPool        commandPool;        /**< Pool the command buffers record into. */
    PvkCommandBufferLevel level;              /**< Primary or secondary. */
    uint32_t              commandBufferCount; /**< Number of command buffers to allocate. */
} PvkCommandBufferAllocateInfo;

/** @brief Flags for PvkCommandBufferBeginInfo. */
typedef enum PvkCommandBufferUsageFlagBits {
    PVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT      = 1, /**< The recording is submitted (or executed) once; a second use without recording again fails. */
    PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT = 2, /**< A secondary command buffer that runs entirely inside the primary's pvkCmdBeginRendering() / pvkCmdEndRendering(). Ignored for a primary. */
    PVK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT     = 4  /**< The command buffer may be pending more than once. Accepted, changes nothing: one submission runs at a time. */
} PvkCommandBufferUsageFlagBits;

/**
 * @brief What a secondary command buffer inherits from the primary that runs it.
 *
 * No render pass, subpass, framebuffer or query fields: ps2vk renders with
 * pvkCmdBeginRendering() alone and has no queries. The attachment formats are
 * what a secondary recorded with PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT
 * knows about the images it draws into; the color format is not needed, since
 * pipelines do not depend on it.
 */
typedef struct PvkCommandBufferInheritanceInfo {
    PvkFormat depthAttachmentFormat; /**< Format of the depth attachment the primary renders with, or PVK_FORMAT_UNDEFINED for none. The pipelines bound inside must match it, as they match pvkCmdBeginRendering()'s. */
} PvkCommandBufferInheritanceInfo;

/** @brief Parameters for pvkBeginCommandBuffer(). */
typedef struct PvkCommandBufferBeginInfo {
    uint32_t                               flags;           /**< PvkCommandBufferUsageFlagBits ORed together; 0 turns none of them on. */
    const PvkCommandBufferInheritanceInfo *inheritanceInfo; /**< For a secondary command buffer; ignored for a primary. */
} PvkCommandBufferBeginInfo;

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

/** @brief One attachment of a rendering pass: the image, the load op, the store op and the clear value. */
typedef struct PvkRenderingAttachmentInfo {
    PvkImageView         imageView;  /**< A one-level view of the target: a swapchain image or a PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT image for color, a depth image for depth; NULL for no attachment. */
    PvkAttachmentLoadOp  loadOp;     /**< What pvkCmdBeginRendering() does with the pixels the image already holds. */
    PvkAttachmentStoreOp storeOp;    /**< Whether the image's pixels are needed after pvkCmdEndRendering(). */
    PvkClearValue        clearValue; /**< Value used when loadOp is CLEAR. */
} PvkRenderingAttachmentInfo;

/** @brief Flags for PvkRenderingInfo. */
typedef enum PvkRenderingFlagBits {
    PVK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT = 1 /**< The drawing comes from secondary command buffers run with pvkCmdExecuteCommands(); draws recorded in the primary itself fail. */
} PvkRenderingFlagBits;

/**
 * @brief Parameters for pvkCmdBeginRendering().
 *
 * The depth attachment is optional (imageView NULL). When present it must be
 * as wide as the color attachment's level, since the GS gives both buffers one
 * width, and the pipelines drawn with it must name its format in
 * depthAttachmentFormat. Rendering into a texture and sampling it later in the
 * same command buffer needs a pvkCmdPipelineBarrier() between the two, as
 * in Vulkan: the barrier carries the GS TEXFLUSH that empties the texture
 * cache.
 */
typedef struct PvkRenderingInfo {
    uint32_t                   flags;           /**< PvkRenderingFlagBits ORed together; 0 turns none of them on, so the primary draws itself. */
    PvkRect2D                  renderArea;      /**< Area to render (and clear), in pixels of the attachment's level. */
    PvkRenderingAttachmentInfo colorAttachment; /**< The color image. */
    PvkRenderingAttachmentInfo depthAttachment; /**< The depth image, or imageView NULL for none. */
} PvkRenderingInfo;

/**
 * @brief Creates a command pool, allocating its block once.
 * @param device The device.
 * @param info Pool parameters, or NULL for no flags and the default size.
 * @param[out] pool The new pool.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateCommandPool(PvkDevice device, const PvkCommandPoolCreateInfo *info, PvkCommandPool *pool);

/**
 * @brief Destroys a pool and every command buffer allocated from it. Waits for a submission still reading one of them.
 * @param device The device.
 * @param pool Pool to destroy, or NULL.
 */
void      pvkDestroyCommandPool(PvkDevice device, PvkCommandPool pool);

/**
 * @brief Discards every recording in the pool; its command buffers stay allocated. Waits for a submission still reading one of them.
 * @param device The device.
 * @param pool The pool.
 * @param flags PvkCommandPoolResetFlagBits, or 0.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED if one of its command buffers is recording.
 */
PvkResult pvkResetCommandPool(PvkDevice device, PvkCommandPool pool, uint32_t flags);

/**
 * @brief Allocates command buffers from a pool. They hold no recording until pvkBeginCommandBuffer().
 * @param device The device.
 * @param info Pool, level and count.
 * @param[out] cmds The new command buffers, info->commandBufferCount of them.
 * @return PVK_SUCCESS, PVK_ERROR_VALIDATION_FAILED or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkAllocateCommandBuffers(PvkDevice device, const PvkCommandBufferAllocateInfo *info, PvkCommandBuffer *cmds);

/**
 * @brief Frees command buffers back to their pool. Their space in the block comes back at the next pvkResetCommandPool().
 * @param device The device.
 * @param pool The pool they were allocated from.
 * @param count Number of entries in cmds.
 * @param cmds Command buffers to free; NULL entries are skipped. None may be in use by a submission.
 */
void      pvkFreeCommandBuffers(PvkDevice device, PvkCommandPool pool, uint32_t count, const PvkCommandBuffer *cmds);

/**
 * @brief Starts recording into the free space of the command buffer's pool, discarding anything recorded before.
 *
 * Recording a secondary command buffer again (or resetting its pool) leaves
 * every primary that executed it unusable: pvkQueueSubmit() refuses such a
 * primary until it is recorded again.
 *
 * @param cmd The command buffer.
 * @param info Usage flags and, for a secondary command buffer, what it inherits; NULL for no flags.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED if the pool is full, another of its command buffers is
 * recording, this one was recorded before without PVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT or a pool
 * reset, or a secondary command buffer has no inheritanceInfo.
 */
PvkResult pvkBeginCommandBuffer(PvkCommandBuffer cmd, const PvkCommandBufferBeginInfo *info);

/**
 * @brief Finishes recording; the space not used is given back to the pool.
 *
 * A primary's chain ends with a GS FINISH and a DMA END tag, a secondary's
 * with a DMA RET tag back to the primary that called it.
 *
 * @param cmd The command buffer.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED if any command failed to record.
 */
PvkResult pvkEndCommandBuffer(PvkCommandBuffer cmd);

/**
 * @brief Runs secondary command buffers from a primary one.
 *
 * Records a DMA CALL tag into each secondary's chain. Inside pvkCmdBeginRendering() /
 * pvkCmdEndRendering() the rendering must have PVK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
 * and each secondary must have been recorded with PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT
 * and the attachments' depth format; outside, the secondaries must not have that bit. Afterwards the
 * primary's bound pipeline, descriptor sets, vertex and index buffers, push constants and dynamic
 * state are undefined, as in Vulkan: the secondary replaced the VU1 program, its memory and the GS
 * registers, so bind them again before the primary's next draw.
 *
 * @param cmd A primary command buffer that is recording.
 * @param count Number of entries in cmds.
 * @param cmds Secondary command buffers that have finished recording, from pools of the primary's family, in the order they run.
 */
void pvkCmdExecuteCommands(PvkCommandBuffer cmd, uint32_t count, const PvkCommandBuffer *cmds);

/**
 * @brief Starts rendering into a color image and optionally a depth image, clearing them if requested.
 *
 * Sets GS FRAME, ZBUF, SCISSOR (the attachment level's size) and XYOFFSET. A
 * clear is a sprite fill over the render area; depth and color are cleared in
 * one pass when both ask for it. The color attachment may be a swapchain
 * image or any level of a PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT texture; the
 * pipelines drawn with it should have a viewport of that level's size.
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
 * @brief Sets the viewport for the following draws with a pipeline that has PVK_DYNAMIC_STATE_VIEWPORT.
 *
 * Pipelines without it keep their own viewport. The value stays set for the
 * rest of the command buffer, across pvkCmdBindPipeline(). The first draw
 * after a change rewrites VU1's viewport transform, after waiting for the
 * running batch.
 *
 * @param cmd The command buffer.
 * @param firstViewport Must be 0: the GS has one viewport.
 * @param viewportCount Must be 1.
 * @param viewports The viewport.
 */
void pvkCmdSetViewport(PvkCommandBuffer cmd, uint32_t firstViewport, uint32_t viewportCount,
                       const PvkViewport *viewports);

/**
 * @brief Sets the scissor rectangle for the following draws with a pipeline that has PVK_DYNAMIC_STATE_SCISSOR.
 *
 * Pixels outside the rectangle are not written. It is clipped to the
 * attachment level; a draw whose rectangle is empty after clipping draws
 * nothing. The value stays set for the rest of the command buffer. Sent as GS
 * SCISSOR with the draw state.
 *
 * @param cmd The command buffer.
 * @param firstScissor Must be 0: the GS has one scissor.
 * @param scissorCount Must be 1.
 * @param scissors The rectangle in framebuffer pixels.
 */
void pvkCmdSetScissor(PvkCommandBuffer cmd, uint32_t firstScissor, uint32_t scissorCount, const PvkRect2D *scissors);

/**
 * @brief Sets the blend constants for the following draws with a pipeline that has PVK_DYNAMIC_STATE_BLEND_CONSTANTS.
 *
 * Only blendConstants[3] has an effect, through the CONSTANT_ALPHA factors;
 * the GS has no constant color. The value stays set for the rest of the
 * command buffer. Sent as GS ALPHA.FIX with the draw state.
 *
 * @param cmd The command buffer.
 * @param blendConstants RGBA, each 0..1; [3] becomes ALPHA.FIX (1 = 128).
 */
void pvkCmdSetBlendConstants(PvkCommandBuffer cmd, const float blendConstants[4]);

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
 * @brief Binds vertex buffers to bindings for the following draws.
 *
 * A draw reads binding b of its pipeline from buffers[b - firstBinding] at
 * offsets[b - firstBinding]. The vertices stream from the buffers into VU1
 * by DMA when the command buffer runs, so the buffers' contents at submit
 * time are what the draw sees. The DMA moves whole 16-byte units: a batch
 * of a binding whose stride is not a multiple of 16 starts at the vertex on
 * the last 16-byte boundary before its first and ends at the one after its
 * last, so up to 15 vertices around a draw are read too; pvkCreateBuffer()
 * leaves room after the buffer for this, and the extra vertices are not
 * drawn.
 *
 * @param cmd The command buffer.
 * @param firstBinding The binding the first buffer goes to.
 * @param bindingCount Number of buffers; firstBinding + bindingCount at most PVK_MAX_VERTEX_BINDINGS.
 * @param buffers The buffers.
 * @param offsets Byte offset of the first vertex in each buffer; each a multiple of 16.
 */
void pvkCmdBindVertexBuffers(PvkCommandBuffer cmd, uint32_t firstBinding, uint32_t bindingCount,
                             const PvkBuffer *buffers, const size_t *offsets);

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

/** @brief A region copied by pvkCmdCopyBufferToImage(). */
typedef struct PvkBufferImageCopy {
    size_t    bufferOffset; /**< Byte offset of the first pixel in the buffer; a multiple of 16. */
    uint32_t  mipLevel;     /**< Destination mip level, 0 for the base level; below the image's mipLevels. */
    PvkRect2D rect;         /**< Destination rectangle in that level (level n is the image size shifted right by n). The buffer holds its rows back to back with no padding, texels in the image's format (see PvkFormat). For a color table: (0, 0, entries, 1). */
} PvkBufferImageCopy;

/**
 * @brief Uploads texels from a buffer into one mip level of a texture, or the entries of a color table.
 *
 * A GS host-to-local transfer (BITBLTBUF at the level's VRAM, TRXPOS, TRXREG, TRXDIR, then the
 * texels as a GIF IMAGE packet). A texture's texels stream straight from the
 * buffer by DMA when the command buffer is submitted, so the buffer must
 * stay unchanged until then; the region's bytes must be a multiple of 16
 * (4 CT32 texels, 16 CT24, 8 CT16, 16 T8, 32 T4). A color table's entries
 * are copied into the command buffer as it is recorded, rearranged into the
 * GS's CSM1 layout (a 256-entry table swaps index bits 3 and 4), so its
 * buffer is free again at once. In a graphics family command buffer the
 * packets go through VIF1 DIRECT (PATH2); in a transfer family one they go
 * down the GIF channel (PATH3), each 16K-qword IMAGE packet ending its GIF
 * packet so a draw chain may take the GIF between them. Must be called
 * outside pvkCmdBeginRendering() / pvkCmdEndRendering().
 *
 * @param cmd The command buffer.
 * @param buffer Source texels in the image's format.
 * @param image Destination texture or color table.
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
 * @brief Draws vertices from the bound vertex buffers.
 *
 * Must be called between pvkCmdBeginRendering() and pvkCmdEndRendering(),
 * with a pipeline and its vertex buffers bound. The pipeline's
 * depthAttachmentFormat must match the depth attachment (or its absence). The
 * pipeline's topology groups the vertices into primitives; only whole ones are
 * drawn. The vertices are drawn once per instance, in instance order, each
 * instance starting its primitives afresh, with the instance-rate bindings
 * supplying what differs between them. When the whole draw fits one batch of
 * VU1 work, a batch holds as many instances as its output has room for, so
 * a small mesh drawn many times is sent once per batch.
 *
 * @param cmd The command buffer.
 * @param vertexCount Number of vertices to draw.
 * @param instanceCount Number of instances; 0 draws nothing.
 * @param firstVertex Index of the first vertex.
 * @param firstInstance Index of the first instance in the instance-rate bindings.
 */
void pvkCmdDraw(PvkCommandBuffer cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                uint32_t firstInstance);

/**
 * @brief Draws vertices in the order the bound index buffer gives.
 *
 * Needs what pvkCmdDraw() needs plus an index buffer. The vertices a batch
 * refers to are sent to VU1 as one block, so index lists that stay local in
 * the vertex buffer (a mesh in draw order) use the least DMA; VU1 then fetches
 * each vertex by its index. The pipeline's topology groups the indices into
 * primitives; only whole ones are drawn. Instances work as in pvkCmdDraw().
 *
 * @param cmd The command buffer.
 * @param indexCount Number of indices to draw.
 * @param instanceCount Number of instances; 0 draws nothing.
 * @param firstIndex Position of the first index in the index buffer.
 * @param vertexOffset Added to every index before it selects a vertex.
 * @param firstInstance Index of the first instance in the instance-rate bindings.
 */
void pvkCmdDrawIndexed(PvkCommandBuffer cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                       int32_t vertexOffset, uint32_t firstInstance);

/** @brief One draw read by pvkCmdDrawIndirect(): the parameters of pvkCmdDraw(), in its order. */
typedef struct PvkDrawIndirectCommand {
    uint32_t vertexCount;   /**< Number of vertices to draw. */
    uint32_t instanceCount; /**< Number of instances; 0 draws nothing. */
    uint32_t firstVertex;   /**< Index of the first vertex. */
    uint32_t firstInstance; /**< Index of the first instance; 0 unless the device has PvkPhysicalDeviceFeatures::drawIndirectFirstInstance. */
} PvkDrawIndirectCommand;

/** @brief One draw read by pvkCmdDrawIndexedIndirect(): the parameters of pvkCmdDrawIndexed(), in its order. */
typedef struct PvkDrawIndexedIndirectCommand {
    uint32_t indexCount;    /**< Number of indices to draw. */
    uint32_t instanceCount; /**< Number of instances; 0 draws nothing. */
    uint32_t firstIndex;    /**< Position of the first index in the index buffer. */
    int32_t  vertexOffset;  /**< Added to every index before it selects a vertex. */
    uint32_t firstInstance; /**< Index of the first instance; 0 unless the device has PvkPhysicalDeviceFeatures::drawIndirectFirstInstance. */
} PvkDrawIndexedIndirectCommand;

/**
 * @brief Draws with parameters read from a buffer: drawCount PvkDrawIndirectCommand
 * structures starting at offset, each stride bytes after the previous.
 *
 * Each command is drawn as pvkCmdDraw() with its four values would be, in
 * order, so it needs what pvkCmdDraw() needs. The EE reads the commands when
 * this call is recorded and builds the VU1 batches from them, so the buffer's
 * contents at record time are what is drawn, and the buffer is free again
 * when the call returns. (Deviation from Vulkan, where the GPU reads the
 * buffer when the command buffer executes: VU1 cannot read a count and start
 * a draw itself.) A drawCount above 1 needs
 * PvkPhysicalDeviceFeatures::multiDrawIndirect and a stride that is a
 * multiple of 4 and at least sizeof(PvkDrawIndirectCommand); drawCount 0
 * draws nothing.
 *
 * @param cmd The command buffer.
 * @param buffer Buffer holding the commands; bound to memory.
 * @param offset Byte offset of the first command; a multiple of 4.
 * @param drawCount Number of commands; at most PvkPhysicalDeviceLimits::maxDrawIndirectCount.
 * @param stride Bytes from one command to the next; ignored when drawCount is 0 or 1.
 */
void pvkCmdDrawIndirect(PvkCommandBuffer cmd, PvkBuffer buffer, size_t offset, uint32_t drawCount, uint32_t stride);

/**
 * @brief Draws with the bound index buffer and parameters read from a buffer: drawCount
 * PvkDrawIndexedIndirectCommand structures starting at offset, each stride bytes after the previous.
 *
 * Each command is drawn as pvkCmdDrawIndexed() with its five values would
 * be, in order, so it needs what pvkCmdDrawIndexed() needs. The commands
 * are read when this call is recorded, as in pvkCmdDrawIndirect(), and the
 * same rules apply to drawCount and stride, with
 * sizeof(PvkDrawIndexedIndirectCommand) as the least stride.
 *
 * @param cmd The command buffer.
 * @param buffer Buffer holding the commands; bound to memory.
 * @param offset Byte offset of the first command; a multiple of 4.
 * @param drawCount Number of commands; at most PvkPhysicalDeviceLimits::maxDrawIndirectCount.
 * @param stride Bytes from one command to the next; ignored when drawCount is 0 or 1.
 */
void pvkCmdDrawIndexedIndirect(PvkCommandBuffer cmd, PvkBuffer buffer, size_t offset, uint32_t drawCount,
                               uint32_t stride);

/** @} */

/* ======================================================================== */
/**
 * @defgroup sync Submission and synchronization
 * @brief Sending command buffers to the hardware and waiting for them.
 * @{
 */


/** @brief Parameters for pvkCreateFence(). May be NULL. */
typedef struct PvkFenceCreateInfo {
    uint32_t signaled; /**< Nonzero to create the fence already signaled. */
} PvkFenceCreateInfo;

/** @brief Parameters for pvkCreateSemaphore(). May be NULL. */
typedef struct PvkSemaphoreCreateInfo {
    uint32_t flags; /**< Reserved; 0. */
} PvkSemaphoreCreateInfo;

/**
 * @brief Points in the hardware's processing of a command buffer.
 *
 * A command buffer is one VIF1 DMA chain that the GS consumes in order, and
 * a wait holds back the start of that chain. So every stage maps to the same
 * point, the chain's DMA start, and a wait at any stage is a wait at the
 * start; the values are kept so a mask reads as in a Vulkan program.
 */
typedef enum PvkPipelineStageFlagBits {
    PVK_PIPELINE_STAGE_TOP_OF_PIPE_BIT             = 0x00000001, /**< The chain's DMA start. */
    PVK_PIPELINE_STAGE_DRAW_INDIRECT_BIT           = 0x00000002, /**< Same point. */
    PVK_PIPELINE_STAGE_VERTEX_INPUT_BIT            = 0x00000004, /**< Same point: the VIF unpacks vertices as the chain runs. */
    PVK_PIPELINE_STAGE_VERTEX_SHADER_BIT           = 0x00000008, /**< Same point: VU1 runs as the chain runs. */
    PVK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT         = 0x00000080, /**< Same point: GS register state set by the chain. */
    PVK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT    = 0x00000100, /**< Same point: GS TEST. */
    PVK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT     = 0x00000200, /**< Same point: GS TEST. */
    PVK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x00000400, /**< Same point: GS FRAME writes. */
    PVK_PIPELINE_STAGE_TRANSFER_BIT                = 0x00001000, /**< Same point: image copies in the chain. */
    PVK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT          = 0x00002000, /**< Same point. */
    PVK_PIPELINE_STAGE_ALL_GRAPHICS_BIT            = 0x00008000, /**< Same point. */
    PVK_PIPELINE_STAGE_ALL_COMMANDS_BIT            = 0x00010000  /**< Same point. */
} PvkPipelineStageFlagBits;
/** @brief Bitmask of PvkPipelineStageFlagBits. */
typedef uint32_t PvkPipelineStageFlags;

/** @brief Ways a stage reads or writes memory; names what a barrier makes visible. */
typedef enum PvkAccessFlagBits {
    PVK_ACCESS_INDIRECT_COMMAND_READ_BIT          = 0x00000001, /**< Draw parameters read when the draw is recorded. */
    PVK_ACCESS_INDEX_READ_BIT                     = 0x00000002, /**< Indices read when the draw is recorded. */
    PVK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT          = 0x00000004, /**< Vertex buffers read by the VIF as the chain runs. */
    PVK_ACCESS_UNIFORM_READ_BIT                   = 0x00000008, /**< Uniform buffers read by the VIF as the chain runs. */
    PVK_ACCESS_SHADER_READ_BIT                    = 0x00000020, /**< A texture sampled through GS TEX0; a barrier whose destination has this bit ends with a GS TEXFLUSH. */
    PVK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT         = 0x00000100, /**< Pixels written through GS FRAME. */
    PVK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT = 0x00000400, /**< Depth written through GS ZBUF. */
    PVK_ACCESS_TRANSFER_WRITE_BIT                 = 0x00001000, /**< Texels written by pvkCmdCopyBufferToImage(). */
    PVK_ACCESS_HOST_WRITE_BIT                     = 0x00004000, /**< CPU writes to mapped memory; written back at pvkQueueSubmit(). */
    PVK_ACCESS_MEMORY_READ_BIT                    = 0x00008000, /**< Any read. */
    PVK_ACCESS_MEMORY_WRITE_BIT                   = 0x00010000  /**< Any write. */
} PvkAccessFlagBits;
/** @brief Bitmask of PvkAccessFlagBits. */
typedef uint32_t PvkAccessFlags;

/** @brief Flags for pvkCmdPipelineBarrier(). */
typedef enum PvkDependencyFlagBits {
    PVK_DEPENDENCY_BY_REGION_BIT = 0x00000001 /**< The dependency holds per pixel; the GS finishes whole draws, so the barrier is the same. */
} PvkDependencyFlagBits;
/** @brief Bitmask of PvkDependencyFlagBits. */
typedef uint32_t PvkDependencyFlags;

/** @brief Barrier source and destination queue family meaning no ownership transfer. */
#define PVK_QUEUE_FAMILY_IGNORED (~0u)

/** @brief A barrier over all memory. */
typedef struct PvkMemoryBarrier {
    PvkAccessFlags srcAccessMask; /**< Writes made before the barrier. */
    PvkAccessFlags dstAccessMask; /**< Reads and writes made after it. */
} PvkMemoryBarrier;

/** @brief A barrier over a range of a buffer. */
typedef struct PvkBufferMemoryBarrier {
    PvkAccessFlags srcAccessMask;       /**< Writes made before the barrier. */
    PvkAccessFlags dstAccessMask;       /**< Reads and writes made after it. */
    uint32_t       srcQueueFamilyIndex; /**< Family giving the range up, or PVK_QUEUE_FAMILY_IGNORED with dstQueueFamilyIndex for no handover. */
    uint32_t       dstQueueFamilyIndex; /**< Family taking the range over; the command buffer's family must be one of the two. */
    PvkBuffer      buffer;              /**< The buffer. */
    size_t         offset;              /**< First byte of the range. */
    size_t         size;                /**< Bytes from offset, or PVK_WHOLE_SIZE. */
} PvkBufferMemoryBarrier;

/** @brief A barrier over a range of an image's mip levels, with its layout change. */
typedef struct PvkImageMemoryBarrier {
    PvkAccessFlags           srcAccessMask;       /**< Writes made before the barrier. */
    PvkAccessFlags           dstAccessMask;       /**< Reads and writes made after it. */
    PvkImageLayout           oldLayout;           /**< The layout the image is in, or PVK_IMAGE_LAYOUT_UNDEFINED when its contents may be discarded. */
    PvkImageLayout           newLayout;           /**< The layout the image is in after the barrier. */
    uint32_t                 srcQueueFamilyIndex; /**< Family giving the levels up, or PVK_QUEUE_FAMILY_IGNORED with dstQueueFamilyIndex for no handover. */
    uint32_t                 dstQueueFamilyIndex; /**< Family taking the levels over; the command buffer's family must be one of the two. */
    PvkImage                 image;               /**< The image. */
    PvkImageSubresourceRange subresourceRange;    /**< The levels covered. */
} PvkImageMemoryBarrier;

/**
 * @brief Records a barrier: the work recorded after it waits for the work recorded before it.
 *
 * A command buffer is one VIF1 DMA chain, so when the barrier is reached the
 * only work still running is the VU1 program and the GS primitives of the
 * last draw, and the GS texture cache may hold texels of a level that was
 * uploaded or rendered into before. The barrier is a VIF FLUSHE when
 * srcStageMask names only vertex stages (waits for VU1 to end) and a VIF
 * FLUSH otherwise (waits for VU1 to end and the GS to take every primitive
 * and texel), followed by a GS TEXFLUSH when a barrier's dstAccessMask has
 * PVK_ACCESS_SHADER_READ_BIT, so the next sampling draw reads VRAM and not
 * the cache. A barrier taking a resource over from the transfer family
 * (srcQueueFamilyIndex 1) opens with a VIF FLUSHA instead, which also waits
 * for the PATH3 packet the GIF is taking. A layout change and an ownership
 * transfer move no bytes: the GS reads VRAM the same way in every layout and
 * from either path. In a transfer family command buffer the barrier records
 * nothing: the GIF channel carries no VIF codes and the GS takes the copies
 * in order. Must be called outside pvkCmdBeginRendering() /
 * pvkCmdEndRendering().
 *
 * @param cmd The command buffer.
 * @param srcStageMask Stages whose work must have finished.
 * @param dstStageMask Stages whose work waits.
 * @param dependencyFlags PvkDependencyFlagBits ORed together, or 0.
 * @param memoryBarrierCount Number of entries in memoryBarriers.
 * @param memoryBarriers Barriers over all memory.
 * @param bufferMemoryBarrierCount Number of entries in bufferMemoryBarriers.
 * @param bufferMemoryBarriers Barriers over buffer ranges.
 * @param imageMemoryBarrierCount Number of entries in imageMemoryBarriers.
 * @param imageMemoryBarriers Barriers over image level ranges.
 */
void pvkCmdPipelineBarrier(PvkCommandBuffer cmd, PvkPipelineStageFlags srcStageMask, PvkPipelineStageFlags dstStageMask,
                           PvkDependencyFlags dependencyFlags,
                           uint32_t memoryBarrierCount, const PvkMemoryBarrier *memoryBarriers,
                           uint32_t bufferMemoryBarrierCount, const PvkBufferMemoryBarrier *bufferMemoryBarriers,
                           uint32_t imageMemoryBarrierCount, const PvkImageMemoryBarrier *imageMemoryBarriers);

/** @brief One batch of command buffers for pvkQueueSubmit(). */
typedef struct PvkSubmitInfo {
    uint32_t                waitSemaphoreCount;   /**< Number of entries in waitSemaphores and waitDstStageMask, at most 4. */
    const PvkSemaphore     *waitSemaphores;       /**< Semaphores that must be signaled before the first command buffer's chain starts; each is reset by the wait. */
    const PvkPipelineStageFlags *waitDstStageMask; /**< Stage at which each wait takes effect: every stage is the chain's DMA start. */
    uint32_t                commandBufferCount;   /**< Number of entries in commandBuffers; 0 submits only the semaphore operations. */
    const PvkCommandBuffer *commandBuffers;       /**< Primary command buffers, run in order. */
    uint32_t                signalSemaphoreCount; /**< Number of entries in signalSemaphores, at most 4. */
    const PvkSemaphore     *signalSemaphores;     /**< Semaphores signaled when the last command buffer has finished drawing. */
} PvkSubmitInfo;

/**
 * @brief Queues command buffers for the hardware.
 *
 * Writes the CPU cache back to RAM and places each command buffer's DMA
 * chain in the queue's ring of 8. Chains are started in ring order by the
 * DMAC and vblank interrupt handlers, each as soon as the queue's channel
 * (VIF1 for the graphics queue, GIF for the transfer queue) is free and its
 * wait semaphores are signaled, so the GS may still be drawing the chain
 * before it. The call sleeps only while the ring is full. Each chain ends
 * with a GS SIGNAL whose ID names the queue and ring entry; its INTC_GS
 * handler reads the ID back from SIGLBLID, signals the submit's semaphores
 * after the submit's last chain and the fence after the last chain of the
 * call. A batch with no command buffer takes one ring entry too: its waits
 * are consumed and its signals and the fence raised once every earlier
 * entry has finished. A submission without a fence uses one the queue
 * keeps, so it waits for the previous fenceless submission to finish first.
 *
 * @param queue The queue.
 * @param submitCount Number of entries in submits.
 * @param submits The batches, run in order.
 * @param fence Unsignaled fence not in use by another submission or acquire,
 *              to signal when the last command buffer finishes drawing; or NULL.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED if a command buffer is
 *         secondary, still recording, failed to record, holds no recording
 *         (never recorded, or its pool was reset since), was recorded with
 *         PVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT and submitted before,
 *         executes a secondary command buffer that was recorded again or
 *         lost its recording since; if a batch has more than 4 semaphores
 *         in a list; if a wait semaphore has no signal
 *         pending or a signal semaphore is signaled or has one pending;
 *         if a command buffer's pool is of another family than the queue; or
 *         if the fence is signaled or in use.
 */
PvkResult pvkQueueSubmit(PvkQueue queue, uint32_t submitCount, const PvkSubmitInfo *submits, PvkFence fence);

/**
 * @brief Creates a semaphore, unsignaled.
 * @param device The device.
 * @param info Semaphore parameters, or NULL.
 * @param[out] semaphore The new semaphore.
 * @return PVK_SUCCESS or PVK_ERROR_OUT_OF_HOST_MEMORY.
 */
PvkResult pvkCreateSemaphore(PvkDevice device, const PvkSemaphoreCreateInfo *info, PvkSemaphore *semaphore);

/**
 * @brief Destroys a semaphore.
 *
 * Waits until the queue is idle first, since a queued chain or present may still name it.
 *
 * @param device The device.
 * @param semaphore Semaphore to destroy.
 */
void      pvkDestroySemaphore(PvkDevice device, PvkSemaphore semaphore);

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
 * @param fences The fences; none may be in use by a submission.
 * @return PVK_SUCCESS, or PVK_ERROR_VALIDATION_FAILED if a fence is in use.
 */
PvkResult pvkResetFences(PvkDevice device, uint32_t count, const PvkFence *fences);

/**
 * @brief Waits until the fences' submissions have finished drawing.
 *
 * The thread sleeps on each pending fence's semaphore until the GS SIGNAL
 * interrupt releases it; a fence that is already signaled costs nothing.
 *
 * @param device The device.
 * @param count Number of fences.
 * @param fences The fences.
 * @return PVK_SUCCESS.
 */
PvkResult pvkWaitForFences(PvkDevice device, uint32_t count, const PvkFence *fences);

/** @} */

/**
 * @defgroup queries Queries
 * @brief Timestamps the GS writes as a command buffer runs, read back after it finishes.
 * @{
 */

/** @brief A pool of query slots. Each slot holds one timestamp and whether the slot has been written. */
typedef struct PvkQueryPool_T *PvkQueryPool;

/** @brief What a query measures. */
typedef enum PvkQueryType {
    PVK_QUERY_TYPE_OCCLUSION           = 0, /**< Pixels that passed the depth test. The GS counts no pixels, so pvkCreateQueryPool() fails with PVK_ERROR_FEATURE_NOT_PRESENT. */
    PVK_QUERY_TYPE_PIPELINE_STATISTICS = 1, /**< Primitive and shader invocation counts. The GS and VU1 count nothing, so pvkCreateQueryPool() fails with PVK_ERROR_FEATURE_NOT_PRESENT. */
    PVK_QUERY_TYPE_TIMESTAMP           = 2  /**< The EE COP0 Count register, read when the GS reaches the pvkCmdWriteTimestamp(). */
} PvkQueryType;

/** @brief Most slots in one query pool: the GS SIGNAL ID carries the slot in 12 bits. */
#define PVK_MAX_QUERIES_PER_POOL 4096
/** @brief Most query pools a device has at once: the GS SIGNAL ID carries the pool's number in 4 bits. */
#define PVK_MAX_QUERY_POOLS 16

/** @brief Parameters for pvkCreateQueryPool(). */
typedef struct PvkQueryPoolCreateInfo {
    uint32_t     flags;              /**< Reserved; 0. */
    PvkQueryType queryType;          /**< PVK_QUERY_TYPE_TIMESTAMP. */
    uint32_t     queryCount;         /**< Slots in the pool, 1 to PVK_MAX_QUERIES_PER_POOL. */
    uint32_t     pipelineStatistics; /**< Reserved; 0. */
} PvkQueryPoolCreateInfo;

/** @brief How pvkGetQueryPoolResults() writes the results. */
typedef enum PvkQueryResultFlagBits {
    PVK_QUERY_RESULT_64_BIT_BIT            = 0x00000001, /**< Each value is a uint64_t; a uint32_t otherwise. */
    PVK_QUERY_RESULT_WAIT_BIT              = 0x00000002, /**< Sleep until every slot asked for has been written. */
    PVK_QUERY_RESULT_WITH_AVAILABILITY_BIT = 0x00000004, /**< After each value, one more of the same size: nonzero when the slot has been written. */
    PVK_QUERY_RESULT_PARTIAL_BIT           = 0x00000008  /**< Not allowed for a timestamp pool, the only kind: pvkGetQueryPoolResults() fails with PVK_ERROR_VALIDATION_FAILED. */
} PvkQueryResultFlagBits;
/** @brief Bitmask of PvkQueryResultFlagBits. */
typedef uint32_t PvkQueryResultFlags;

/**
 * @brief Creates a pool of timestamp slots, all unwritten.
 *
 * The pool takes one of the PVK_MAX_QUERY_POOLS numbers of the device, which
 * the INTC_GS handler finds in a SIGNAL ID.
 *
 * @param device The device.
 * @param info Type and slot count.
 * @param pool Receives the pool.
 * @return PVK_SUCCESS, PVK_ERROR_FEATURE_NOT_PRESENT for a type other than
 *         PVK_QUERY_TYPE_TIMESTAMP, PVK_ERROR_VALIDATION_FAILED for a count
 *         of 0 or above PVK_MAX_QUERIES_PER_POOL, or
 *         PVK_ERROR_OUT_OF_HOST_MEMORY, also when every pool number is taken.
 */
PvkResult pvkCreateQueryPool(PvkDevice device, const PvkQueryPoolCreateInfo *info, PvkQueryPool *pool);

/**
 * @brief Destroys a query pool.
 * @param device The device.
 * @param pool The pool, or NULL; no submitted command buffer may still write to the pool.
 */
void      pvkDestroyQueryPool(PvkDevice device, PvkQueryPool pool);

/**
 * @brief Marks slots unwritten from the CPU, at once.
 * @param device The device.
 * @param pool The pool.
 * @param firstQuery First slot.
 * @param queryCount Slots from firstQuery; the range must lie in the pool.
 */
void      pvkResetQueryPool(PvkDevice device, PvkQueryPool pool, uint32_t firstQuery, uint32_t queryCount);

/**
 * @brief Records a reset: the slots are marked unwritten when the GS reaches this point.
 *
 * A GS SIGNAL whose ID names the pool and the range; the INTC_GS handler
 * clears the slots. A slot must be reset, here or with pvkResetQueryPool(),
 * before each pvkCmdWriteTimestamp() into the slot. Must be called outside
 * pvkCmdBeginRendering() / pvkCmdEndRendering().
 *
 * @param cmd The command buffer, of either queue family.
 * @param pool The pool.
 * @param firstQuery First slot.
 * @param queryCount Slots from firstQuery; the range must lie in the pool.
 */
void      pvkCmdResetQueryPool(PvkCommandBuffer cmd, PvkQueryPool pool, uint32_t firstQuery, uint32_t queryCount);

/**
 * @brief Records a timestamp: the slot takes the time at which the GS reaches this point.
 *
 * A VIF FLUSH (none in a transfer family command buffer), a GS FINISH and a
 * GS SIGNAL whose ID names the pool and slot; the INTC_GS handler reads the
 * EE COP0 Count register when the SIGNAL interrupt fires and stores the
 * count in the slot. The GS has taken every primitive recorded before this
 * point, so every value of @p pipelineStage marks that same point, as every
 * stage of a wait marks the chain's DMA start (see PvkPipelineStageFlagBits).
 * Count runs at the EE clock, PvkPhysicalDeviceLimits::timestampPeriod
 * nanoseconds per tick, and has 32 bits: the difference of two values is
 * right while the two lie less than 2^32 ticks (14.5 s) apart.
 *
 * @param cmd The command buffer, of either queue family; inside or outside a rendering.
 * @param pipelineStage One PvkPipelineStageFlagBits value.
 * @param pool The pool.
 * @param query The slot, below the pool's queryCount.
 */
void      pvkCmdWriteTimestamp(PvkCommandBuffer cmd, PvkPipelineStageFlagBits pipelineStage, PvkQueryPool pool, uint32_t query);

/**
 * @brief Copies slot values out of a pool.
 *
 * Slot firstQuery + i is written at @p data + i × @p stride as a uint32_t,
 * or a uint64_t with PVK_QUERY_RESULT_64_BIT_BIT; with
 * PVK_QUERY_RESULT_WITH_AVAILABILITY_BIT a second value of the same size
 * follows, nonzero when the slot has been written. With
 * PVK_QUERY_RESULT_WAIT_BIT the thread sleeps until every slot in the range
 * has been written, and never wakes for a slot no submitted command buffer
 * writes. Without the bit, an unwritten slot is skipped and the call returns
 * PVK_NOT_READY after writing the slots that are ready.
 *
 * @param device The device.
 * @param pool The pool.
 * @param firstQuery First slot.
 * @param queryCount Slots from firstQuery; the range must lie in the pool.
 * @param dataSize Bytes at @p data; must hold the last slot's values.
 * @param data Where the values go.
 * @param stride Bytes between one slot's values and the next's.
 * @param flags PvkQueryResultFlagBits.
 * @return PVK_SUCCESS, PVK_NOT_READY, or PVK_ERROR_VALIDATION_FAILED for a
 *         range outside the pool, PVK_QUERY_RESULT_PARTIAL_BIT, a @p stride
 *         smaller than the values or a @p dataSize too small.
 */
PvkResult pvkGetQueryPoolResults(PvkDevice device, PvkQueryPool pool, uint32_t firstQuery, uint32_t queryCount,
                                 size_t dataSize, void *data, size_t stride, PvkQueryResultFlags flags);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* PS2VK_H */
