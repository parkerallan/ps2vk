/* Swapchain: framebuffers in GS VRAM, presented by switching DISPFB at vsync. */
#include <stdlib.h>

#include <gs_privileged.h>

#include "pvk_internal.h"

static PvkSwapchain handler_swapchain;

/* Nonzero when nothing shows the image and no queued flip will. */
static int image_free(PvkSwapchain sc, uint32_t image)
{
    uint32_t i;

    if (sc->displayed == image)
        return 0;
    for (i = sc->flipHead; i != sc->flipTail; i = (i + 1) % PVK_MAX_PENDING_FLIPS)
        if (sc->flips[i].image == image)
            return 0;
    return 1;
}

/* Completes an acquire whose image came free. Interrupts disabled, or from the handler. */
static void acquire_signal(PvkSwapchain sc, uint32_t image)
{
    struct PvkAcquire *a = &sc->acquires[image];

    if (!a->pending || !image_free(sc, image))
        return;
    a->pending = 0;
    if (a->semaphore)
        a->semaphore->count++;
    if (a->fence) {
        a->fence->pending  = 0;
        a->fence->signaled = 1;
        iSignalSema(a->fence->sema);
    }
}

/* INTC_VBLANK_S: flips to the oldest queued present once its semaphores are signaled. */
static s32 vblank_handler(s32 cause)
{
    PvkSwapchain sc = handler_swapchain;
    struct PvkFlip *f;
    uint32_t i, old;

    (void)cause;
    if (!sc || sc->flipHead == sc->flipTail)
        goto out;
    /* Flip on even fields only (CSR.FIELD), so both fields show one frame. */
    if (sc->interlaced && ((*GS_REG_CSR >> 13) & 1))
        goto out;
    f = &sc->flips[sc->flipHead];
    for (i = 0; i < f->waitCount; i++)
        if (f->waits[i]->count <= 0)
            goto out;
    for (i = 0; i < f->waitCount; i++)
        f->waits[i]->count--;
    {
        const framebuffer_t *fb = &sc->images[f->image].fb;
        graph_set_framebuffer_filtered(fb->address, fb->width, fb->psm, 0, 0);
    }
    old           = sc->displayed;
    sc->displayed = f->image;
    sc->flipHead  = (sc->flipHead + 1) % PVK_MAX_PENDING_FLIPS;
    iSignalSema(sc->flipSema);
    for (i = 0; i < sc->imageCount; i++)
        acquire_signal(sc, (old + i) % sc->imageCount);
    pvk_queue_kick(&sc->device->queue);
out:
    ExitHandler();
    return 0;
}

void pvk_swapchain_wait_flips(PvkSwapchain swapchain)
{
    while (swapchain->flipHead != swapchain->flipTail)
        WaitSema(swapchain->flipSema);
}

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

    sc->device     = device;
    sc->width      = info->width;
    sc->height     = info->height;
    sc->imageCount = info->imageCount;
    {
        ee_sema_t sema = { 0 };
        sema.init_count = 0;
        sema.max_count  = 1;
        sc->flipSema = CreateSema(&sema);
        if (sc->flipSema < 0) {
            free(sc);
            return PVK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    for (i = 0; i < sc->imageCount; i++) {
        framebuffer_t *fb = &sc->images[i].fb;
        uint32_t address;

        if (!pvk_vram_alloc(device, pvk_page_words(sc->width, sc->height, PVK_FORMAT_CT32), &address)) {
            pvk_error("out of GS memory for swapchain image %u\n", (unsigned)i);
            while (i--)
                pvk_vram_free(device, sc->images[i].fb.address);
            DeleteSema(sc->flipSema);
            free(sc);
            return PVK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        sc->images[i].format = PVK_FORMAT_CT32;
        sc->images[i].bound  = 1;
        sc->images[i].usage  = PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | PVK_IMAGE_USAGE_SAMPLED_BIT;
        fb->address = address;
        fb->width   = sc->width;
        fb->height  = sc->height;
        fb->psm     = GS_PSM_32;
        fb->mask    = 0;
        pvk_image_texture_params(&sc->images[i]);
    }

    /* Sets the video mode and shows image 0. */
    {
        PvkResult res = set_display_mode(sc, info->displayMode);
        if (res != PVK_SUCCESS) {
            pvkDestroySwapchain(device, sc);
            return res;
        }
    }

    /* Image 0 is on screen, so the first image to render into is 1. */
    sc->displayed = 0;
    sc->nextImage = 1;

    handler_swapchain = sc;
    sc->vblankHandler = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);
    device->swapchain = sc;

    *swapchain = sc;
    return PVK_SUCCESS;
}

void pvkDestroySwapchain(PvkDevice device, PvkSwapchain swapchain)
{
    uint32_t i;

    if (!swapchain)
        return;
    if (device->swapchain == swapchain) {
        pvk_queue_wait_idle(&device->queue);
        DisableIntc(INTC_VBLANK_S);
        RemoveIntcHandler(INTC_VBLANK_S, swapchain->vblankHandler);
        handler_swapchain = NULL;
        device->swapchain = NULL;
    }
    for (i = 0; i < swapchain->imageCount; i++)
        pvk_vram_free(device, swapchain->images[i].fb.address);
    DeleteSema(swapchain->flipSema);
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

PvkResult pvkAcquireNextImage(PvkDevice device, PvkSwapchain swapchain, uint64_t timeout, PvkSemaphore semaphore,
                              PvkFence fence, uint32_t *imageIndex)
{
    uint32_t image = swapchain->nextImage;
    struct PvkAcquire *a = &swapchain->acquires[image];
    PvkResult res;

    (void)device;

    if (!semaphore && !fence) {
        pvk_error("pvkAcquireNextImage: a semaphore or a fence is needed to know when the image is free\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (semaphore && (res = pvk_semaphores_check("pvkAcquireNextImage", 1, &semaphore, 1, 0)) != PVK_SUCCESS)
        return res;
    if (fence && (fence->pending || fence->signaled)) {
        pvk_error("pvkAcquireNextImage: fence is %s\n", fence->pending ? "in use" : "signaled; reset it first");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (a->pending) {
        pvk_error("pvkAcquireNextImage: every image is acquired already; present one first\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }

    DI();
    /* Nothing queued will take it off the screen: nothing to wait for. */
    if (swapchain->displayed == image && swapchain->flipHead == swapchain->flipTail) {
        EI();
        return timeout ? PVK_TIMEOUT : PVK_NOT_READY;
    }
    if (semaphore)
        pvk_semaphores_check("pvkAcquireNextImage", 1, &semaphore, 1, 1);
    if (fence)
        fence->pending = 1;
    a->pending   = 1;
    a->semaphore = semaphore;
    a->fence     = fence;
    acquire_signal(swapchain, image);
    EI();

    swapchain->nextImage = (image + 1) % swapchain->imageCount;
    *imageIndex = image;
    return PVK_SUCCESS;
}

static PvkResult present_check(PvkQueue queue, const PvkPresentInfo *info)
{
    PvkSwapchain sc;

    if (info->swapchainCount != 1) {
        pvk_error("pvkQueuePresent: swapchainCount must be 1; the device has one swapchain\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    sc = info->swapchains[0];
    if (sc != queue->device->swapchain) {
        pvk_error("pvkQueuePresent: the swapchain was not created on this queue's device\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (info->imageIndices[0] >= sc->imageCount) {
        pvk_error("pvkQueuePresent: image index %u is out of range\n", (unsigned)info->imageIndices[0]);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    return pvk_semaphores_check("pvkQueuePresent", info->waitSemaphoreCount, info->waitSemaphores, 0, 0);
}

PvkResult pvkQueuePresent(PvkQueue queue, const PvkPresentInfo *info)
{
    PvkSwapchain sc;
    struct PvkFlip *f;
    uint32_t i;
    PvkResult res;

    res = present_check(queue, info);
    if (info->results && info->swapchainCount)
        info->results[0] = res;
    if (res != PVK_SUCCESS)
        return res;
    sc = info->swapchains[0];
    pvk_semaphores_check("pvkQueuePresent", info->waitSemaphoreCount, info->waitSemaphores, 0, 1);

    /* Room for one flip per image; the vblank handler frees them. */
    while ((sc->flipTail + 1) % PVK_MAX_PENDING_FLIPS == sc->flipHead)
        WaitSema(sc->flipSema);

    f        = &sc->flips[sc->flipTail];
    f->image = info->imageIndices[0];
    for (i = 0; i < info->waitSemaphoreCount; i++)
        f->waits[i] = info->waitSemaphores[i];
    f->waitCount = info->waitSemaphoreCount;
    DI();
    sc->flipTail = (sc->flipTail + 1) % PVK_MAX_PENDING_FLIPS;
    EI();
    return PVK_SUCCESS;
}
