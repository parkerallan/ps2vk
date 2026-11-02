/* Swapchain: framebuffers in GS VRAM, presented by switching DISPFB at vsync. */
#include <stdlib.h>

#include <gs_privileged.h>

#include "pvk_internal.h"

/* Sets the video mode; interlaced modes are field mode over a full-height framebuffer. */
static PvkResult set_display_mode(PvkSwapchain sc, PvkDisplayMode displayMode)
{
    int interlace, mode, ffmd;

    switch (displayMode) {
    case PVK_DISPLAY_MODE_AUTO:
        interlace = GRAPH_MODE_INTERLACED; mode = graph_get_region(); ffmd = GRAPH_MODE_FIELD;
        break;
    case PVK_DISPLAY_MODE_NTSC:
        interlace = GRAPH_MODE_INTERLACED; mode = GRAPH_MODE_NTSC; ffmd = GRAPH_MODE_FIELD;
        break;
    case PVK_DISPLAY_MODE_PAL:
        interlace = GRAPH_MODE_INTERLACED; mode = GRAPH_MODE_PAL; ffmd = GRAPH_MODE_FIELD;
        break;
    case PVK_DISPLAY_MODE_480P:
        interlace = GRAPH_MODE_NONINTERLACED; mode = GRAPH_MODE_HDTV_480P; ffmd = GRAPH_MODE_FRAME;
        break;
    default:
        pvk_error("swapchain displayMode %d is not a PvkDisplayMode\n", (int)displayMode);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    sc->interlaced = interlace == GRAPH_MODE_INTERLACED;

    /* What graph_initialize does, with the mode chosen here (no flicker filter). */
    graph_set_mode(interlace, mode, ffmd, 0);
    graph_set_screen(0, 0, (int)sc->width, (int)sc->height);
    graph_set_bgcolor(0, 0, 0);
    graph_set_framebuffer_filtered(sc->images[0].fb.address, (int)sc->width, GS_PSM_32, 0, 0);
    graph_enable_output();
    return PVK_SUCCESS;
}

PvkResult pvkCreateSwapchain(PvkDevice device, const PvkSwapchainCreateInfo *info, PvkSwapchain *swapchain)
{
    PvkSwapchain sc;
    uint32_t i;

    (void)device;

    if (info->format != PVK_FORMAT_CT32) {
        pvk_error("swapchain format must be PVK_FORMAT_CT32\n");
        return PVK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (info->imageCount != 2) {
        pvk_error("swapchain imageCount must be 2\n");
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }

    sc = calloc(1, sizeof(struct PvkSwapchain_T));
    if (!sc)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;

    sc->width      = info->width;
    sc->height     = info->height;
    sc->imageCount = info->imageCount;

    for (i = 0; i < sc->imageCount; i++) {
        framebuffer_t *fb = &sc->images[i].fb;
        int address = graph_vram_allocate(sc->width, sc->height, GS_PSM_32, GRAPH_ALIGN_PAGE);

        if (address < 0) {
            pvk_error("out of GS memory for swapchain image %u\n", (unsigned)i);
            free(sc);
            return PVK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        fb->address = address;
        fb->width   = sc->width;
        fb->height  = sc->height;
        fb->psm     = GS_PSM_32;
        fb->mask    = 0;
    }

    /* Sets the video mode and shows image 0. */
    {
        PvkResult res = set_display_mode(sc, info->displayMode);
        if (res != PVK_SUCCESS) {
            free(sc);
            return res;
        }
    }

    /* Image 0 is on screen, so the first image to render into is 1. */
    sc->nextImage = 1;

    *swapchain = sc;
    return PVK_SUCCESS;
}

void pvkDestroySwapchain(PvkDevice device, PvkSwapchain swapchain)
{
    (void)device;
    free(swapchain);
}

PvkResult pvkGetSwapchainImages(PvkDevice device, PvkSwapchain swapchain, uint32_t *count, PvkImage *images)
{
    uint32_t i;

    (void)device;

    if (!images) {
        *count = swapchain->imageCount;
        return PVK_SUCCESS;
    }
    for (i = 0; i < *count && i < swapchain->imageCount; i++)
        images[i] = &swapchain->images[i];
    *count = i;
    return PVK_SUCCESS;
}

PvkResult pvkAcquireNextImage(PvkDevice device, PvkSwapchain swapchain, uint32_t *imageIndex)
{
    (void)device;
    *imageIndex = swapchain->nextImage;
    return PVK_SUCCESS;
}

PvkResult pvkQueuePresent(PvkQueue queue, PvkSwapchain swapchain, uint32_t imageIndex)
{
    const framebuffer_t *fb = &swapchain->images[imageIndex].fb;

    /* No semaphores yet: make sure rendering into the image has finished. */
    pvkDeviceWaitIdle(queue->device);

    graph_wait_vsync();
    if (swapchain->interlaced) {
        /* Flip on even fields only (CSR.FIELD), so both fields show one frame. */
        while ((*GS_REG_CSR >> 13) & 1)
            graph_wait_vsync();
    }
    graph_set_framebuffer_filtered(fb->address, fb->width, fb->psm, 0, 0);

    swapchain->nextImage = (imageIndex + 1) % swapchain->imageCount;
    return PVK_SUCCESS;
}
