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

/* Color images keep libdraw's framebuffer_t (the swapchain presents from it);
 * depth images reuse its address/width/height with a GS_PSMZ_* psm. A color
 * image can also be sampled: TEX0 takes its address, texture buffer width
 * and log2 size; MIPTBP1/2 the addresses and widths of the other levels. */
struct PvkImage_T {
    PvkFormat     format;
    int           colorTable; /* a palette of fb.width entries at TEX0.CBP */
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
/* Fills tbw/tw/th from fb.width and fb.height, and level 0's entries. */
void pvk_image_texture_params(PvkImage image);
/* Largest depth value a depth format stores; depth is flipped so near = this. */
float pvk_depth_max(PvkFormat format);

struct PvkSwapchain_T {
    uint32_t          width, height;
    uint32_t          imageCount;
    uint32_t          nextImage;
    int               interlaced;  /* flip only on even fields, so both fields show one frame */
    struct PvkImage_T images[PVK_MAX_SWAPCHAIN_IMAGES];
};

struct PvkBuffer_T {
    void  *data;     /* 64-byte aligned EE RAM, read by DMA */
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
};

/* What one bindings[] slot of a set holds: an image and sampler for a combined
 * image sampler, a buffer range for a uniform buffer. All NULL until written. */
struct PvkDescriptorSlot {
    PvkImage   image;
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

struct PvkPipeline_T {
    const PvkShaderCode *vs;
    const PvkShaderCode *fs;
    PvkPipelineLayout    layout;
    uint32_t samplerSet;      /* when fs->textured: set index and bindings[] slot of its sampler2D */
    uint32_t samplerSlot;
    uint32_t uniformSlots[PVK_MAX_UNIFORM_BLOCKS]; /* bindings[] slot of each vs->uniformBlocks[] entry in its set layout */
    uint32_t inputQwords;    /* qwords per vertex in VU1 memory */
    uint32_t outputQwords;    /* qwords per vertex VU1 emits: RGBAQ, XYZ2, plus ST when textured */
    uint32_t vertexStride;    /* bytes per vertex in the vertex buffer */
    uint32_t maxBatch;        /* vertices per VU1 run, a whole number of list primitives */
    uint32_t primStep;        /* vertices a draw grows by: the list primitive's size, 1 for strips and fans */
    uint32_t primOverlap;     /* vertices a strip or fan batch repeats from the previous batch */
    int      fan;             /* repeats the first vertex of the draw, not the one before the overlap */
    int      strip;           /* a triangle strip: the winding flips every triangle */
    float    cullSign;        /* VU1 culls a triangle when its signed area * cullSign < 0; 0 = no culling */
    int      cullAll;         /* FRONT_AND_BACK on triangles: draws are skipped */
    u64      prim;            /* GS PRIM value for the GIF tag */
    PvkFormat depthFormat;    /* depth attachment it draws with, or UNDEFINED */
    uint32_t ztst;            /* GS TEST.ZTST: 0 never, 1 always, 2 >=, 3 > */
    uint32_t zmsk;            /* GS ZBUF.ZMSK: 1 = no depth writes */
    u64      alpha;           /* GS ALPHA */
    float    context[PVK_VU1_CONTEXT_QWORDS * 4] __attribute__((aligned(16)));
};

struct PvkCommandBuffer_T {
    PvkDevice   device;
    packet2_t  *packet;       /* VIF1 DMA chain */
    PvkPipeline pipeline;
    PvkImage    colorImage;   /* attachments of the current pvkCmdBeginRendering */
    PvkImage    depthImage;   /* or NULL */
    int         gsStateDirty; /* FRAME/ZBUF/TEST/TEX0 must be re-sent before the next draw */
    int         uniformsDirty; /* the uniform blocks' buffers must be sent to VU1 before the next draw */
    PvkDescriptorSet sets[PVK_MAX_DESCRIPTOR_SETS]; /* bound with pvkCmdBindDescriptorSets, or NULL */
    PvkBuffer   vertexBuffer;
    size_t      vertexOffset;
    PvkBuffer   indexBuffer;  /* or NULL */
    size_t      indexOffset;
    uint8_t     push[PVK_MAX_PUSH_CONSTANT_BYTES]; /* what VU1's push constant range holds */
    uint32_t    pushSet;      /* bytes set so far (high-water mark) */
    struct PvkPipelineLayout_T pushLayout; /* ranges of the layout they were set with */
    int         recording;
    int         rendering;
    int         failed;       /* a recording error makes the buffer unusable */
};

struct PvkFence_T {
    int pending;              /* submitted, not yet waited on */
    int signaled;
};

#endif /* PVK_INTERNAL_H */
