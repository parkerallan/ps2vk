/* Images: pixel memory in GS VRAM. Depth buffers and textures are created
 * here; framebuffers come from the swapchain. */
#include <stdlib.h>

#include "pvk_internal.h"

int pvk_gs_psm(PvkFormat format)
{
    switch (format) {
    case PVK_FORMAT_CT32: return GS_PSM_32;
    case PVK_FORMAT_Z32:  return GS_PSMZ_32;
    case PVK_FORMAT_Z24:  return GS_PSMZ_24;
    case PVK_FORMAT_Z16:  return GS_PSMZ_16;
    default:              return -1;
    }
}

float pvk_depth_max(PvkFormat format)
{
    switch (format) {
    case PVK_FORMAT_Z16: return 65535.0f;
    /* VU1's ftoi0 produces a signed 32-bit integer, so Z32 stops short of 2^31
     * (at a value a float holds exactly). */
    case PVK_FORMAT_Z32: return 2147483392.0f;
    default:             return 16777215.0f; /* Z24, and the range used without a depth buffer */
    }
}

/* Smallest n with 2^n >= v. */
static uint32_t log2_ceil(uint32_t v)
{
    uint32_t n = 0;
    while ((1u << n) < v)
        n++;
    return n;
}

void pvk_image_texture_params(PvkImage image)
{
    /* TEX0 addresses textures in 64-texel wide columns, so anything narrower
     * still occupies a 64-texel row in VRAM. */
    image->tbw = (image->fb.width < 64 ? 64 : image->fb.width) / 64;
    image->tw  = log2_ceil(image->fb.width);
    image->th  = log2_ceil(image->fb.height);
    if (!image->levels)
        image->levels = 1;
    image->levelAddress[0] = image->fb.address;
    image->levelTbw[0]     = image->tbw;
}

PvkResult pvkCreateImage(PvkDevice device, const PvkImageCreateInfo *info, PvkImage *image)
{
    PvkImage img;
    int psm, address, alignment, width;
    int depth = info->format == PVK_FORMAT_Z32 || info->format == PVK_FORMAT_Z24 || info->format == PVK_FORMAT_Z16;
    uint32_t levels = depth || !info->mipLevels ? 1 : info->mipLevels, l;

    (void)device;

    if (!depth && info->format != PVK_FORMAT_CT32) {
        pvk_error("pvkCreateImage: format must be PVK_FORMAT_CT32 (a texture) or a depth format "
                  "(PVK_FORMAT_Z32/Z24/Z16)\n");
        return PVK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (!depth) {
        /* The GS addresses a texture by log2 size (TEX0.TW/TH), so a texture is a
         * power of two on each side, up to the 1024 the GS samples. */
        if (info->width < 8 || info->height < 8 || info->width > 1024 || info->height > 1024 ||
            (info->width & (info->width - 1)) || (info->height & (info->height - 1))) {
            pvk_error("pvkCreateImage: a PVK_FORMAT_CT32 texture must be a power of two from 8 to 1024 "
                      "on each side (%ux%u given)\n", (unsigned)info->width, (unsigned)info->height);
            return PVK_ERROR_FEATURE_NOT_PRESENT;
        }
        /* MXL is 3 bits (levels 0 to 6); the smallest level keeps the 8-texel minimum. */
        if (levels > PVK_MAX_MIP_LEVELS || (info->width >> (levels - 1)) < 8 || (info->height >> (levels - 1)) < 8) {
            pvk_error("pvkCreateImage: %u mip levels, but a %ux%u texture has at most %u (each at least 8 "
                      "on each side, up to PVK_MAX_MIP_LEVELS)\n", (unsigned)levels, (unsigned)info->width,
                      (unsigned)info->height, (unsigned)PVK_MAX_MIP_LEVELS);
            return PVK_ERROR_VALIDATION_FAILED;
        }
    }
    psm = pvk_gs_psm(info->format);

    img = calloc(1, sizeof(struct PvkImage_T));
    if (!img)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;

    /* ZBUF addresses are in 8 KB pages, like FRAME; TEX0 addresses are in
     * 256-byte blocks. A texture narrower than 64 texels still takes 64-texel rows. */
    alignment = depth ? GRAPH_ALIGN_PAGE : GRAPH_ALIGN_BLOCK;
    width     = !depth && info->width < 64 ? 64 : (int)info->width;
    address   = graph_vram_allocate(width, (int)info->height, psm, alignment);
    if (address < 0) {
        pvk_error("out of GS memory for a %ux%u %s image\n", (unsigned)info->width,
                  (unsigned)info->height, depth ? "depth" : "texture");
        free(img);
        return PVK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    img->format     = info->format;
    img->fb.address = address;
    img->fb.width   = info->width;
    img->fb.height  = info->height;
    img->fb.psm     = psm;
    img->fb.mask    = 0;
    img->levels     = levels;
    pvk_image_texture_params(img);

    /* Each further level is its own block-aligned texture, half the size. */
    for (l = 1; l < levels; l++) {
        uint32_t w = info->width >> l, h = info->height >> l;
        address = graph_vram_allocate(w < 64 ? 64 : (int)w, (int)h, psm, GRAPH_ALIGN_BLOCK);
        if (address < 0) {
            pvk_error("out of GS memory for level %u of a %ux%u texture\n", (unsigned)l, (unsigned)info->width,
                      (unsigned)info->height);
            free(img);
            return PVK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        img->levelAddress[l] = (uint32_t)address;
        img->levelTbw[l]     = (w < 64 ? 64 : w) / 64;
    }

    *image = img;
    return PVK_SUCCESS;
}

void pvkDestroyImage(PvkDevice device, PvkImage image)
{
    /* graph_vram_free only rewinds the allocator, so the VRAM stays taken. */
    (void)device;
    free(image);
}
