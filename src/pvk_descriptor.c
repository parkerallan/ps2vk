/* Descriptor set layouts, pools and sets. A set holds the image each of its
 * combined image sampler bindings reads; the command buffer turns the bound
 * set into GS TEX0 with the draw state. */
#include <stdlib.h>
#include <string.h>

#include "pvk_internal.h"

int pvk_set_layout_compatible(const struct PvkDescriptorSetLayout_T *a, const struct PvkDescriptorSetLayout_T *b)
{
    return a->bindingCount == b->bindingCount &&
           memcmp(a->bindings, b->bindings, a->bindingCount * sizeof(a->bindings[0])) == 0;
}

int pvk_set_layout_find(const struct PvkDescriptorSetLayout_T *layout, uint32_t binding)
{
    uint32_t i;

    for (i = 0; i < layout->bindingCount; i++)
        if (layout->bindings[i].binding == binding)
            return (int)i;
    return -1;
}

PvkResult pvkCreateDescriptorSetLayout(PvkDevice device, const PvkDescriptorSetLayoutCreateInfo *info,
                                       PvkDescriptorSetLayout *layout)
{
    PvkDescriptorSetLayout l;
    uint32_t i, j;

    (void)device;

    if (info->bindingCount > PVK_MAX_DESCRIPTOR_BINDINGS) {
        pvk_error("descriptor set layout: %u bindings; at most %d\n", (unsigned)info->bindingCount,
                  PVK_MAX_DESCRIPTOR_BINDINGS);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    for (i = 0; i < info->bindingCount; i++) {
        const PvkDescriptorSetLayoutBinding *b = &info->bindings[i];

        if (b->descriptorType != PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            pvk_error("descriptor set layout: binding %u has type %d; only "
                      "PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER exists yet\n",
                      (unsigned)b->binding, (int)b->descriptorType);
            return PVK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (b->descriptorCount != 1) {
            pvk_error("descriptor set layout: binding %u has descriptorCount %u; binding arrays are "
                      "not supported, use 1\n", (unsigned)b->binding, (unsigned)b->descriptorCount);
            return PVK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (!b->stageFlags || b->stageFlags & ~(PVK_SHADER_STAGE_VERTEX | PVK_SHADER_STAGE_FRAGMENT)) {
            pvk_error("descriptor set layout: binding %u has stage flags 0x%x; use PvkShaderStage values\n",
                      (unsigned)b->binding, (unsigned)b->stageFlags);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        for (j = 0; j < i; j++) {
            if (info->bindings[j].binding == b->binding) {
                pvk_error("descriptor set layout: binding %u is listed twice\n", (unsigned)b->binding);
                return PVK_ERROR_VALIDATION_FAILED;
            }
        }
    }

    l = calloc(1, sizeof(struct PvkDescriptorSetLayout_T));
    if (!l)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;

    /* Keep them sorted by binding number, so equal layouts compare equal
     * whatever order the application listed them in. */
    for (i = 0; i < info->bindingCount; i++) {
        PvkDescriptorSetLayoutBinding b = info->bindings[i];
        for (j = i; j > 0 && l->bindings[j - 1].binding > b.binding; j--)
            l->bindings[j] = l->bindings[j - 1];
        l->bindings[j] = b;
    }
    l->bindingCount = info->bindingCount;

    *layout = l;
    return PVK_SUCCESS;
}

void pvkDestroyDescriptorSetLayout(PvkDevice device, PvkDescriptorSetLayout layout)
{
    (void)device;
    free(layout);
}

PvkResult pvkCreateDescriptorPool(PvkDevice device, const PvkDescriptorPoolCreateInfo *info, PvkDescriptorPool *pool)
{
    PvkDescriptorPool p;
    uint32_t i, images = 0;

    (void)device;

    if (info->maxSets == 0) {
        pvk_error("descriptor pool: maxSets must be at least 1\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    for (i = 0; i < info->poolSizeCount; i++) {
        if (info->poolSizes[i].type != PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            pvk_error("descriptor pool: pool size %u has type %d; only "
                      "PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER exists yet\n",
                      (unsigned)i, (int)info->poolSizes[i].type);
            return PVK_ERROR_FEATURE_NOT_PRESENT;
        }
        images += info->poolSizes[i].descriptorCount;
    }

    p = calloc(1, sizeof(struct PvkDescriptorPool_T));
    if (!p)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    p->sets = calloc(info->maxSets, sizeof(struct PvkDescriptorSet_T));
    if (!p->sets) {
        free(p);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }
    p->maxSets   = info->maxSets;
    p->maxImages = images;

    *pool = p;
    return PVK_SUCCESS;
}

void pvkDestroyDescriptorPool(PvkDevice device, PvkDescriptorPool pool)
{
    (void)device;
    if (!pool)
        return;
    free(pool->sets);
    free(pool);
}

PvkResult pvkResetDescriptorPool(PvkDevice device, PvkDescriptorPool pool)
{
    (void)device;
    pool->usedSets   = 0;
    pool->usedImages = 0;
    return PVK_SUCCESS;
}

PvkResult pvkAllocateDescriptorSets(PvkDevice device, const PvkDescriptorSetAllocateInfo *info, PvkDescriptorSet *sets)
{
    PvkDescriptorPool pool = info->descriptorPool;
    uint32_t i, images = 0;

    (void)device;

    for (i = 0; i < info->descriptorSetCount; i++)
        images += info->setLayouts[i]->bindingCount;

    if (pool->usedSets + info->descriptorSetCount > pool->maxSets || pool->usedImages + images > pool->maxImages) {
        pvk_error("pvkAllocateDescriptorSets: the pool has %u of %u sets and %u of %u combined image "
                  "samplers left; %u sets with %u are asked for\n",
                  (unsigned)(pool->maxSets - pool->usedSets), (unsigned)pool->maxSets,
                  (unsigned)(pool->maxImages - pool->usedImages), (unsigned)pool->maxImages,
                  (unsigned)info->descriptorSetCount, (unsigned)images);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for (i = 0; i < info->descriptorSetCount; i++) {
        PvkDescriptorSet s = &pool->sets[pool->usedSets++];
        s->layout = *info->setLayouts[i];
        memset(s->images, 0, sizeof(s->images));
        sets[i] = s;
    }
    pool->usedImages += images;
    return PVK_SUCCESS;
}

void pvkUpdateDescriptorSets(PvkDevice device, uint32_t writeCount, const PvkWriteDescriptorSet *writes)
{
    uint32_t i;

    (void)device;

    for (i = 0; i < writeCount; i++) {
        const PvkWriteDescriptorSet *w = &writes[i];
        int slot = pvk_set_layout_find(&w->dstSet->layout, w->dstBinding);

        if (slot < 0) {
            pvk_error("pvkUpdateDescriptorSets: write %u names binding %u, which the set's layout "
                      "does not have; skipped\n", (unsigned)i, (unsigned)w->dstBinding);
            continue;
        }
        if (w->descriptorType != w->dstSet->layout.bindings[slot].descriptorType) {
            pvk_error("pvkUpdateDescriptorSets: write %u has type %d but binding %u is type %d; skipped\n",
                      (unsigned)i, (int)w->descriptorType, (unsigned)w->dstBinding,
                      (int)w->dstSet->layout.bindings[slot].descriptorType);
            continue;
        }
        if (!w->imageInfo || !w->imageInfo->image || w->imageInfo->image->format != PVK_FORMAT_CT32) {
            pvk_error("pvkUpdateDescriptorSets: write %u needs imageInfo with a PVK_FORMAT_CT32 image; "
                      "skipped\n", (unsigned)i);
            continue;
        }
        w->dstSet->images[slot] = w->imageInfo->image;
    }
}
