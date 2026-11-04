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
    cmd->colorImage   = NULL;
    cmd->depthImage   = NULL;
    cmd->gsStateDirty = 0;
    cmd->vertexBuffer = NULL;
    cmd->vertexOffset = 0;
    cmd->indexBuffer  = NULL;
    cmd->indexOffset  = 0;
    cmd->pushSet      = 0;
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
static qword_t *cmd_clear(qword_t *q, const PvkRenderingInfo *info, int clearColor, int clearDepth)
{
    const PvkRect2D *area = &info->renderArea;
    const framebuffer_t *fb = &info->colorAttachment.image->fb;
    const PvkImage depth = info->depthAttachment.image;
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
    int clearColor = color->loadOp == PVK_ATTACHMENT_LOAD_OP_CLEAR;
    int clearDepth = depth->image && depth->loadOp == PVK_ATTACHMENT_LOAD_OP_CLEAR;
    zbuffer_t z = { 0 };
    qword_t *q;

    if (!color->image || color->image->format != PVK_FORMAT_CT32) {
        pvk_error("pvkCmdBeginRendering: the color attachment must be a PVK_FORMAT_CT32 image\n");
        cmd->failed = 1;
        return;
    }
    if (depth->image) {
        if (!is_depth_format(depth->image->format)) {
            pvk_error("pvkCmdBeginRendering: the depth attachment is not a depth image\n");
            cmd->failed = 1;
            return;
        }
        if (depth->image->fb.width != color->image->fb.width) {
            pvk_error("pvkCmdBeginRendering: the depth attachment (%u wide) must be as wide as the "
                      "color attachment (%u): the GS gives both one width\n",
                      (unsigned)depth->image->fb.width, (unsigned)color->image->fb.width);
            cmd->failed = 1;
            return;
        }
    }

    if (!cmd_reserve(cmd, 28 + (clearColor || clearDepth ? 5 + 2 * clear_strips(&info->renderArea) : 0)))
        return;

    /* ZBUF for the environment; the test and write mask come from the pipeline at draw time. */
    z.enable  = depth->image != NULL;
    z.method  = ZTEST_METHOD_ALLPASS;
    z.address = depth->image ? depth->image->fb.address : 0;
    z.zsm     = depth->image ? gs_zsm(depth->image->format) : GS_ZBUF_32;
    z.mask    = 1;

    q = cmd_open_direct(cmd);
    q = draw_setup_environment(q, 0, &color->image->fb, &z);
    q = draw_primitive_xyoffset(q, 0, PVK_GS_ORIGIN, PVK_GS_ORIGIN);
    if (clearColor || clearDepth)
        q = cmd_clear(q, info, clearColor, clearDepth);
    cmd_close_direct(cmd, q);

    cmd->colorImage   = color->image;
    cmd->depthImage   = depth->image;
    cmd->gsStateDirty = 1;
    cmd->rendering    = 1;
}

/* FRAME, ZBUF and TEST for drawing with the bound pipeline into the current
 * attachments. Sent before the first draw after pvkCmdBeginRendering or
 * pvkCmdBindPipeline. Uses 7 qwords of the chain. */
static void cmd_gs_state(PvkCommandBuffer cmd)
{
    const framebuffer_t *fb = &cmd->colorImage->fb;
    const PvkImage depth = cmd->depthImage;
    const PvkPipeline p = cmd->pipeline;
    qword_t *q;

    q = cmd_open_direct(cmd);
    q = gs_ad(q, GIF_SET_TAG(3, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    q = gs_ad(q, GS_SET_FRAME(fb->address >> 11, fb->width >> 6, fb->psm, 0), GS_REG_FRAME);
    q = gs_ad(q, GS_SET_ZBUF(depth ? depth->fb.address >> 11 : 0, depth ? gs_zsm(depth->format) : GS_ZBUF_32,
                             depth ? p->zmsk : 1), GS_REG_ZBUF);
    /* No alpha test (Vulkan has none); depth test from the pipeline. */
    q = gs_ad(q, GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, p->ztst), GS_REG_TEST);
    cmd_close_direct(cmd, q);

    cmd->gsStateDirty = 0;
}

void pvkCmdEndRendering(PvkCommandBuffer cmd)
{
    cmd->rendering = 0;
}

void pvkCmdBindPipeline(PvkCommandBuffer cmd, PvkPipeline pipeline)
{
    const PvkShaderCode *vs = pipeline->vs;
    u32 *start = (u32 *)vs->vu1CodeStart;
    u32 *end   = (u32 *)vs->vu1CodeEnd;
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

    cmd->pipeline     = pipeline;
    cmd->gsStateDirty = 1; /* its depth test and write mask */
}

void pvkCmdPushConstants(PvkCommandBuffer cmd, uint32_t offset, uint32_t size, const void *data)
{
    uint32_t first, last, q, i;
    const uint32_t *words;

    if ((offset | size) & 3 || size == 0 || offset + size > PVK_MAX_PUSH_CONSTANT_BYTES) {
        pvk_error("pvkCmdPushConstants: offset %u and size %u must be multiples of 4 "
                  "within %d bytes\n", (unsigned)offset, (unsigned)size, PVK_MAX_PUSH_CONSTANT_BYTES);
        cmd->failed = 1;
        return;
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

void pvkCmdBindVertexBuffer(PvkCommandBuffer cmd, PvkBuffer buffer, size_t offset)
{
    if (offset & 15) {
        pvk_error("vertex buffer offset must be a multiple of 16\n");
        cmd->failed = 1;
        return;
    }
    cmd->vertexBuffer = buffer;
    cmd->vertexOffset = offset;
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

/* What every draw needs: rendering, a pipeline whose depth format matches the
 * attachment, a vertex buffer and the push constants the shader reads. Sends
 * the GS state if it changed. Fails the buffer and returns 0 otherwise. */
static int draw_ready(PvkCommandBuffer cmd, const char *name)
{
    PvkPipeline p = cmd->pipeline;

    if (!cmd->rendering || !p || !cmd->vertexBuffer) {
        pvk_error("%s needs pvkCmdBeginRendering, a pipeline and a vertex buffer\n", name);
        cmd->failed = 1;
        return 0;
    }
    if (p->vs->pushConstantBytes > cmd->pushSet) {
        pvk_error("%s: vertex shader '%s' reads %u bytes of push constants but only %u "
                  "were set with pvkCmdPushConstants\n", name, p->vs->name,
                  (unsigned)p->vs->pushConstantBytes, (unsigned)cmd->pushSet);
        cmd->failed = 1;
        return 0;
    }
    if (p->depthFormat != (cmd->depthImage ? cmd->depthImage->format : PVK_FORMAT_UNDEFINED)) {
        pvk_error("%s: the pipeline's depthAttachmentFormat (%d) does not match the depth "
                  "attachment of pvkCmdBeginRendering (%d)\n", name, (int)p->depthFormat,
                  cmd->depthImage ? (int)cmd->depthImage->format : (int)PVK_FORMAT_UNDEFINED);
        cmd->failed = 1;
        return 0;
    }

    if (cmd->gsStateDirty) {
        if (!cmd_reserve(cmd, 7))
            return 0;
        cmd_gs_state(cmd);
    }
    return 1;
}

/* VU1 input memory a batch can fill, after the header. */
#define PVK_BATCH_INPUT_QWORDS (PVK_VU1_OUTPUT_ADDR - PVK_VU1_INPUT_ADDR - PVK_VU1_INPUT_HEADER)

/* Most indices in one batch: whole triangles that fit the output area. */
#define PVK_BATCH_MAX_INDICES \
    (((PVK_VU1_MEMORY_QWORDS - PVK_VU1_OUTPUT_ADDR - 1) / PVK_VU1_OUTPUT_PER_VERTEX) / 3 * 3)

/* Chain qwords a batch of `vertexQwords` of vertices and `indices` indices
 * needs, including the flush and the program start. */
static uint32_t batch_qwords(uint32_t vertexQwords, uint32_t indices)
{
    uint32_t n = 8 + 2 * (vertexQwords / PVK_MAX_UNPACK_QWORDS + 1);
    if (indices)
        n += 1 + (8 + 2 * indices + 15) / 16;
    return n;
}

/* Batch header: the GIF tag VU1 copies to its output, how many vertices to
 * process and, for an indexed batch, where the index list starts. The
 * previous batch must be finished (VU1 done, output kicked) before its input
 * and output memory is overwritten, so a flush comes first. */
static void batch_begin(PvkCommandBuffer cmd, uint32_t count, uint32_t indexOffset)
{
    cmd_flush(cmd);
    packet2_utils_vu_open_unpack(cmd->packet, PVK_VU1_INPUT_ADDR, 0);
    packet2_add_2x_s64(cmd->packet,
                       GIF_SET_TAG(count, 1, 1, cmd->pipeline->prim, GIF_FLG_PACKED, 2),
                       GIF_REG_RGBAQ | (GIF_REG_XYZ2 << 4));
    packet2_add_u32(cmd->packet, count);
    packet2_add_u32(cmd->packet, indexOffset);
    packet2_add_u32(cmd->packet, 0);
    packet2_add_u32(cmd->packet, 0);
    packet2_utils_vu_close_unpack(cmd->packet);
}

/* Vertices go straight from the buffer to VU1 memory (DMA REF, no copy). */
static void batch_vertices(PvkCommandBuffer cmd, uint32_t dest, const uint8_t *src, uint32_t qwords)
{
    uint32_t sent = 0;

    while (sent < qwords) {
        uint32_t chunk = qwords - sent < PVK_MAX_UNPACK_QWORDS ? qwords - sent : PVK_MAX_UNPACK_QWORDS;
        packet2_utils_vu_add_unpack_data(cmd->packet, dest + sent, (void *)(src + (size_t)sent * 16), chunk, 0);
        sent += chunk;
    }
}

/* Indices, less `base`, copied into the chain: VIF UNPACK S_16 expands each
 * 16-bit value to one VU1 qword, so VU1 reads them with ILW. */
static void batch_indices(PvkCommandBuffer cmd, uint32_t dest, const uint16_t *idx, uint32_t count, uint32_t base)
{
    uint32_t i;

    packet2_chain_open_cnt(cmd->packet, 0, 0, 0);
    packet2_vif_stcycl(cmd->packet, 0, 0x0101, 0);
    packet2_vif_open_unpack(cmd->packet, P2_UNPACK_S_16, dest, 0, 0, 1, 0);
    for (i = 0; i < count; i += 2) {
        uint32_t word = idx[i] - base;
        if (i + 1 < count)
            word |= (uint32_t)(idx[i + 1] - base) << 16;
        packet2_add_u32(cmd->packet, word);
    }
    while ((u32)cmd->packet->next & 15)
        packet2_add_u32(cmd->packet, 0); /* VIF NOPs up to the qword boundary */
    packet2_vif_close_unpack_manual(cmd->packet, count);
    packet2_chain_close_tag(cmd->packet);
}

void pvkCmdDraw(PvkCommandBuffer cmd, uint32_t vertexCount, uint32_t firstVertex)
{
    PvkPipeline p = cmd->pipeline;
    uint8_t *base;
    uint32_t done = 0;

    if (!draw_ready(cmd, "pvkCmdDraw"))
        return;
    if (cmd->vertexOffset + (size_t)(firstVertex + vertexCount) * p->vertexStride > cmd->vertexBuffer->size) {
        pvk_error("pvkCmdDraw reads past the end of the vertex buffer\n");
        cmd->failed = 1;
        return;
    }

    vertexCount -= vertexCount % 3; /* whole triangles only */
    base = (uint8_t *)cmd->vertexBuffer->data + cmd->vertexOffset + (size_t)firstVertex * p->vertexStride;

    while (done < vertexCount) {
        uint32_t n      = vertexCount - done < p->maxBatch ? vertexCount - done : p->maxBatch;
        uint32_t qwords = n * p->inputQwords;

        if (!cmd_reserve(cmd, batch_qwords(qwords, 0)))
            return;
        batch_begin(cmd, n, 0);
        batch_vertices(cmd, PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_HEADER, base + (size_t)done * p->vertexStride, qwords);
        packet2_utils_vu_add_start_program(cmd->packet, 0);
        done += n;
    }
}

void pvkCmdDrawIndexed(PvkCommandBuffer cmd, uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset)
{
    PvkPipeline p = cmd->pipeline;
    const uint16_t *idx;
    const uint8_t *vertices;
    size_t vertexSlots;
    uint32_t done = 0;

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

    indexCount -= indexCount % 3; /* whole triangles only */
    idx         = (const uint16_t *)((const uint8_t *)cmd->indexBuffer->data + cmd->indexOffset) + firstIndex;
    vertices    = (const uint8_t *)cmd->vertexBuffer->data + cmd->vertexOffset;
    vertexSlots = (cmd->vertexBuffer->size - cmd->vertexOffset) / p->vertexStride;

    while (done < indexCount) {
        uint32_t lo = idx[done], hi = lo, n = 0, k, vq;
        int64_t first;

        /* Grow the batch a triangle at a time while its indices and the run of
         * vertices they span (sent as one block, so they need no copy) fit. */
        while (done + n < indexCount && n < PVK_BATCH_MAX_INDICES) {
            uint32_t l = lo, h = hi;
            for (k = 0; k < 3; k++) {
                uint32_t v = idx[done + n + k];
                if (v < l) l = v;
                if (v > h) h = v;
            }
            if ((h - l + 1) * p->inputQwords + n + 3 > PVK_BATCH_INPUT_QWORDS)
                break;
            lo = l;
            hi = h;
            n += 3;
        }

        if (n == 0) {
            /* One triangle whose vertices lie too far apart for a block: send
             * the three vertices one by one, as an in-order batch. */
            if (!cmd_reserve(cmd, 8 + 2 * 3))
                return;
            for (k = 0; k < 3; k++) {
                first = (int64_t)idx[done + k] + vertexOffset;
                if (first < 0 || first >= (int64_t)vertexSlots) {
                    pvk_error("pvkCmdDrawIndexed: index %u + vertexOffset %d is outside the vertex buffer\n",
                              (unsigned)idx[done + k], (int)vertexOffset);
                    cmd->failed = 1;
                    return;
                }
            }
            batch_begin(cmd, 3, 0);
            for (k = 0; k < 3; k++)
                batch_vertices(cmd, PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_HEADER + k * p->inputQwords,
                               vertices + (size_t)((int64_t)idx[done + k] + vertexOffset) * p->vertexStride,
                               p->inputQwords);
            packet2_utils_vu_add_start_program(cmd->packet, 0);
            done += 3;
            continue;
        }

        first = (int64_t)lo + vertexOffset;
        if (first < 0 || (int64_t)hi + vertexOffset >= (int64_t)vertexSlots) {
            pvk_error("pvkCmdDrawIndexed: indices %u..%u + vertexOffset %d reach outside the vertex buffer\n",
                      (unsigned)lo, (unsigned)hi, (int)vertexOffset);
            cmd->failed = 1;
            return;
        }
        vq = (hi - lo + 1) * p->inputQwords;

        if (!cmd_reserve(cmd, batch_qwords(vq, n)))
            return;
        batch_begin(cmd, n, PVK_VU1_INPUT_HEADER + vq);
        batch_vertices(cmd, PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_HEADER,
                       vertices + (size_t)first * p->vertexStride, vq);
        batch_indices(cmd, PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_HEADER + vq, idx + done, n, lo);
        packet2_utils_vu_add_start_program(cmd->packet, 0);
        done += n;
    }
}
