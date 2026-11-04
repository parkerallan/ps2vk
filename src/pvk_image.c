/* Images: pixel memory in GS VRAM. Depth buffers are created here; color
 * images come from the swapchain. */
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

PvkResult pvkCreateImage(PvkDevice device, const PvkImageCreateInfo *info, PvkImage *image)
{
    PvkImage img;
    int psm, address;

    (void)device;

    if (info->format != PVK_FORMAT_Z32 && info->format != PVK_FORMAT_Z24 && info->format != PVK_FORMAT_Z16) {
        pvk_error("pvkCreateImage: only depth formats (PVK_FORMAT_Z32/Z24/Z16) can be created; "
                  "color images come from the swapchain\n");
        return PVK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    psm = pvk_gs_psm(info->format);

    img = calloc(1, sizeof(struct PvkImage_T));
    if (!img)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;

    /* ZBUF addresses are in 8 KB pages, like FRAME. */
    address = graph_vram_allocate((int)info->width, (int)info->height, psm, GRAPH_ALIGN_PAGE);
    if (address < 0) {
        pvk_error("out of GS memory for a %ux%u depth image\n", (unsigned)info->width, (unsigned)info->height);
        free(img);
        return PVK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    img->format     = info->format;
    img->fb.address = address;
    img->fb.width   = info->width;
    img->fb.height  = info->height;
    img->fb.psm     = psm;
    img->fb.mask    = 0;

    *image = img;
    return PVK_SUCCESS;
}

void pvkDestroyImage(PvkDevice device, PvkImage image)
{
    /* graph_vram_free only rewinds the allocator, so the VRAM stays taken. */
    (void)device;
    free(image);
}
