/* Internal structures shared by the ps2vk runtime. */
#ifndef PVK_INTERNAL_H
#define PVK_INTERNAL_H

#include <stdio.h>

#include <tamtypes.h>
#include <dma.h>
#include <draw.h>
#include <graph.h>
#include <gif_registers.h>
#include <gif_tags.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <packet2.h>
#include <packet2_utils.h>

#include "ps2vk.h"
#include "ps2vk_vu1_layout.h"

/* The GS primitive coordinate space is 4096x4096. Framebuffer pixel (0, 0)
 * sits at this coordinate (via XYOFFSET), leaving room on every side. */
#define PVK_GS_ORIGIN 1024.0f

#define PVK_DEFAULT_CMD_QWORDS 4096
#define PVK_MAX_SWAPCHAIN_IMAGES 2

#define pvk_error(...) printf("ps2vk: " __VA_ARGS__)

struct PvkInstance_T {
    int unused;
};

/* One in-order queue. Only one submission is in flight at a time: the GS
 * FINISH flag can't tell submissions apart, so submit waits for the last one. */
struct PvkQueue_T {
    PvkDevice device;
    int       busy;     /* a submission may still be running */
    PvkFence  fence;    /* fence to signal when it completes, or NULL */
};

void pvk_queue_wait_idle(PvkQueue queue);

struct PvkDevice_T {
    struct PvkQueue_T queue;
};

struct PvkImage_T {
    framebuffer_t fb;
};

struct PvkSwapchain_T {
    uint32_t          width, height;
    uint32_t          imageCount;
    uint32_t          nextImage;
    struct PvkImage_T images[PVK_MAX_SWAPCHAIN_IMAGES];
};

struct PvkBuffer_T {
    void  *data;     /* 64-byte aligned EE RAM, read by DMA */
    size_t size;
};

struct PvkShaderModule_T {
    const PvkShaderCode *code;
};

struct PvkPipeline_T {
    const PvkShaderCode *vs;
    const PvkShaderCode *fs;
    uint32_t inputQwords;     /* qwords per vertex in VU1 memory */
    uint32_t vertexStride;    /* bytes per vertex in the vertex buffer */
    uint32_t maxBatch;        /* vertices per VU1 run */
    u64      prim;            /* GS PRIM value for the GIF tag */
    float    context[PVK_VU1_CONTEXT_QWORDS * 4] __attribute__((aligned(16)));
};

struct PvkCommandBuffer_T {
    PvkDevice   device;
    packet2_t  *packet;       /* VIF1 DMA chain */
    PvkPipeline pipeline;
    PvkBuffer   vertexBuffer;
    size_t      vertexOffset;
    int         recording;
    int         rendering;
    int         failed;       /* a recording error makes the buffer unusable */
};

struct PvkFence_T {
    int pending;              /* submitted, not yet waited on */
    int signaled;
};

#endif /* PVK_INTERNAL_H */
