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
    .maxStorageBufferRange               = PVK_MAX_STORAGE_BYTES,
    .maxPerStageDescriptorStorageBuffers = PVK_MAX_STORAGE_BLOCKS,
    .maxComputeWorkGroupCount            = { 65535, 65535, 65535 },
    .maxComputeWorkGroupInvocations      = PVK_MAX_COMPUTE_WORK_GROUP_SIZE * PVK_MAX_COMPUTE_WORK_GROUP_SIZE *
                                           PVK_MAX_COMPUTE_WORK_GROUP_SIZE,
    .maxComputeWorkGroupSize             = { PVK_MAX_COMPUTE_WORK_GROUP_SIZE, PVK_MAX_COMPUTE_WORK_GROUP_SIZE,
                                             PVK_MAX_COMPUTE_WORK_GROUP_SIZE },
    .maxVertexInputAttributes            = PVK_MAX_VERTEX_INPUTS,
    .maxVertexInputBindings              = PVK_MAX_VERTEX_BINDINGS,
    .maxViewports                        = 1,
    .maxViewportDimensions               = { 2048, 2048 },
    .viewportBoundsRange                 = { -PVK_GS_ORIGIN, 4096.0f - PVK_GS_ORIGIN - 1.0f },
    .maxFramebufferWidth                 = 2048,
    .maxFramebufferHeight                = 2048,
    .maxColorAttachments                 = 1,
    .maxDrawIndirectCount                = 0xffffffffu,
    .minUniformBufferOffsetAlignment     = 16,
    .minStorageBufferOffsetAlignment     = 16,
    .minMemoryMapAlignment               = 64,
    .maxSamplerLodBias                   = 127.9375f,
    .maxSamplerAnisotropy                = 1.0f,
    .pointSizeRange                      = { 1.0f, 1.0f },
    .lineWidthRange                      = { 1.0f, 1.0f },
    .subPixelPrecisionBits               = 4,
    .timestampPeriod                     = 1.0e9f / 294912000.0f,
    .timestampComputeAndGraphics         = 1,
};

static const PvkPhysicalDeviceFeatures supported_features = {
    .robustBufferAccess        = 1,
    .multiDrawIndirect         = 1,
    .drawIndirectFirstInstance = 1,
};

/* A GS line is a run of single pixels with no pattern; PRIM.AA1 smooths it. */
static const PvkPhysicalDeviceLineRasterizationFeatures supported_line_features = {
    .bresenhamLines = 1,
    .smoothLines    = 1,
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

void pvkGetPhysicalDeviceFeatures2(PvkPhysicalDevice physicalDevice, PvkPhysicalDeviceFeatures2 *features)
{
    struct PvkBaseStructure *s;

    if (features->sType != PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        pvk_error("pvkGetPhysicalDeviceFeatures2: sType is %d, not PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2\n",
                  (int)features->sType);
        return;
    }
    pvkGetPhysicalDeviceFeatures(physicalDevice, &features->features);
    for (s = features->pNext; s; s = s->pNext)
        if (s->sType == PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES) {
            PvkPhysicalDeviceLineRasterizationFeatures *f = (PvkPhysicalDeviceLineRasterizationFeatures *)s;
            void *next = f->pNext;

            *f = supported_line_features;
            f->sType = PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES;
            f->pNext = next;
        }
}

void pvkGetPhysicalDeviceProperties2(PvkPhysicalDevice physicalDevice, PvkPhysicalDeviceProperties2 *properties)
{
    struct PvkBaseStructure *s;

    if (properties->sType != PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2) {
        pvk_error("pvkGetPhysicalDeviceProperties2: sType is %d, not PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2\n",
                  (int)properties->sType);
        return;
    }
    pvkGetPhysicalDeviceProperties(physicalDevice, &properties->properties);
    for (s = properties->pNext; s; s = s->pNext)
        if (s->sType == PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES)
            ((PvkPhysicalDeviceLineRasterizationProperties *)s)->lineSubPixelPrecisionBits =
                pvk_limits.subPixelPrecisionBits;
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

/* Family 0 feeds VIF1 (PATH2) and so VU1, family 1 the GIF channel (PATH3). */
static const PvkQueueFamilyProperties queue_families[PVK_MAX_QUEUE_FAMILIES] = {
    { PVK_QUEUE_GRAPHICS_BIT | PVK_QUEUE_COMPUTE_BIT | PVK_QUEUE_TRANSFER_BIT, 1, 32, { 1, 1, 1 } },
    { PVK_QUEUE_TRANSFER_BIT, 1, 32, { 1, 1, 1 } },
};

void pvkGetPhysicalDeviceQueueFamilyProperties(PvkPhysicalDevice physicalDevice, uint32_t *count,
                                               PvkQueueFamilyProperties *properties)
{
    uint32_t i;

    (void)physicalDevice;
    if (!properties) {
        *count = PVK_MAX_QUEUE_FAMILIES;
        return;
    }
    if (*count > PVK_MAX_QUEUE_FAMILIES)
        *count = PVK_MAX_QUEUE_FAMILIES;
    for (i = 0; i < *count; i++)
        properties[i] = queue_families[i];
}

PvkResult pvkCreateDevice(PvkPhysicalDevice physicalDevice, const PvkDeviceCreateInfo *info, PvkDevice *device)
{
    PvkDevice dev;
    int families[PVK_MAX_QUEUE_FAMILIES] = { 0 };
    const PvkPhysicalDeviceFeatures *enabled;
    const PvkPhysicalDeviceLineRasterizationFeatures *lines = NULL;
    const struct PvkBaseStructure *s;
    uint32_t i;

    (void)physicalDevice;

    if (!info || info->sType != PVK_STRUCTURE_TYPE_DEVICE_CREATE_INFO) {
        pvk_error("pvkCreateDevice: sType is not PVK_STRUCTURE_TYPE_DEVICE_CREATE_INFO\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    enabled = info->enabledFeatures;
    for (s = info->pNext; s; s = s->pNext) {
        if (s->sType == PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            if (info->enabledFeatures) {
                pvk_error("pvkCreateDevice: enabledFeatures must be NULL with a PvkPhysicalDeviceFeatures2 in pNext\n");
                return PVK_ERROR_VALIDATION_FAILED;
            }
            enabled = &((const PvkPhysicalDeviceFeatures2 *)s)->features;
        } else if (s->sType == PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES) {
            lines = (const PvkPhysicalDeviceLineRasterizationFeatures *)s;
        }
    }
    if (!info->queueCreateInfoCount) {
        pvk_error("pvkCreateDevice: queueCreateInfos must name at least one queue family\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    for (i = 0; i < info->queueCreateInfoCount; i++) {
        const PvkDeviceQueueCreateInfo *q = &info->queueCreateInfos[i];
        if (q->queueFamilyIndex >= PVK_MAX_QUEUE_FAMILIES) {
            pvk_error("pvkCreateDevice: queue family %u does not exist; 0 is graphics, 1 transfer\n",
                      (unsigned)q->queueFamilyIndex);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (families[q->queueFamilyIndex]) {
            pvk_error("pvkCreateDevice: queue family %u is named twice\n", (unsigned)q->queueFamilyIndex);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (q->queueCount != 1) {
            pvk_error("pvkCreateDevice: queue family %u has 1 queue, %u asked for\n",
                      (unsigned)q->queueFamilyIndex, (unsigned)q->queueCount);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        families[q->queueFamilyIndex] = 1;
    }
    if (lines) {
        const PvkPhysicalDeviceLineRasterizationFeatures *have = &supported_line_features;
        const char *missing = lines->rectangularLines && !have->rectangularLines ? "rectangularLines"
                            : lines->bresenhamLines && !have->bresenhamLines ? "bresenhamLines"
                            : lines->smoothLines && !have->smoothLines ? "smoothLines"
                            : lines->stippledRectangularLines && !have->stippledRectangularLines
                                ? "stippledRectangularLines"
                            : lines->stippledBresenhamLines && !have->stippledBresenhamLines ? "stippledBresenhamLines"
                            : lines->stippledSmoothLines && !have->stippledSmoothLines ? "stippledSmoothLines" : NULL;
        if (missing) {
            pvk_error("pvkCreateDevice: %s is not supported\n", missing);
            return PVK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (enabled) {
        const PvkPhysicalDeviceFeatures *f = enabled;
        const char *missing = f->robustBufferAccess && !supported_features.robustBufferAccess ? "robustBufferAccess"
                            : f->multiDrawIndirect && !supported_features.multiDrawIndirect ? "multiDrawIndirect"
                            : f->drawIndirectFirstInstance && !supported_features.drawIndirectFirstInstance
                                ? "drawIndirectFirstInstance" : NULL;
        if (missing) {
            pvk_error("pvkCreateDevice: %s is not supported\n", missing);
            return PVK_ERROR_FEATURE_NOT_PRESENT;
        }
    }

    dev = calloc(1, sizeof(struct PvkDevice_T));
    if (!dev)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    dev->queue.device    = dev;
    dev->transfer.device = dev;
    for (i = 0; i < PVK_MAX_QUEUE_FAMILIES; i++)
        dev->families[i] = families[i];
    if (enabled)
        dev->features = *enabled;
    if (lines)
        dev->lineFeatures = *lines;
    /* Reset the GIF: the BIOS (OSDSYS) leaves it busy, which stalls the first
     * VIF1 DIRECT transfer forever (ps2gl does the same in glutInit). */
    GIF_REG_CTRL = 1;

    /* VIF1 carries the graphics queue's chains, the GIF channel (PATH3) the transfer queue's. */
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);
    dma_channel_fast_waits(DMA_CHANNEL_VIF1);

    /* The graphics queue always exists: its INTC_GS handler completes both queues' chains. */
    if (pvk_queue_init(&dev->queue, 0) != PVK_SUCCESS) {
        free(dev);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (families[1] && pvk_queue_init(&dev->transfer, 1) != PVK_SUCCESS) {
        pvk_queue_shutdown(&dev->queue);
        free(dev);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }

    *device = dev;
    return PVK_SUCCESS;
}

void pvkDestroyDevice(PvkDevice device)
{
    if (!device)
        return;
    pvkDeviceWaitIdle(device);
    if (device->families[1])
        pvk_queue_shutdown(&device->transfer);
    pvk_queue_shutdown(&device->queue);
    free(device);
}

void pvkGetDeviceQueue(PvkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, PvkQueue *queue)
{
    *queue = NULL;
    if (queueFamilyIndex >= PVK_MAX_QUEUE_FAMILIES || !device->families[queueFamilyIndex]) {
        pvk_error("pvkGetDeviceQueue: queue family %u was not asked for at pvkCreateDevice\n",
                  (unsigned)queueFamilyIndex);
        return;
    }
    if (queueIndex != 0) {
        pvk_error("pvkGetDeviceQueue: queue family %u has 1 queue, index %u asked for\n",
                  (unsigned)queueFamilyIndex, (unsigned)queueIndex);
        return;
    }
    *queue = queueFamilyIndex ? &device->transfer : &device->queue;
}

PvkResult pvkDeviceWaitIdle(PvkDevice device)
{
    pvk_device_wait_idle(device);
    return PVK_SUCCESS;
}
