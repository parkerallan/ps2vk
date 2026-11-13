/* Command buffers: recorded as a VIF1 DMA chain, sent as-is on submit. */
#include <stdlib.h>
#include <string.h>

#include "pvk_internal.h"

/* VIF UNPACK writes at most 256 qwords per code; stay below that. */
#define PVK_MAX_UNPACK_QWORDS 255

/* Make sure `qwords` more fit in the chain; mark the buffer failed if not. */
static int cmd_reserve(PvkCommandBuffer cmd, uint32_t qwords)
{
    if (cmd->failed)
        return 0;
    if (packet2_get_qw_count(cmd->packet) + qwords + 2 > cmd->packet->max_qwords_count) {
        pvk_error("command buffer full (%u qwords); create it with a larger maxQwords\n",
                  (unsigned)cmd->packet->max_qwords_count);
        cmd->failed = 1;
        return 0;
    }
    return 1;
}

/* A CNT tag carrying only a VIF FLUSH: wait for VU1 and the GS paths to go idle. */
static void cmd_flush(PvkCommandBuffer cmd)
{
    packet2_chain_open_cnt(cmd->packet, 0, 0, 0);
    packet2_vif_flush(cmd->packet, 0);
    packet2_vif_nop(cmd->packet, 0);
    packet2_chain_close_tag(cmd->packet);
}

/* Open a VIF DIRECT (PATH2) block; GS packets are written at the returned pointer. */
static qword_t *cmd_open_direct(PvkCommandBuffer cmd)
{
    packet2_chain_open_cnt(cmd->packet, 0, 0, 0);
    packet2_vif_flush(cmd->packet, 0);
    packet2_vif_open_direct(cmd->packet, 0);
    return cmd->packet->next;
}

static void cmd_close_direct(PvkCommandBuffer cmd, qword_t *end)
{
    /* libdraw leaves EOP clear on its GIF tags. End the PATH2 packet here,
     * or the GIF keeps waiting for more and every later FLUSH or XGKICK stalls. */
    end->dw[0] = GIF_SET_TAG(0, 1, 0, 0, GIF_FLG_PACKED, 0);
    end->dw[1] = 0;
    end++;

    packet2_update(cmd->packet, end);
    packet2_vif_close_direct_auto(cmd->packet);
    packet2_chain_close_tag(cmd->packet);
}

PvkResult pvkCreateCommandBuffer(PvkDevice device, const PvkCommandBufferCreateInfo *info, PvkCommandBuffer *cmd)
{
    PvkCommandBuffer cb;
    uint32_t qwords = info && info->maxQwords ? info->maxQwords : PVK_DEFAULT_CMD_QWORDS;

    cb = calloc(1, sizeof(struct PvkCommandBuffer_T));
    if (!cb)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;

    cb->device = device;
    cb->packet = packet2_create(qwords, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
    if (!cb->packet) {
        free(cb);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }

    *cmd = cb;
    return PVK_SUCCESS;
}

void pvkDestroyCommandBuffer(PvkDevice device, PvkCommandBuffer cmd)
{
    (void)device;
    if (!cmd)
        return;
    packet2_free(cmd->packet);
    free(cmd);
}

PvkResult pvkBeginCommandBuffer(PvkCommandBuffer cmd)
{
    packet2_reset(cmd->packet, 0);
    cmd->pipeline     = NULL;
    cmd->colorView    = NULL;
    cmd->depthView    = NULL;
    cmd->gsStateDirty = 0;
    cmd->uniformsDirty = 0;
    memset(cmd->sets, 0, sizeof(cmd->sets));
    memset(cmd->vertexBuffers, 0, sizeof(cmd->vertexBuffers));
    memset(cmd->vertexOffsets, 0, sizeof(cmd->vertexOffsets));
    cmd->indexBuffer  = NULL;
    cmd->indexOffset  = 0;
    cmd->pushSet      = 0;
    memset(&cmd->pushLayout, 0, sizeof(cmd->pushLayout));
    cmd->rendering    = 0;
    cmd->failed       = 0;
    cmd->recording    = 1;
    return PVK_SUCCESS;
}

PvkResult pvkEndCommandBuffer(PvkCommandBuffer cmd)
{
    qword_t *q;

    if (cmd->rendering) {
        pvk_error("pvkEndCommandBuffer inside pvkCmdBeginRendering/pvkCmdEndRendering\n");
        cmd->failed = 1;
    }

    /* GS FINISH after all drawing, so the fence can tell when the GS is done. */
    if (cmd_reserve(cmd, 5)) {
        q = cmd_open_direct(cmd);
        q = draw_finish(q);
        cmd_close_direct(cmd, q);
        packet2_utils_vu_add_end_tag(cmd->packet);
    }

    cmd->recording = 0;
    return cmd->failed ? PVK_ERROR_VALIDATION_FAILED : PVK_SUCCESS;
}

/* GS ZBUF.ZSM for a depth format. */
static int gs_zsm(PvkFormat format)
{
    switch (format) {
    case PVK_FORMAT_Z16: return GS_ZBUF_16;
    case PVK_FORMAT_Z24: return GS_ZBUF_24;
    default:             return GS_ZBUF_32;
    }
}

static int is_depth_format(PvkFormat format)
{
    return format == PVK_FORMAT_Z32 || format == PVK_FORMAT_Z24 || format == PVK_FORMAT_Z16;
}

/* Vulkan depth 0..1 to the flipped integer the depth buffer stores. */
static uint32_t gs_depth(float depth, PvkFormat format)
{
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;
    return (uint32_t)((1.0f - depth) * pvk_depth_max(format));
}

/* One packed A+D register write. */
static qword_t *gs_ad(qword_t *q, u64 value, u64 reg)
{
    q->dw[0] = value;
    q->dw[1] = reg;
    return q + 1;
}

/* GS primitive coordinate (12.4 fixed point) of a framebuffer pixel. */
static int gs_coord(int32_t pixel)
{
    return (int)((PVK_GS_ORIGIN + (float)pixel) * 16.0f);
}

/* The GS fills sprites fastest in narrow vertical strips. */
#define PVK_CLEAR_STRIP 32

static uint32_t clear_strips(const PvkRect2D *area)
{
    return (area->width + PVK_CLEAR_STRIP - 1) / PVK_CLEAR_STRIP;
}

/* Fill the render area with the clear values: one sprite pass that writes the
 * color buffer, the depth buffer, or both, masking the one that isn't cleared.
 * Uses 5 + 2 * clear_strips(area) qwords. */
static qword_t *cmd_clear(qword_t *q, const PvkRenderingInfo *info, const framebuffer_t *fb, const PvkImage depth,
                          int clearColor, int clearDepth)
{
    const PvkRect2D *area = &info->renderArea;
    const float *rgba = info->colorAttachment.clearValue.color.float32;
    union { float f; u32 u; } one = { 1.0f };
    uint32_t strips = clear_strips(area);
    uint32_t z = 0, s;
    int y0 = gs_coord(area->y), y1 = gs_coord(area->y + (int32_t)area->height);

    if (clearDepth)
        z = gs_depth(info->depthAttachment.clearValue.depthStencil.depth, depth->format);

    q = gs_ad(q, GIF_SET_TAG(5 + 2 * strips, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    q = gs_ad(q, GS_SET_FRAME(fb->address >> 11, fb->width >> 6, fb->psm, clearColor ? 0 : 0xFFFFFFFF), GS_REG_FRAME);
    q = gs_ad(q, GS_SET_ZBUF(depth ? depth->fb.address >> 11 : 0, depth ? gs_zsm(depth->format) : GS_ZBUF_32,
                             clearDepth ? 0 : 1), GS_REG_ZBUF);
    q = gs_ad(q, GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, ZTEST_METHOD_ALLPASS), GS_REG_TEST);
    q = gs_ad(q, GS_SET_PRIM(PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), GS_REG_PRIM);
    q = gs_ad(q, GS_SET_RGBAQ((int)(rgba[0] * 255.0f), (int)(rgba[1] * 255.0f),
                              (int)(rgba[2] * 255.0f), (int)(rgba[3] * 128.0f), one.u), GS_REG_RGBAQ);
    for (s = 0; s < strips; s++) {
        int32_t x0 = area->x + (int32_t)(s * PVK_CLEAR_STRIP);
        int32_t x1 = x0 + PVK_CLEAR_STRIP;
        if (x1 > area->x + (int32_t)area->width)
            x1 = area->x + (int32_t)area->width;
        q = gs_ad(q, GS_SET_XYZ(gs_coord(x0), y0, z), GS_REG_XYZ2);
        q = gs_ad(q, GS_SET_XYZ(gs_coord(x1), y1, z), GS_REG_XYZ2);
    }
    return q;
}

void pvkCmdBeginRendering(PvkCommandBuffer cmd, const PvkRenderingInfo *info)
{
    const PvkRenderingAttachmentInfo *color = &info->colorAttachment;
    const PvkRenderingAttachmentInfo *depth = &info->depthAttachment;
    PvkImageView colorView = color->imageView, depthView = depth->imageView;
    int clearColor = color->loadOp == PVK_ATTACHMENT_LOAD_OP_CLEAR;
    int clearDepth = depthView && depth->loadOp == PVK_ATTACHMENT_LOAD_OP_CLEAR;
    PvkImage depthImage = depthView ? depthView->image : NULL;
    framebuffer_t fb;
    zbuffer_t z = { 0 };
    qword_t *q;

    if (!colorView || !(colorView->image->usage & PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || colorView->levels != 1) {
        pvk_error("pvkCmdBeginRendering: the color attachment must be a one-level view of a swapchain image or "
                  "an image created with PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT\n");
        cmd->failed = 1;
        return;
    }
    pvk_view_framebuffer(colorView, &fb);
    if (depthImage) {
        if (!is_depth_format(depthImage->format)) {
            pvk_error("pvkCmdBeginRendering: the depth attachment is not a depth image\n");
            cmd->failed = 1;
            return;
        }
        if (depthImage->fb.width != fb.width) {
            pvk_error("pvkCmdBeginRendering: the depth attachment (%u wide) must be as wide as the "
                      "color attachment's level (%u): the GS gives both one width\n",
                      (unsigned)depthImage->fb.width, (unsigned)fb.width);
            cmd->failed = 1;
            return;
        }
    }

    if (!cmd_reserve(cmd, 28 + (clearColor || clearDepth ? 5 + 2 * clear_strips(&info->renderArea) : 0)))
        return;

    /* ZBUF for the environment; the test and write mask come from the pipeline at draw time. */
    z.enable  = depthImage != NULL;
    z.method  = ZTEST_METHOD_ALLPASS;
    z.address = depthImage ? depthImage->fb.address : 0;
    z.zsm     = depthImage ? gs_zsm(depthImage->format) : GS_ZBUF_32;
    z.mask    = 1;

    q = cmd_open_direct(cmd);
    q = draw_setup_environment(q, 0, &fb, &z);
    q = draw_primitive_xyoffset(q, 0, PVK_GS_ORIGIN, PVK_GS_ORIGIN);
    if (clearColor || clearDepth)
        q = cmd_clear(q, info, &fb, depthImage, clearColor, clearDepth);
    cmd_close_direct(cmd, q);

    cmd->colorView    = colorView;
    cmd->depthView    = depthView;
    cmd->gsStateDirty = 1;
    cmd->rendering    = 1;
}

/* Chain qwords cmd_gs_state() uses. */
#define PVK_GS_STATE_QWORDS 15

/* FRAME, ZBUF, TEST and ALPHA for drawing with the bound pipeline into the current
 * attachments, plus TEX0, TEXA, TEX1, CLAMP and the MIPTBPs for the image and sampler
 * in the bound descriptor set when the pipeline samples one. Sent before the first draw
 * after pvkCmdBeginRendering, pvkCmdBindPipeline or pvkCmdBindDescriptorSets. */
static void cmd_gs_state(PvkCommandBuffer cmd)
{
    const PvkImage depth = cmd->depthView ? cmd->depthView->image : NULL;
    const PvkPipeline p = cmd->pipeline;
    int textured = p->fs->textured;
    const struct PvkDescriptorSlot *slot = textured ? &cmd->sets[p->samplerSet]->slots[p->samplerSlot] : NULL;
    int mipmapped = textured && slot->view->levels > 1;
    framebuffer_t fb;
    qword_t *q;

    pvk_view_framebuffer(cmd->colorView, &fb);
    q = cmd_open_direct(cmd);
    q = gs_ad(q, GIF_SET_TAG(textured ? (mipmapped ? 11 : 9) : 4, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    q = gs_ad(q, GS_SET_FRAME(fb.address >> 11, fb.width >> 6, fb.psm, 0), GS_REG_FRAME);
    q = gs_ad(q, GS_SET_ZBUF(depth ? depth->fb.address >> 11 : 0, depth ? gs_zsm(depth->format) : GS_ZBUF_32,
                             depth ? p->zmsk : 1), GS_REG_ZBUF);
    /* No alpha test (Vulkan has none); depth test from the pipeline. */
    q = gs_ad(q, GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, p->ztst), GS_REG_TEST);
    q = gs_ad(q, p->alpha, GS_REG_ALPHA);
    if (textured) {
        const PvkImageView v = slot->view;
        const PvkImage t = v->image, c = slot->colorTable;
        const PvkSampler s = slot->sampler;
        uint32_t base = v->baseLevel, mxl = v->levels - 1 < s->maxLevel ? v->levels - 1 : s->maxLevel;
        uint32_t a[PVK_MAX_MIP_LEVELS] = { 0 }, w[PVK_MAX_MIP_LEVELS] = { 0 }, l;
        /* The view's levels, renumbered from its base level. */
        for (l = 0; l < v->levels; l++) {
            a[l] = t->levelAddress[base + l];
            w[l] = t->levelTbw[base + l];
        }
        /* The texture's alpha is used (TCC), the function comes from the fragment
         * shader, filters, levels and LOD from the sampler: LOD = log2(w) + K (L = 0). */
        q = gs_ad(q, GS_SET_TEX0(a[0] >> 6, w[0], t->fb.psm, t->tw - base, t->th - base, TEXTURE_COMPONENTS_RGBA,
                                 p->fs->textureFunction, c ? c->fb.address >> 6 : 0, c ? c->fb.psm : 0, 0, 0,
                                 c ? 1 : 0), GS_REG_TEX0);
        /* TEXA: CT24 reads opaque, a set CT16 alpha bit is 0x80. */
        q = gs_ad(q, GS_SET_TEXA(t->format == PVK_FORMAT_CT24 ? 0x80 : 0, 0, 0x80), GS_REG_TEXA);
        q = gs_ad(q, GS_SET_TEX1(LOD_FORMULAIC, mxl, s->mmag, mxl ? s->mmip : s->mmin, 0, 0, s->k), GS_REG_TEX1);
        q = gs_ad(q, GS_SET_CLAMP(s->wms, s->wmt, 0, 0, 0, 0), GS_REG_CLAMP);
        if (mipmapped) {
            q = gs_ad(q, GS_SET_MIPTBP1(a[1] >> 6, w[1], a[2] >> 6, w[2], a[3] >> 6, w[3]), GS_REG_MIPTBP1);
            q = gs_ad(q, GS_SET_MIPTBP2(a[4] >> 6, w[4], a[5] >> 6, w[5], a[6] >> 6, w[6]), GS_REG_MIPTBP2);
        }
        /* The texture cache may hold texels from before an upload or a render into it. */
        q = gs_ad(q, GS_SET_TEXFLUSH(0), GS_REG_TEXFLUSH);
    }
    cmd_close_direct(cmd, q);

    cmd->gsStateDirty = 0;
}

void pvkCmdBindDescriptorSets(PvkCommandBuffer cmd, PvkPipelineLayout layout, uint32_t firstSet,
                              uint32_t setCount, const PvkDescriptorSet *sets)
{
    uint32_t i;

    if (firstSet + setCount > layout->setLayoutCount || firstSet + setCount > PVK_MAX_DESCRIPTOR_SETS) {
        pvk_error("pvkCmdBindDescriptorSets: sets %u to %u, but the pipeline layout has %u set layouts\n",
                  (unsigned)firstSet, (unsigned)(firstSet + setCount), (unsigned)layout->setLayoutCount);
        cmd->failed = 1;
        return;
    }
    for (i = 0; i < setCount; i++) {
        if (!sets[i] || !pvk_set_layout_compatible(&sets[i]->layout, &layout->setLayouts[firstSet + i])) {
            pvk_error("pvkCmdBindDescriptorSets: set %u was not allocated with a layout matching the "
                      "pipeline layout's set layout %u\n", (unsigned)i, (unsigned)(firstSet + i));
            cmd->failed = 1;
            return;
        }
    }
    for (i = 0; i < setCount; i++)
        cmd->sets[firstSet + i] = sets[i];
    cmd->gsStateDirty  = 1; /* the image the draws sample */
    cmd->uniformsDirty = 1; /* and the buffers behind the uniform blocks */
}

/* Most qwords of image data in one GIF IMAGE packet: NLOOP is 15 bits, and the
 * DMA REF and VIF DIRECT counts are 16. */
#define PVK_IMAGE_CHUNK_QWORDS 16384

/* Copies a color table into the chain in CSM1 layout (256 entries: 16x16 with index bits 3 and 4 swapped). */
static void cmd_copy_color_table(PvkCommandBuffer cmd, PvkBuffer buffer, PvkImage table, const PvkBufferImageCopy *region)
{
    const PvkRect2D *r = &region->rect;
    uint32_t entries = table->fb.width, bytes = pvk_format_bits(table->format) / 8, qwords = entries * bytes / 16, i;
    const uint8_t *src = (const uint8_t *)buffer->data + region->bufferOffset;
    uint8_t *dst;
    qword_t *q;

    if (region->mipLevel || r->x || r->y || r->width != entries || r->height != 1 || region->bufferOffset % 16) {
        pvk_error("pvkCmdCopyBufferToImage: a color table is uploaded whole: mip level 0, region (0, 0, %u, 1), "
                  "bufferOffset a multiple of 16\n", (unsigned)entries);
        cmd->failed = 1;
        return;
    }
    if (region->bufferOffset + (size_t)entries * bytes > buffer->size) {
        pvk_error("pvkCmdCopyBufferToImage reads past the end of the buffer\n");
        cmd->failed = 1;
        return;
    }
    if (!cmd_reserve(cmd, 8 + qwords))
        return;

    q = cmd_open_direct(cmd);
    q = gs_ad(q, GIF_SET_TAG(4, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    q = gs_ad(q, GS_SET_BITBLTBUF(0, 0, 0, table->fb.address >> 6, 1, table->fb.psm), GS_REG_BITBLTBUF);
    q = gs_ad(q, GS_SET_TRXPOS(0, 0, 0, 0, 0), GS_REG_TRXPOS);
    q = gs_ad(q, GS_SET_TRXREG(entries == 256 ? 16 : 8, entries == 256 ? 16 : 2), GS_REG_TRXREG);
    q = gs_ad(q, GS_SET_TRXDIR(0), GS_REG_TRXDIR);
    q = gs_ad(q, GIF_SET_TAG(qwords, 0, 0, 0, GIF_FLG_IMAGE, 0), 0);
    dst = (uint8_t *)q;
    for (i = 0; i < entries; i++) {
        uint32_t slot = entries == 256 ? (i & ~0x18u) | ((i & 8) << 1) | ((i & 16) >> 1) : i;
        memcpy(dst + slot * bytes, src + i * bytes, bytes);
    }
    cmd_close_direct(cmd, q + qwords);
}

void pvkCmdCopyBufferToImage(PvkCommandBuffer cmd, PvkBuffer buffer, PvkImage image, const PvkBufferImageCopy *region)
{
    const PvkRect2D *r = &region->rect;
    const uint8_t *src;
    uint32_t bytes, qwords, sent = 0, level = region->mipLevel, width, height;
    qword_t *q;

    if (!(image->usage & PVK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        pvk_error("pvkCmdCopyBufferToImage: the image was not created with PVK_IMAGE_USAGE_TRANSFER_DST_BIT "
                  "(textures and color tables have it by default)\n");
        cmd->failed = 1;
        return;
    }
    if (cmd->rendering) {
        pvk_error("pvkCmdCopyBufferToImage must be called outside pvkCmdBeginRendering/pvkCmdEndRendering\n");
        cmd->failed = 1;
        return;
    }
    if (image->colorTable) {
        cmd_copy_color_table(cmd, buffer, image, region);
        return;
    }
    if (level >= image->levels) {
        pvk_error("pvkCmdCopyBufferToImage: mip level %u, but the image has %u\n", (unsigned)level,
                  (unsigned)image->levels);
        cmd->failed = 1;
        return;
    }
    width  = image->fb.width >> level;
    height = image->fb.height >> level;
    if (r->x < 0 || r->y < 0 || r->width == 0 || r->height == 0 ||
        (uint32_t)r->x + r->width > width || (uint32_t)r->y + r->height > height) {
        pvk_error("pvkCmdCopyBufferToImage: the region (%d, %d, %ux%u) is outside the %ux%u level %u\n",
                  (int)r->x, (int)r->y, (unsigned)r->width, (unsigned)r->height, (unsigned)width,
                  (unsigned)height, (unsigned)level);
        cmd->failed = 1;
        return;
    }
    bytes = r->width * r->height * pvk_format_bits(image->format) / 8;
    if (bytes % 16 || region->bufferOffset % 16) {
        pvk_error("pvkCmdCopyBufferToImage: the GIF moves whole qwords: the region's texels must fill a "
                  "multiple of 16 bytes and bufferOffset must be a multiple of 16\n");
        cmd->failed = 1;
        return;
    }
    qwords = bytes / 16;
    if (region->bufferOffset + (size_t)qwords * 16 > buffer->size) {
        pvk_error("pvkCmdCopyBufferToImage reads past the end of the buffer\n");
        cmd->failed = 1;
        return;
    }
    src = (const uint8_t *)buffer->data + region->bufferOffset;

    /* Chain: a DIRECT block with the transfer setup, then per chunk a DIRECT
     * block holding the IMAGE tag and a REF tag streaming the pixels from the
     * buffer through a DIRECT of its own (the two VIF codes in the REF tag). */
    if (!cmd_reserve(cmd, 7 + 3 * ((qwords + PVK_IMAGE_CHUNK_QWORDS - 1) / PVK_IMAGE_CHUNK_QWORDS)))
        return;

    q = cmd_open_direct(cmd);
    q = gs_ad(q, GIF_SET_TAG(4, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    q = gs_ad(q, GS_SET_BITBLTBUF(0, 0, 0, image->levelAddress[level] >> 6, image->levelTbw[level], image->fb.psm),
              GS_REG_BITBLTBUF);
    q = gs_ad(q, GS_SET_TRXPOS(0, 0, r->x, r->y, 0), GS_REG_TRXPOS);
    q = gs_ad(q, GS_SET_TRXREG(r->width, r->height), GS_REG_TRXREG);
    q = gs_ad(q, GS_SET_TRXDIR(0), GS_REG_TRXDIR); /* host to local */
    packet2_update(cmd->packet, q);
    packet2_vif_close_direct_auto(cmd->packet);
    packet2_chain_close_tag(cmd->packet);

    while (sent < qwords) {
        uint32_t chunk = qwords - sent < PVK_IMAGE_CHUNK_QWORDS ? qwords - sent : PVK_IMAGE_CHUNK_QWORDS;
        int last = sent + chunk == qwords;

        packet2_chain_open_cnt(cmd->packet, 0, 0, 0);
        packet2_vif_nop(cmd->packet, 0);
        packet2_vif_open_direct(cmd->packet, 0);
        packet2_add_2x_s64(cmd->packet, GIF_SET_TAG(chunk, last, 0, 0, GIF_FLG_IMAGE, 0), 0);
        packet2_vif_close_direct_auto(cmd->packet);
        packet2_chain_close_tag(cmd->packet);

        packet2_chain_ref(cmd->packet, src + (size_t)sent * 16, chunk, 0, 0, 0);
        packet2_vif_nop(cmd->packet, 0);
        packet2_add_u32(cmd->packet, MAKE_VIF_CODE(chunk, 0, P2_VIF_DIRECT, 0));
        sent += chunk;
    }
}

void pvkCmdEndRendering(PvkCommandBuffer cmd)
{
    cmd->rendering = 0;
}

void pvkCmdBindPipeline(PvkCommandBuffer cmd, PvkPipeline pipeline)
{
    u32 *start = (u32 *)pipeline->code;
    u32 *end   = (u32 *)pipeline->codeEnd;
    uint32_t i;

    if (!cmd_reserve(cmd, packet2_utils_get_packet_size_for_program(start, end) + 8))
        return;

    /* Upload the vertex shader (VIF waits for VU1 to stop first), then the
     * per-pipeline constants. */
    packet2_vif_add_micro_program(cmd->packet, 0, start, end);

    packet2_utils_vu_open_unpack(cmd->packet, PVK_VU1_CONTEXT_ADDR, 0);
    for (i = 0; i < PVK_VU1_CONTEXT_QWORDS * 4; i++)
        packet2_add_float(cmd->packet, pipeline->context[i]);
    packet2_utils_vu_close_unpack(cmd->packet);

    cmd->pipeline      = pipeline;
    cmd->gsStateDirty  = 1; /* its depth test and write mask */
    cmd->uniformsDirty = 1; /* its shader places the uniform blocks in VU1 memory */
}

void pvkCmdPushConstants(PvkCommandBuffer cmd, PvkPipelineLayout layout, uint32_t stageFlags,
                         uint32_t offset, uint32_t size, const void *data)
{
    uint32_t first, last, q, i, mask;
    const uint32_t *words;

    if ((offset | size) & 3 || size == 0 || offset + size > PVK_MAX_PUSH_CONSTANT_BYTES) {
        pvk_error("pvkCmdPushConstants: offset %u and size %u must be multiples of 4 "
                  "within %d bytes\n", (unsigned)offset, (unsigned)size, PVK_MAX_PUSH_CONSTANT_BYTES);
        cmd->failed = 1;
        return;
    }
    mask = pvk_push_word_mask(offset, size);
    if (!stageFlags || stageFlags & ~(PVK_SHADER_STAGE_VERTEX | PVK_SHADER_STAGE_FRAGMENT)
        || ((stageFlags & PVK_SHADER_STAGE_VERTEX) && (layout->pushWords[0] & mask) != mask)
        || ((stageFlags & PVK_SHADER_STAGE_FRAGMENT) && (layout->pushWords[1] & mask) != mask)) {
        pvk_error("pvkCmdPushConstants: bytes %u to %u are not in the layout's push constant "
                  "ranges for stage flags 0x%x\n", (unsigned)offset, (unsigned)(offset + size),
                  (unsigned)stageFlags);
        cmd->failed = 1;
        return;
    }

    /* Values set through a layout with other ranges are lost, as in Vulkan. */
    if (!pvk_layout_push_compatible(&cmd->pushLayout, layout)) {
        cmd->pushSet    = 0;
        cmd->pushLayout = *layout;
    }

    /* VIF UNPACK writes whole qwords, so merge into a copy of the range and
     * send the qwords this update touches. */
    memcpy(cmd->push + offset, data, size);
    if (offset + size > cmd->pushSet)
        cmd->pushSet = offset + size;
    first = offset / 16;
    last  = (offset + size - 1) / 16;

    if (!cmd_reserve(cmd, 4 + (last - first + 1)))
        return;

    /* A running batch loads its push constants in its first instructions,
     * so wait for VU1 to finish before overwriting them. */
    cmd_flush(cmd);
    packet2_utils_vu_open_unpack(cmd->packet, PVK_VU1_PUSH_ADDR + first, 0);
    words = (const uint32_t *)(cmd->push + first * 16);
    for (q = first; q <= last; q++)
        for (i = 0; i < 4; i++)
            packet2_add_u32(cmd->packet, *words++);
    packet2_utils_vu_close_unpack(cmd->packet);
}

void pvkCmdBindVertexBuffers(PvkCommandBuffer cmd, uint32_t firstBinding, uint32_t bindingCount,
                             const PvkBuffer *buffers, const size_t *offsets)
{
    uint32_t i;

    if (firstBinding + bindingCount > PVK_MAX_VERTEX_BINDINGS) {
        pvk_error("pvkCmdBindVertexBuffers: bindings %u..%u go past PVK_MAX_VERTEX_BINDINGS\n",
                  (unsigned)firstBinding, (unsigned)(firstBinding + bindingCount - 1));
        cmd->failed = 1;
        return;
    }
    for (i = 0; i < bindingCount; i++) {
        if (!buffers[i] || (offsets[i] & 15)) {
            pvk_error("pvkCmdBindVertexBuffers: binding %u needs a buffer and an offset that is a multiple of 16\n",
                      (unsigned)(firstBinding + i));
            cmd->failed = 1;
            return;
        }
        cmd->vertexBuffers[firstBinding + i] = buffers[i];
        cmd->vertexOffsets[firstBinding + i] = offsets[i];
    }
}

void pvkCmdBindIndexBuffer(PvkCommandBuffer cmd, PvkBuffer buffer, size_t offset, PvkIndexType type)
{
    if (type != PVK_INDEX_TYPE_UINT16) {
        pvk_error("pvkCmdBindIndexBuffer: only PVK_INDEX_TYPE_UINT16 is supported\n");
        cmd->failed = 1;
        return;
    }
    if (offset & 1) {
        pvk_error("index buffer offset must be a multiple of 2\n");
        cmd->failed = 1;
        return;
    }
    cmd->indexBuffer = buffer;
    cmd->indexOffset = offset;
}

/* The set bound at `index`, which must have the layout the pipeline expects
 * there and have had bindings[] `slot` written. `what` names the shader
 * resource for the error; NULL fails the buffer. */
static PvkDescriptorSet bound_set(PvkCommandBuffer cmd, const char *name, uint32_t index, uint32_t slot,
                                  const char *what, const char *shader, uint32_t binding)
{
    PvkDescriptorSet s = cmd->sets[index];
    const struct PvkDescriptorSlot *d;

    if (!s || !pvk_set_layout_compatible(&s->layout, &cmd->pipeline->layout->setLayouts[index])) {
        pvk_error("%s: %s '%s' reads binding %u of set %u, but %s at set index %u\n", name, what, shader,
                  (unsigned)binding, (unsigned)index,
                  s ? "a set with another layout is bound" : "no descriptor set is bound", (unsigned)index);
        cmd->failed = 1;
        return NULL;
    }
    d = &s->slots[slot];
    if (!d->view && !d->buffer) {
        pvk_error("%s: binding %u of the descriptor set at index %u was never written with "
                  "pvkUpdateDescriptorSets\n", name, (unsigned)binding, (unsigned)index);
        cmd->failed = 1;
        return NULL;
    }
    return s;
}

/* The vertex shader's uniform blocks: each bound buffer range must hold the
 * block. When a pipeline or set was bound since the last draw, the blocks are
 * sent to VU1 memory again, streamed from the buffers by DMA REF tags after a
 * FLUSH, since a running batch loads them in its first instructions. */
static int send_uniforms(PvkCommandBuffer cmd, const char *name)
{
    const PvkPipeline p = cmd->pipeline;
    const PvkShaderCode *vs = p->vs;
    const struct PvkDescriptorSlot *slots[PVK_MAX_UNIFORM_BLOCKS];
    uint32_t i;

    for (i = 0; i < vs->uniformBlockCount; i++) {
        const PvkShaderUniformBlock *u = &vs->uniformBlocks[i];
        uint32_t qwords = (u->bytes + 15) / 16;
        PvkDescriptorSet s = bound_set(cmd, name, u->set, p->uniformSlots[i], "vertex shader", vs->name, u->binding);

        if (!s)
            return 0;
        slots[i] = &s->slots[p->uniformSlots[i]];
        if (slots[i]->range < u->bytes || slots[i]->offset + (size_t)qwords * 16 > slots[i]->buffer->size) {
            pvk_error("%s: the uniform block at set %u, binding %u of vertex shader '%s' is %u bytes "
                      "(%u whole qwords), but the bound buffer range holds %u\n", name, (unsigned)u->set,
                      (unsigned)u->binding, vs->name, (unsigned)u->bytes, (unsigned)qwords,
                      (unsigned)slots[i]->range);
            cmd->failed = 1;
            return 0;
        }
    }

    if (!cmd->uniformsDirty || !vs->uniformBlockCount)
        return 1;
    if (!cmd_reserve(cmd, 1 + 2 * vs->uniformBlockCount))
        return 0;
    cmd_flush(cmd);
    for (i = 0; i < vs->uniformBlockCount; i++) {
        const PvkShaderUniformBlock *u = &vs->uniformBlocks[i];
        packet2_utils_vu_add_unpack_data(cmd->packet, PVK_VU1_UNIFORM_ADDR + u->qword,
                                         (uint8_t *)slots[i]->buffer->data + slots[i]->offset,
                                         (u->bytes + 15) / 16, 0);
    }
    cmd->uniformsDirty = 0;
    return 1;
}

/* What every draw needs: rendering, a pipeline whose depth format matches the
 * attachment, a vertex buffer, the push constants the shader reads and the
 * descriptor sets its uniform blocks and sampler read. Sends the uniform
 * blocks and the GS state if they changed. Fails the buffer and returns 0
 * otherwise. */
static int draw_ready(PvkCommandBuffer cmd, const char *name)
{
    PvkPipeline p = cmd->pipeline;
    uint32_t b;

    if (!cmd->rendering || !p) {
        pvk_error("%s needs pvkCmdBeginRendering and a pipeline\n", name);
        cmd->failed = 1;
        return 0;
    }
    for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++) {
        if (p->vertexBindings[b].used && !cmd->vertexBuffers[b]) {
            pvk_error("%s: the pipeline reads vertex binding %u but no buffer is bound to it "
                      "(pvkCmdBindVertexBuffers)\n", name, (unsigned)b);
            cmd->failed = 1;
            return 0;
        }
    }
    if (p->vs->pushConstantBytes > cmd->pushSet
        || (p->vs->pushConstantBytes && !pvk_layout_push_compatible(&cmd->pushLayout, p->layout))) {
        pvk_error("%s: vertex shader '%s' reads %u bytes of push constants but only %u "
                  "were set with pvkCmdPushConstants through the pipeline's layout\n", name, p->vs->name,
                  (unsigned)p->vs->pushConstantBytes, (unsigned)cmd->pushSet);
        cmd->failed = 1;
        return 0;
    }
    if (p->depthFormat != (cmd->depthView ? cmd->depthView->image->format : PVK_FORMAT_UNDEFINED)) {
        pvk_error("%s: the pipeline's depthAttachmentFormat (%d) does not match the depth "
                  "attachment of pvkCmdBeginRendering (%d)\n", name, (int)p->depthFormat,
                  cmd->depthView ? (int)cmd->depthView->image->format : (int)PVK_FORMAT_UNDEFINED);
        cmd->failed = 1;
        return 0;
    }

    if (!send_uniforms(cmd, name))
        return 0;
    if (p->fs->textured && !bound_set(cmd, name, p->samplerSet, p->samplerSlot, "fragment shader", p->fs->name,
                                      (uint32_t)p->fs->samplerBinding))
        return 0;

    if (cmd->gsStateDirty) {
        if (!cmd_reserve(cmd, PVK_GS_STATE_QWORDS))
            return 0;
        cmd_gs_state(cmd);
    }
    return !p->cullAll; /* FRONT_AND_BACK: every triangle is discarded, so nothing to send */
}

/* VU1 input memory a batch can fill, after the header. */
#define PVK_BATCH_INPUT_QWORDS (PVK_VU1_OUTPUT_ADDR - PVK_VU1_INPUT_ADDR - PVK_VU1_INPUT_HEADER)

/* Most indices in one batch: whole primitives that fit the output area. */
static uint32_t batch_max_indices(const PvkPipeline p)
{
    return ((PVK_VU1_MEMORY_QWORDS - PVK_VU1_OUTPUT_ADDR - 1) / p->outputQwords) / p->primStep * p->primStep;
}

/* Vertices a batch starting at draw position `done` repeats from the previous
 * batch, as positions in the draw: the last primOverlap ones, or for a fan the
 * hub (position 0) and the last one. Returns how many. */
static uint32_t batch_carry(const PvkPipeline p, uint32_t done, uint32_t carry[2])
{
    uint32_t n = done ? p->primOverlap : 0, i;

    for (i = 0; i < n; i++)
        carry[i] = done - n + i;
    if (n && p->fan)
        carry[0] = 0;
    return n;
}

/* Chain qwords a batch of `vertices` vertices (alignment lead and tail
 * included) and `indices` indices needs, including the flush and the program
 * start: a REF tag per UNPACK chunk of each binding, or a copied vertex. */
static uint32_t batch_qwords(const PvkPipeline p, uint32_t vertices, uint32_t indices)
{
    uint32_t n = 9, b;

    for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++)
        if (p->vertexBindings[b].used)
            n += (vertices + 2 * p->align) / p->vertexBindings[b].chunk + 2;
    if (indices)
        n += 1 + (8 + 2 * indices + 15) / 16;
    return n;
}

/* Vertices every used binding's buffer holds from its bound offset. */
static uint32_t vertex_slots(PvkCommandBuffer cmd)
{
    const PvkPipeline p = cmd->pipeline;
    uint32_t b, slots = UINT32_MAX;

    for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++) {
        const PvkBuffer buf = cmd->vertexBuffers[b];
        size_t n;

        if (!p->vertexBindings[b].used)
            continue;
        n = buf->size > cmd->vertexOffsets[b] ? (buf->size - cmd->vertexOffsets[b]) / p->vertexBindings[b].stride : 0;
        if (n < slots)
            slots = (uint32_t)n;
    }
    return slots;
}

/* 1 when the bound sampler takes texture coordinates in texels (PRIM.FST). */
static uint32_t cmd_fst(PvkCommandBuffer cmd)
{
    const PvkPipeline p = cmd->pipeline;
    return p->fs->textured ? cmd->sets[p->samplerSet]->slots[p->samplerSlot].sampler->fst : 0;
}

/* Batch header: the GIF tag VU1 copies to its output, how many vertices to
 * process, for an indexed batch where the index list starts, the winding
 * sign to cull with (flipped for an odd strip triangle at draw position
 * `first`), whether to store UV instead of ST, and the block position of the
 * batch's first vertex. The previous batch must be finished before its memory
 * is overwritten, so a flush comes first. */
static void batch_begin(PvkCommandBuffer cmd, uint32_t count, uint32_t indexOffset, uint32_t first, uint32_t lead)
{
    const PvkPipeline p = cmd->pipeline;
    uint32_t fst = cmd_fst(cmd);
    u64 regs = p->outputQwords == PVK_VU1_OUTPUT_PER_VERTEX_TEXTURED
             ? (fst ? GIF_REG_UV : GIF_REG_ST) | (GIF_REG_RGBAQ << 4) | (GIF_REG_XYZ2 << 8)
             : GIF_REG_RGBAQ | (GIF_REG_XYZ2 << 4);

    cmd_flush(cmd);
    packet2_utils_vu_open_unpack(cmd->packet, PVK_VU1_INPUT_ADDR, 0);
    packet2_add_2x_s64(cmd->packet,
                       GIF_SET_TAG(count, 1, 1, p->prim | (fst ? PVK_PRIM_FST : 0), GIF_FLG_PACKED, p->outputQwords),
                       regs);
    packet2_add_u32(cmd->packet, count);
    packet2_add_u32(cmd->packet, indexOffset);
    packet2_add_float(cmd->packet, p->strip && (first & 1) ? -p->cullSign : p->cullSign);
    packet2_add_u32(cmd->packet, fst);
    packet2_add_u32(cmd->packet, lead * p->inputQwords);
    packet2_add_u32(cmd->packet, 0);
    packet2_add_u32(cmd->packet, 0);
    packet2_add_u32(cmd->packet, 0);
    packet2_utils_vu_close_unpack(cmd->packet);
}

/* VU1 address of binding b's attributes of the vertex at block position `pos`. */
static uint32_t block_addr(const PvkPipeline p, uint32_t b, uint32_t pos)
{
    return PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_HEADER + pos * p->inputQwords + p->vertexBindings[b].slot;
}

static const uint8_t *binding_vertex(PvkCommandBuffer cmd, uint32_t b, uint32_t vertex)
{
    return (const uint8_t *)cmd->vertexBuffers[b]->data + cmd->vertexOffsets[b]
         + (size_t)vertex * cmd->pipeline->vertexBindings[b].stride;
}

/* Binding b's vertices [first, first + count) straight from its buffer to block
 * position `pos` on (DMA REF, no copy): STCYCL interleaves them with the other
 * bindings' qwords, one UNPACK per chunk. `first` and `count` are multiples of
 * the binding's align, so each REF moves whole qwords. */
static void batch_ref(PvkCommandBuffer cmd, uint32_t b, uint32_t first, uint32_t count, uint32_t pos)
{
    const PvkPipeline p = cmd->pipeline;
    const struct PvkVertexBinding *vb = &p->vertexBindings[b];
    const uint8_t *src = binding_vertex(cmd, b, first);

    while (count) {
        uint32_t n = count < vb->chunk ? count : vb->chunk;

        packet2_chain_ref(cmd->packet, src, n * vb->stride / 16, 0, 0, 0);
        if (vb->count == p->inputQwords)
            packet2_vif_stcycl(cmd->packet, 0, 0x0101, 0);
        else
            packet2_vif_stcycl(cmd->packet, vb->count, p->inputQwords, 0);
        packet2_vif_open_unpack(cmd->packet, vb->mode, block_addr(p, b, pos), 0, 0, vb->usn, 0);
        packet2_vif_close_unpack_manual(cmd->packet, n * vb->count);
        src   += (size_t)n * vb->stride;
        pos   += n;
        count -= n;
    }
}

/* One vertex of binding b, copied into the chain when a REF cannot move it
 * alone (a stride that is no multiple of 16): the fan hub and the vertices of
 * a primitive sent one by one. */
static void batch_one(PvkCommandBuffer cmd, uint32_t b, uint32_t vertex, uint32_t pos)
{
    const PvkPipeline p = cmd->pipeline;
    const struct PvkVertexBinding *vb = &p->vertexBindings[b];
    const uint8_t *src = binding_vertex(cmd, b, vertex);
    uint32_t i;

    if (vb->align == 1) {
        batch_ref(cmd, b, vertex, 1, pos);
        return;
    }
    packet2_chain_open_cnt(cmd->packet, 0, 0, 0);
    packet2_vif_stcycl(cmd->packet, 0, 0x0101, 0);
    packet2_vif_open_unpack(cmd->packet, vb->mode, block_addr(p, b, pos), 0, 0, vb->usn, 0);
    for (i = 0; i < vb->stride; i += 4) {
        uint32_t word = 0;
        memcpy(&word, src + i, vb->stride - i < 4 ? vb->stride - i : 4);
        packet2_add_u32(cmd->packet, word);
    }
    while ((u32)cmd->packet->next & 15)
        packet2_add_u32(cmd->packet, 0); /* VIF NOPs up to the qword boundary */
    packet2_vif_close_unpack_manual(cmd->packet, vb->count);
    packet2_chain_close_tag(cmd->packet);
}

/* The vertices [first, first + count) of every binding, `first` at block
 * position `pos`: each binding starts at its last aligned vertex at or before
 * `first` (pos is at least that far in) and ends at the next aligned one. */
static void batch_range(PvkCommandBuffer cmd, uint32_t first, uint32_t count, uint32_t pos)
{
    const PvkPipeline p = cmd->pipeline;
    uint32_t b;

    for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++) {
        const struct PvkVertexBinding *vb = &p->vertexBindings[b];
        uint32_t l;

        if (!vb->used)
            continue;
        l = first % vb->align;
        batch_ref(cmd, b, first - l, (l + count + vb->align - 1) / vb->align * vb->align, pos - l);
    }
}

/* Block vertices a range takes, lead and tails included. */
static uint32_t block_vertices(const PvkPipeline p, uint32_t first, uint32_t count)
{
    uint32_t lead = first % p->align, b, max = lead + count;

    for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++) {
        const struct PvkVertexBinding *vb = &p->vertexBindings[b];
        uint32_t l, end;

        if (!vb->used)
            continue;
        l   = first % vb->align;
        end = lead - l + (l + count + vb->align - 1) / vb->align * vb->align;
        if (end > max)
            max = end;
    }
    return max;
}

/* Index `i` of a batch's list: the carried ones come before the draw's. */
static uint32_t batch_index(const uint16_t *pre, uint32_t preCount, const uint16_t *idx, uint32_t i)
{
    return i < preCount ? pre[i] : idx[i - preCount];
}

/* Indices, less `base`, copied into the chain: VIF UNPACK S_16 expands each
 * 16-bit value to one VU1 qword, so VU1 reads them with ILW. */
static void batch_indices(PvkCommandBuffer cmd, uint32_t dest, const uint16_t *pre, uint32_t preCount,
                          const uint16_t *idx, uint32_t count, uint32_t base)
{
    uint32_t i, total = preCount + count;

    packet2_chain_open_cnt(cmd->packet, 0, 0, 0);
    packet2_vif_stcycl(cmd->packet, 0, 0x0101, 0);
    packet2_vif_open_unpack(cmd->packet, P2_UNPACK_S_16, dest, 0, 0, 1, 0);
    for (i = 0; i < total; i += 2) {
        uint32_t word = batch_index(pre, preCount, idx, i) - base;
        if (i + 1 < total)
            word |= (uint32_t)(batch_index(pre, preCount, idx, i + 1) - base) << 16;
        packet2_add_u32(cmd->packet, word);
    }
    while ((u32)cmd->packet->next & 15)
        packet2_add_u32(cmd->packet, 0); /* VIF NOPs up to the qword boundary */
    packet2_vif_close_unpack_manual(cmd->packet, total);
    packet2_chain_close_tag(cmd->packet);
}

void pvkCmdDraw(PvkCommandBuffer cmd, uint32_t vertexCount, uint32_t firstVertex)
{
    PvkPipeline p = cmd->pipeline;
    uint32_t done = 0, b;

    if (!draw_ready(cmd, "pvkCmdDraw"))
        return;
    if ((uint64_t)firstVertex + vertexCount > vertex_slots(cmd)) {
        pvk_error("pvkCmdDraw reads past the end of a vertex buffer\n");
        cmd->failed = 1;
        return;
    }

    vertexCount -= vertexCount % p->primStep; /* whole primitives only */
    if (vertexCount <= p->primOverlap)
        return;

    while (done < vertexCount) {
        uint32_t carry[2], c = batch_carry(p, done, carry);
        uint32_t n     = vertexCount - done < p->maxBatch - c ? vertexCount - done : p->maxBatch - c;
        uint32_t first = firstVertex + done - c, lead;

        if (!cmd_reserve(cmd, batch_qwords(p, c + n, 0) + 2))
            return;
        if (c && p->fan) {
            /* The hub is not next to the run: the run goes first, then the hub
             * over the run's unused lead vertex. */
            first = firstVertex + carry[1];
            lead  = first % p->align;
            batch_begin(cmd, c + n, 0, done - c, lead);
            batch_range(cmd, first, c - 1 + n, lead + 1);
            for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++)
                if (p->vertexBindings[b].used)
                    batch_one(cmd, b, firstVertex, lead);
        } else {
            lead = first % p->align;
            batch_begin(cmd, c + n, 0, done - c, lead);
            batch_range(cmd, first, c + n, lead);
        }
        packet2_utils_vu_add_start_program(cmd->packet, 0);
        done += n;
    }
}

void pvkCmdDrawIndexed(PvkCommandBuffer cmd, uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset)
{
    PvkPipeline p = cmd->pipeline;
    const uint16_t *idx;
    uint32_t vertexSlots, done = 0, b;

    if (!draw_ready(cmd, "pvkCmdDrawIndexed"))
        return;
    if (!cmd->indexBuffer) {
        pvk_error("pvkCmdDrawIndexed needs an index buffer bound with pvkCmdBindIndexBuffer\n");
        cmd->failed = 1;
        return;
    }
    if (cmd->indexOffset + (size_t)(firstIndex + indexCount) * 2 > cmd->indexBuffer->size) {
        pvk_error("pvkCmdDrawIndexed reads past the end of the index buffer\n");
        cmd->failed = 1;
        return;
    }

    indexCount -= indexCount % p->primStep; /* whole primitives only */
    if (indexCount <= p->primOverlap)
        return;
    idx         = (const uint16_t *)((const uint8_t *)cmd->indexBuffer->data + cmd->indexOffset) + firstIndex;
    vertexSlots = vertex_slots(cmd);

    while (done < indexCount) {
        uint32_t carry[2], c = batch_carry(p, done, carry);
        uint16_t pre[2];
        uint32_t lo, hi, n = 0, k, lead, vq;
        int64_t first;

        for (k = 0; k < c; k++)
            pre[k] = idx[carry[k]];
        lo = hi = c ? pre[0] : idx[done];
        for (k = 1; k < c; k++) {
            if (pre[k] < lo) lo = pre[k];
            if (pre[k] > hi) hi = pre[k];
        }

        /* Grow the batch a primitive at a time while its indices and the run
         * of vertices they span (sent as one block, so they need no copy) fit. */
        while (done + n < indexCount && c + n + p->primStep <= batch_max_indices(p)) {
            uint32_t l = lo, h = hi;
            for (k = 0; k < p->primStep; k++) {
                uint32_t v = idx[done + n + k];
                if (v < l) l = v;
                if (v > h) h = v;
            }
            if ((h - l + 1 + 2 * (p->align - 1)) * p->inputQwords + c + n + p->primStep > PVK_BATCH_INPUT_QWORDS)
                break;
            lo = l;
            hi = h;
            n += p->primStep;
        }

        if (n == 0) {
            /* One primitive whose vertices lie too far apart for a block: send
             * its vertices one by one, as an in-order batch. */
            uint32_t total = c + p->primStep;

            if (!cmd_reserve(cmd, 9 + 2 * total * PVK_MAX_VERTEX_BINDINGS))
                return;
            for (k = 0; k < total; k++) {
                first = (int64_t)batch_index(pre, c, idx + done, k) + vertexOffset;
                if (first < 0 || first >= (int64_t)vertexSlots) {
                    pvk_error("pvkCmdDrawIndexed: index %u + vertexOffset %d is outside the vertex buffer\n",
                              (unsigned)batch_index(pre, c, idx + done, k), (int)vertexOffset);
                    cmd->failed = 1;
                    return;
                }
            }
            batch_begin(cmd, total, 0, done - c, 0);
            for (k = 0; k < total; k++)
                for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++)
                    if (p->vertexBindings[b].used)
                        batch_one(cmd, b, (uint32_t)((int64_t)batch_index(pre, c, idx + done, k) + vertexOffset), k);
            packet2_utils_vu_add_start_program(cmd->packet, 0);
            done += p->primStep;
            continue;
        }

        first = (int64_t)lo + vertexOffset;
        if (first < 0 || (int64_t)hi + vertexOffset >= (int64_t)vertexSlots) {
            pvk_error("pvkCmdDrawIndexed: indices %u..%u + vertexOffset %d reach outside the vertex buffer\n",
                      (unsigned)lo, (unsigned)hi, (int)vertexOffset);
            cmd->failed = 1;
            return;
        }
        lead = (uint32_t)first % p->align;
        vq   = block_vertices(p, (uint32_t)first, hi - lo + 1) * p->inputQwords;

        if (!cmd_reserve(cmd, batch_qwords(p, hi - lo + 1, c + n)))
            return;
        batch_begin(cmd, c + n, PVK_VU1_INPUT_HEADER + vq, done - c, lead);
        batch_range(cmd, (uint32_t)first, hi - lo + 1, lead);
        batch_indices(cmd, PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_HEADER + vq, pre, c, idx + done, n, lo);
        packet2_utils_vu_add_start_program(cmd->packet, 0);
        done += n;
    }
}
