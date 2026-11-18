/* Instance, physical device, device and queue: capability constants and one-time DMA setup. */
#include <stdlib.h>
#include <string.h>

#include "pvk_internal.h"

/* The one set of numbers both the application and the library's checks read. */
const PvkPhysicalDeviceLimits pvk_limits = {
    .maxImageDimension2D                 = 1024,
    .maxMipLevels                        = PVK_MAX_MIP_LEVELS,
    .maxUniformBufferRange               = PVK_MAX_UNIFORM_BYTES,
    .maxPushConstantsSize                = PVK_MAX_PUSH_CONSTANT_BYTES,
    .maxBoundDescriptorSets              = PVK_MAX_DESCRIPTOR_SETS,
    .maxDescriptorSetBindings            = PVK_MAX_DESCRIPTOR_BINDINGS,
    .maxPerStageDescriptorSamplers       = 1,
    .maxPerStageDescriptorUniformBuffers = PVK_MAX_UNIFORM_BLOCKS,
    .maxPerStageDescriptorSampledImages  = 1,
    .maxVertexInputAttributes            = PVK_MAX_VERTEX_INPUTS,
    .maxVertexInputBindings              = PVK_MAX_VERTEX_BINDINGS,
    .maxViewports                        = 1,
    .maxViewportDimensions               = { 2048, 2048 },
    .viewportBoundsRange                 = { -PVK_GS_ORIGIN, 4096.0f - PVK_GS_ORIGIN - 1.0f },
    .maxFramebufferWidth                 = 2048,
    .maxFramebufferHeight                = 2048,
    .maxColorAttachments                 = 1,
    .minUniformBufferOffsetAlignment     = 16,
    .minMemoryMapAlignment               = 64,
    .maxSamplerLodBias                   = 127.9375f,
    .maxSamplerAnisotropy                = 1.0f,
    .pointSizeRange                      = { 1.0f, 1.0f },
    .lineWidthRange                      = { 1.0f, 1.0f },
    .subPixelPrecisionBits               = 4,
};

static const PvkPhysicalDeviceFeatures supported_features = {
    .robustBufferAccess = 1,
};

PvkResult pvkCreateInstance(const PvkInstanceCreateInfo *info, PvkInstance *instance)
{
    (void)info;
    *instance = calloc(1, sizeof(struct PvkInstance_T));
    return *instance ? PVK_SUCCESS : PVK_ERROR_OUT_OF_HOST_MEMORY;
}

void pvkDestroyInstance(PvkInstance instance)
{
    free(instance);
}

PvkResult pvkEnumeratePhysicalDevices(PvkInstance instance, uint32_t *count, PvkPhysicalDevice *devices)
{
    if (!devices) {
        *count = 1;
        return PVK_SUCCESS;
    }
    if (*count == 0)
        return PVK_INCOMPLETE;
    devices[0] = &instance->physicalDevice;
    *count = 1;
    return PVK_SUCCESS;
}

void pvkGetPhysicalDeviceProperties(PvkPhysicalDevice physicalDevice, PvkPhysicalDeviceProperties *properties)
{
    (void)physicalDevice;
    memset(properties, 0, sizeof(*properties));
    properties->apiVersion    = PVK_API_VERSION_1_0;
    properties->driverVersion = 1;
    properties->vendorID      = 0x054C;
    properties->deviceID      = 0;
    properties->deviceType    = PVK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strcpy(properties->deviceName, "PlayStation 2 Graphics Synthesizer");
    properties->limits        = pvk_limits;
}

void pvkGetPhysicalDeviceFeatures(PvkPhysicalDevice physicalDevice, PvkPhysicalDeviceFeatures *features)
{
    (void)physicalDevice;
    *features = supported_features;
}

void pvkGetPhysicalDeviceMemoryProperties(PvkPhysicalDevice physicalDevice, PvkPhysicalDeviceMemoryProperties *properties)
{
    (void)physicalDevice;
    properties->memoryTypeCount = 2;
    properties->memoryTypes[0].propertyFlags = PVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | PVK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    properties->memoryTypes[0].heapIndex     = 0;
    properties->memoryTypes[1].propertyFlags = PVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    properties->memoryTypes[1].heapIndex     = 1;
    properties->memoryHeapCount = 2;
    properties->memoryHeaps[0].size  = 32u << 20;
    properties->memoryHeaps[0].flags = 0;
    properties->memoryHeaps[1].size  = 4u << 20;
    properties->memoryHeaps[1].flags = PVK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

PvkResult pvkCreateDevice(PvkPhysicalDevice physicalDevice, const PvkDeviceCreateInfo *info, PvkDevice *device)
{
    PvkDevice dev;

    (void)physicalDevice;

    if (info && info->enabledFeatures && info->enabledFeatures->robustBufferAccess &&
        !supported_features.robustBufferAccess) {
        pvk_error("pvkCreateDevice: robustBufferAccess is not supported\n");
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }

    dev = calloc(1, sizeof(struct PvkDevice_T));
    if (!dev)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    dev->queue.device = dev;
    if (info && info->enabledFeatures)
        dev->features = *info->enabledFeatures;

    /* Reset the GIF: the BIOS (OSDSYS) leaves it busy, which stalls the first
     * VIF1 DIRECT transfer forever (ps2gl does the same in glutInit). */
    GIF_REG_CTRL = 1;

    /* GIF (PATH3) is used for one-off setup, VIF1 for command buffers. */
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);
    dma_channel_fast_waits(DMA_CHANNEL_VIF1);

    *device = dev;
    return PVK_SUCCESS;
}

void pvkDestroyDevice(PvkDevice device)
{
    if (!device)
        return;
    pvkDeviceWaitIdle(device);
    free(device);
}

void pvkGetDeviceQueue(PvkDevice device, PvkQueue *queue)
{
    *queue = &device->queue;
}

PvkResult pvkDeviceWaitIdle(PvkDevice device)
{
    pvk_queue_wait_idle(&device->queue);
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    return PVK_SUCCESS;
}
