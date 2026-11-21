/* Queue submission, fences and semaphores: chains wait in a ring per queue and are
 * started from interrupt handlers; a GS SIGNAL per chain, taken by an INTC_GS handler.
 * The graphics queue owns the VIF1 channel (PATH2), the transfer queue the GIF channel (PATH3). */
#include <stdlib.h>

#include <dma_registers.h>
#include <gs_privileged.h>

#include "pvk_internal.h"

#define GS_IMR_SIGMSK (1u << 8)

#define D1_CHCR ((volatile u32 *)0x10009000)
#define D1_QWC  ((volatile u32 *)0x10009020)
#define D1_TADR ((volatile u32 *)0x10009030)
#define D2_CHCR ((volatile u32 *)0x1000A000)
#define D2_QWC  ((volatile u32 *)0x1000A020)
#define D2_TADR ((volatile u32 *)0x1000A030)

/* SIGNAL ID of a ring entry: the index, and the queue's family above it. */
#define PVK_SIGNAL_ID(queue, index) ((uint32_t)(index) | (queue)->family << 8)

static s32 gs_handler_id   = -1;
static s32 vif1_handler_id = -1;
static s32 gif_handler_id  = -1;
static PvkDevice handler_device;

/* The head entry has finished: raises its semaphores and fence and frees it. */
static void complete_head(PvkQueue queue)
{
    struct PvkSubmission *e = &queue->ring[queue->head];
    uint32_t i;

    for (i = 0; i < e->signalCount; i++)
        e->signals[i]->count++;
    if (e->fence) {
        e->fence->pending  = 0;
        e->fence->signaled = 1;
        iSignalSema(e->fence->sema);
    }
    e->state    = 0;
    queue->head = (queue->head + 1) % PVK_MAX_PENDING_SUBMISSIONS;
    iSignalSema(queue->room);
}

static int waits_signaled(const struct PvkSubmission *e)
{
    uint32_t i;

    for (i = 0; i < e->waitCount; i++)
        if (e->waits[i]->count <= 0)
            return 0;
    return 1;
}

static void waits_consume(struct PvkSubmission *e)
{
    uint32_t i;

    for (i = 0; i < e->waitCount; i++)
        e->waits[i]->count--;
}

/* Starts the first queued chain if the channel is free and its semaphores
 * are signaled. A semaphore-only entry completes in place once it is the head. */
void pvk_queue_kick(PvkQueue queue)
{
    struct PvkSubmission *e;
    uint32_t idx;

    for (;;) {
        for (idx = queue->head; idx != queue->tail; idx = (idx + 1) % PVK_MAX_PENDING_SUBMISSIONS)
            if (queue->ring[idx].state == 1)
                break;
        if (idx == queue->tail)
            return;
        e = &queue->ring[idx];
        if (!e->cmd) {
            if (idx != queue->head || !waits_signaled(e))
                return;
            waits_consume(e);
            complete_head(queue);
            continue;
        }
        if (*queue->chcr & 0x100 || !waits_signaled(e))
            return;
        waits_consume(e);
        e->state = 2;
        *queue->qwc  = 0;
        *queue->tadr = DMA_SET_TADR((u32)e->cmd->packet.base, 0);
        /* Chain mode; the tags carry VIF codes (TTE) on VIF1 only. */
        *queue->chcr = DMA_SET_CHCR(1, 1, 0, queue->family == 0, 0, 1, 0);
        return;
    }
}

void pvk_device_kick(PvkDevice device)
{
    pvk_queue_kick(&device->queue);
    if (device->families[1])
        pvk_queue_kick(&device->transfer);
}

/* The EE COP0 Count register: one tick per EE clock. */
static inline uint32_t ee_count(void)
{
    uint32_t count;

    asm volatile("mfc0 %0, $9" : "=r"(count));
    return count;
}

/* INTC_GS fires on SIGNAL: the oldest sent chain of the queue the ID names has finished. */
static s32 gs_signal_handler(s32 cause)
{
    PvkDevice dev = handler_device;
    uint32_t ticks = ee_count();

    (void)cause;
    if (*GS_REG_CSR & 1) {
        if (dev) {
            uint32_t id = (uint32_t)*GS_REG_SIGLBLID;
            if (id & PVK_QUERY_SIGNAL_BIT) {
                pvk_query_signal(dev, id, ticks);
            } else {
                PvkQueue queue = (id >> 8) & 1 ? &dev->transfer : &dev->queue;
                if (queue->head != queue->tail && queue->ring[queue->head].state == 2)
                    complete_head(queue);
            }
        }
        /* Acknowledge, or the GS stalls at its next SIGNAL. */
        *GS_REG_CSR = 1;
        if (dev)
            pvk_device_kick(dev);
    }
    /* The kernel masks the event in IMR before calling handlers; unmask it for the next SIGNAL. */
    iGsPutIMR(iGsGetIMR() & ~GS_IMR_SIGMSK);
    ExitHandler();
    return 0;
}

/* A channel finished a chain: the queue's next one may go while the GS takes this one. */
static int vif1_handler(int channel)
{
    (void)channel;
    if (handler_device)
        pvk_queue_kick(&handler_device->queue);
    ExitHandler();
    return 0;
}

static int gif_handler(int channel)
{
    (void)channel;
    if (handler_device)
        pvk_queue_kick(&handler_device->transfer);
    ExitHandler();
    return 0;
}

static void handlers_install(PvkQueue queue)
{
    handler_device = queue->device;
    if (queue->family == 1) {
        gif_handler_id = AddDmacHandler(DMA_CHANNEL_GIF, gif_handler, 0);
        EnableDmac(DMA_CHANNEL_GIF);
        return;
    }
    *GS_REG_CSR = 1;
    gs_handler_id = AddIntcHandler(INTC_GS, gs_signal_handler, 0);
    EnableIntc(INTC_GS);
    /* Through the kernel, which keeps the shadow of the write-only IMR. */
    GsPutIMR(GsGetIMR() & ~GS_IMR_SIGMSK);
    vif1_handler_id = AddDmacHandler(DMA_CHANNEL_VIF1, vif1_handler, 0);
    EnableDmac(DMA_CHANNEL_VIF1);
}

static void handlers_remove(PvkQueue queue)
{
    if (queue->family == 1) {
        if (gif_handler_id < 0)
            return;
        DisableDmac(DMA_CHANNEL_GIF);
        RemoveDmacHandler(DMA_CHANNEL_GIF, gif_handler_id);
        gif_handler_id = -1;
        return;
    }
    if (gs_handler_id < 0)
        return;
    DisableDmac(DMA_CHANNEL_VIF1);
    RemoveDmacHandler(DMA_CHANNEL_VIF1, vif1_handler_id);
    GsPutIMR(GsGetIMR() | GS_IMR_SIGMSK);
    DisableIntc(INTC_GS);
    RemoveIntcHandler(INTC_GS, gs_handler_id);
    gs_handler_id   = -1;
    vif1_handler_id = -1;
    handler_device  = NULL;
}

static s32 create_binary_sema(void)
{
    ee_sema_t sema = { 0 };

    sema.init_count = 0;
    sema.max_count  = 1;
    return CreateSema(&sema);
}

PvkResult pvk_fence_init(PvkFence fence, int signaled)
{
    fence->sema = create_binary_sema();
    if (fence->sema < 0)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    fence->pending  = 0;
    fence->signaled = signaled;
    return PVK_SUCCESS;
}

PvkResult pvk_queue_init(PvkQueue queue, uint32_t family)
{
    if (pvk_fence_init(&queue->idle, 1) != PVK_SUCCESS)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    queue->room = create_binary_sema();
    if (queue->room < 0) {
        DeleteSema(queue->idle.sema);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }
    queue->family = family;
    queue->chcr   = family ? D2_CHCR : D1_CHCR;
    queue->qwc    = family ? D2_QWC : D1_QWC;
    queue->tadr   = family ? D2_TADR : D1_TADR;
    queue->head = queue->tail = 0;
    handlers_install(queue);
    return PVK_SUCCESS;
}

void pvk_queue_shutdown(PvkQueue queue)
{
    handlers_remove(queue);
    DeleteSema(queue->room);
    DeleteSema(queue->idle.sema);
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
    if (queue->family == 0 && queue->device->swapchain)
        pvk_swapchain_wait_flips(queue->device->swapchain);
}

void pvk_device_wait_idle(PvkDevice device)
{
    pvk_queue_wait_idle(&device->queue);
    if (device->families[1])
        pvk_queue_wait_idle(&device->transfer);
}

static PvkResult check_cmd(PvkQueue queue, PvkCommandBuffer cmd)
{
    uint32_t i;

    if (cmd->pool->family != queue->family) {
        pvk_error("pvkQueueSubmit: command buffer is from a pool of queue family %u, the queue is of family %u\n",
                  (unsigned)cmd->pool->family, (unsigned)queue->family);
        return PVK_ERROR_VALIDATION_FAILED;
    }
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
    /* The CALL tags point at the secondaries' recordings as they were. */
    for (i = 0; i < cmd->executedCount; i++) {
        const struct PvkExecuted *e = &cmd->executed[i];
        if (e->cmd->generation != e->generation || !e->cmd->size) {
            pvk_error("pvkQueueSubmit: a secondary command buffer it executes was recorded again or lost its "
                      "recording; record the primary again\n");
            return PVK_ERROR_VALIDATION_FAILED;
        }
    }
    return PVK_SUCCESS;
}

PvkResult pvk_semaphores_check(const char *who, uint32_t count, const PvkSemaphore *sems, int signal, int apply)
{
    uint32_t i;

    if (count > PVK_MAX_SUBMIT_SEMAPHORES) {
        pvk_error("%s: at most %d semaphores per list\n", who, PVK_MAX_SUBMIT_SEMAPHORES);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    for (i = 0; i < count; i++) {
        if (signal && sems[i]->balance) {
            pvk_error("%s: semaphore is signaled or has a signal pending; it must be waited on first\n", who);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (!signal && !sems[i]->balance) {
            pvk_error("%s: semaphore has no signal pending to wait for\n", who);
            return PVK_ERROR_VALIDATION_FAILED;
        }
    }
    if (apply)
        for (i = 0; i < count; i++)
            sems[i]->balance = signal;
    return PVK_SUCCESS;
}

/* Takes the next free ring entry, sleeping while the ring is full. */
static struct PvkSubmission *ring_take(PvkQueue queue)
{
    for (;;) {
        uint32_t next = (queue->tail + 1) % PVK_MAX_PENDING_SUBMISSIONS;
        if (next != queue->head)
            break;
        WaitSema(queue->room);
    }
    return &queue->ring[queue->tail];
}

PvkResult pvkQueueSubmit(PvkQueue queue, uint32_t submitCount, const PvkSubmitInfo *submits, PvkFence fence)
{
    uint32_t s, c, i, first, n = 0;
    PvkResult res;

    for (s = 0; s < submitCount; s++) {
        const PvkSubmitInfo *si = &submits[s];
        for (c = 0; c < si->commandBufferCount; c++)
            if ((res = check_cmd(queue, si->commandBuffers[c])) != PVK_SUCCESS)
                return res;
        res = pvk_semaphores_check("pvkQueueSubmit", si->waitSemaphoreCount, si->waitSemaphores, 0, 0);
        if (res != PVK_SUCCESS)
            return res;
        res = pvk_semaphores_check("pvkQueueSubmit", si->signalSemaphoreCount, si->signalSemaphores, 1, 0);
        if (res != PVK_SUCCESS)
            return res;
        n += si->commandBufferCount ? si->commandBufferCount : 1;
    }
    if (fence && fence->pending) {
        pvk_error("pvkQueueSubmit: fence is still in use by an earlier submission\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (fence && fence->signaled) {
        pvk_error("pvkQueueSubmit: fence is signaled; reset it first\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (!n)
        return PVK_SUCCESS;

    for (s = 0; s < submitCount; s++) {
        pvk_semaphores_check("pvkQueueSubmit", submits[s].waitSemaphoreCount, submits[s].waitSemaphores, 0, 1);
        pvk_semaphores_check("pvkQueueSubmit", submits[s].signalSemaphoreCount, submits[s].signalSemaphores, 1, 1);
    }

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

    /* Fill the entries unqueued, so no handler starts one before the cache is written back. */
    first = queue->tail;
    for (s = 0; s < submitCount; s++) {
        const PvkSubmitInfo *si = &submits[s];
        uint32_t entries = si->commandBufferCount ? si->commandBufferCount : 1;
        for (c = 0; c < entries; c++) {
            struct PvkSubmission *e = ring_take(queue);
            PvkCommandBuffer cmd = si->commandBufferCount ? si->commandBuffers[c] : NULL;
            int last = s == submitCount - 1 && c == entries - 1;

            e->cmd    = cmd;
            e->fence  = last ? fence : NULL;
            e->waitCount = e->signalCount = 0;
            if (c == 0) {
                for (i = 0; i < si->waitSemaphoreCount; i++)
                    e->waits[i] = si->waitSemaphores[i];
                e->waitCount = si->waitSemaphoreCount;
            }
            if (c == entries - 1) {
                for (i = 0; i < si->signalSemaphoreCount; i++)
                    e->signals[i] = si->signalSemaphores[i];
                e->signalCount = si->signalSemaphoreCount;
            }
            if (cmd) {
                cmd->used = 1;
                cmd->packet.base[cmd->signalQw].dw[0] = GS_SET_SIGNAL(PVK_SIGNAL_ID(queue, e - queue->ring), 0xFFFFFFFF);
            }
            queue->tail = (queue->tail + 1) % PVK_MAX_PENDING_SUBMISSIONS;
        }
    }
    /* Writes back the data cache (vertex buffers and the SIGNAL IDs included) before sending. */
    FlushCache(0);

    DI();
    for (i = first; i != queue->tail; i = (i + 1) % PVK_MAX_PENDING_SUBMISSIONS)
        queue->ring[i].state = 1;
    pvk_queue_kick(queue);
    EI();
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
    if (device->transfer.last == fence)
        device->transfer.last = NULL;
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

PvkResult pvkCreateSemaphore(PvkDevice device, const PvkSemaphoreCreateInfo *info, PvkSemaphore *semaphore)
{
    PvkSemaphore s;

    (void)device;
    (void)info;

    s = calloc(1, sizeof(struct PvkSemaphore_T));
    if (!s)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    *semaphore = s;
    return PVK_SUCCESS;
}

void pvkDestroySemaphore(PvkDevice device, PvkSemaphore semaphore)
{
    if (!semaphore)
        return;
    /* A queued chain or flip may still name it. */
    pvk_device_wait_idle(device);
    free(semaphore);
}
