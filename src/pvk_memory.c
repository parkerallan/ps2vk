/* Buffers: plain EE RAM that DMA reads directly. */
#include <malloc.h>
#include <stdlib.h>

#include "pvk_internal.h"

PvkResult pvkCreateBuffer(PvkDevice device, const PvkBufferCreateInfo *info, PvkBuffer *buffer)
{
    PvkBuffer buf;

    (void)device;

    buf = calloc(1, sizeof(struct PvkBuffer_T));
    if (!buf)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;

    /* DMA needs 16-byte alignment; 64 keeps buffers on their own cache lines. */
    buf->size = (info->size + 15) & ~(size_t)15;
    buf->data = memalign(64, buf->size);
    if (!buf->data) {
        free(buf);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }

    *buffer = buf;
    return PVK_SUCCESS;
}

void pvkDestroyBuffer(PvkDevice device, PvkBuffer buffer)
{
    (void)device;
    if (!buffer)
        return;
    free(buffer->data);
    free(buffer);
}

PvkResult pvkMapMemory(PvkDevice device, PvkBuffer buffer, void **data)
{
    (void)device;
    *data = buffer->data;
    return PVK_SUCCESS;
}

void pvkUnmapMemory(PvkDevice device, PvkBuffer buffer)
{
    /* The data cache is written back when a command buffer is submitted. */
    (void)device;
    (void)buffer;
}
