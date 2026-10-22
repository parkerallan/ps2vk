/* Queue submission and fences. */
#include <stdlib.h>

#include "pvk_internal.h"

void pvk_queue_wait_idle(PvkQueue queue)
{
    if (!queue->busy)
        return;

    /* The DMA chain has been read, then the GS reaches the FINISH write
     * at the end of the command buffer. */
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    draw_wait_finish();

    if (queue->fence) {
        queue->fence->pending  = 0;
        queue->fence->signaled = 1;
        queue->fence = NULL;
    }
    queue->busy = 0;
}

PvkResult pvkQueueSubmit(PvkQueue queue, PvkCommandBuffer cmd, PvkFence fence)
{
    if (cmd->recording) {
        pvk_error("pvkQueueSubmit: command buffer is still recording\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (cmd->failed) {
        pvk_error("pvkQueueSubmit: command buffer had recording errors\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }

    pvk_queue_wait_idle(queue);

    if (fence) {
        fence->signaled = 0;
        fence->pending  = 1;
    }
    queue->fence = fence;
    queue->busy  = 1;

    /* Writes back the data cache (vertex buffers included) before sending. */
    dma_channel_send_packet2(cmd->packet, DMA_CHANNEL_VIF1, 1);
    return PVK_SUCCESS;
}

PvkResult pvkCreateFence(PvkDevice device, const PvkFenceCreateInfo *info, PvkFence *fence)
{
    PvkFence f;

    (void)device;

    f = calloc(1, sizeof(struct PvkFence_T));
    if (!f)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    f->signaled = info && info->signaled;

    *fence = f;
    return PVK_SUCCESS;
}

void pvkDestroyFence(PvkDevice device, PvkFence fence)
{
    if (fence && fence->pending)
        pvk_queue_wait_idle(&device->queue);
    free(fence);
}

PvkResult pvkResetFences(PvkDevice device, uint32_t count, const PvkFence *fences)
{
    uint32_t i;

    (void)device;
    for (i = 0; i < count; i++)
        fences[i]->signaled = 0;
    return PVK_SUCCESS;
}

PvkResult pvkWaitForFences(PvkDevice device, uint32_t count, const PvkFence *fences)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (fences[i]->pending)
            pvk_queue_wait_idle(&device->queue);
    }
    return PVK_SUCCESS;
}
