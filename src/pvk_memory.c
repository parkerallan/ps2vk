/* Device memory: EE RAM and GS VRAM allocations, and the buffers placed in EE RAM. */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "pvk_internal.h"

#define VRAM_WORDS      (1u << 20)                /* 4 MB of 32-bit words */
#define VRAM_PAGE_WORDS 2048u                     /* an 8 KB GS page */

/* First-fit over the sorted list of taken ranges; sizes in words, page multiples. */
int pvk_vram_alloc(PvkDevice device, uint32_t words, uint32_t *address)
{
    uint32_t i, start = 0;

    if (device->vramCount == PVK_MAX_VRAM_ALLOCATIONS)
        return 0;
    for (i = 0; i <= device->vramCount; i++) {
        uint32_t end = i < device->vramCount ? device->vram[i].start : VRAM_WORDS;
        if (end - start >= words)
            break;
        if (i < device->vramCount)
            start = device->vram[i].start + device->vram[i].words;
    }
    if (i > device->vramCount)
        return 0;
    memmove(&device->vram[i + 1], &device->vram[i], (device->vramCount - i) * sizeof(device->vram[0]));
    device->vram[i].start = start;
    device->vram[i].words = words;
    device->vramCount++;
    *address = start;
    return 1;
}

void pvk_vram_free(PvkDevice device, uint32_t address)
{
    uint32_t i;

    for (i = 0; i < device->vramCount; i++)
        if (device->vram[i].start == address) {
            memmove(&device->vram[i], &device->vram[i + 1], (device->vramCount - i - 1) * sizeof(device->vram[0]));
            device->vramCount--;
            return;
        }
}

PvkResult pvkAllocateMemory(PvkDevice device, const PvkMemoryAllocateInfo *info, PvkDeviceMemory *memory)
{
    PvkDeviceMemory mem;

    if (info->memoryTypeIndex >= PVK_MAX_MEMORY_TYPES || info->allocationSize == 0) {
        pvk_error("pvkAllocateMemory: memoryTypeIndex %u must be 0 (EE RAM) or 1 (GS VRAM) and allocationSize nonzero\n",
                  (unsigned)info->memoryTypeIndex);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    mem = calloc(1, sizeof(struct PvkDeviceMemory_T));
    if (!mem)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    mem->typeIndex = info->memoryTypeIndex;
    mem->size      = info->allocationSize;

    if (mem->typeIndex == 0) {
        mem->data = memalign(64, (mem->size + 63) & ~(size_t)63);
        if (!mem->data) {
            pvk_error("pvkAllocateMemory: out of EE RAM for %u bytes\n", (unsigned)mem->size);
            free(mem);
            return PVK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    } else {
        uint32_t words = (uint32_t)((mem->size + 3) / 4);
        words = (words + VRAM_PAGE_WORDS - 1) & ~(VRAM_PAGE_WORDS - 1);
        mem->size = (size_t)words * 4;
        if (!pvk_vram_alloc(device, words, &mem->vram)) {
            pvk_error("pvkAllocateMemory: out of GS VRAM for %u bytes\n", (unsigned)mem->size);
            free(mem);
            return PVK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }

    *memory = mem;
    return PVK_SUCCESS;
}

void pvkFreeMemory(PvkDevice device, PvkDeviceMemory memory)
{
    if (!memory)
        return;
    if (memory->typeIndex == 0)
        free(memory->data);
    else
        pvk_vram_free(device, memory->vram);
    free(memory);
}

PvkResult pvkMapMemory(PvkDevice device, PvkDeviceMemory memory, size_t offset, size_t size, void **data)
{
    (void)device;
    if (memory->typeIndex != 0) {
        pvk_error("pvkMapMemory: GS VRAM is not host visible; fill images with pvkCmdCopyBufferToImage\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (size == PVK_WHOLE_SIZE && offset < memory->size)
        size = memory->size - offset;
    if (size == 0 || offset > memory->size || size > memory->size - offset) {
        pvk_error("pvkMapMemory: offset %u and size %u must be a nonempty part of the %u-byte allocation\n",
                  (unsigned)offset, (unsigned)size, (unsigned)memory->size);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    *data = (uint8_t *)memory->data + offset;
    return PVK_SUCCESS;
}

void pvkUnmapMemory(PvkDevice device, PvkDeviceMemory memory)
{
    /* The data cache is written back when a command buffer is submitted. */
    (void)device;
    (void)memory;
}

/* CONCURRENT needs at least two different families that exist. */
int pvk_sharing_check(const char *who, PvkSharingMode mode, uint32_t count, const uint32_t *families)
{
    uint32_t i, j;

    if (mode == PVK_SHARING_MODE_EXCLUSIVE)
        return 1;
    if (mode != PVK_SHARING_MODE_CONCURRENT || count < 2) {
        pvk_error("%s: sharingMode must be EXCLUSIVE, or CONCURRENT with at least 2 queue families\n", who);
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (families[i] >= PVK_MAX_QUEUE_FAMILIES) {
            pvk_error("%s: queue family %u does not exist\n", who, (unsigned)families[i]);
            return 0;
        }
        for (j = 0; j < i; j++)
            if (families[i] == families[j]) {
                pvk_error("%s: queue family %u is named twice\n", who, (unsigned)families[i]);
                return 0;
            }
    }
    return 1;
}

PvkResult pvkCreateBuffer(PvkDevice device, const PvkBufferCreateInfo *info, PvkBuffer *buffer)
{
    PvkBuffer buf;

    (void)device;

    if (!pvk_sharing_check("pvkCreateBuffer", info->sharingMode, info->queueFamilyIndexCount, info->queueFamilyIndices))
        return PVK_ERROR_VALIDATION_FAILED;
    buf = calloc(1, sizeof(struct PvkBuffer_T));
    if (!buf)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    buf->size = (info->size + 15) & ~(size_t)15;

    *buffer = buf;
    return PVK_SUCCESS;
}

void pvkDestroyBuffer(PvkDevice device, PvkBuffer buffer)
{
    (void)device;
    free(buffer);
}

void pvkGetBufferMemoryRequirements(PvkDevice device, PvkBuffer buffer, PvkMemoryRequirements *requirements)
{
    (void)device;
    /* A vertex binding is read in whole 16-byte units, up to 48 bytes past its last vertex. */
    requirements->size           = buffer->size + 48;
    requirements->alignment      = 64;
    requirements->memoryTypeBits = 1u << 0;
}

PvkResult pvkBindBufferMemory(PvkDevice device, PvkBuffer buffer, PvkDeviceMemory memory, size_t offset)
{
    (void)device;
    if (buffer->data) {
        pvk_error("pvkBindBufferMemory: the buffer is bound already\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (memory->typeIndex != 0 || (offset & 63) || offset > memory->size || buffer->size + 48 > memory->size - offset) {
        pvk_error("pvkBindBufferMemory: needs type 0 (EE RAM) memory, an offset that is a multiple of 64 and "
                  "%u bytes from it (%u-byte allocation, offset %u)\n", (unsigned)(buffer->size + 48),
                  (unsigned)memory->size, (unsigned)offset);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    buffer->memory = memory;
    buffer->data   = (uint8_t *)memory->data + offset;
    return PVK_SUCCESS;
}
