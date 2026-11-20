/* Queue submission and fences: a GS SIGNAL per submission, taken by an INTC_GS handler. */
#include <stdlib.h>

#include <gs_privileged.h>

#include "pvk_internal.h"

#define GS_IMR_SIGMSK (1u << 8)

static s32 gs_handler_id = -1;

/* INTC_GS fires on SIGNAL: the ID is the fence of the submission that just ended. */
static s32 gs_signal_handler(s32 cause)
{
    PvkFence f;

    (void)cause;
    if (*GS_REG_CSR & 1) {
        f = (PvkFence)(uint32_t)*GS_REG_SIGLBLID;
        if (f) {
            f->pending  = 0;
            f->signaled = 1;
            iSignalSema(f->sema);
        }
        /* Acknowledge, or the GS stalls at its next SIGNAL. */
        *GS_REG_CSR = 1;
    }
    /* The kernel masks the event in IMR before calling handlers; unmask it for the next SIGNAL. */
    iGsPutIMR(iGsGetIMR() & ~GS_IMR_SIGMSK);
    ExitHandler();
    return 0;
}

void pvk_gs_signal_install(void)
{
    if (gs_handler_id >= 0)
        return;
    *GS_REG_CSR = 1;
    gs_handler_id = AddIntcHandler(INTC_GS, gs_signal_handler, 0);
    EnableIntc(INTC_GS);
    /* Through the kernel, which keeps the shadow of the write-only IMR. */
    GsPutIMR(GsGetIMR() & ~GS_IMR_SIGMSK);
}

void pvk_gs_signal_remove(void)
{
    if (gs_handler_id < 0)
        return;
    GsPutIMR(GsGetIMR() | GS_IMR_SIGMSK);
    DisableIntc(INTC_GS);
    RemoveIntcHandler(INTC_GS, gs_handler_id);
    gs_handler_id = -1;
}

PvkResult pvk_fence_init(PvkFence fence, int signaled)
{
    ee_sema_t sema = { 0 };

    sema.init_count = 0;
    sema.max_count  = 1;
    fence->sema = CreateSema(&sema);
    if (fence->sema < 0)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    fence->pending  = 0;
    fence->signaled = signaled;
    return PVK_SUCCESS;
}

/* Sleeps until the fence's SIGNAL has been taken; no CPU time while waiting. */
void pvk_fence_wait(PvkFence fence)
{
    if (!fence->pending)
        return;
    WaitSema(fence->sema);
}

void pvk_queue_wait_idle(PvkQueue queue)
{
    if (queue->last)
        pvk_fence_wait(queue->last);
    queue->last = NULL;
}

PvkResult pvkQueueSubmit(PvkQueue queue, PvkCommandBuffer cmd, PvkFence fence)
{
    uint32_t i;

    if (cmd->level != PVK_COMMAND_BUFFER_LEVEL_PRIMARY) {
        pvk_error("pvkQueueSubmit: a secondary command buffer runs through pvkCmdExecuteCommands\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (cmd->recording) {
        pvk_error("pvkQueueSubmit: command buffer is still recording\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (cmd->failed) {
        pvk_error("pvkQueueSubmit: command buffer had recording errors\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (!cmd->size) {
        pvk_error("pvkQueueSubmit: command buffer holds no recording\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (cmd->usage & PVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT && cmd->used) {
        pvk_error("pvkQueueSubmit: command buffer was recorded with PVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT "
                  "and submitted already\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (fence && fence->pending) {
        pvk_error("pvkQueueSubmit: fence is still in use by an earlier submission\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (fence && fence->signaled) {
        pvk_error("pvkQueueSubmit: fence is signaled; reset it first\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    /* The CALL tags point at the secondaries' recordings as they were. */
    for (i = 0; i < cmd->executedCount; i++) {
        const struct PvkExecuted *e = &cmd->executed[i];
        if (e->cmd->generation != e->generation || !e->cmd->size) {
            pvk_error("pvkQueueSubmit: a secondary command buffer it executes was recorded again or lost its "
                      "recording; record the primary again\n");
            return PVK_ERROR_VALIDATION_FAILED;
        }
    }
    cmd->used = 1;

    if (!fence) {
        fence = &queue->idle;
        pvk_fence_wait(fence);
    }
    /* A signal nobody waited for would satisfy the next wait early. */
    while (PollSema(fence->sema) >= 0)
        ;
    fence->signaled = 0;
    fence->pending  = 1;
    queue->last     = fence;

    cmd->packet.base[cmd->signalQw].dw[0] = GS_SET_SIGNAL((uint32_t)fence, 0xFFFFFFFF);

    /* One chain at a time on the channel; the GS may still be drawing the last one. */
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    /* Writes back the data cache (vertex buffers and the SIGNAL ID included) before sending. */
    dma_channel_send_packet2(&cmd->packet, DMA_CHANNEL_VIF1, 1);
    return PVK_SUCCESS;
}

PvkResult pvkCreateFence(PvkDevice device, const PvkFenceCreateInfo *info, PvkFence *fence)
{
    PvkFence f;

    (void)device;

    f = calloc(1, sizeof(struct PvkFence_T));
    if (!f)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    if (pvk_fence_init(f, info && info->signaled) != PVK_SUCCESS) {
        free(f);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }

    *fence = f;
    return PVK_SUCCESS;
}

void pvkDestroyFence(PvkDevice device, PvkFence fence)
{
    if (!fence)
        return;
    pvk_fence_wait(fence);
    if (device->queue.last == fence)
        device->queue.last = NULL;
    DeleteSema(fence->sema);
    free(fence);
}

PvkResult pvkResetFences(PvkDevice device, uint32_t count, const PvkFence *fences)
{
    uint32_t i;

    (void)device;
    for (i = 0; i < count; i++) {
        if (fences[i]->pending) {
            pvk_error("pvkResetFences: fence is still in use by a submission\n");
            return PVK_ERROR_VALIDATION_FAILED;
        }
        fences[i]->signaled = 0;
    }
    return PVK_SUCCESS;
}

PvkResult pvkWaitForFences(PvkDevice device, uint32_t count, const PvkFence *fences)
{
    uint32_t i;

    (void)device;
    for (i = 0; i < count; i++)
        pvk_fence_wait(fences[i]);
    return PVK_SUCCESS;
}
