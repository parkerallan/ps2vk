/* Images: pixel memory in GS VRAM. Depth buffers and textures are created
 * here; framebuffers come from the swapchain. */
#include <stdlib.h>

#include "pvk_internal.h"

int pvk_gs_psm(PvkFormat format)
{
    switch (format) {
    case PVK_FORMAT_CT32: return GS_PSM_32;
    case PVK_FORMAT_CT24: return GS_PSM_24;
    case PVK_FORMAT_CT16: return GS_PSM_16;
    case PVK_FORMAT_T8:   return GS_PSM_8;
    case PVK_FORMAT_T4:   return GS_PSM_4;
    case PVK_FORMAT_Z32:  return GS_PSMZ_32;
    case PVK_FORMAT_Z24:  return GS_PSMZ_24;
    case PVK_FORMAT_Z16:  return GS_PSMZ_16;
    default:              return -1;
    }
}

uint32_t pvk_format_bits(PvkFormat format)
{
    switch (format) {
    case PVK_FORMAT_CT32: return 32;
    case PVK_FORMAT_CT24: return 24;
    case PVK_FORMAT_CT16: return 16;
    case PVK_FORMAT_T8:   return 8;
    case PVK_FORMAT_T4:   return 4;
    default:              return 0;
    }
}

int pvk_is_color_format(PvkFormat format)
{
    return pvk_format_bits(format) != 0;
}

/* GS block numbering within a page: 8x4 blocks for CT32/CT24/T8, 4x8 for CT16/T4. */
static const uint8_t block_order_8x4[32] = {
     0,  1,  4,  5, 16, 17, 20, 21,
     2,  3,  6,  7, 18, 19, 22, 23,
     8,  9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
};
static const uint8_t block_order_4x8[32] = {
     0,  2,  8, 10,
     1,  3,  9, 11,
     4,  6, 12, 14,
     5,  7, 13, 15,
    16, 18, 24, 26,
    17, 19, 25, 27,
    20, 22, 28, 30,
    21, 23, 29, 31,
};

/* Blocks from a texture's first to the last one its rows touch. */
static uint32_t texture_blocks(uint32_t width, uint32_t height, PvkFormat format, uint32_t bufferWidth)
{
    uint32_t bits = pvk_format_bits(format);
    int tall = bits == 16 || bits == 4;
    uint32_t pw = bits >= 16 ? 64 : 128;
    uint32_t ph = bits >= 24 ? 32 : bits == 4 ? 128 : 64;
    uint32_t cols = tall ? 4 : 8, rows = tall ? 8 : 4;
    uint32_t bw = pw / cols, bh = ph / rows;
    const uint8_t *order = tall ? block_order_4x8 : block_order_8x4;
    uint32_t pagesAcross = (bufferWidth + pw - 1) / pw;
    uint32_t lastPage = ((height - 1) / ph) * pagesAcross + (width - 1) / pw;
    uint32_t lastW = width - ((width - 1) / pw) * pw, lastH = height - ((height - 1) / ph) * ph;
    uint32_t x, y, last = 0;

    for (y = 0; y < (lastH + bh - 1) / bh; y++)
        for (x = 0; x < (lastW + bw - 1) / bw; x++)
            if (order[y * cols + x] > last)
                last = order[y * cols + x];
    return lastPage * 32 + last + 1;
}

static int alloc_blocks(uint32_t blocks)
{
    return graph_vram_allocate(64, (int)blocks, GS_PSM_32, GRAPH_ALIGN_BLOCK);
}

/* TBW is in 64-texel units; 8- and 4-bit rows are at least a 128-texel page. */
static uint32_t buffer_width(uint32_t width, PvkFormat format)
{
    uint32_t min = pvk_format_bits(format) <= 8 ? 128 : 64;
    return width < min ? min : width;
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
    image->tbw = buffer_width(image->fb.width, image->format) / 64;
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
    int psm, address;
    int depth = info->format == PVK_FORMAT_Z32 || info->format == PVK_FORMAT_Z24 || info->format == PVK_FORMAT_Z16;
    int table = info->height == 1 && (info->format == PVK_FORMAT_CT32 || info->format == PVK_FORMAT_CT16);
    uint32_t levels = depth || table || !info->mipLevels ? 1 : info->mipLevels, l;

    (void)device;

    if (!depth && !pvk_is_color_format(info->format)) {
        pvk_error("pvkCreateImage: format must be a color format (PVK_FORMAT_CT32/CT24/CT16/T8/T4) or a depth "
                  "format (PVK_FORMAT_Z32/Z24/Z16)\n");
        return PVK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (table && info->width != 16 && info->width != 256) {
        pvk_error("pvkCreateImage: a color table has 16 or 256 entries (%u given)\n", (unsigned)info->width);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (!depth && !table) {
        /* The GS addresses a texture by log2 size (TEX0.TW/TH), so a texture is a
         * power of two on each side, up to the 1024 the GS samples. */
        if (info->width < 8 || info->height < 8 || info->width > 1024 || info->height > 1024 ||
            (info->width & (info->width - 1)) || (info->height & (info->height - 1))) {
            pvk_error("pvkCreateImage: a texture must be a power of two from 8 to 1024 on each side "
                      "(%ux%u given); a color table is 16 or 256 by 1\n", (unsigned)info->width,
                      (unsigned)info->height);
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

    /* ZBUF addresses are in 8 KB pages, TEX0 and CBP addresses in 256-byte blocks. */
    if (depth)
        address = graph_vram_allocate((int)info->width, (int)info->height, psm, GRAPH_ALIGN_PAGE);
    else if (table)
        address = alloc_blocks(texture_blocks(info->width == 256 ? 16 : 8, info->width == 256 ? 16 : 2,
                                              info->format, 64));
    else
        address = alloc_blocks(texture_blocks(info->width, info->height, info->format,
                                              buffer_width(info->width, info->format)));
    if (address < 0) {
        pvk_error("out of GS memory for a %ux%u %s image\n", (unsigned)info->width,
                  (unsigned)info->height, depth ? "depth" : table ? "color table" : "texture");
        free(img);
        return PVK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    img->format     = info->format;
    img->colorTable = table;
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
        address = alloc_blocks(texture_blocks(w, h, info->format, buffer_width(w, info->format)));
        if (address < 0) {
            pvk_error("out of GS memory for level %u of a %ux%u texture\n", (unsigned)l, (unsigned)info->width,
                      (unsigned)info->height);
            free(img);
            return PVK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        img->levelAddress[l] = (uint32_t)address;
        img->levelTbw[l]     = buffer_width(w, info->format) / 64;
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
