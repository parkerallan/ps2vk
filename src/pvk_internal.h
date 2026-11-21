/* Internal structures shared by the ps2vk runtime. */
#ifndef PVK_INTERNAL_H
#define PVK_INTERNAL_H

#include <stdio.h>

#include <tamtypes.h>
#include <kernel.h>
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

/* VU1 input memory a batch can fill, after the header. */
#define PVK_BATCH_INPUT_QWORDS (PVK_VU1_OUTPUT_ADDR - PVK_VU1_INPUT_ADDR - PVK_VU1_INPUT_HEADER)
#define PVK_MAX_SWAPCHAIN_IMAGES 2
#define PVK_MAX_PENDING_FLIPS (PVK_MAX_SWAPCHAIN_IMAGES + 1)

#define pvk_error(...) printf("ps2vk: " __VA_ARGS__)

/* The one PS2; its properties are constants. */
struct PvkPhysicalDevice_T {
    int unused;
};

struct PvkInstance_T {
    struct PvkPhysicalDevice_T physicalDevice;
};

/* What pvkGetPhysicalDeviceProperties reports; every size check reads it. */
extern const PvkPhysicalDeviceLimits pvk_limits;

/* Signaled by the GS SIGNAL at the end of the submission it was passed to,
 * or by the vblank handler when an acquired swapchain image leaves the screen. */
struct PvkFence_T {
    s32          sema;      /* EE semaphore the handler releases */
    volatile int pending;   /* submitted or given to an acquire, not signaled yet */
    volatile int signaled;
};

/* Binary semaphore. `count` is the GPU-side state: raised by the completing handler, lowered when the
 * waiting operation starts. `balance` is the same bookkeeping at API-call time, for validation. */
struct PvkSemaphore_T {
    volatile int count;
    int          balance;
};

#define PVK_MAX_SUBMIT_SEMAPHORES  4
#define PVK_MAX_PENDING_SUBMISSIONS 8

/* One command buffer's chain, queued on the VIF1 channel. Its SIGNAL
 * carries the ring index; the INTC_GS handler frees it. */
struct PvkSubmission {
    PvkCommandBuffer cmd;                               /* NULL: a batch of semaphore operations alone */
    PvkFence     fence;                                 /* of the last chain of the batch, else NULL */
    PvkSemaphore waits[PVK_MAX_SUBMIT_SEMAPHORES];      /* of the first chain of a submit */
    uint32_t     waitCount;
    PvkSemaphore signals[PVK_MAX_SUBMIT_SEMAPHORES];    /* of the last chain of a submit */
    uint32_t     signalCount;
    volatile int state;                                 /* 0 free, 1 queued, 2 chain sent */
};

/* One in-order queue. Chains wait in a ring and are started from the DMAC and vblank handlers once
 * the channel is free and their semaphores are signaled; the INTC_GS handler completes each chain,
 * finding the queue and entry in the SIGNAL ID (entry | family << 8) it reads back from SIGLBLID. */
struct PvkQueue_T {
    PvkDevice device;
    uint32_t  family;       /* 0: VIF1 channel (PATH2), 1: GIF channel (PATH3) */
    volatile u32 *chcr, *qwc, *tadr; /* the channel's registers */
    PvkFence  last;         /* fence of the newest submission; NULL = idle */
    struct PvkFence_T idle; /* stands in when a submission has no fence */
    struct PvkSubmission ring[PVK_MAX_PENDING_SUBMISSIONS];
    volatile uint32_t head, tail; /* ring[head..tail) are in flight */
    s32       room;         /* EE semaphore released whenever a ring entry frees */
};

void      pvk_queue_wait_idle(PvkQueue queue);
void      pvk_queue_kick(PvkQueue queue); /* interrupts disabled, or from a handler */
/* Kicks every queue the device has: a completed chain or flip may unblock either. */
void      pvk_device_kick(PvkDevice device);
/* Waits for every queue and the swapchain's flips. */
void      pvk_device_wait_idle(PvkDevice device);
PvkResult pvk_fence_init(PvkFence fence, int signaled);
void      pvk_fence_wait(PvkFence fence);
PvkResult pvk_queue_init(PvkQueue queue, uint32_t family);
void      pvk_queue_shutdown(PvkQueue queue);
/* Validates a wait (signal = 0) or signal list against the semaphores' balances, applying it when `apply`. */
PvkResult pvk_semaphores_check(const char *who, uint32_t count, const PvkSemaphore *sems, int signal, int apply);
/* Sleeps until every queued present has flipped. */
void      pvk_swapchain_wait_flips(PvkSwapchain swapchain);

#define PVK_MAX_VRAM_ALLOCATIONS 64

/* A taken range of GS VRAM, in 32-bit words (the GS's address unit). */
struct PvkVramRange {
    uint32_t start, words;
};

struct PvkDevice_T {
    struct PvkQueue_T queue;    /* graphics, family 0 */
    struct PvkQueue_T transfer; /* family 1 */
    int       families[PVK_MAX_QUEUE_FAMILIES]; /* asked for at pvkCreateDevice */
    PvkSwapchain swapchain;   /* the one swapchain, or NULL: pvk_queue_wait_idle drains its flips */
    PvkPhysicalDeviceFeatures features;
    uint32_t vramCount;       /* taken VRAM ranges, sorted by start: swapchain framebuffers and type 1 allocations */
    struct PvkVramRange vram[PVK_MAX_VRAM_ALLOCATIONS];
    PvkQueryPool queryPools[PVK_MAX_QUERY_POOLS]; /* by the number a SIGNAL ID carries, or NULL */
};

/* Takes `words` words of VRAM (a multiple of a page) at the first fit; 0 when none. */
int  pvk_vram_alloc(PvkDevice device, uint32_t words, uint32_t *address);
void pvk_vram_free(PvkDevice device, uint32_t address);

/* Timestamp slots the INTC_GS handler fills: a SIGNAL ID with bit 31 set names a pool and slot. */
struct PvkQueryPool_T {
    PvkDevice device;
    uint32_t  index;             /* its number in device->queryPools */
    uint32_t  count;
    s32       sema;              /* EE semaphore released whenever a slot is written */
    volatile uint32_t *values;   /* EE COP0 Count at the SIGNAL */
    volatile uint8_t  *written;
};

#define PVK_QUERY_SIGNAL_BIT 0x80000000u
#define PVK_QUERY_RESET_BIT  0x40000000u
/* Timestamp ID: pool number in bits 24-27, slot in bits 0-11. A reset ID adds the slot count - 1 in bits 12-23. */
#define PVK_QUERY_ID(pool, slot)               (PVK_QUERY_SIGNAL_BIT | (pool)->index << 24 | (slot))
#define PVK_QUERY_RESET_ID(pool, first, count) (PVK_QUERY_ID(pool, first) | PVK_QUERY_RESET_BIT | ((count) - 1) << 12)
/* From the INTC_GS handler: stores `ticks` in the slot, or clears the range, the ID names. */
void pvk_query_signal(PvkDevice device, uint32_t id, uint32_t ticks);

/* One allocation: EE RAM (type 0, `data`) or GS VRAM (type 1, `vram` word address). */
struct PvkDeviceMemory_T {
    uint32_t typeIndex;
    size_t   size;
    void    *data;
    uint32_t vram;
};

/* Color images keep libdraw's framebuffer_t (the swapchain presents from it);
 * depth images reuse its address/width/height with a GS_PSMZ_* psm. A color
 * image can also be sampled: TEX0 takes its address, texture buffer width
 * and log2 size; MIPTBP1/2 the addresses and widths of the other levels. */
struct PvkImage_T {
    PvkFormat     format;
    uint32_t      usage;      /* PvkImageUsageFlagBits */
    int           colorTable; /* a palette of fb.width entries at TEX0.CBP */
    PvkDeviceMemory memory;   /* bound with pvkBindImageMemory; NULL for a swapchain image */
    int           bound;      /* has VRAM: bound, or a swapchain image */
    uint32_t      alignWords; /* VRAM alignment of each level: a page for attachments, a block otherwise */
    uint32_t      levelWords[PVK_MAX_MIP_LEVELS]; /* VRAM words each level takes */
    framebuffer_t fb;         /* level 0 */
    uint32_t      tbw;        /* GS TEX0.TBW / BITBLTBUF.DBW: buffer width in 64-texel units */
    uint32_t      tw, th;     /* GS TEX0.TW/TH: log2 of the width and height */
    uint32_t      levels;     /* mip levels, 1 to PVK_MAX_MIP_LEVELS */
    uint32_t      levelAddress[PVK_MAX_MIP_LEVELS]; /* VRAM word address of each level ([0] = fb.address) */
    uint32_t      levelTbw[PVK_MAX_MIP_LEVELS];     /* buffer width of each level ([0] = tbw) */
};

/* GS psm for a PvkFormat, or -1. */
int pvk_gs_psm(PvkFormat format);
/* Bits per texel of a color format, 0 otherwise. */
uint32_t pvk_format_bits(PvkFormat format);
int pvk_is_color_format(PvkFormat format);
/* VRAM words a width x height framebuffer or depth buffer of that format takes, in whole pages. */
uint32_t pvk_page_words(uint32_t width, uint32_t height, PvkFormat format);
/* Fills tbw/tw/th from fb.width and fb.height, and level 0's entries. */
void pvk_image_texture_params(PvkImage image);

/* A range of an image's mip levels: what a set samples or rendering targets. */
struct PvkImageView_T {
    PvkImage image;
    uint32_t baseLevel;
    uint32_t levels;
};

/* The view's base level as a framebuffer_t: FRAME address, width (FBW and SCISSOR), height, psm. */
void pvk_view_framebuffer(PvkImageView view, framebuffer_t *fb);
/* Largest depth value a depth format stores; depth is flipped so near = this. */
float pvk_depth_max(PvkFormat format);
/* The VU1 context's viewport scale and offset qwords (8 floats) for a viewport and depth format. */
void pvk_viewport_context(const PvkViewport *vp, PvkFormat depthFormat, float *ctx);

/* A queued pvkQueuePresent: the vblank handler flips to `image` once its semaphores are signaled. */
struct PvkFlip {
    uint32_t     image;
    PvkSemaphore waits[PVK_MAX_SUBMIT_SEMAPHORES];
    uint32_t     waitCount;
};

/* A pvkAcquireNextImage whose image is still on screen or has a flip queued onto it. */
struct PvkAcquire {
    int          pending;
    PvkSemaphore semaphore;
    PvkFence     fence;
};

struct PvkSwapchain_T {
    PvkDevice         device;
    uint32_t          width, height;
    uint32_t          imageCount;
    uint32_t          nextImage;
    int               interlaced;  /* flip only on even fields, so both fields show one frame */
    volatile uint32_t displayed;   /* image DISPFB points at */
    struct PvkFlip    flips[PVK_MAX_PENDING_FLIPS];
    volatile uint32_t flipHead, flipTail;
    struct PvkAcquire acquires[PVK_MAX_SWAPCHAIN_IMAGES];
    s32               flipSema;    /* released by the vblank handler after each flip */
    s32               vblankHandler;
    struct PvkImage_T images[PVK_MAX_SWAPCHAIN_IMAGES];
};

/* Validates a buffer's or image's sharing mode and queue family list. */
int pvk_sharing_check(const char *who, PvkSharingMode mode, uint32_t count, const uint32_t *families);

struct PvkBuffer_T {
    PvkDeviceMemory memory; /* bound with pvkBindBufferMemory, NULL until then */
    void  *data;     /* 64-byte aligned EE RAM, read by DMA; NULL until bound */
    size_t size;
};

struct PvkShaderModule_T {
    const PvkShaderCode *code;
};

/* Bindings sorted by binding number. Copied by value into pipeline layouts
 * and sets, so the handle may be destroyed after they are made. */
struct PvkDescriptorSetLayout_T {
    uint32_t                      bindingCount;
    PvkDescriptorSetLayoutBinding bindings[PVK_MAX_DESCRIPTOR_BINDINGS];
};

/* Nonzero if the two layouts have the same bindings. */
int pvk_set_layout_compatible(const struct PvkDescriptorSetLayout_T *a, const struct PvkDescriptorSetLayout_T *b);
/* Index into bindings[] of the binding with that number, or -1. */
int pvk_set_layout_find(const struct PvkDescriptorSetLayout_T *layout, uint32_t binding);

/* The sampler's settings as GS register fields. MMIN depends on whether the
 * bound image has levels to pick from, so it is chosen at draw time. */
struct PvkSampler_T {
    uint32_t mmag;       /* TEX1.MMAG: LOD_MAG_x */
    uint32_t mmin;       /* TEX1.MMIN with one level: LOD_MIN_x */
    uint32_t mmip;       /* TEX1.MMIN with levels: LOD_MIN_x_MIPMAP_x */
    uint32_t maxLevel;   /* TEX1.MXL cap from maxLod */
    uint32_t k;          /* TEX1.K: mipLodBias as signed 7.4 fixed point */
    uint32_t wms, wmt;   /* CLAMP.WMS/WMT: WRAP_* */
    uint32_t fst;        /* PRIM.FST: coordinates in texels, stored as UV */
};

/* PRIM.FST, ORed into a pipeline's prim for a draw with such a sampler. */
#define PVK_PRIM_FST (1ULL << 8)
/* PRIM.CTXT: the draw uses the GS's second register set (FRAME_2, ZBUF_2, TEST_2, ...). */
#define PVK_PRIM_CTXT (1ULL << 9)

/* What one bindings[] slot of a set holds: an image and sampler for a combined
 * image sampler, a buffer range for a uniform buffer. All NULL until written. */
struct PvkDescriptorSlot {
    PvkImageView view;
    PvkImage   colorTable; /* for a T8/T4 image */
    PvkSampler sampler;
    PvkBuffer  buffer;
    size_t    offset;
    size_t    range;
};

struct PvkDescriptorSet_T {
    struct PvkDescriptorSetLayout_T layout;
    struct PvkDescriptorSlot slots[PVK_MAX_DESCRIPTOR_BINDINGS];
};

/* Sets are handed out in order and only come back together, on reset. */
struct PvkDescriptorPool_T {
    uint32_t maxSets, usedSets;
    uint32_t maxImages, usedImages;   /* combined image samplers */
    uint32_t maxBuffers, usedBuffers; /* uniform buffers */
    struct PvkDescriptorSet_T *sets;  /* maxSets of them */
};

/* Which 4-byte words of the 128-byte push constant range each stage may
 * read, one bit per word, plus the set layouts. Two layouts with equal masks
 * are push-constant compatible in Vulkan's sense. */
struct PvkPipelineLayout_T {
    uint32_t pushWords[2];    /* [0] vertex stage, [1] fragment stage */
    uint32_t setLayoutCount;
    struct PvkDescriptorSetLayout_T setLayouts[PVK_MAX_DESCRIPTOR_SETS];
};

/* Bits of a layout's word mask for the bytes [offset, offset + size). */
uint32_t pvk_push_word_mask(uint32_t offset, uint32_t size);
/* Nonzero if the two layouts have the same push constant ranges. */
int pvk_layout_push_compatible(const struct PvkPipelineLayout_T *a, const struct PvkPipelineLayout_T *b);

/* One vertex buffer binding of a pipeline: a stream the VIF unpacks with one
 * format, one element per vertex or per instance. */
struct PvkVertexBinding {
    int      used;            /* declared, and some shader input reads it */
    int      instance;        /* advances per instance, not per vertex */
    uint32_t stride;          /* bytes per element */
    uint32_t count;           /* attributes (consecutive shader inputs) it holds */
    uint32_t slot;            /* VU1 qword of its first attribute within a vertex (or an instance) */
    uint32_t mode;            /* VIF UNPACK format (P2_UNPACK_*) */
    uint32_t usn;             /* unsigned 8/16-bit data */
    uint32_t align;           /* vertices per whole 16-byte unit: 16 / gcd(16, stride) */
    uint32_t chunk;           /* most vertices per UNPACK code, a multiple of align */
};

/* One patched copy of a vertex shader's VU1 code, shared by the pipelines
 * whose patching came out identical and by the cache that holds it. */
struct PvkProgram {
    const PvkShaderCode *vs;
    uint32_t *code;
    uint32_t *codeEnd;
    uint32_t  refs;           /* pipelines and caches referring to it */
    struct PvkProgram *next;  /* next in its cache */
};

struct PvkPipelineCache_T {
    struct PvkProgram *programs;
};

struct PvkPipeline_T {
    const PvkShaderCode *vs;
    const PvkShaderCode *fs;
    PvkPipelineLayout    layout;
    struct PvkProgram *program; /* its VU1 code: vs's with the input conversions and specialization constants patched in */
    uint32_t samplerSet;      /* when fs->textured: set index and bindings[] slot of its sampler2D */
    uint32_t samplerSlot;
    uint32_t uniformSlots[PVK_MAX_UNIFORM_BLOCKS]; /* bindings[] slot of each vs->uniformBlocks[] entry in its set layout */
    uint32_t vertexQwords;    /* qwords per vertex in VU1 memory: the vertex-rate inputs */
    uint32_t instanceQwords;  /* qwords per instance: the instance-rate inputs, 0 = none */
    uint32_t outputQwords;    /* qwords per vertex VU1 emits: RGBAQ, XYZ2, plus ST when textured */
    struct PvkVertexBinding vertexBindings[PVK_MAX_VERTEX_BINDINGS];
    uint32_t align;           /* batches start on a vertex that is a multiple of this: lcm of the vertex-rate bindings' align */
    uint32_t instanceAlign;   /* the same for the instance-rate bindings */
    uint32_t inputBudget;     /* input qwords a batch's vertices and indices may take: room for one instance is kept */
    uint32_t maxBatch;        /* vertices per VU1 run, a whole number of list primitives */
    uint32_t primStep;        /* vertices a draw grows by: the list primitive's size, 1 for strips and fans */
    uint32_t primOverlap;     /* vertices a strip or fan batch repeats from the previous batch */
    int      fan;             /* repeats the first vertex of the draw, not the one before the overlap */
    int      strip;           /* a triangle strip: the winding flips every triangle */
    float    cullSign;        /* VU1 culls a triangle when its signed area * cullSign < 0; 0 = no culling */
    int      cullAll;         /* FRONT_AND_BACK on triangles: draws are skipped */
    u64      prim;            /* GS PRIM value for the GIF tag; CTXT = zmsk */
    PvkFormat depthFormat;    /* depth attachment it draws with, or UNDEFINED */
    uint32_t ztst;            /* GS TEST.ZTST: 0 never, 1 always, 2 >=, 3 > */
    uint32_t zmsk;            /* GS context it draws in: 2 (ZBUF_2.ZMSK = 1) when it writes no depth, else 1 */
    u64      alpha;           /* GS ALPHA */
    int      blendFix;        /* ALPHA.C = FIX: the blend constants' alpha */
    uint32_t dynamic;         /* 1 << PvkDynamicState for each state set while recording */
    PvkRect2D scissor;        /* static scissor; zero size = the attachment level */
    float    context[PVK_VU1_CONTEXT_QWORDS * 4] __attribute__((aligned(16)));
};

/* One block of EE RAM; recordings are stacked in it from the bottom. */
struct PvkCommandPool_T {
    PvkDevice  device;
    uint32_t   flags;         /* PvkCommandPoolCreateFlagBits */
    uint32_t   family;        /* queue family its command buffers are submitted to */
    qword_t   *block;         /* 64-byte aligned */
    uint32_t   qwords;        /* its size */
    uint32_t   used;          /* qwords below the top of the stack */
    PvkCommandBuffer recording; /* the command buffer open at the top, or NULL */
    PvkCommandBuffer buffers; /* allocated from it, linked through `next` */
};

/* A secondary command buffer a primary calls, and which of its recordings. */
struct PvkExecuted {
    PvkCommandBuffer cmd;
    uint32_t         generation;
};

struct PvkCommandBuffer_T {
    PvkDevice   device;
    PvkCommandPool pool;
    PvkCommandBuffer next;    /* next in pool->buffers */
    PvkCommandBufferLevel level;
    uint32_t    start;        /* qword offset of its recording in pool->block */
    uint32_t    size;         /* qwords recorded; 0 = holds no recording */
    uint32_t    generation;   /* counts its recordings */
    uint32_t    usage;        /* PvkCommandBufferUsageFlagBits of the recording */
    int         used;         /* submitted or executed since it was recorded */
    struct PvkExecuted *executed; /* the secondaries a primary calls, checked at submit */
    uint32_t    executedCount, executedCap;
    packet2_t   packet;       /* VIF1 (family 0) or GIF channel (family 1) DMA chain, inside the pool's block */
    uint32_t    signalQw;     /* qword of the GS SIGNAL at a primary's end; its ID is set at submit */
    PvkPipeline pipeline;
    const struct PvkProgram *program; /* the program last uploaded to VU1 in this recording, or NULL */
    PvkImageView colorView;   /* attachments of the current pvkCmdBeginRendering; NULL when inherited */
    PvkImageView depthView;   /* or NULL */
    int         inherits;     /* a secondary with RENDER_PASS_CONTINUE: draws into the primary's attachments */
    PvkFormat   inheritedDepthFormat;
    int         secondaryContents; /* the rendering takes its draws from pvkCmdExecuteCommands only */
    int         gsStateDirty; /* TEST/ALPHA/SCISSOR/TEX0 must be re-sent before the next draw */
    int         uniformsDirty; /* the uniform blocks' buffers must be sent to VU1 before the next draw */
    PvkDescriptorSet sets[PVK_MAX_DESCRIPTOR_SETS]; /* bound with pvkCmdBindDescriptorSets, or NULL */
    PvkBuffer   vertexBuffers[PVK_MAX_VERTEX_BINDINGS]; /* bound with pvkCmdBindVertexBuffers, or NULL */
    size_t      vertexOffsets[PVK_MAX_VERTEX_BINDINGS];
    PvkBuffer   indexBuffer;  /* or NULL */
    size_t      indexOffset;
    uint8_t     push[PVK_MAX_PUSH_CONSTANT_BYTES]; /* what VU1's push constant range holds */
    uint32_t    pushSet;      /* bytes set so far (high-water mark) */
    struct PvkPipelineLayout_T pushLayout; /* ranges of the layout they were set with */
    uint32_t    dynamicSet;   /* 1 << PvkDynamicState for each pvkCmdSet* called */
    PvkViewport viewport;     /* set with pvkCmdSetViewport */
    int         viewportDirty; /* VU1's viewport transform differs from `viewport` */
    PvkRect2D   scissor;      /* set with pvkCmdSetScissor */
    float       blendConstants[4]; /* set with pvkCmdSetBlendConstants */
    int         recording;
    int         rendering;
    int         failed;       /* a recording error makes the buffer unusable */
};

#endif /* PVK_INTERNAL_H */
