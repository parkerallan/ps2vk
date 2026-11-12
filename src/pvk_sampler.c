/* Samplers: the filter, LOD and wrap fields of GS TEX1 and CLAMP, written with
 * the draw state for the texture bound next to them. */
#include <math.h>
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
    int linear = info->minFilter == PVK_FILTER_LINEAR, mipLinear = info->mipmapMode == PVK_SAMPLER_MIPMAP_MODE_LINEAR;
    /* K is signed 7.4 fixed point: 1/16 steps from -128 to 127.9375. */
    float k = roundf(info->mipLodBias * 16.0f);

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
    if (!(k >= -2048.0f && k <= 2047.0f)) {
        pvk_error("pvkCreateSampler: mipLodBias %f is outside TEX1.K's range, -128 to 127.9375\n", info->mipLodBias);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (info->minLod != 0.0f || !(info->maxLod >= 0.0f)) {
        pvk_error("pvkCreateSampler: minLod %f, maxLod %f; the GS only clamps the LOD from above, so minLod "
                  "must be 0 and maxLod at least 0\n", info->minLod, info->maxLod);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }

    s = calloc(1, sizeof(struct PvkSampler_T));
    if (!s)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;

    s->mmag = info->magFilter == PVK_FILTER_LINEAR ? LOD_MAG_LINEAR : LOD_MAG_NEAREST;
    s->mmin = linear ? LOD_MIN_LINEAR : LOD_MIN_NEAREST;
    s->mmip = linear ? (mipLinear ? LOD_MIN_LINE_MIPMAP_LINE : LOD_MIN_LINE_MIPMAP_NEAR)
                     : (mipLinear ? LOD_MIN_NEAR_MIPMAP_LINE : LOD_MIN_NEAR_MIPMAP_NEAR);
    s->maxLevel = info->maxLod >= (float)(PVK_MAX_MIP_LEVELS - 1) ? PVK_MAX_MIP_LEVELS - 1 : (uint32_t)info->maxLod;
    s->k    = (uint32_t)(int32_t)k & 0xFFF;
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
