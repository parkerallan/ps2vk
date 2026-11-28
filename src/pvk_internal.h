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
#define PVK_MAX_SWAPCHAIN_IMAGES 3
#define PVK_MAX_PENDING_FLIPS (PVK_MAX_SWAPCHAIN_IMAGES + 1)

#define pvk_error(...) printf("ps2vk: " __VA_ARGS__)

/* The one PS2; its properties are constants. */
struct PvkPhysicalDevice_T {
    int unused;
};

struct PvkInstance_T {
    struct PvkPhysicalDevice_T physicalDevice;
};

/* How every struct of a pNext chain starts. */
struct PvkBaseStructure {
    PvkStructureType         sType;
    struct PvkBaseStructure *pNext;
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
    volatile uint32_t readbackNext;                     /* cmd->readbacks[] entry its chain stops at next */
};

/* SIGNAL IDs of a pvkCmdCopyImageToBuffer region: the chain has stopped (family in bit 8), */
#define PVK_READBACK_SIGNAL_BIT 0x20000000u
/* and the GS has taken the transfer setup. */
#define PVK_READBACK_GO_BIT     0x10000000u
#define PVK_READBACK_SETUP_QWORDS 8

/* One pvkCmdCopyImageToBuffer region, the storage blocks a dispatch wrote, or an indirect draw to build. The chain ends before it and goes on at `resume`. */
struct PvkReadback {
    qword_t  *setup;      /* VIF1 packet: MSKPATH3, FLUSHA, the transfer registers, a SIGNAL, TRXDIR = 1 */
    qword_t  *resume;     /* DMA tag the chain goes on from */
    void     *dst;        /* where the DMA writes: the buffer, or `stage` */
    uint32_t  qwords;
    void     *stage;      /* EE RAM for a region the DMA cannot write in place, else NULL */
    int       owned;      /* `stage` is freed with this recording; 0 in a primary's copy of a secondary's */
    uint8_t  *out;        /* staged: the region's first byte in the buffer */
    uint32_t  rows, rowBytes, outPitch;  /* staged: rows, bytes copied per row, bytes between buffer rows */
    uint32_t  stagePitch, stageSkip;     /* staged: bytes between stage rows, bytes before the region in one */
    uint32_t  tableEntries, tableFirst;  /* a color table of that size: `rows` entries from tableFirst, rowBytes each */
    PvkCommandBuffer indirectCmd; /* an indirect draw the chain stops in front of: its command buffer, else NULL */
    uint32_t  indirectIndex;      /* and its entry in that command buffer's indirects[] */
    uint32_t  vu1Count;   /* a dispatch's or mesh draw's written storage blocks, copied out of VU1 data memory; 0 for an image region */
    struct {
        void    *dst;     /* where in the buffer */
        uint32_t qword;   /* VU1 data memory qword the block starts at */
        uint32_t bytes;
    } vu1[2 * PVK_MAX_STORAGE_BLOCKS]; /* a mesh shader's and a task shader's */
};

/* A pvkCmdCopyBufferToImage region DMA cannot stream in place: packed into `stage` at submit. */
struct PvkUpload {
    void          *stage;
    const uint8_t *src;
    uint32_t       rows, rowBytes, srcPitch;
};

/* One indirect draw call, or one command of a mesh one: its commands are read when the chain stops in front of it. */
struct PvkIndirect {
    int            kind;      /* 0 pvkCmdDrawIndirect, 1 pvkCmdDrawIndexedIndirect, 2 pvkCmdDrawMeshTasksIndirectEXT */
    const uint8_t *at;        /* the first command in the buffer */
    uint32_t       drawCount, stride;
    qword_t       *jump;      /* kinds 0 and 1: the NEXT tag that leads into `chain` */
    qword_t       *chain;     /* kinds 0 and 1: the draws' DMA chain, built when the chain stops; NULL until then */
    PvkPipeline    pipeline;  /* kinds 0 and 1: what the call drew with */
    PvkDescriptorSet sets[PVK_MAX_DESCRIPTOR_SETS];
    PvkBuffer      vertexBuffers[PVK_MAX_VERTEX_BINDINGS];
    size_t         vertexOffsets[PVK_MAX_VERTEX_BINDINGS];
    PvkBuffer      indexBuffer;
    size_t         indexOffset;
    uint32_t      *counts;    /* kind 2: the work group counts in the chain, 4 floats then 4 ints */
    uint32_t      *mscal;     /* kind 2: the VIF code that starts VU1, a NOP while a count is 0 */
    uint32_t       mscalCode;
    int            task;      /* kind 2: the counts are a task shader's */
    int            tess;      /* kinds 0 and 1 with a tessellation pipeline: built a batch of patches at a time */
    int            direct;    /* tess: pvkCmdDraw or pvkCmdDrawIndexed, whose values are `params` */
    PvkDrawIndexedIndirectCommand params;
    uint32_t       stepDraw, stepInstance, stepDone; /* tess: command, instance and vertex the next batch starts at */
    uint32_t       ranPatches; /* tess: patches VU1 has just run the control shader for */
    uint32_t       ranDrawn;  /* tess: how many of them are drawn already */
    uint32_t       chainCap;  /* tess: qwords `chain` holds */
    qword_t       *records;   /* tess: those patches' records, copied out of VU1 */
    float         *coords;    /* tess: their tessellation coordinates, a qword each */
    uint32_t       coordsCap; /* in qwords */
};

/* The region the handlers are reading back; stage 0 = none. */
struct PvkReadbackRun {
    volatile int stage;   /* 1 waiting for the channels, 2 setup sent, 3 DMA reading, 4 pvkQueueSubmit builds an indirect draw */
    PvkQueue     queue;   /* whose head entry stopped */
    const struct PvkReadback *rb;
    uint32_t     done, chunk; /* qwords read, qwords of the running DMA */
    int          unmasked;    /* PATH3 was let go so the GIF channel's chain can finish */
    PvkQueue     pending;     /* the other queue's chain stopped for a readback meanwhile, or NULL */
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
    s32       indirect;     /* EE semaphore released when the head chain stops in front of an indirect draw */
};

/* Sleeps until every submitted chain has run; queued presents may still wait for their vblank. */
void      pvk_queue_wait_chains(PvkQueue queue);
/* The same, and until every queued present is on screen. */
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
/* Flips or drops the queued presents that need no vblank; nonzero if one was. Interrupts disabled, or from a handler. */
int       pvk_swapchain_pump(PvkSwapchain swapchain);

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
    PvkPhysicalDeviceLineRasterizationFeatures lineFeatures; /* turned on at pvkCreateDevice */
    PvkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures;        /* turned on at pvkCreateDevice */
    uint32_t vramCount;      /* taken VRAM ranges, sorted by start: swapchain framebuffers and type 1 allocations */
    struct PvkVramRange vram[PVK_MAX_VRAM_ALLOCATIONS];
    PvkQueryPool queryPools[PVK_MAX_QUERY_POOLS]; /* by the number a SIGNAL ID carries, or NULL */
    struct PvkReadbackRun readback;
};

/* Packs the command buffer's staged upload regions, and its secondaries', from their buffers. */
void pvk_cmd_pack_uploads(PvkCommandBuffer cmd);
/* Reads the commands of entry `index` of the command buffer's indirects[] and builds the draws; `done` is 0
 * when the chain stops for the entry again. Not from a handler. */
PvkResult pvk_cmd_build_indirect(PvkCommandBuffer cmd, uint32_t index, int *done);
/* The tessellation coordinates of a patch, a qword (u, v, w, 0) per vertex of its triangles, lines or
 * points; `out` NULL only counts them. */
uint32_t pvk_tessellate(uint32_t domain, uint32_t spacing, int clockwise, int pointMode, const float *outer,
                        const float *inner, float *out);

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

/* A queued pvkQueuePresent: `image` is shown once its semaphores are signaled, when the present mode says. */
struct PvkFlip {
    uint32_t     image;
    PvkSemaphore waits[PVK_MAX_SUBMIT_SEMAPHORES];
    uint32_t     waitCount;
};

/* A pvkAcquireNextImage whose image is still on screen or has an older present queued. */
struct PvkAcquire {
    int          pending;
    uint32_t     flipsLeft; /* presents still to be shown before the image is free */
    PvkSemaphore semaphore;
    PvkFence     fence;
};

struct PvkSwapchain_T {
    PvkDevice         device;
    uint32_t          width, height;
    uint32_t          imageCount;
    uint32_t          nextImage;   /* where the search for a free image starts */
    PvkPresentMode    presentMode;
    int               interlaced;  /* flip only on even fields, so both fields show one frame */
    int               graphMode;   /* libgraph's GRAPH_MODE_* of the video signal */
    u64               display;     /* GS DISPLAY: where on the TV picture the image goes, and its magnification */
    uint32_t          vblankNs;    /* nanoseconds between two vblanks */
    volatile uint32_t vblanks;     /* counted by the vblank handler */
    volatile int      late;        /* a vblank passed with no present ready (FIFO_RELAXED flips at once) */
    int               acquired[PVK_MAX_SWAPCHAIN_IMAGES]; /* handed out by an acquire, not presented yet */
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

/* A shader built from a .pvs file stays alive while a module, pipeline or program refers to it. */
void pvk_shader_retain(const PvkShaderCode *code);
void pvk_shader_release(const PvkShaderCode *code);

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

/* PRIM.AA1: the GS scales the alpha of the border pixels of a line or triangle by their coverage. */
#define PVK_PRIM_AA1 (1ULL << 7)
/* Bit of PVK_DYNAMIC_STATE_LINE_STIPPLE in a `dynamic` mask; the other states are 1 << their value. */
#define PVK_DYNAMIC_LINE_STIPPLE_BIT (1u << 5)
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
    uint32_t maxStorage, usedStorage; /* storage buffers */
    struct PvkDescriptorSet_T *sets;  /* maxSets of them */
};

/* Which 4-byte words of the 128-byte push constant range each stage may
 * read, one bit per word, plus the set layouts. Two layouts with equal masks
 * are push-constant compatible in Vulkan's sense. */
struct PvkPipelineLayout_T {
    uint32_t pushWords[7];    /* [0] vertex, [1] fragment, [2] compute, [3] task, [4] mesh, [5] and [6] tessellation control and evaluation stage */
    uint32_t setLayoutCount;
    struct PvkDescriptorSetLayout_T setLayouts[PVK_MAX_DESCRIPTOR_SETS];
};

#define PVK_ALL_SHADER_STAGES (PVK_SHADER_STAGE_VERTEX | PVK_SHADER_STAGE_FRAGMENT | PVK_SHADER_STAGE_COMPUTE_BIT | \
                               PVK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | PVK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | \
                               PVK_SHADER_STAGE_TASK_BIT_EXT | PVK_SHADER_STAGE_MESH_BIT_EXT)

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
    PvkAllocationCallbacks alloc;    /* a compute pipeline's callbacks it and its code came from */
    int                    hasAlloc;
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
    PvkAllocationCallbacks alloc;    /* the callbacks a compute pipeline was created with */
    int                    hasAlloc;
    int      compute;         /* a compute pipeline: vs is its compute shader, and nothing below uniformSlots applies */
    uint32_t storageSlots[PVK_MAX_STORAGE_BLOCKS]; /* compute and mesh: bindings[] slot of each vs->storageBlocks[] entry in its set layout */
    int      mesh;            /* a mesh pipeline: vs is its mesh shader and ts its task shader, and it has no vertex input */
    const PvkShaderCode *ts;  /* or NULL */
    uint32_t taskUniformSlots[PVK_MAX_UNIFORM_BLOCKS]; /* bindings[] slot of each ts->uniformBlocks[] entry */
    uint32_t taskStorageSlots[PVK_MAX_STORAGE_BLOCKS]; /* and of each ts->storageBlocks[] entry */
    uint32_t meshEntry;       /* instruction pair a mesh draw starts VU1 at: the task program, else the mesh program */
    int      tess;            /* a tessellation pipeline: vs's program calls tcs's behind it, and draws end in `eval` */
    const PvkShaderCode *tcs, *tes; /* or NULL */
    uint32_t tcsUniformSlots[PVK_MAX_UNIFORM_BLOCKS]; /* bindings[] slot of each tcs->uniformBlocks[] entry */
    uint32_t tesUniformSlots[PVK_MAX_UNIFORM_BLOCKS]; /* and of each tes->uniformBlocks[] entry */
    uint32_t patchControlPoints;
    uint32_t tessRecord;      /* qwords of a patch record */
    uint32_t tessPatches;     /* patches one VU1 run of the control program has records for */
    uint32_t tessFirst;       /* VU1 qword of the first of those records */
    struct PvkPipeline_T *eval; /* tes's program as a pipeline of its own: one vec4 stream, the tessellation coordinates */
    uint32_t entry;           /* instruction pair a batch starts VU1 at */
    uint32_t outputEnd;       /* VU1 qword behind a batch's output area */
    uint32_t inputQwords;     /* VU1 qwords a batch's input may take, after the header */
    const PvkShaderCode *vs;
    const PvkShaderCode *fs;
    PvkPipelineLayout    layout;
    struct PvkProgram *program; /* its VU1 code: vs's with the input conversions and specialization constants patched in */
    uint32_t samplerSet;      /* when fs->textured: set index and bindings[] slot of its sampler2D */
    uint32_t samplerSlot;
    uint32_t uniformSlots[PVK_MAX_UNIFORM_BLOCKS]; /* bindings[] slot of each vs->uniformBlocks[] entry in its set layout */
    uint32_t vertexQwords;    /* qwords per vertex in VU1 memory: the vertex-rate inputs */
    uint32_t instanceQwords;  /* qwords per instance: the instance-rate inputs, 0 = none */
    uint32_t outputQwords;    /* qwords per vertex VU1 emits: RGBAQ, XYZ2, plus ST when textured, plus FOG */
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
    PvkFormat stencilFormat;  /* stencil attachment it draws with (the color image's format), or UNDEFINED */
    uint32_t date, datm;      /* GS TEST.DATE/DATM: the stencil test on the pixel's top alpha bit */
    uint32_t fba;             /* GS FBA: written pixels get the stencil bit set */
    int      stencilNever;    /* the stencil test passes no pixel: draws are skipped */
    uint32_t ate, atst, aref; /* GS TEST.ATE/ATST/AREF: the alpha test of a fragment shader with a discard */
    uint32_t texcoordOut, fogOut; /* VU1 emits ST (or UV), FOG per vertex */
    uint32_t fogcol;          /* GS FOGCOL when the fragment shader fogs (PRIM.FGE) */
    uint32_t ztst;            /* GS TEST.ZTST: 0 never, 1 always, 2 >=, 3 > */
    uint32_t zmsk;            /* GS context it draws in: 2 (ZBUF_2.ZMSK = 1) when it writes no depth, else 1 */
    u64      alpha;           /* GS ALPHA */
    uint32_t fbmsk;           /* GS FRAME.FBMSK: the bits of the channels colorWriteMask leaves out */
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
    struct PvkReadback *readbacks; /* its pvkCmdCopyImageToBuffer regions in chain order, secondaries' included */
    uint32_t    readbackCount, readbackCap;
    struct PvkUpload *uploads;     /* its staged pvkCmdCopyBufferToImage regions */
    uint32_t    uploadCount, uploadCap;
    struct PvkIndirect *indirects; /* its indirect draws */
    uint32_t    indirectCount, indirectCap;
    int         replay;       /* a copy pvkQueueSubmit builds an indirect call's draws with, not a recording */
    packet2_t   packet;       /* VIF1 (family 0) or GIF channel (family 1) DMA chain, inside the pool's block */
    uint32_t    signalQw;     /* qword of the GS SIGNAL at a primary's end; its ID is set at submit */
    PvkPipeline pipeline;
    PvkPipeline computePipeline; /* bound at PVK_PIPELINE_BIND_POINT_COMPUTE, or NULL */
    PvkDescriptorSet computeSets[PVK_MAX_DESCRIPTOR_SETS]; /* the compute bind point's sets, or NULL */
    int         vu1Compute;   /* VU1 holds a dispatch's program and memory: the next draw sends the graphics pipeline's again */
    const struct PvkProgram *program; /* the program last uploaded to VU1 in this recording, or NULL */
    PvkImageView colorView;   /* attachments of the current pvkCmdBeginRendering; NULL when inherited */
    PvkImageView depthView;   /* or NULL */
    int         inherits;     /* a secondary with RENDER_PASS_CONTINUE: draws into the primary's attachments */
    PvkFormat   inheritedDepthFormat;
    PvkFormat   stencilFormat; /* of the current rendering's stencil attachment, or UNDEFINED */
    PvkFormat   inheritedStencilFormat;
    int         secondaryContents; /* the rendering takes its draws from pvkCmdExecuteCommands only */
    int         gsStateDirty; /* TEST/ALPHA/SCISSOR/TEX0 must be re-sent before the next draw */
    uint32_t    fbmsk[2];     /* FBMSK the GS holds in FRAME_1 and FRAME_2 */
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
    uint32_t    lineStippleFactor; /* set with pvkCmdSetLineStipple */
    uint16_t    lineStipplePattern;
    int         recording;
    int         rendering;
    int         failed;       /* a recording error makes the buffer unusable */
};

#endif /* PVK_INTERNAL_H */
