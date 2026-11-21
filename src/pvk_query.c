/* Query pools: timestamp slots. A pvkCmdWriteTimestamp is a GS SIGNAL whose ID names the pool and
 * slot; the INTC_GS handler reads the EE COP0 Count register and stores it through pvk_query_signal. */
#include <stdlib.h>
#include <string.h>

#include "pvk_internal.h"

PvkResult pvkCreateQueryPool(PvkDevice device, const PvkQueryPoolCreateInfo *info, PvkQueryPool *pool)
{
    PvkQueryPool p;
    ee_sema_t sema = { 0 };
    uint32_t i;

    *pool = NULL;
    if (info->queryType != PVK_QUERY_TYPE_TIMESTAMP) {
        pvk_error("pvkCreateQueryPool: query type %d; the GS counts no pixels or primitives, only "
                  "PVK_QUERY_TYPE_TIMESTAMP exists\n", (int)info->queryType);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (info->queryCount == 0 || info->queryCount > PVK_MAX_QUERIES_PER_POOL) {
        pvk_error("pvkCreateQueryPool: queryCount %u; 1 to %d\n", (unsigned)info->queryCount, PVK_MAX_QUERIES_PER_POOL);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    for (i = 0; i < PVK_MAX_QUERY_POOLS && device->queryPools[i]; i++)
        ;
    if (i == PVK_MAX_QUERY_POOLS) {
        pvk_error("pvkCreateQueryPool: the device has %d query pools already\n", PVK_MAX_QUERY_POOLS);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }
    p = calloc(1, sizeof(*p));
    if (!p)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    p->values  = calloc(info->queryCount, sizeof(*p->values));
    p->written = calloc(info->queryCount, sizeof(*p->written));
    sema.init_count = 0;
    sema.max_count  = 1;
    p->sema = CreateSema(&sema);
    if (!p->values || !p->written || p->sema < 0) {
        if (p->sema >= 0)
            DeleteSema(p->sema);
        free((void *)p->values);
        free((void *)p->written);
        free(p);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }
    p->device = device;
    p->index  = i;
    p->count  = info->queryCount;
    device->queryPools[i] = p;
    *pool = p;
    return PVK_SUCCESS;
}

void pvkDestroyQueryPool(PvkDevice device, PvkQueryPool pool)
{
    if (!pool)
        return;
    device->queryPools[pool->index] = NULL;
    DeleteSema(pool->sema);
    free((void *)pool->values);
    free((void *)pool->written);
    free(pool);
}

/* Fails for a range outside the pool. */
static int query_range(const char *who, PvkQueryPool pool, uint32_t first, uint32_t count)
{
    if (first < pool->count && count >= 1 && count <= pool->count - first)
        return 1;
    pvk_error("%s: slots %u to %u lie outside the pool's %u\n", who, (unsigned)first, (unsigned)(first + count - 1),
              (unsigned)pool->count);
    return 0;
}

void pvkResetQueryPool(PvkDevice device, PvkQueryPool pool, uint32_t firstQuery, uint32_t queryCount)
{
    (void)device;
    if (!query_range("pvkResetQueryPool", pool, firstQuery, queryCount))
        return;
    memset((void *)(pool->written + firstQuery), 0, queryCount);
}

void pvk_query_signal(PvkDevice device, uint32_t id, uint32_t ticks)
{
    PvkQueryPool pool = device->queryPools[(id >> 24) & (PVK_MAX_QUERY_POOLS - 1)];
    uint32_t slot = id & (PVK_MAX_QUERIES_PER_POOL - 1), count;

    if (!pool)
        return;
    if (id & PVK_QUERY_RESET_BIT) {
        count = ((id >> 12) & (PVK_MAX_QUERIES_PER_POOL - 1)) + 1;
        if (slot + count <= pool->count)
            memset((void *)(pool->written + slot), 0, count);
        return;
    }
    if (slot >= pool->count)
        return;
    pool->values[slot]  = ticks;
    pool->written[slot] = 1;
    iSignalSema(pool->sema);
}

static int all_written(PvkQueryPool pool, uint32_t first, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++)
        if (!pool->written[first + i])
            return 0;
    return 1;
}

PvkResult pvkGetQueryPoolResults(PvkDevice device, PvkQueryPool pool, uint32_t firstQuery, uint32_t queryCount,
                                 size_t dataSize, void *data, size_t stride, PvkQueryResultFlags flags)
{
    size_t width = flags & PVK_QUERY_RESULT_64_BIT_BIT ? 8 : 4;
    size_t each  = flags & PVK_QUERY_RESULT_WITH_AVAILABILITY_BIT ? 2 * width : width;
    uint32_t i;
    int ready = 1;

    (void)device;
    if (!query_range("pvkGetQueryPoolResults", pool, firstQuery, queryCount))
        return PVK_ERROR_VALIDATION_FAILED;
    if (flags & PVK_QUERY_RESULT_PARTIAL_BIT) {
        pvk_error("pvkGetQueryPoolResults: PVK_QUERY_RESULT_PARTIAL_BIT is not allowed for a timestamp pool\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (stride < each || (size_t)(queryCount - 1) * stride + each > dataSize) {
        pvk_error("pvkGetQueryPoolResults: %u slots of %u bytes at a stride of %u do not fit in %u bytes\n",
                  (unsigned)queryCount, (unsigned)each, (unsigned)stride, (unsigned)dataSize);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (flags & PVK_QUERY_RESULT_WAIT_BIT)
        while (!all_written(pool, firstQuery, queryCount))
            WaitSema(pool->sema);

    for (i = 0; i < queryCount; i++) {
        uint8_t *out = (uint8_t *)data + i * stride;
        int written  = pool->written[firstQuery + i];
        uint64_t v   = pool->values[firstQuery + i];
        uint64_t a   = written;

        if (!written)
            ready = 0;
        if (width == 8) {
            if (written)
                memcpy(out, &v, 8);
            if (flags & PVK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
                memcpy(out + 8, &a, 8);
        } else {
            uint32_t v32 = (uint32_t)v, a32 = (uint32_t)a;
            if (written)
                memcpy(out, &v32, 4);
            if (flags & PVK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
                memcpy(out + 4, &a32, 4);
        }
    }
    return ready ? PVK_SUCCESS : PVK_NOT_READY;
}
