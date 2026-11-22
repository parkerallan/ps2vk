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

/* Completes an acquire whose image came free. Interrupts disabled, or from a handler. */
static void acquire_signal(PvkSwapchain sc, uint32_t image)
{
    struct PvkAcquire *a = &sc->acquires[image];

    if (!a->pending || a->flipsLeft)
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

/* A present went on screen: every waiting acquire is one flip closer, and may complete. */
static void acquires_flipped(PvkSwapchain sc)
{
    uint32_t i;

    for (i = 0; i < sc->imageCount; i++) {
        if (sc->acquires[i].pending && sc->acquires[i].flipsLeft)
            sc->acquires[i].flipsLeft--;
        acquire_signal(sc, i);
    }
}

/* Nonzero when the queued present at `index` has its semaphores signaled, counting
 * one signal for each older queued present that waits for the same semaphore. */
static int flip_ready(PvkSwapchain sc, uint32_t index)
{
    const struct PvkFlip *f = &sc->flips[index];
    uint32_t i, j, k;

    for (i = 0; i < f->waitCount; i++) {
        int need = 1;

        for (j = sc->flipHead; j != index; j = (j + 1) % PVK_MAX_PENDING_FLIPS)
            for (k = 0; k < sc->flips[j].waitCount; k++)
                need += sc->flips[j].waits[k] == f->waits[i];
        if (f->waits[i]->count < need)
            return 0;
    }
    return 1;
}

/* Points both GS read circuits (DISPFB1, DISPFB2) at the image's first line. */
static void show_image(PvkSwapchain sc, uint32_t image)
{
    const framebuffer_t *fb = &sc->images[image].fb;
    u64 dispfb = GS_SET_DISPFB(fb->address >> 11, fb->width >> 6, fb->psm, 0, 0);

    *GS_REG_DISPFB1 = dispfb;
    *GS_REG_DISPFB2 = dispfb;
    sc->displayed   = image;
}

/* Takes the oldest queued present off the queue, lowering its semaphores; with `show`,
 * DISPFB points at its image, without, the image is free again (MAILBOX replaced it). */
static void flip_take(PvkSwapchain sc, int show)
{
    struct PvkFlip *f = &sc->flips[sc->flipHead];
    uint32_t i;

    for (i = 0; i < f->waitCount; i++)
        f->waits[i]->count--;
    if (show) {
        show_image(sc, f->image);
        sc->late = 0;
    }
    sc->flipHead = (sc->flipHead + 1) % PVK_MAX_PENDING_FLIPS;
    iSignalSema(sc->flipSema);
    if (show)
        acquires_flipped(sc);
}

int pvk_swapchain_pump(PvkSwapchain sc)
{
    int did = 0;

    while (sc->flipHead != sc->flipTail && flip_ready(sc, sc->flipHead)) {
        uint32_t next = (sc->flipHead + 1) % PVK_MAX_PENDING_FLIPS;

        if (sc->presentMode == PVK_PRESENT_MODE_MAILBOX) {
            /* A newer present that is ready too replaces the oldest. */
            if (next == sc->flipTail || !flip_ready(sc, next))
                break;
            flip_take(sc, 0);
        } else if (sc->presentMode == PVK_PRESENT_MODE_IMMEDIATE
                   || (sc->presentMode == PVK_PRESENT_MODE_FIFO_RELAXED && sc->late)) {
            flip_take(sc, 1);
        } else {
            break;
        }
        did = 1;
    }
    return did;
}

/* INTC_VBLANK_S: shows the oldest queued present once its semaphores are signaled. */
static s32 vblank_handler(s32 cause)
{
    PvkSwapchain sc = handler_swapchain;

    (void)cause;
    if (!sc)
        goto out;
    sc->vblanks++;
    /* Flip on even fields only (CSR.FIELD), so both fields show one frame. */
    if (!(sc->interlaced && ((*GS_REG_CSR >> 13) & 1)) && sc->presentMode != PVK_PRESENT_MODE_IMMEDIATE) {
        if (sc->flipHead != sc->flipTail && flip_ready(sc, sc->flipHead))
            flip_take(sc, 1);
        else
            sc->late = 1;
    }
    /* Wakes a pvkAcquireNextImage that counts vblanks against its timeout. */
    iSignalSema(sc->flipSema);
    pvk_device_kick(sc->device);
out:
    ExitHandler();
    return 0;
}

void pvk_swapchain_wait_flips(PvkSwapchain swapchain)
{
    while (swapchain->flipHead != swapchain->flipTail)
        WaitSema(swapchain->flipSema);
}

/* A display mode's video signal: libgraph's mode, the picture's width in VCK units, its lines
 * per field, DISPLAY.DX and DY of a non-interlaced picture, and whether it has two fields. */
struct Signal {
    int mode, vck, lines, x, y, fields;
};

static int signal_of(PvkDisplayMode displayMode, struct Signal *s)
{
    static const struct Signal ntsc = { GRAPH_MODE_NTSC, 2560, 224, 652, 26, 1 };
    static const struct Signal pal  = { GRAPH_MODE_PAL, 2560, 256, 680, 37, 1 };
    static const struct Signal p480 = { GRAPH_MODE_HDTV_480P, 1440, 480, 232, 35, 0 };

    switch (displayMode) {
    case PVK_DISPLAY_MODE_AUTO: *s = graph_get_region() == GRAPH_MODE_PAL ? pal : ntsc; return 1;
    case PVK_DISPLAY_MODE_NTSC: *s = ntsc; return 1;
    case PVK_DISPLAY_MODE_PAL:  *s = pal;  return 1;
    case PVK_DISPLAY_MODE_480P: *s = p480; return 1;
    default:                    return 0;
    }
}

/* Checks the size against the display mode and works out GS DISPLAY: the image magnified by
 * whole factors (MAGH up to 16, MAGV up to 4) as far as the picture has room, and centered. */
static PvkResult display_setup(PvkSwapchain sc, const PvkSwapchainCreateInfo *info)
{
    struct Signal s;
    uint32_t w = info->width, h = info->height, maxWidth, lines, magh, magv, dy;

    if (!signal_of(info->displayMode, &s)) {
        pvk_error("swapchain displayMode %d is not a PvkDisplayMode\n", (int)info->displayMode);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* An image of at most one field's lines is shown non-interlaced: the whole image every vblank. */
    sc->interlaced = s.fields && h > (uint32_t)s.lines;
    lines    = sc->interlaced ? 2 * (uint32_t)s.lines : (uint32_t)s.lines;
    maxWidth = (uint32_t)s.vck < pvk_limits.maxFramebufferWidth ? (uint32_t)s.vck : pvk_limits.maxFramebufferWidth;
    if (!w || w % 64 || w > maxWidth || !h || h > lines) {
        pvk_error("swapchain size %ux%u: this display mode shows a width that is a multiple of 64 up to %u "
                  "and a height up to %u\n", (unsigned)w, (unsigned)h, (unsigned)maxWidth,
                  (unsigned)(s.fields ? 2 * s.lines : s.lines));
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    magh = (uint32_t)s.vck / w > 16 ? 16 : (uint32_t)s.vck / w;
    magv = lines / h > 4 ? 4 : lines / h;
    /* Two fields: DY counts lines of both, and an even offset keeps line 0 in the even field. */
    dy = sc->interlaced ? (uint32_t)(s.y - 1) * 2 + ((lines - h * magv) / 2 & ~1u) : (uint32_t)s.y + (lines - h * magv) / 2;
    sc->display   = GS_SET_DISPLAY((uint32_t)s.x + ((uint32_t)s.vck - w * magh) / 2, dy, magh - 1, magv - 1,
                                   w * magh - 1, h * magv - 1);
    sc->graphMode = s.mode;
    sc->vblankNs  = s.mode == GRAPH_MODE_PAL ? 20000000u : 16683350u; /* 50 Hz, 59.94 Hz */
    return PVK_SUCCESS;
}

/* Sets the video signal (no flicker filter) and DISPLAY, and shows image 0. */
static void display_apply(PvkSwapchain sc)
{
    graph_set_mode(sc->interlaced ? GRAPH_MODE_INTERLACED : GRAPH_MODE_NONINTERLACED, sc->graphMode,
                   sc->interlaced ? GRAPH_MODE_FIELD : GRAPH_MODE_FRAME, 0);
    *GS_REG_DISPLAY1 = sc->display;
    *GS_REG_DISPLAY2 = sc->display;
    graph_set_bgcolor(0, 0, 0);
    show_image(sc, 0);
    graph_enable_output();
}

PvkResult pvkCreateSwapchain(PvkDevice device, const PvkSwapchainCreateInfo *info, PvkSwapchain *swapchain)
{
    PvkSwapchain sc;
    uint32_t i;

    if (info->format != PVK_FORMAT_CT32) {
        pvk_error("swapchain format must be PVK_FORMAT_CT32\n");
        return PVK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (info->imageCount < 2 || info->imageCount > PVK_MAX_SWAPCHAIN_IMAGES) {
        pvk_error("swapchain imageCount must be 2 or %d\n", PVK_MAX_SWAPCHAIN_IMAGES);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    switch (info->presentMode) {
    case PVK_PRESENT_MODE_IMMEDIATE:
    case PVK_PRESENT_MODE_MAILBOX:
    case PVK_PRESENT_MODE_FIFO:
    case PVK_PRESENT_MODE_FIFO_RELAXED:
        break;
    default:
        pvk_error("swapchain presentMode %d is not a PvkPresentMode\n", (int)info->presentMode);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }

    sc = calloc(1, sizeof(struct PvkSwapchain_T));
    if (!sc)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;

    sc->device     = device;
    sc->width      = info->width;
    sc->height     = info->height;
    sc->imageCount = info->imageCount;
    sc->presentMode = info->presentMode;
    {
        PvkResult res = display_setup(sc, info);
        if (res != PVK_SUCCESS) {
            free(sc);
            return res;
        }
    }
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

    /* Image 0 is on screen, so the first image to render into is 1. */
    display_apply(sc);
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

/* An image nothing shows, no queued present will show and no acquire handed out, or imageCount. */
static uint32_t pick_free(PvkSwapchain sc)
{
    uint32_t i;

    for (i = 0; i < sc->imageCount; i++) {
        uint32_t image = (sc->nextImage + i) % sc->imageCount;
        if (!sc->acquired[image] && image_free(sc, image))
            return image;
    }
    return sc->imageCount;
}

/* The image that comes free first when presents are shown in order: the one on screen after one
 * flip, then the queued ones in turn. Skips those handed out; imageCount if none. Sets *flips. */
static uint32_t pick_next_free(PvkSwapchain sc, uint32_t *flips)
{
    uint32_t image = sc->displayed, i, n = 1;

    for (i = sc->flipHead; i != sc->flipTail; i = (i + 1) % PVK_MAX_PENDING_FLIPS, n++) {
        if (!sc->acquired[image]) {
            *flips = n;
            return image;
        }
        image = sc->flips[i].image;
    }
    return sc->imageCount;
}

PvkResult pvkAcquireNextImage(PvkDevice device, PvkSwapchain swapchain, uint64_t timeout, PvkSemaphore semaphore,
                              PvkFence fence, uint32_t *imageIndex)
{
    uint32_t image, flips = 0, start = swapchain->vblanks;
    struct PvkAcquire *a;
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

    /* A signal nobody waited for would end the next wait on the fence early. */
    while (fence && PollSema(fence->sema) >= 0)
        ;

    for (;;) {
        DI();
        image = pick_free(swapchain);
        if (image < swapchain->imageCount)
            break;
        /* Nothing queued will free an image: nothing to wait for. */
        if (swapchain->flipHead == swapchain->flipTail) {
            EI();
            return timeout ? PVK_TIMEOUT : PVK_NOT_READY;
        }
        if (swapchain->presentMode != PVK_PRESENT_MODE_MAILBOX) {
            /* Presents are shown in order, so the next image to come free is known now. */
            image = pick_next_free(swapchain, &flips);
            if (image < swapchain->imageCount)
                break;
            EI();
            pvk_error("pvkAcquireNextImage: every image is acquired already; present one first\n");
            return PVK_ERROR_VALIDATION_FAILED;
        }
        EI();
        /* MAILBOX: a flip or a replaced present frees an image, whichever is first; sleep until then. */
        if (!timeout)
            return PVK_NOT_READY;
        if (timeout != UINT64_MAX && (uint64_t)(swapchain->vblanks - start) * swapchain->vblankNs >= timeout)
            return PVK_TIMEOUT;
        WaitSema(swapchain->flipSema);
    }
    a = &swapchain->acquires[image];
    if (semaphore)
        pvk_semaphores_check("pvkAcquireNextImage", 1, &semaphore, 1, 1);
    if (fence)
        fence->pending = 1;
    a->pending   = 1;
    a->flipsLeft = flips;
    a->semaphore = semaphore;
    a->fence     = fence;
    swapchain->acquired[image] = 1;
    acquire_signal(swapchain, image);
    EI();

    swapchain->nextImage = (image + 1) % swapchain->imageCount;
    *imageIndex = image;
    return PVK_SUCCESS;
}

static PvkResult present_check(PvkQueue queue, const PvkPresentInfo *info)
{
    PvkSwapchain sc;

    if (queue->family != 0) {
        pvk_error("pvkQueuePresent: only the graphics queue (family 0) presents\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
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
    if (!sc->acquired[info->imageIndices[0]]) {
        pvk_error("pvkQueuePresent: image %u was not acquired with pvkAcquireNextImage\n", (unsigned)info->imageIndices[0]);
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

    /* Room for one flip per image; flips and replaced presents free them. */
    while ((sc->flipTail + 1) % PVK_MAX_PENDING_FLIPS == sc->flipHead)
        WaitSema(sc->flipSema);

    f        = &sc->flips[sc->flipTail];
    f->image = info->imageIndices[0];
    for (i = 0; i < info->waitSemaphoreCount; i++)
        f->waits[i] = info->waitSemaphores[i];
    f->waitCount = info->waitSemaphoreCount;
    DI();
    sc->acquired[f->image] = 0;
    sc->flipTail = (sc->flipTail + 1) % PVK_MAX_PENDING_FLIPS;
    /* IMMEDIATE, a late FIFO_RELAXED present or a MAILBOX replacement may act on it now. */
    pvk_device_kick(sc->device);
    EI();
    return PVK_SUCCESS;
}
