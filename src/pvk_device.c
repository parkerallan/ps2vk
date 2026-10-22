/* Instance, device and queue: one-time DMA setup. */
#include <stdlib.h>

#include "pvk_internal.h"

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

PvkResult pvkCreateDevice(PvkInstance instance, const PvkDeviceCreateInfo *info, PvkDevice *device)
{
    PvkDevice dev;

    (void)instance;
    (void)info;

    dev = calloc(1, sizeof(struct PvkDevice_T));
    if (!dev)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    dev->queue.device = dev;

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
