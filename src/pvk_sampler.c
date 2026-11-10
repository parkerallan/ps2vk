/* Samplers: the filter and wrap fields of GS TEX1 and CLAMP, written with the
 * draw state for the texture bound next to them. */
#include <stdlib.h>

#include <draw_sampling.h>

#include "pvk_internal.h"

/* GS CLAMP wrap value for an address mode, or -1 when the GS has none. */
static int gs_wrap(PvkSamplerAddressMode mode)
{
    switch (mode) {
    case PVK_SAMPLER_ADDRESS_MODE_REPEAT:        return WRAP_REPEAT;
    case PVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return WRAP_CLAMP;
    default:                                     return -1;
    }
}

PvkResult pvkCreateSampler(PvkDevice device, const PvkSamplerCreateInfo *info, PvkSampler *sampler)
{
    PvkSampler s;
    int wms = gs_wrap(info->addressModeU), wmt = gs_wrap(info->addressModeV);

    (void)device;

    if (info->magFilter > PVK_FILTER_LINEAR || info->minFilter > PVK_FILTER_LINEAR ||
        info->mipmapMode > PVK_SAMPLER_MIPMAP_MODE_LINEAR) {
        pvk_error("pvkCreateSampler: magFilter %d, minFilter %d, mipmapMode %d; use PvkFilter and "
                  "PvkSamplerMipmapMode values\n", (int)info->magFilter, (int)info->minFilter,
                  (int)info->mipmapMode);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (info->addressModeU > PVK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        info->addressModeV > PVK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE) {
        pvk_error("pvkCreateSampler: addressModeU %d, addressModeV %d; use PvkSamplerAddressMode values\n",
                  (int)info->addressModeU, (int)info->addressModeV);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (wms < 0 || wmt < 0) {
        pvk_error("pvkCreateSampler: addressModeU %d, addressModeV %d; the GS only has "
                  "PVK_SAMPLER_ADDRESS_MODE_REPEAT and PVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE\n",
                  (int)info->addressModeU, (int)info->addressModeV);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }

    s = calloc(1, sizeof(struct PvkSampler_T));
    if (!s)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;

    /* One mip level (TEX1.MXL = 0), so MMIN is the plain nearest/linear value. */
    s->mmag = info->magFilter == PVK_FILTER_LINEAR ? LOD_MAG_LINEAR : LOD_MAG_NEAREST;
    s->mmin = info->minFilter == PVK_FILTER_LINEAR ? LOD_MIN_LINEAR : LOD_MIN_NEAREST;
    s->wms  = (uint32_t)wms;
    s->wmt  = (uint32_t)wmt;

    *sampler = s;
    return PVK_SUCCESS;
}

void pvkDestroySampler(PvkDevice device, PvkSampler sampler)
{
    (void)device;
    free(sampler);
}
