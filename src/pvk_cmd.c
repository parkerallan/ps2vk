/* Command buffers: recorded as a VIF1 DMA chain, sent as-is on submit. */
#include <stdlib.h>

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
    cmd->vertexBuffer = NULL;
    cmd->vertexOffset = 0;
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

void pvkCmdBeginRendering(PvkCommandBuffer cmd, const PvkRenderingInfo *info)
{
    const PvkRenderingAttachmentInfo *color = &info->colorAttachment;
    const PvkRect2D *area = &info->renderArea;
    zbuffer_t z = { 0 };
    qword_t *q;

    if (!cmd_reserve(cmd, 72))
        return;

    /* No depth buffer yet: depth test always passes and nothing is written. */
    z.enable = DRAW_DISABLE;
    z.method = ZTEST_METHOD_ALLPASS;
    z.zsm    = GS_ZBUF_32;
    z.mask   = 1;

    q = cmd_open_direct(cmd);
    q = draw_setup_environment(q, 0, &color->image->fb, &z);
    q = draw_primitive_xyoffset(q, 0, PVK_GS_ORIGIN, PVK_GS_ORIGIN);
    q = draw_disable_tests(q, 0, &z);
    if (color->loadOp == PVK_ATTACHMENT_LOAD_OP_CLEAR) {
        /* libdraw adds 2048 to the rectangle it is given (its START_OFFSET). */
        q = draw_clear(q, 0,
                       PVK_GS_ORIGIN - 2048.0f + area->x, PVK_GS_ORIGIN - 2048.0f + area->y,
                       area->width, area->height,
                       (int)(color->clearValue.float32[0] * 255.0f),
                       (int)(color->clearValue.float32[1] * 255.0f),
                       (int)(color->clearValue.float32[2] * 255.0f));
    }
    cmd_close_direct(cmd, q);

    cmd->rendering = 1;
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

    cmd->pipeline = pipeline;
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

void pvkCmdDraw(PvkCommandBuffer cmd, uint32_t vertexCount, uint32_t firstVertex)
{
    PvkPipeline p = cmd->pipeline;
    uint8_t *base;
    uint32_t done = 0;

    if (!cmd->rendering || !p || !cmd->vertexBuffer) {
        pvk_error("pvkCmdDraw needs pvkCmdBeginRendering, a pipeline and a vertex buffer\n");
        cmd->failed = 1;
        return;
    }
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
        uint32_t sent   = 0;

        if (!cmd_reserve(cmd, 8 + 2 * (qwords / PVK_MAX_UNPACK_QWORDS + 1)))
            return;

        /* The previous batch must be finished (VU1 done, output kicked)
         * before its input and output memory is overwritten. */
        cmd_flush(cmd);

        /* Batch header: the GIF tag VU1 copies to its output, and the count. */
        packet2_utils_vu_open_unpack(cmd->packet, PVK_VU1_INPUT_ADDR, 0);
        packet2_add_2x_s64(cmd->packet,
                           GIF_SET_TAG(n, 1, 1, p->prim, GIF_FLG_PACKED, 2),
                           GIF_REG_RGBAQ | (GIF_REG_XYZ2 << 4));
        packet2_add_u32(cmd->packet, n);
        packet2_add_u32(cmd->packet, 0);
        packet2_add_u32(cmd->packet, 0);
        packet2_add_u32(cmd->packet, 0);
        packet2_utils_vu_close_unpack(cmd->packet);

        /* Vertices go straight from the buffer to VU1 memory (DMA REF, no copy). */
        while (sent < qwords) {
            uint32_t chunk = qwords - sent < PVK_MAX_UNPACK_QWORDS ? qwords - sent : PVK_MAX_UNPACK_QWORDS;
            packet2_utils_vu_add_unpack_data(cmd->packet,
                                             PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_HEADER + sent,
                                             base + (size_t)done * p->vertexStride + (size_t)sent * 16,
                                             chunk, 0);
            sent += chunk;
        }

        packet2_utils_vu_add_start_program(cmd->packet, 0);
        done += n;
    }
}
