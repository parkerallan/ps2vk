/* Command buffers: recorded as a DMA chain, sent as-is on submit: a VIF1 chain for the
 * graphics family, a GIF channel chain (no VIF codes, GS packets straight through PATH3) for
 * the transfer family. */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "pvk_internal.h"

/* VIF UNPACK writes at most 256 qwords per code; stay below that. */
#define PVK_MAX_UNPACK_QWORDS 255

/* Writes one A+D register setting. */
static qword_t *gs_ad(qword_t *q, u64 value, u64 reg);
static qword_t *cmd_open_gs(PvkCommandBuffer cmd);
static void cmd_close_gs(PvkCommandBuffer cmd, qword_t *end);
static void cmd_release_copies(PvkCommandBuffer cmd);
static int array_grow(PvkCommandBuffer cmd, void **array, uint32_t count, uint32_t *cap, size_t size);

/* Make sure `qwords` more fit in the chain; mark the buffer failed if not. */
static int cmd_reserve(PvkCommandBuffer cmd, uint32_t qwords)
{
    if (cmd->failed)
        return 0;
    if (packet2_get_qw_count(&cmd->packet) + qwords + 2 > cmd->packet.max_qwords_count) {
        pvk_error("command pool full (%u qwords); create it with a larger maxQwords\n",
                  (unsigned)cmd->pool->qwords);
        cmd->failed = 1;
        return 0;
    }
    return 1;
}

/* A CNT tag carrying only a VIF FLUSH: wait for VU1 and the GS paths to go idle. */
static void cmd_flush(PvkCommandBuffer cmd)
{
    packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
    packet2_vif_flush(&cmd->packet, 0);
    packet2_vif_nop(&cmd->packet, 0);
    packet2_chain_close_tag(&cmd->packet);
}

/* Open a VIF DIRECT (PATH2) block; GS packets are written at the returned pointer. */
static qword_t *cmd_open_direct(PvkCommandBuffer cmd)
{
    packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
    packet2_vif_flush(&cmd->packet, 0);
    packet2_vif_open_direct(&cmd->packet, 0);
    return cmd->packet.next;
}

static void cmd_close_direct(PvkCommandBuffer cmd, qword_t *end)
{
    /* libdraw leaves EOP clear on its GIF tags. End the PATH2 packet here,
     * or the GIF keeps waiting for more and every later FLUSH or XGKICK stalls. */
    end->dw[0] = GIF_SET_TAG(0, 1, 0, 0, GIF_FLG_PACKED, 0);
    end->dw[1] = 0;
    end++;

    packet2_update(&cmd->packet, end);
    packet2_vif_close_direct_auto(&cmd->packet);
    packet2_chain_close_tag(&cmd->packet);
}

/* VU1 code, lower then upper word per pair: XGKICKs the frame packet and stops. */
static const u32 frame_routine[8] __attribute__((aligned(16))) = {
    0x10010000u | PVK_VU1_FRAME_ADDR, 0x000002FFu, /* iaddiu VI01,VI00,frame | NOP */
    0x80000EFCu,                      0x000002FFu, /* xgkick VI01            | NOP */
    0x8000033Cu,                      0x400002FFu, /* NOP                    | NOP[E] */
    0x8000033Cu,                      0x000002FFu, /* NOP                    | NOP */
};

/* The frame routine and the frame packet for VU1: a GIF tag, FRAME_1 and FRAME_2 with FBMSK 0. */
static void cmd_frame_packet(PvkCommandBuffer cmd, const framebuffer_t *fb)
{
    u64 tag = GIF_SET_TAG(2, 1, 0, 0, GIF_FLG_PACKED, 1);
    u32 frame = (u32)GS_SET_FRAME(fb->address >> 11, fb->width >> 6, fb->psm, 0);
    uint32_t c;

    packet2_vif_add_micro_program(&cmd->packet, PVK_VU1_FRAME_ROUTINE, (u32 *)frame_routine, (u32 *)frame_routine + 8);
    packet2_utils_vu_open_unpack(&cmd->packet, PVK_VU1_FRAME_ADDR, 0);
    packet2_add_u32(&cmd->packet, (u32)tag);
    packet2_add_u32(&cmd->packet, (u32)(tag >> 32));
    packet2_add_u32(&cmd->packet, GIF_REG_AD);
    packet2_add_u32(&cmd->packet, 0);
    for (c = 0; c < 2; c++) {
        packet2_add_u32(&cmd->packet, frame);
        packet2_add_u32(&cmd->packet, 0);
        packet2_add_u32(&cmd->packet, GS_REG_FRAME_1 + c);
        packet2_add_u32(&cmd->packet, 0);
        cmd->fbmsk[c] = 0;
    }
    packet2_utils_vu_close_unpack(&cmd->packet);
}

/* Chain qwords cmd_frame_mask() uses. */
#define PVK_FRAME_MASK_QWORDS 3

/* Writes `fbmsk` over the FBMSK word of FRAME_1 or FRAME_2 in VU1's frame packet and runs
 * the frame routine. The word with the attachment's address stays, so a secondary can do it. */
static void cmd_frame_mask(PvkCommandBuffer cmd, uint32_t ctxt, uint32_t fbmsk)
{
    packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
    packet2_vif_flush(&cmd->packet, 0);
    packet2_vif_stcycl(&cmd->packet, 0, 0x0101, 0);
    /* STMASK: of a qword's x, y, z, w only y (the A+D data's upper word) is written. */
    packet2_add_u32(&cmd->packet, MAKE_VIF_CODE(0, 0, P2_VIF_STMASK, 0));
    packet2_add_u32(&cmd->packet, 0xF3F3F3F3u);
    packet2_vif_open_unpack(&cmd->packet, P2_UNPACK_S_32, PVK_VU1_FRAME_ADDR + 1 + ctxt, 0, 1, 1, 0);
    packet2_add_u32(&cmd->packet, fbmsk);
    packet2_vif_close_unpack_manual(&cmd->packet, 1);
    packet2_vif_mscal(&cmd->packet, PVK_VU1_FRAME_ROUTINE, 0);
    packet2_vif_nop(&cmd->packet, 0);
    packet2_vif_nop(&cmd->packet, 0);
    packet2_vif_nop(&cmd->packet, 0);
    packet2_chain_close_tag(&cmd->packet);
    cmd->fbmsk[ctxt] = fbmsk;
}

/* Open a CNT tag on the GIF channel (PATH3); GS packets are written at the returned pointer. */
static qword_t *cmd_open_gif(PvkCommandBuffer cmd)
{
    packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
    return cmd->packet.next;
}

static void cmd_close_gif(PvkCommandBuffer cmd, qword_t *end)
{
    packet2_update(&cmd->packet, end);
    packet2_chain_close_tag(&cmd->packet);
}

/* The queue a pool's command buffers go to. */
static PvkQueue pool_queue(PvkCommandPool pool)
{
    return pool->family ? &pool->device->transfer : &pool->device->queue;
}

/* Fails the recording when the command needs the graphics family. */
static int cmd_graphics(PvkCommandBuffer cmd, const char *who)
{
    if (cmd->pool->family == 0)
        return 1;
    pvk_error("%s: the command buffer is from a pool of the transfer family (1), which has no graphics\n", who);
    cmd->failed = 1;
    return 0;
}

PvkResult pvkCreateCommandPool(PvkDevice device, const PvkCommandPoolCreateInfo *info, PvkCommandPool *pool)
{
    PvkCommandPool p;
    uint32_t flags  = info ? info->flags : 0;
    uint32_t family = info ? info->queueFamilyIndex : 0;
    uint32_t qwords = info && info->maxQwords ? info->maxQwords : PVK_DEFAULT_CMD_QWORDS;

    if (flags & ~(PVK_COMMAND_POOL_CREATE_TRANSIENT_BIT | PVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)) {
        pvk_error("command pool: unknown flags 0x%x\n", (unsigned)flags);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (family >= PVK_MAX_QUEUE_FAMILIES || !device->families[family]) {
        pvk_error("command pool: queue family %u was not asked for at pvkCreateDevice\n", (unsigned)family);
        return PVK_ERROR_VALIDATION_FAILED;
    }

    p = calloc(1, sizeof(struct PvkCommandPool_T));
    if (!p)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    p->block = memalign(64, (size_t)qwords * 16);
    if (!p->block) {
        free(p);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }
    p->device = device;
    p->flags  = flags;
    p->family = family;
    p->qwords = qwords;

    *pool = p;
    return PVK_SUCCESS;
}

void pvkDestroyCommandPool(PvkDevice device, PvkCommandPool pool)
{
    PvkCommandBuffer cmd, next;

    if (!pool)
        return;
    (void)device;
    pvk_queue_wait_chains(pool_queue(pool));
    for (cmd = pool->buffers; cmd; cmd = next) {
        next = cmd->next;
        cmd_release_copies(cmd);
        free(cmd->readbacks);
        free(cmd->uploads);
        free(cmd->executed);
        free(cmd);
    }
    free(pool->block);
    free(pool);
}

PvkResult pvkResetCommandPool(PvkDevice device, PvkCommandPool pool, uint32_t flags)
{
    PvkCommandBuffer cmd;

    (void)device;
    (void)flags;
    if (pool->recording) {
        pvk_error("pvkResetCommandPool: a command buffer of the pool is recording\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    pvk_queue_wait_chains(pool_queue(pool));
    for (cmd = pool->buffers; cmd; cmd = cmd->next) {
        cmd->size = 0;
        cmd_release_copies(cmd);
    }
    pool->used = 0;
    return PVK_SUCCESS;
}

PvkResult pvkAllocateCommandBuffers(PvkDevice device, const PvkCommandBufferAllocateInfo *info, PvkCommandBuffer *cmds)
{
    PvkCommandPool pool = info->commandPool;
    uint32_t i;

    if (info->level != PVK_COMMAND_BUFFER_LEVEL_PRIMARY && info->level != PVK_COMMAND_BUFFER_LEVEL_SECONDARY) {
        pvk_error("pvkAllocateCommandBuffers: level %d is not primary or secondary\n", (int)info->level);
        return PVK_ERROR_VALIDATION_FAILED;
    }

    for (i = 0; i < info->commandBufferCount; i++) {
        PvkCommandBuffer cb = calloc(1, sizeof(struct PvkCommandBuffer_T));
        if (!cb) {
            pvkFreeCommandBuffers(device, pool, i, cmds);
            return PVK_ERROR_OUT_OF_HOST_MEMORY;
        }
        cb->device = device;
        cb->pool   = pool;
        cb->level  = info->level;
        cb->next   = pool->buffers;
        pool->buffers = cb;
        cmds[i] = cb;
    }
    return PVK_SUCCESS;
}

void pvkFreeCommandBuffers(PvkDevice device, PvkCommandPool pool, uint32_t count, const PvkCommandBuffer *cmds)
{
    PvkCommandBuffer *link;
    uint32_t i;

    (void)device;
    for (i = 0; i < count; i++) {
        PvkCommandBuffer cmd = cmds[i];
        if (!cmd)
            continue;
        if (pool->recording == cmd) {
            pool->recording = NULL;
            pool->used = cmd->start;
        } else if (cmd->size && cmd->start + cmd->size == pool->used) {
            pool->used = cmd->start;
        }
        for (link = &pool->buffers; *link; link = &(*link)->next) {
            if (*link == cmd) {
                *link = cmd->next;
                break;
            }
        }
        cmd_release_copies(cmd);
        free(cmd->readbacks);
        free(cmd->uploads);
        free(cmd->executed);
        free(cmd);
    }
}

/* Forget the bound pipeline, sets, buffers, push constants and dynamic state:
 * at the start of a recording, and after pvkCmdExecuteCommands. */
static void cmd_reset_state(PvkCommandBuffer cmd)
{
    cmd->pipeline     = NULL;
    cmd->program      = NULL;
    cmd->gsStateDirty = 0;
    cmd->uniformsDirty = 0;
    memset(cmd->sets, 0, sizeof(cmd->sets));
    memset(cmd->vertexBuffers, 0, sizeof(cmd->vertexBuffers));
    memset(cmd->vertexOffsets, 0, sizeof(cmd->vertexOffsets));
    cmd->indexBuffer  = NULL;
    cmd->indexOffset  = 0;
    cmd->pushSet      = 0;
    memset(&cmd->pushLayout, 0, sizeof(cmd->pushLayout));
    cmd->dynamicSet   = 0;
    cmd->viewportDirty = 0;
}

static int is_depth_format(PvkFormat format)
{
    return format == PVK_FORMAT_Z32 || format == PVK_FORMAT_Z24 || format == PVK_FORMAT_Z16;
}

PvkResult pvkBeginCommandBuffer(PvkCommandBuffer cmd, const PvkCommandBufferBeginInfo *info)
{
    PvkCommandPool pool = cmd->pool;
    uint32_t room, flags = info ? info->flags : 0;
    int secondary = cmd->level == PVK_COMMAND_BUFFER_LEVEL_SECONDARY;
    const PvkCommandBufferInheritanceInfo *inherit = info && secondary ? info->inheritanceInfo : NULL;

    if (cmd->recording) {
        pvk_error("pvkBeginCommandBuffer: already recording\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (pool->recording) {
        pvk_error("pvkBeginCommandBuffer: another command buffer of the pool is recording\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (flags & ~(PVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT
                  | PVK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)) {
        pvk_error("pvkBeginCommandBuffer: unknown flags 0x%x\n", (unsigned)flags);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (secondary && !inherit) {
        pvk_error("pvkBeginCommandBuffer: a secondary command buffer needs inheritanceInfo\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (inherit && inherit->stencilAttachmentFormat != PVK_FORMAT_UNDEFINED &&
        inherit->stencilAttachmentFormat != PVK_FORMAT_CT32 && inherit->stencilAttachmentFormat != PVK_FORMAT_CT16) {
        pvk_error("pvkBeginCommandBuffer: inheritanceInfo->stencilAttachmentFormat must be PVK_FORMAT_CT32, "
                  "PVK_FORMAT_CT16 or PVK_FORMAT_UNDEFINED\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (inherit && inherit->depthAttachmentFormat != PVK_FORMAT_UNDEFINED && !is_depth_format(inherit->depthAttachmentFormat)) {
        pvk_error("pvkBeginCommandBuffer: inheritanceInfo->depthAttachmentFormat must be a depth format or PVK_FORMAT_UNDEFINED\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (cmd->size) {
        if (!(pool->flags & PVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)) {
            pvk_error("pvkBeginCommandBuffer: recorded before; reset the pool, or create it with "
                      "PVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT\n");
            return PVK_ERROR_VALIDATION_FAILED;
        }
        pvk_queue_wait_chains(pool_queue(pool));
        if (cmd->start + cmd->size == pool->used)
            pool->used = cmd->start;
        cmd->size = 0;
    }

    /* The chain grows into whatever is free above the stack, a packet2_t's u16 at most. */
    room = pool->qwords - pool->used;
    if (room > 0xffff)
        room = 0xffff;
    if (room == 0) {
        pvk_error("pvkBeginCommandBuffer: command pool full (%u qwords); reset it, or create it with a larger maxQwords\n",
                  (unsigned)pool->qwords);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    cmd->start = pool->used;
    cmd->packet.max_qwords_count   = room;
    cmd->packet.type               = P2_TYPE_NORMAL;
    cmd->packet.mode               = P2_MODE_CHAIN;
    cmd->packet.tte                = pool->family == 0; /* the GIF channel takes no VIF codes */
    cmd->packet.base               = pool->block + cmd->start;
    cmd->packet.next               = cmd->packet.base;
    cmd->packet.tag_opened_at      = NULL;
    cmd->packet.vif_code_opened_at = NULL;
    pool->recording = cmd;

    cmd_reset_state(cmd);
    cmd->generation++;
    cmd->usage        = flags;
    cmd->used         = 0;
    cmd->executedCount = 0;
    cmd_release_copies(cmd);
    cmd->colorView    = NULL;
    cmd->depthView    = NULL;
    /* Inside the primary's rendering from the first command on. */
    cmd->inherits     = secondary && (flags & PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
    cmd->inheritedDepthFormat = inherit ? inherit->depthAttachmentFormat : PVK_FORMAT_UNDEFINED;
    cmd->inheritedStencilFormat = inherit ? inherit->stencilAttachmentFormat : PVK_FORMAT_UNDEFINED;
    cmd->stencilFormat = PVK_FORMAT_UNDEFINED;
    cmd->secondaryContents = 0;
    cmd->rendering    = cmd->inherits;
    cmd->fbmsk[0]     = 0; /* a secondary inside the primary's rendering starts with all channels written */
    cmd->fbmsk[1]     = 0;
    cmd->failed       = 0;
    cmd->recording    = 1;

    /* With a transfer queue, a draw chain holds PATH3 off (VIF MSKPATH3) until its end, so an
     * upload takes the GIF between draw chains and not between the packets of one. */
    if (!secondary && pool->family == 0 && cmd->device->families[1] && cmd_reserve(cmd, 1)) {
        packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
        packet2_vif_mskpath3(&cmd->packet, 0x8000, 0);
        packet2_vif_nop(&cmd->packet, 0);
        packet2_chain_close_tag(&cmd->packet);
    }
    return PVK_SUCCESS;
}

PvkResult pvkEndCommandBuffer(PvkCommandBuffer cmd)
{
    qword_t *q;
    uint32_t c;

    if (cmd->rendering && !cmd->inherits) {
        pvk_error("pvkEndCommandBuffer inside pvkCmdBeginRendering/pvkCmdEndRendering\n");
        cmd->failed = 1;
    }

    /* Inside the primary's rendering: leave FRAME_1 and FRAME_2 writing all channels again. */
    for (c = 0; cmd->inherits && c < 2; c++)
        if (cmd->fbmsk[c] && cmd_reserve(cmd, PVK_FRAME_MASK_QWORDS))
            cmd_frame_mask(cmd, c, 0);

    if (cmd->level == PVK_COMMAND_BUFFER_LEVEL_SECONDARY) {
        /* Back to the primary's chain after the CALL tag that came here. */
        if (cmd_reserve(cmd, 1)) {
            packet2_chain_ret(&cmd->packet, 0, 0);
            if (cmd->packet.tte) {
                packet2_vif_nop(&cmd->packet, 0);
                packet2_vif_nop(&cmd->packet, 0);
            }
            packet2_chain_close_tag(&cmd->packet);
        }
    } else if (cmd->pool->family == 1) {
        /* END tag holding the GS FINISH and SIGNAL; the ID is filled in at submit. */
        if (cmd_reserve(cmd, 4)) {
            packet2_chain_open_end(&cmd->packet, 0, 0);
            q = cmd->packet.next;
            q = gs_ad(q, GIF_SET_TAG(2, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
            q = gs_ad(q, GS_SET_FINISH(0), GS_REG_FINISH);
            cmd->signalQw = q - cmd->packet.base;
            q = gs_ad(q, GS_SET_SIGNAL(0, 0xFFFFFFFF), GS_REG_SIGNAL);
            packet2_update(&cmd->packet, q);
            packet2_chain_close_tag(&cmd->packet);
        }
    } else if (cmd_reserve(cmd, 6)) {
        /* GS FINISH, then SIGNAL: its interrupt releases the submission's fence.
         * The SIGNAL ID is filled in at submit, when the fence is known. */
        q = cmd_open_direct(cmd);
        q->dw[0] = GIF_SET_TAG(2, 1, 0, 0, GIF_FLG_PACKED, 1);
        q->dw[1] = GIF_REG_AD;
        q++;
        q->dw[0] = GS_SET_FINISH(0);
        q->dw[1] = GS_REG_FINISH;
        q++;
        cmd->signalQw = q - cmd->packet.base;
        q->dw[0] = GS_SET_SIGNAL(0, 0xFFFFFFFF);
        q->dw[1] = GS_REG_SIGNAL;
        q++;
        cmd_close_direct(cmd, q);
        /* The END tag lets PATH3 go again (MSKPATH3 off) once the chain is in. */
        packet2_chain_open_end(&cmd->packet, 0, 0);
        if (cmd->device->families[1])
            packet2_vif_mskpath3(&cmd->packet, 0, 0);
        else
            packet2_vif_nop(&cmd->packet, 0);
        packet2_vif_nop(&cmd->packet, 0);
        packet2_chain_close_tag(&cmd->packet);
    }

    /* Keep what was recorded; the rest of the block is free again. */
    cmd->size = packet2_get_qw_count(&cmd->packet);
    cmd->pool->used = cmd->start + cmd->size;
    cmd->pool->recording = NULL;

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
 * Uses at most 6 + 2 * clear_strips(area) qwords. */
static qword_t *cmd_clear(qword_t *q, const PvkRenderingInfo *info, const framebuffer_t *fb, const PvkImage depth,
                          int clearColor, int clearDepth, int stencil, int clearStencil)
{
    const PvkRect2D *area = &info->renderArea;
    const float *rgba = info->colorAttachment.clearValue.color.float32;
    union { float f; u32 u; } one = { 1.0f };
    uint32_t strips = clear_strips(area);
    uint32_t z = 0, s, mask = clearColor ? 0 : 0xFFFFFFFF;
    int y0 = gs_coord(area->y), y1 = gs_coord(area->y + (int32_t)area->height);
    int alpha = (int)(rgba[3] * 128.0f);

    /* With a stencil attachment alpha bit 7 is the stencil bit: cleared to the value, or kept. */
    if (stencil) {
        mask  = clearStencil ? mask & ~0x80000000u : mask | 0x80000000u;
        alpha = (alpha & 0x7F) | (clearStencil && (info->stencilAttachment.clearValue.depthStencil.stencil & 1) ? 0x80 : 0);
    }

    if (clearDepth)
        z = gs_depth(info->depthAttachment.clearValue.depthStencil.depth, depth->format);

    q = gs_ad(q, GIF_SET_TAG(5 + (stencil != 0) + 2 * strips, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    q = gs_ad(q, GS_SET_FRAME(fb->address >> 11, fb->width >> 6, fb->psm, mask), GS_REG_FRAME);
    /* The clear writes the stencil bit itself; without a stencil attachment FBA stays libdraw's. */
    if (stencil)
        q = gs_ad(q, GS_SET_FBA(0), GS_REG_FBA_1);
    q = gs_ad(q, GS_SET_ZBUF(depth ? depth->fb.address >> 11 : 0, depth ? gs_zsm(depth->format) : GS_ZBUF_32,
                             clearDepth ? 0 : 1), GS_REG_ZBUF);
    q = gs_ad(q, GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, ZTEST_METHOD_ALLPASS), GS_REG_TEST);
    q = gs_ad(q, GS_SET_PRIM(PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), GS_REG_PRIM);
    q = gs_ad(q, GS_SET_RGBAQ((int)(rgba[0] * 255.0f), (int)(rgba[1] * 255.0f),
                              (int)(rgba[2] * 255.0f), alpha, one.u), GS_REG_RGBAQ);
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

/* Checks an attachment's load and store ops are values of their enums. */
static int attachment_ops_valid(const PvkRenderingAttachmentInfo *a, const char *which)
{
    switch (a->loadOp) {
    case PVK_ATTACHMENT_LOAD_OP_LOAD:
    case PVK_ATTACHMENT_LOAD_OP_CLEAR:
    case PVK_ATTACHMENT_LOAD_OP_DONT_CARE:
    case PVK_ATTACHMENT_LOAD_OP_NONE:
        break;
    default:
        pvk_error("pvkCmdBeginRendering: the %s attachment's loadOp %d is not a PvkAttachmentLoadOp\n", which, (int)a->loadOp);
        return 0;
    }
    switch (a->storeOp) {
    case PVK_ATTACHMENT_STORE_OP_STORE:
    case PVK_ATTACHMENT_STORE_OP_DONT_CARE:
    case PVK_ATTACHMENT_STORE_OP_NONE:
        break;
    default:
        pvk_error("pvkCmdBeginRendering: the %s attachment's storeOp %d is not a PvkAttachmentStoreOp\n", which, (int)a->storeOp);
        return 0;
    }
    return 1;
}

void pvkCmdBeginRendering(PvkCommandBuffer cmd, const PvkRenderingInfo *info)
{
    const PvkRenderingAttachmentInfo *color = &info->colorAttachment;
    const PvkRenderingAttachmentInfo *depth = &info->depthAttachment;
    PvkImageView colorView = color->imageView, depthView = depth->imageView;
    int clearColor = color->loadOp == PVK_ATTACHMENT_LOAD_OP_CLEAR;
    int clearDepth = depthView && depth->loadOp == PVK_ATTACHMENT_LOAD_OP_CLEAR;
    PvkImage depthImage = depthView ? depthView->image : NULL;
    const PvkRenderingAttachmentInfo *stencil = &info->stencilAttachment;
    int clearStencil = stencil->imageView && stencil->loadOp == PVK_ATTACHMENT_LOAD_OP_CLEAR;
    framebuffer_t fb;
    zbuffer_t z = { 0 };
    qword_t *q;

    if (!cmd_graphics(cmd, "pvkCmdBeginRendering"))
        return;
    if (cmd->rendering) {
        pvk_error("pvkCmdBeginRendering: already rendering%s\n", cmd->inherits
                  ? " (a secondary command buffer recorded with PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT "
                    "is inside the primary's)" : "");
        cmd->failed = 1;
        return;
    }
    if (info->flags & ~PVK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) {
        pvk_error("pvkCmdBeginRendering: unknown flags 0x%x\n", (unsigned)info->flags);
        cmd->failed = 1;
        return;
    }
    if (!colorView || !(colorView->image->usage & PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || colorView->levels != 1) {
        pvk_error("pvkCmdBeginRendering: the color attachment must be a one-level view of a swapchain image or "
                  "an image created with PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT\n");
        cmd->failed = 1;
        return;
    }
    if (!attachment_ops_valid(color, "color") || (depthView && !attachment_ops_valid(depth, "depth")) ||
        (stencil->imageView && !attachment_ops_valid(stencil, "stencil"))) {
        cmd->failed = 1;
        return;
    }
    if (stencil->imageView && (stencil->imageView->image != colorView->image ||
                               stencil->imageView->baseLevel != colorView->baseLevel ||
                               pvk_format_bits(colorView->image->format) == 24)) {
        pvk_error("pvkCmdBeginRendering: the stencil attachment must be a view of the color attachment's level, "
                  "and that image PVK_FORMAT_CT32 or PVK_FORMAT_CT16: the stencil is the top alpha bit of its pixels\n");
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

    if (!cmd_reserve(cmd, 56 + (clearColor || clearDepth || clearStencil ? 6 + 2 * clear_strips(&info->renderArea) : 0)))
        return;

    /* Both GS register sets point at the attachments: context 1 writes depth,
     * context 2 (ZBUF_2.ZMSK = 1) does not. A pipeline picks one with PRIM.CTXT,
     * so draws never write ZBUF or FRAME's address and need not know the addresses. */
    z.enable  = depthImage != NULL;
    z.method  = ZTEST_METHOD_ALLPASS;
    z.address = depthImage ? depthImage->fb.address : 0;
    z.zsm     = depthImage ? gs_zsm(depthImage->format) : GS_ZBUF_32;
    z.mask    = 1;

    q = cmd_open_direct(cmd);
    q = draw_setup_environment(q, 1, &fb, &z);
    q = draw_primitive_xyoffset(q, 1, PVK_GS_ORIGIN, PVK_GS_ORIGIN);
    z.mask    = depthImage ? 0 : 1;
    q = draw_setup_environment(q, 0, &fb, &z);
    q = draw_primitive_xyoffset(q, 0, PVK_GS_ORIGIN, PVK_GS_ORIGIN);
    if (clearColor || clearDepth || clearStencil) {
        q = cmd_clear(q, info, &fb, depthImage, clearColor, clearDepth, stencil->imageView != NULL, clearStencil);
        /* The clear masked FRAME_1/ZBUF_1; restore context 1. */
        q = gs_ad(q, GIF_SET_TAG(2, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        q = gs_ad(q, GS_SET_FRAME(fb.address >> 11, fb.width >> 6, fb.psm, 0), GS_REG_FRAME_1);
        q = gs_ad(q, GS_SET_ZBUF(z.address >> 11, z.zsm, z.mask), GS_REG_ZBUF_1);
    }
    cmd_close_direct(cmd, q);
    cmd_frame_packet(cmd, &fb);

    cmd->colorView    = colorView;
    cmd->depthView    = depthView;
    cmd->stencilFormat = stencil->imageView ? colorView->image->format : PVK_FORMAT_UNDEFINED;
    cmd->secondaryContents = (info->flags & PVK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) != 0;
    cmd->gsStateDirty = 1;
    cmd->rendering    = 1;
}

/* Chain qwords cmd_gs_state() uses. */
#define PVK_GS_STATE_QWORDS 17

/* The scissor the draws use (the pipeline's, or the command buffer's when dynamic),
 * clipped to the attachment level, or to the GS's 2048x2048 when the attachment
 * is the primary's and unknown. Returns 0 when nothing is left. */
static int cmd_scissor(PvkCommandBuffer cmd, const framebuffer_t *fb, const char *name, PvkRect2D *out)
{
    const PvkPipeline p = cmd->pipeline;
    const PvkRect2D *r = p->dynamic & (1u << PVK_DYNAMIC_STATE_SCISSOR) ? &cmd->scissor : &p->scissor;
    int32_t x0 = 0, y0 = 0, x1 = fb ? (int32_t)fb->width : 2048, y1 = fb ? (int32_t)fb->height : 2048;

    if (!fb && !(r->width && r->height)) {
        pvk_error("%s: a secondary command buffer recorded with PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT "
                  "does not know the attachment; the pipeline's scissor (or pvkCmdSetScissor) must give a rectangle\n", name);
        cmd->failed = 1;
        return 0;
    }
    if (r->width && r->height) {
        if (r->x > x0) x0 = r->x;
        if (r->y > y0) y0 = r->y;
        if (r->x + (int32_t)r->width < x1)  x1 = r->x + (int32_t)r->width;
        if (r->y + (int32_t)r->height < y1) y1 = r->y + (int32_t)r->height;
    }
    out->x = x0;
    out->y = y0;
    out->width  = x1 > x0 ? (uint32_t)(x1 - x0) : 0;
    out->height = y1 > y0 ? (uint32_t)(y1 - y0) : 0;
    return out->width && out->height;
}

/* TEST, ALPHA and SCISSOR for drawing with the bound pipeline, in the GS context its
 * PRIM.CTXT picks, plus TEX0, TEXA, TEX1, CLAMP and the MIPTBPs for the image and sampler
 * in the bound descriptor set when the pipeline samples one. Sent before the first draw
 * after pvkCmdBeginRendering, pvkCmdBindPipeline, pvkCmdBindDescriptorSets or a dynamic
 * scissor or blend constant change. ZBUF stays as pvkCmdBeginRendering set it. */
static void cmd_gs_state(PvkCommandBuffer cmd, const PvkRect2D *scissor)
{
    const PvkPipeline p = cmd->pipeline;
    int textured = p->fs->textured;
    const struct PvkDescriptorSlot *slot = textured ? &cmd->sets[p->samplerSet]->slots[p->samplerSlot] : NULL;
    int mipmapped = textured && slot->view->levels > 1;
    u64 alpha = p->alpha, ctxt = p->zmsk;
    qword_t *q;

    if (p->blendFix && p->dynamic & (1u << PVK_DYNAMIC_STATE_BLEND_CONSTANTS)) {
        float k = cmd->blendConstants[3];
        k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
        alpha = (alpha & ~((u64)0xFF << 32)) | ((u64)(int)(k * 128.0f + 0.5f) << 32);
    }

    q = cmd_open_direct(cmd);
    q = gs_ad(q, GIF_SET_TAG((textured ? (mipmapped ? 9 : 7) : 3) + (p->stencilFormat != PVK_FORMAT_UNDEFINED), 0, 0,
                             0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    /* No alpha test (Vulkan has none); depth test and stencil test (DATE/DATM) from the pipeline. */
    q = gs_ad(q, GS_SET_TEST(0, 0, 0, 0, p->date, p->datm, 1, p->ztst), GS_REG_TEST_1 + ctxt);
    if (p->stencilFormat != PVK_FORMAT_UNDEFINED)
        q = gs_ad(q, GS_SET_FBA(p->fba), GS_REG_FBA_1 + ctxt);
    q = gs_ad(q, alpha, GS_REG_ALPHA_1 + ctxt);
    q = gs_ad(q, GS_SET_SCISSOR(scissor->x, scissor->x + (int32_t)scissor->width - 1,
                                scissor->y, scissor->y + (int32_t)scissor->height - 1), GS_REG_SCISSOR_1 + ctxt);
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
                                 c ? 1 : 0), GS_REG_TEX0_1 + ctxt);
        /* TEXA: CT24 reads opaque, a set CT16 alpha bit is 0x80. */
        q = gs_ad(q, GS_SET_TEXA(t->format == PVK_FORMAT_CT24 ? 0x80 : 0, 0, 0x80), GS_REG_TEXA);
        q = gs_ad(q, GS_SET_TEX1(LOD_FORMULAIC, mxl, s->mmag, mxl ? s->mmip : s->mmin, 0, 0, s->k), GS_REG_TEX1_1 + ctxt);
        q = gs_ad(q, GS_SET_CLAMP(s->wms, s->wmt, 0, 0, 0, 0), GS_REG_CLAMP_1 + ctxt);
        if (mipmapped) {
            q = gs_ad(q, GS_SET_MIPTBP1(a[1] >> 6, w[1], a[2] >> 6, w[2], a[3] >> 6, w[3]), GS_REG_MIPTBP1_1 + ctxt);
            q = gs_ad(q, GS_SET_MIPTBP2(a[4] >> 6, w[4], a[5] >> 6, w[5], a[6] >> 6, w[6]), GS_REG_MIPTBP2_1 + ctxt);
        }
    }
    cmd_close_direct(cmd, q);

    cmd->gsStateDirty = 0;
}

void pvkCmdBindDescriptorSets(PvkCommandBuffer cmd, PvkPipelineLayout layout, uint32_t firstSet,
                              uint32_t setCount, const PvkDescriptorSet *sets)
{
    uint32_t i;

    if (!cmd_graphics(cmd, "pvkCmdBindDescriptorSets"))
        return;
    if (firstSet + setCount > layout->setLayoutCount || firstSet + setCount > pvk_limits.maxBoundDescriptorSets) {
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

/* Slot of color table entry i in the GS's CSM1 layout: a 256-entry table swaps index bits 3 and 4. */
static uint32_t table_slot(uint32_t entries, uint32_t i)
{
    return entries == 256 ? (i & ~0x18u) | ((i & 8) << 1) | ((i & 16) >> 1) : i;
}

/* A copy region checked against its image and buffer. */
struct CopyRegion {
    uint32_t level, x, y, w, h;
    uint32_t rowBytes; /* bytes of one region row */
    uint32_t pitch;    /* bytes from one buffer row to the next */
    uint32_t bytes;    /* rowBytes * h */
    int      direct;   /* DMA can use the buffer in place */
};

static int copy_region(PvkCommandBuffer cmd, const char *who, PvkBuffer buffer, PvkImage image,
                       const PvkBufferImageCopy *r, struct CopyRegion *out)
{
    const PvkImageSubresourceLayers *sub = &r->imageSubresource;
    uint32_t bits = pvk_format_bits(image->format);
    /* A T4 byte holds 2 texels: T4 regions are copied in 2-texel blocks. */
    uint32_t block = bits == 4 ? 2 : 1, blockBytes = bits == 4 ? 1 : bits / 8;
    uint32_t w = r->imageExtent.width, h = r->imageExtent.height, width, height;
    uint32_t rowLength = r->bufferRowLength ? r->bufferRowLength : w;

    if (sub->aspectMask != PVK_IMAGE_ASPECT_COLOR_BIT || sub->baseArrayLayer != 0 || sub->layerCount != 1 ||
        sub->mipLevel >= image->levels) {
        pvk_error("%s: imageSubresource must name PVK_IMAGE_ASPECT_COLOR_BIT, layer 0, 1 layer and a mip level "
                  "below %u (aspect 0x%x, layer %u, %u layers, level %u given)\n", who, (unsigned)image->levels,
                  (unsigned)sub->aspectMask, (unsigned)sub->baseArrayLayer, (unsigned)sub->layerCount,
                  (unsigned)sub->mipLevel);
        cmd->failed = 1;
        return 0;
    }
    width  = image->fb.width >> sub->mipLevel;
    height = image->fb.height >> sub->mipLevel;
    if (r->imageOffset.x < 0 || r->imageOffset.y < 0 || r->imageOffset.z != 0 || r->imageExtent.depth != 1 ||
        w == 0 || h == 0 || (uint32_t)r->imageOffset.x > width || w > width - (uint32_t)r->imageOffset.x ||
        (uint32_t)r->imageOffset.y > height || h > height - (uint32_t)r->imageOffset.y) {
        pvk_error("%s: the region (%d, %d, %d) %ux%ux%u is outside the %ux%ux1 level %u\n", who,
                  (int)r->imageOffset.x, (int)r->imageOffset.y, (int)r->imageOffset.z, (unsigned)w, (unsigned)h,
                  (unsigned)r->imageExtent.depth, (unsigned)width, (unsigned)height, (unsigned)sub->mipLevel);
        cmd->failed = 1;
        return 0;
    }
    if (rowLength < w || (r->bufferImageHeight && r->bufferImageHeight < h)) {
        pvk_error("%s: bufferRowLength %u and bufferImageHeight %u must be 0 or at least the region's %ux%u\n", who,
                  (unsigned)r->bufferRowLength, (unsigned)r->bufferImageHeight, (unsigned)w, (unsigned)h);
        cmd->failed = 1;
        return 0;
    }
    if ((uint32_t)r->imageOffset.x % block || w % block || rowLength % block) {
        pvk_error("%s: a PVK_FORMAT_T4 byte holds 2 texels: imageOffset.x, imageExtent.width and bufferRowLength "
                  "must be even\n", who);
        cmd->failed = 1;
        return 0;
    }
    out->level    = sub->mipLevel;
    out->x        = (uint32_t)r->imageOffset.x;
    out->y        = (uint32_t)r->imageOffset.y;
    out->w        = w;
    out->h        = h;
    out->rowBytes = w / block * blockBytes;
    out->pitch    = rowLength / block * blockBytes;
    out->bytes    = out->rowBytes * h;
    if (r->bufferOffset > buffer->size ||
        (size_t)(h - 1) * out->pitch + out->rowBytes > buffer->size - r->bufferOffset) {
        pvk_error("%s: the region's rows end past the end of the %u-byte buffer\n", who, (unsigned)buffer->size);
        cmd->failed = 1;
        return 0;
    }
    /* DMA moves whole qwords from or to a 16-byte boundary. */
    out->direct = r->bufferOffset % 16 == 0 && out->bytes % 16 == 0 && (out->pitch == out->rowBytes || h == 1);
    return 1;
}

/* What every copy command checks first. */
static int copy_begin(PvkCommandBuffer cmd, const char *who, PvkBuffer buffer, PvkImage image, uint32_t usageBit,
                      const char *usageName, PvkImageLayout layout, PvkImageLayout optimal)
{
    if (!(image->usage & usageBit)) {
        pvk_error("%s: the image was not created with %s\n", who, usageName);
    } else if (!buffer->data || !image->bound) {
        pvk_error("%s: the buffer and the image need memory bound\n", who);
    } else if (cmd->rendering) {
        pvk_error("%s must be called outside pvkCmdBeginRendering/pvkCmdEndRendering\n", who);
    } else if (layout != optimal && layout != PVK_IMAGE_LAYOUT_GENERAL) {
        pvk_error("%s: image layout %d is not the transfer layout of the copy or PVK_IMAGE_LAYOUT_GENERAL\n", who,
                  (int)layout);
    } else {
        return 1;
    }
    cmd->failed = 1;
    return 0;
}

/* Zeroed EE RAM of whole qwords that DMA reads or writes for a region; owned by the recording. */
static void *stage_alloc(PvkCommandBuffer cmd, const char *who, uint32_t qwords)
{
    size_t size = ((size_t)qwords * 16 + 63) & ~(size_t)63;
    void *stage = memalign(64, size);

    if (!stage) {
        pvk_error("%s: out of EE RAM for a %u-byte copy of the region\n", who, (unsigned)size);
        cmd->failed = 1;
        return NULL;
    }
    memset(stage, 0, size);
    return stage;
}

/* Room for one more entry in a growing array; fails the recording when out of memory. */
static int array_grow(PvkCommandBuffer cmd, void **array, uint32_t count, uint32_t *cap, size_t size)
{
    void *grown;

    if (count < *cap)
        return 1;
    grown = realloc(*array, (*cap ? *cap * 2 : 4) * size);
    if (!grown) {
        pvk_error("copy command: out of memory\n");
        cmd->failed = 1;
        return 0;
    }
    *array = grown;
    *cap   = *cap ? *cap * 2 : 4;
    return 1;
}

/* Frees the staged regions of the last recording. */
static void cmd_release_copies(PvkCommandBuffer cmd)
{
    uint32_t i;

    for (i = 0; i < cmd->readbackCount; i++)
        if (cmd->readbacks[i].owned)
            free(cmd->readbacks[i].stage);
    for (i = 0; i < cmd->uploadCount; i++)
        free(cmd->uploads[i].stage);
    cmd->readbackCount = 0;
    cmd->uploadCount   = 0;
}

static void pack_uploads(PvkCommandBuffer cmd)
{
    uint32_t i, r;

    for (i = 0; i < cmd->uploadCount; i++) {
        const struct PvkUpload *u = &cmd->uploads[i];
        for (r = 0; r < u->rows; r++)
            memcpy((uint8_t *)u->stage + (size_t)r * u->rowBytes, u->src + (size_t)r * u->srcPitch, u->rowBytes);
    }
}

void pvk_cmd_pack_uploads(PvkCommandBuffer cmd)
{
    uint32_t i;

    pack_uploads(cmd);
    for (i = 0; i < cmd->executedCount; i++)
        pack_uploads(cmd->executed[i].cmd);
}

/* One host-to-local transfer of h buffer rows into a rectangle; the rows are staged unless DMA
 * can stream them in place. On VIF1 each block is a DIRECT; on the GIF channel every chunk ends its GIF packet. */
static int cmd_upload(PvkCommandBuffer cmd, const char *who, uint32_t address, uint32_t bufferWidth, uint32_t psm,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *src, uint32_t rowBytes, uint32_t pitch)
{
    uint32_t bytes = rowBytes * h, qwords = (bytes + 15) / 16, sent = 0;
    int path3 = cmd->pool->family == 1;
    qword_t *q;

    if ((uintptr_t)src % 16 || bytes % 16 || (pitch != rowBytes && h != 1)) {
        /* pvkQueueSubmit packs the rows into the stage; the GS drops what follows the last texel. */
        struct PvkUpload *u;
        void *stage;

        if (!array_grow(cmd, (void **)&cmd->uploads, cmd->uploadCount, &cmd->uploadCap, sizeof(*cmd->uploads)) ||
            !(stage = stage_alloc(cmd, who, qwords)))
            return 0;
        u = &cmd->uploads[cmd->uploadCount++];
        u->stage    = stage;
        u->src      = src;
        u->rows     = h;
        u->rowBytes = rowBytes;
        u->srcPitch = pitch;
        src = stage;
    }
    if (!cmd_reserve(cmd, 7 + 3 * ((qwords + PVK_IMAGE_CHUNK_QWORDS - 1) / PVK_IMAGE_CHUNK_QWORDS)))
        return 0;

    q = path3 ? cmd_open_gif(cmd) : cmd_open_direct(cmd);
    q = gs_ad(q, GIF_SET_TAG(4, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    q = gs_ad(q, GS_SET_BITBLTBUF(0, 0, 0, address >> 6, bufferWidth, psm), GS_REG_BITBLTBUF);
    q = gs_ad(q, GS_SET_TRXPOS(0, 0, x, y, 0), GS_REG_TRXPOS);
    q = gs_ad(q, GS_SET_TRXREG(w, h), GS_REG_TRXREG);
    q = gs_ad(q, GS_SET_TRXDIR(0), GS_REG_TRXDIR); /* host to local */
    packet2_update(&cmd->packet, q);
    if (!path3)
        packet2_vif_close_direct_auto(&cmd->packet);
    packet2_chain_close_tag(&cmd->packet);

    /* Per chunk an IMAGE tag, then a REF tag streaming the texels (on VIF1 with the DIRECT code). */
    while (sent < qwords) {
        uint32_t chunk = qwords - sent < PVK_IMAGE_CHUNK_QWORDS ? qwords - sent : PVK_IMAGE_CHUNK_QWORDS;
        int eop = path3 || sent + chunk == qwords;

        packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
        if (!path3) {
            packet2_vif_nop(&cmd->packet, 0);
            packet2_vif_open_direct(&cmd->packet, 0);
        }
        packet2_add_2x_s64(&cmd->packet, GIF_SET_TAG(chunk, eop, 0, 0, GIF_FLG_IMAGE, 0), 0);
        if (!path3)
            packet2_vif_close_direct_auto(&cmd->packet);
        packet2_chain_close_tag(&cmd->packet);

        packet2_chain_ref(&cmd->packet, src + (size_t)sent * 16, chunk, 0, 0, 0);
        if (!path3) {
            packet2_vif_nop(&cmd->packet, 0);
            packet2_add_u32(&cmd->packet, MAKE_VIF_CODE(chunk, 0, P2_VIF_DIRECT, 0));
        }
        sent += chunk;
    }
    return 1;
}

void pvkCmdCopyBufferToImage(PvkCommandBuffer cmd, PvkBuffer buffer, PvkImage image, PvkImageLayout layout,
                             uint32_t regionCount, const PvkBufferImageCopy *regions)
{
    static const char who[] = "pvkCmdCopyBufferToImage";
    uint32_t r;

    if (!copy_begin(cmd, who, buffer, image, PVK_IMAGE_USAGE_TRANSFER_DST_BIT, "PVK_IMAGE_USAGE_TRANSFER_DST_BIT",
                    layout, PVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))
        return;
    for (r = 0; r < regionCount; r++) {
        const uint8_t *src = (const uint8_t *)buffer->data + regions[r].bufferOffset;
        struct CopyRegion cr;

        if (!copy_region(cmd, who, buffer, image, &regions[r], &cr))
            return;
        if (image->colorTable) {
            /* A transfer per run of entries that are neighbors in the table's CSM1 layout. */
            uint32_t entries = image->fb.width, bytes = cr.rowBytes / cr.w, cols = entries == 256 ? 16 : 8, i, j;

            for (i = cr.x; i < cr.x + cr.w; i = j) {
                uint32_t slot = table_slot(entries, i);

                for (j = i + 1; j < cr.x + cr.w; j++)
                    if (table_slot(entries, j) != slot + (j - i) || table_slot(entries, j) / cols != slot / cols)
                        break;
                if (!cmd_upload(cmd, who, image->fb.address, 1, image->fb.psm, slot % cols, slot / cols, j - i, 1,
                                src + (i - cr.x) * bytes, (j - i) * bytes, (j - i) * bytes))
                    return;
            }
        } else if (!cmd_upload(cmd, who, image->levelAddress[cr.level], image->levelTbw[cr.level], image->fb.psm,
                               cr.x, cr.y, cr.w, cr.h, src, cr.rowBytes, cr.pitch)) {
            return;
        }
    }
}

void pvkCmdCopyImageToBuffer(PvkCommandBuffer cmd, PvkImage image, PvkImageLayout layout, PvkBuffer buffer,
                             uint32_t regionCount, const PvkBufferImageCopy *regions)
{
    static const char who[] = "pvkCmdCopyImageToBuffer";
    uint32_t i;

    if (!copy_begin(cmd, who, buffer, image, PVK_IMAGE_USAGE_TRANSFER_SRC_BIT, "PVK_IMAGE_USAGE_TRANSFER_SRC_BIT",
                    layout, PVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))
        return;
    for (i = 0; i < regionCount; i++) {
        const PvkBufferImageCopy *region = &regions[i];
        struct PvkReadback rb = { 0 };
        struct CopyRegion cr;
        uint32_t address, bufferWidth, x, y, w, h;
        qword_t *q;
        u32 *vif;

        if (!copy_region(cmd, who, buffer, image, region, &cr))
            return;
        address     = image->levelAddress[cr.level];
        bufferWidth = image->levelTbw[cr.level];
        rb.out      = (uint8_t *)buffer->data + region->bufferOffset;
        if (image->colorTable) {
            /* The whole table in CSM1 layout; the handler picks the entries. */
            rb.tableEntries = image->fb.width;
            rb.tableFirst   = cr.x;
            rb.rows         = cr.w;
            rb.rowBytes     = cr.rowBytes / cr.w;
            x = y = 0;
            w = rb.tableEntries == 256 ? 16 : 8;
            h = rb.tableEntries / w;
            bufferWidth = 1;
            rb.qwords   = rb.tableEntries * rb.rowBytes / 16;
        } else if (cr.direct) {
            x = cr.x, y = cr.y, w = cr.w, h = cr.h;
            rb.dst    = rb.out;
            rb.qwords = cr.bytes / 16;
        } else {
            /* Whole buffer-width rows are whole qwords in every format. */
            x = 0, y = cr.y, w = bufferWidth * 64, h = cr.h;
            rb.rows       = cr.h;
            rb.rowBytes   = cr.rowBytes;
            rb.outPitch   = cr.pitch;
            rb.stagePitch = w * cr.rowBytes / cr.w;
            rb.stageSkip  = cr.x * cr.rowBytes / cr.w;
            rb.qwords     = rb.stagePitch * h / 16;
        }
        if (!cmd_reserve(cmd, PVK_READBACK_SETUP_QWORDS + 12) ||
            !array_grow(cmd, (void **)&cmd->readbacks, cmd->readbackCount, &cmd->readbackCap, sizeof(*cmd->readbacks)))
            return;
        if (!rb.dst) {
            rb.stage = stage_alloc(cmd, who, rb.qwords);
            if (!rb.stage)
                return;
            rb.dst   = rb.stage;
            rb.owned = 1;
        }

        /* FINISH and a SIGNAL, then the chain ends: the INTC_GS handler takes over from here. */
        q = cmd_open_gs(cmd);
        q = gs_ad(q, GIF_SET_TAG(2, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        q = gs_ad(q, GS_SET_FINISH(0), GS_REG_FINISH);
        q = gs_ad(q, GS_SET_SIGNAL(PVK_READBACK_SIGNAL_BIT | cmd->pool->family << 8, 0xFFFFFFFF), GS_REG_SIGNAL);
        cmd_close_gs(cmd, q);
        packet2_chain_open_end(&cmd->packet, 0, 0);
        if (cmd->packet.tte) {
            packet2_vif_nop(&cmd->packet, 0);
            packet2_vif_nop(&cmd->packet, 0);
        }
        packet2_chain_close_tag(&cmd->packet);

        /* Not part of the chain: the handler sends these qwords down VIF1 in normal mode. */
        rb.setup = (qword_t *)cmd->packet.next;
        vif = (u32 *)rb.setup;
        vif[0] = MAKE_VIF_CODE(0, 0, P2_VIF_NOP, 0);
        vif[1] = MAKE_VIF_CODE(0x8000, 0, P2_VIF_MSKPATH3, 0);
        vif[2] = MAKE_VIF_CODE(0, 0, P2_VIF_FLUSHA, 0);
        vif[3] = MAKE_VIF_CODE(PVK_READBACK_SETUP_QWORDS - 1, 0, P2_VIF_DIRECT, 0);
        q = rb.setup + 1;
        q = gs_ad(q, GIF_SET_TAG(6, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        q = gs_ad(q, GS_SET_BITBLTBUF(address >> 6, bufferWidth, image->fb.psm, 0, 0, 0), GS_REG_BITBLTBUF);
        q = gs_ad(q, GS_SET_TRXPOS(x, y, 0, 0, 0), GS_REG_TRXPOS);
        q = gs_ad(q, GS_SET_TRXREG(w, h), GS_REG_TRXREG);
        q = gs_ad(q, GS_SET_FINISH(0), GS_REG_FINISH);
        q = gs_ad(q, GS_SET_SIGNAL(PVK_READBACK_GO_BIT, 0xFFFFFFFF), GS_REG_SIGNAL);
        q = gs_ad(q, GS_SET_TRXDIR(1), GS_REG_TRXDIR); /* local to host */
        packet2_update(&cmd->packet, q);

        /* The chain goes on here; a draw chain holds PATH3 off as pvkBeginCommandBuffer set it. */
        rb.resume = (qword_t *)cmd->packet.next;
        if (cmd->pool->family == 0 && cmd->device->families[1]) {
            packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
            packet2_vif_mskpath3(&cmd->packet, 0x8000, 0);
            packet2_vif_nop(&cmd->packet, 0);
            packet2_chain_close_tag(&cmd->packet);
        }
        cmd->readbacks[cmd->readbackCount++] = rb;
    }
}

/* Stages after which the GS may still be drawing: everything past the vertex shader. */
#define PVK_GS_STAGES (PVK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | PVK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | \
                       PVK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | PVK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | \
                       PVK_PIPELINE_STAGE_TRANSFER_BIT | PVK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | \
                       PVK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | PVK_PIPELINE_STAGE_ALL_COMMANDS_BIT)

/* Both PVK_QUEUE_FAMILY_IGNORED, or two families the device has, one of them the command buffer's. */
static int barrier_queue_families(PvkCommandBuffer cmd, const char *what, uint32_t src, uint32_t dst)
{
    PvkDevice dev = cmd->device;

    if ((src == PVK_QUEUE_FAMILY_IGNORED) != (dst == PVK_QUEUE_FAMILY_IGNORED) ||
        (src != PVK_QUEUE_FAMILY_IGNORED && (src >= PVK_MAX_QUEUE_FAMILIES || dst >= PVK_MAX_QUEUE_FAMILIES ||
                                             !dev->families[src] || !dev->families[dst]))) {
        pvk_error("pvkCmdPipelineBarrier: %s barrier names queue family %u -> %u; both PVK_QUEUE_FAMILY_IGNORED, "
                  "or two families the device was created with\n", what, (unsigned)src, (unsigned)dst);
        cmd->failed = 1;
        return 0;
    }
    if (src != dst && src != cmd->pool->family && dst != cmd->pool->family) {
        pvk_error("pvkCmdPipelineBarrier: %s barrier hands over from family %u to %u, but the command buffer is "
                  "of family %u\n", what, (unsigned)src, (unsigned)dst, (unsigned)cmd->pool->family);
        cmd->failed = 1;
        return 0;
    }
    return 1;
}

void pvkCmdPipelineBarrier(PvkCommandBuffer cmd, PvkPipelineStageFlags srcStageMask, PvkPipelineStageFlags dstStageMask,
                           PvkDependencyFlags dependencyFlags,
                           uint32_t memoryBarrierCount, const PvkMemoryBarrier *memoryBarriers,
                           uint32_t bufferMemoryBarrierCount, const PvkBufferMemoryBarrier *bufferMemoryBarriers,
                           uint32_t imageMemoryBarrierCount, const PvkImageMemoryBarrier *imageMemoryBarriers)
{
    uint32_t i, dstAccess = 0;
    int fromTransfer = 0;
    qword_t *q;

    (void)dstStageMask;
    if (cmd->rendering) {
        pvk_error("pvkCmdPipelineBarrier must be called outside pvkCmdBeginRendering/pvkCmdEndRendering\n");
        cmd->failed = 1;
        return;
    }
    if (dependencyFlags & ~PVK_DEPENDENCY_BY_REGION_BIT) {
        pvk_error("pvkCmdPipelineBarrier: unknown dependency flags 0x%x\n", (unsigned)dependencyFlags);
        cmd->failed = 1;
        return;
    }
    for (i = 0; i < memoryBarrierCount; i++)
        dstAccess |= memoryBarriers[i].dstAccessMask;
    for (i = 0; i < bufferMemoryBarrierCount; i++) {
        const PvkBufferMemoryBarrier *b = &bufferMemoryBarriers[i];
        if (!barrier_queue_families(cmd, "buffer", b->srcQueueFamilyIndex, b->dstQueueFamilyIndex))
            return;
        if (!b->buffer || b->offset > b->buffer->size ||
            (b->size != PVK_WHOLE_SIZE && b->offset + b->size > b->buffer->size)) {
            pvk_error("pvkCmdPipelineBarrier: buffer barrier %u covers bytes outside the buffer\n", (unsigned)i);
            cmd->failed = 1;
            return;
        }
        dstAccess |= b->dstAccessMask;
        fromTransfer |= b->srcQueueFamilyIndex == 1 && b->dstQueueFamilyIndex == 0;
    }
    for (i = 0; i < imageMemoryBarrierCount; i++) {
        const PvkImageMemoryBarrier *b = &imageMemoryBarriers[i];
        const PvkImageSubresourceRange *r = &b->subresourceRange;
        uint32_t aspect;

        if (!barrier_queue_families(cmd, "image", b->srcQueueFamilyIndex, b->dstQueueFamilyIndex))
            return;
        if (!b->image) {
            pvk_error("pvkCmdPipelineBarrier: image barrier %u has no image\n", (unsigned)i);
            cmd->failed = 1;
            return;
        }
        aspect = pvk_is_color_format(b->image->format) ? PVK_IMAGE_ASPECT_COLOR_BIT : PVK_IMAGE_ASPECT_DEPTH_BIT;
        if (r->aspectMask != aspect) {
            pvk_error("pvkCmdPipelineBarrier: image barrier %u names aspect 0x%x; the image's is 0x%x\n",
                      (unsigned)i, (unsigned)r->aspectMask, (unsigned)aspect);
            cmd->failed = 1;
            return;
        }
        if (r->baseMipLevel >= b->image->levels ||
            (r->levelCount != PVK_REMAINING_MIP_LEVELS && r->baseMipLevel + r->levelCount > b->image->levels) ||
            r->baseArrayLayer != 0 || (r->layerCount != 1 && r->layerCount != PVK_REMAINING_ARRAY_LAYERS)) {
            pvk_error("pvkCmdPipelineBarrier: image barrier %u covers levels or layers the image does not have\n",
                      (unsigned)i);
            cmd->failed = 1;
            return;
        }
        dstAccess |= b->dstAccessMask;
        fromTransfer |= b->srcQueueFamilyIndex == 1 && b->dstQueueFamilyIndex == 0;
    }
    /* The GIF channel carries no VIF codes, and the GS takes the transfer queue's copies in order. */
    if (cmd->pool->family == 1)
        return;
    if (!cmd_reserve(cmd, 5))
        return;

    /* Taking over from the transfer queue: FLUSHA also waits for the PATH3 packet the GIF is taking. */
    if (fromTransfer) {
        packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
        packet2_vif_flusha(&cmd->packet, 0);
        packet2_vif_nop(&cmd->packet, 0);
        packet2_chain_close_tag(&cmd->packet);
    }
    if (dstAccess & PVK_ACCESS_SHADER_READ_BIT) {
        /* The DIRECT block opens with a FLUSH; TEXFLUSH drops the texels the cache holds. */
        q = cmd_open_direct(cmd);
        q = gs_ad(q, GIF_SET_TAG(1, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        q = gs_ad(q, GS_SET_TEXFLUSH(0), GS_REG_TEXFLUSH);
        cmd_close_direct(cmd, q);
    } else if (srcStageMask & PVK_GS_STAGES) {
        cmd_flush(cmd);
    } else if (srcStageMask & (PVK_PIPELINE_STAGE_VERTEX_INPUT_BIT | PVK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                               PVK_PIPELINE_STAGE_DRAW_INDIRECT_BIT)) {
        packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
        packet2_vif_flushe(&cmd->packet, 0);
        packet2_vif_nop(&cmd->packet, 0);
        packet2_chain_close_tag(&cmd->packet);
    }
}


#define PVK_ALL_STAGES (PVK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | PVK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | \
                        PVK_PIPELINE_STAGE_VERTEX_INPUT_BIT | PVK_PIPELINE_STAGE_VERTEX_SHADER_BIT | PVK_GS_STAGES)

/* Open a GS packet on the command buffer's path: DIRECT (PATH2) or the GIF channel (PATH3). */
static qword_t *cmd_open_gs(PvkCommandBuffer cmd)
{
    return cmd->pool->family ? cmd_open_gif(cmd) : cmd_open_direct(cmd);
}

static void cmd_close_gs(PvkCommandBuffer cmd, qword_t *end)
{
    if (cmd->pool->family)
        cmd_close_gif(cmd, end);
    else
        cmd_close_direct(cmd, end);
}

/* Fails the recording for a slot range outside the pool. */
static int cmd_query_range(PvkCommandBuffer cmd, const char *who, PvkQueryPool pool, uint32_t first, uint32_t count)
{
    if (pool && first < pool->count && count >= 1 && count <= pool->count - first)
        return 1;
    pvk_error("%s: slots %u to %u lie outside the pool's %u\n", who, (unsigned)first, (unsigned)(first + count - 1),
              pool ? (unsigned)pool->count : 0u);
    cmd->failed = 1;
    return 0;
}

void pvkCmdResetQueryPool(PvkCommandBuffer cmd, PvkQueryPool pool, uint32_t firstQuery, uint32_t queryCount)
{
    qword_t *q;

    if (cmd->rendering) {
        pvk_error("pvkCmdResetQueryPool must be called outside pvkCmdBeginRendering/pvkCmdEndRendering\n");
        cmd->failed = 1;
        return;
    }
    if (!cmd_query_range(cmd, "pvkCmdResetQueryPool", pool, firstQuery, queryCount) || !cmd_reserve(cmd, 5))
        return;
    /* The handler clears the slots when the GS reaches the SIGNAL. */
    q = cmd_open_gs(cmd);
    q = gs_ad(q, GIF_SET_TAG(1, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    q = gs_ad(q, GS_SET_SIGNAL(PVK_QUERY_RESET_ID(pool, firstQuery, queryCount), 0xFFFFFFFF), GS_REG_SIGNAL);
    cmd_close_gs(cmd, q);
}

void pvkCmdWriteTimestamp(PvkCommandBuffer cmd, PvkPipelineStageFlagBits pipelineStage, PvkQueryPool pool, uint32_t query)
{
    qword_t *q;

    if (!pipelineStage || (pipelineStage & (pipelineStage - 1)) || (pipelineStage & ~PVK_ALL_STAGES)) {
        pvk_error("pvkCmdWriteTimestamp: pipelineStage 0x%x is not one stage\n", (unsigned)pipelineStage);
        cmd->failed = 1;
        return;
    }
    if (!cmd_query_range(cmd, "pvkCmdWriteTimestamp", pool, query, 1) || !cmd_reserve(cmd, 6))
        return;
    /* FLUSH (in the DIRECT open), FINISH, then the SIGNAL the handler times. */
    q = cmd_open_gs(cmd);
    q = gs_ad(q, GIF_SET_TAG(2, 1, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    q = gs_ad(q, GS_SET_FINISH(0), GS_REG_FINISH);
    q = gs_ad(q, GS_SET_SIGNAL(PVK_QUERY_ID(pool, query), 0xFFFFFFFF), GS_REG_SIGNAL);
    cmd_close_gs(cmd, q);
}
void pvkCmdEndRendering(PvkCommandBuffer cmd)
{
    if (cmd->inherits) {
        pvk_error("pvkCmdEndRendering in a secondary command buffer recorded with "
                  "PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT: the primary ends the rendering\n");
        cmd->failed = 1;
        return;
    }
    cmd->rendering = 0;
    cmd->secondaryContents = 0;
}

void pvkCmdExecuteCommands(PvkCommandBuffer cmd, uint32_t count, const PvkCommandBuffer *cmds)
{
    PvkFormat depthFormat = cmd->depthView ? cmd->depthView->image->format : PVK_FORMAT_UNDEFINED;
    uint32_t i, r;

    if (cmd->level != PVK_COMMAND_BUFFER_LEVEL_PRIMARY) {
        pvk_error("pvkCmdExecuteCommands: only a primary command buffer runs secondary ones\n");
        cmd->failed = 1;
        return;
    }
    for (i = 0; i < count; i++)
        if (cmds[i]->pool->family != cmd->pool->family) {
            pvk_error("pvkCmdExecuteCommands: secondary %u is from a pool of queue family %u, the primary of %u\n",
                      (unsigned)i, (unsigned)cmds[i]->pool->family, (unsigned)cmd->pool->family);
            cmd->failed = 1;
            return;
        }
    if (cmd->rendering && !cmd->secondaryContents) {
        pvk_error("pvkCmdExecuteCommands inside a rendering begun without "
                  "PVK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT\n");
        cmd->failed = 1;
        return;
    }
    for (i = 0; i < count; i++) {
        PvkCommandBuffer s = cmds[i];

        if (!s || s->level != PVK_COMMAND_BUFFER_LEVEL_SECONDARY || s->device != cmd->device) {
            pvk_error("pvkCmdExecuteCommands: entry %u is not a secondary command buffer of the device\n", (unsigned)i);
            cmd->failed = 1;
            return;
        }
        if (s->recording || s->failed || !s->size) {
            pvk_error("pvkCmdExecuteCommands: entry %u is %s\n", (unsigned)i,
                      s->recording ? "still recording" : s->failed ? "a failed recording" : "not recorded");
            cmd->failed = 1;
            return;
        }
        if (s->inherits != cmd->rendering) {
            pvk_error("pvkCmdExecuteCommands: entry %u was recorded %s PVK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT "
                      "but runs %s pvkCmdBeginRendering/pvkCmdEndRendering\n", (unsigned)i,
                      cmd->rendering ? "without" : "with", cmd->rendering ? "inside" : "outside");
            cmd->failed = 1;
            return;
        }
        if (s->inherits && s->inheritedStencilFormat != cmd->stencilFormat) {
            pvk_error("pvkCmdExecuteCommands: entry %u inherits stencil format %d, but the rendering's stencil attachment is %d\n",
                      (unsigned)i, (int)s->inheritedStencilFormat, (int)cmd->stencilFormat);
            cmd->failed = 1;
            return;
        }
        if (s->inherits && s->inheritedDepthFormat != depthFormat) {
            pvk_error("pvkCmdExecuteCommands: entry %u inherits depth format %d, but the rendering's depth attachment is %d\n",
                      (unsigned)i, (int)s->inheritedDepthFormat, (int)depthFormat);
            cmd->failed = 1;
            return;
        }
        if (s->usage & PVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT && s->used) {
            pvk_error("pvkCmdExecuteCommands: entry %u was recorded with PVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT "
                      "and has run already\n", (unsigned)i);
            cmd->failed = 1;
            return;
        }
    }
    if (cmd->executedCount + count > cmd->executedCap) {
        uint32_t cap = cmd->executedCap ? cmd->executedCap * 2 : 8;
        struct PvkExecuted *e;

        while (cap < cmd->executedCount + count)
            cap *= 2;
        e = realloc(cmd->executed, cap * sizeof(*e));
        if (!e) {
            pvk_error("pvkCmdExecuteCommands: out of memory\n");
            cmd->failed = 1;
            return;
        }
        cmd->executed    = e;
        cmd->executedCap = cap;
    }
    if (!cmd_reserve(cmd, count))
        return;

    /* A CALL tag per secondary: the DMA runs its chain and its RET tag comes back here. */
    for (i = 0; i < count; i++) {
        PvkCommandBuffer s = cmds[i];

        packet2_chain_call(&cmd->packet, s->pool->block + s->start, 0, 0, 0);
        if (cmd->packet.tte) {
            packet2_vif_nop(&cmd->packet, 0);
            packet2_vif_nop(&cmd->packet, 0);
        }
        packet2_chain_close_tag(&cmd->packet);
        s->used = 1;
        for (r = 0; r < s->readbackCount; r++) {
            if (!array_grow(cmd, (void **)&cmd->readbacks, cmd->readbackCount, &cmd->readbackCap, sizeof(*cmd->readbacks)))
                return;
            cmd->readbacks[cmd->readbackCount] = s->readbacks[r];
            cmd->readbacks[cmd->readbackCount++].owned = 0;
        }
        cmd->executed[cmd->executedCount].cmd        = s;
        cmd->executed[cmd->executedCount].generation = s->generation;
        cmd->executedCount++;
    }

    /* The secondaries replaced the VU1 program and memory and the GS draw
     * registers; as in Vulkan, the primary's state is undefined from here. */
    cmd_reset_state(cmd);
}

void pvkCmdBindPipeline(PvkCommandBuffer cmd, PvkPipeline pipeline)
{
    u32 *start = (u32 *)pipeline->program->code;
    u32 *end   = (u32 *)pipeline->program->codeEnd;
    float context[PVK_VU1_CONTEXT_QWORDS * 4];
    uint32_t i;

    if (!cmd_graphics(cmd, "pvkCmdBindPipeline"))
        return;
    if (!cmd_reserve(cmd, packet2_utils_get_packet_size_for_program(start, end) + 8))
        return;

    /* Upload the vertex shader (VIF waits for VU1 to stop first), or only wait when this
     * recording already uploaded the program; then the per-pipeline constants. */
    if (cmd->program == pipeline->program)
        cmd_flush(cmd);
    else
        packet2_vif_add_micro_program(&cmd->packet, 0, start, end);
    cmd->program = pipeline->program;

    memcpy(context, pipeline->context, sizeof(context));
    if (pipeline->dynamic & (1u << PVK_DYNAMIC_STATE_VIEWPORT) && cmd->dynamicSet & (1u << PVK_DYNAMIC_STATE_VIEWPORT)) {
        pvk_viewport_context(&cmd->viewport, pipeline->depthFormat, context);
        cmd->viewportDirty = 0;
    }
    packet2_utils_vu_open_unpack(&cmd->packet, PVK_VU1_CONTEXT_ADDR, 0);
    for (i = 0; i < PVK_VU1_CONTEXT_QWORDS * 4; i++)
        packet2_add_float(&cmd->packet, context[i]);
    packet2_utils_vu_close_unpack(&cmd->packet);

    cmd->pipeline      = pipeline;
    cmd->gsStateDirty  = 1; /* its depth test and write mask */
    cmd->uniformsDirty = 1; /* its shader places the uniform blocks in VU1 memory */
}

void pvkCmdSetViewport(PvkCommandBuffer cmd, uint32_t firstViewport, uint32_t viewportCount,
                       const PvkViewport *viewports)
{
    if (!cmd_graphics(cmd, "pvkCmdSetViewport"))
        return;
    if (firstViewport != 0 || viewportCount != 1) {
        pvk_error("pvkCmdSetViewport: viewport %u, count %u; the GS has one viewport\n", (unsigned)firstViewport,
                  (unsigned)viewportCount);
        cmd->failed = 1;
        return;
    }
    cmd->viewport      = viewports[0];
    cmd->dynamicSet   |= 1u << PVK_DYNAMIC_STATE_VIEWPORT;
    cmd->viewportDirty = 1;
}

void pvkCmdSetScissor(PvkCommandBuffer cmd, uint32_t firstScissor, uint32_t scissorCount, const PvkRect2D *scissors)
{
    if (!cmd_graphics(cmd, "pvkCmdSetScissor"))
        return;
    if (firstScissor != 0 || scissorCount != 1) {
        pvk_error("pvkCmdSetScissor: scissor %u, count %u; the GS has one scissor\n", (unsigned)firstScissor,
                  (unsigned)scissorCount);
        cmd->failed = 1;
        return;
    }
    cmd->scissor      = scissors[0];
    cmd->dynamicSet  |= 1u << PVK_DYNAMIC_STATE_SCISSOR;
    cmd->gsStateDirty = 1;
}

void pvkCmdSetBlendConstants(PvkCommandBuffer cmd, const float blendConstants[4])
{
    if (!cmd_graphics(cmd, "pvkCmdSetBlendConstants"))
        return;
    memcpy(cmd->blendConstants, blendConstants, sizeof(cmd->blendConstants));
    cmd->dynamicSet  |= 1u << PVK_DYNAMIC_STATE_BLEND_CONSTANTS;
    cmd->gsStateDirty = 1;
}

void pvkCmdPushConstants(PvkCommandBuffer cmd, PvkPipelineLayout layout, uint32_t stageFlags,
                         uint32_t offset, uint32_t size, const void *data)
{
    uint32_t first, last, q, i, mask;
    const uint32_t *words;

    if (!cmd_graphics(cmd, "pvkCmdPushConstants"))
        return;
    if ((offset | size) & 3 || size == 0 || offset + size > pvk_limits.maxPushConstantsSize) {
        pvk_error("pvkCmdPushConstants: offset %u and size %u must be multiples of 4 "
                  "within %u bytes (maxPushConstantsSize)\n", (unsigned)offset, (unsigned)size,
                  (unsigned)pvk_limits.maxPushConstantsSize);
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
    packet2_utils_vu_open_unpack(&cmd->packet, PVK_VU1_PUSH_ADDR + first, 0);
    words = (const uint32_t *)(cmd->push + first * 16);
    for (q = first; q <= last; q++)
        for (i = 0; i < 4; i++)
            packet2_add_u32(&cmd->packet, *words++);
    packet2_utils_vu_close_unpack(&cmd->packet);
}

void pvkCmdBindVertexBuffers(PvkCommandBuffer cmd, uint32_t firstBinding, uint32_t bindingCount,
                             const PvkBuffer *buffers, const size_t *offsets)
{
    uint32_t i;

    if (!cmd_graphics(cmd, "pvkCmdBindVertexBuffers"))
        return;
    if (firstBinding + bindingCount > pvk_limits.maxVertexInputBindings) {
        pvk_error("pvkCmdBindVertexBuffers: bindings %u..%u go past maxVertexInputBindings\n",
                  (unsigned)firstBinding, (unsigned)(firstBinding + bindingCount - 1));
        cmd->failed = 1;
        return;
    }
    for (i = 0; i < bindingCount; i++) {
        if (!buffers[i] || !buffers[i]->data || (offsets[i] & 15)) {
            pvk_error("pvkCmdBindVertexBuffers: binding %u needs a buffer with memory bound and an offset that is a multiple of 16\n",
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
    if (!cmd_graphics(cmd, "pvkCmdBindIndexBuffer"))
        return;
    if (type != PVK_INDEX_TYPE_UINT16) {
        pvk_error("pvkCmdBindIndexBuffer: only PVK_INDEX_TYPE_UINT16 is supported\n");
        cmd->failed = 1;
        return;
    }
    if (!buffer || !buffer->data || (offset & 1)) {
        pvk_error("pvkCmdBindIndexBuffer: needs a buffer with memory bound and an offset that is a multiple of 2\n");
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
        packet2_utils_vu_add_unpack_data(&cmd->packet, PVK_VU1_UNIFORM_ADDR + u->qword,
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
    uint32_t b, missing;
    framebuffer_t fb;
    PvkRect2D scissor;
    PvkFormat depthFormat;

    if (!cmd_graphics(cmd, name))
        return 0;
    depthFormat = cmd->inherits ? cmd->inheritedDepthFormat
                          : cmd->depthView ? cmd->depthView->image->format : PVK_FORMAT_UNDEFINED;

    if (!cmd->rendering || !p) {
        pvk_error("%s needs pvkCmdBeginRendering and a pipeline\n", name);
        cmd->failed = 1;
        return 0;
    }
    if (cmd->secondaryContents) {
        pvk_error("%s in a rendering begun with PVK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT: "
                  "its draws come from pvkCmdExecuteCommands\n", name);
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
    if (p->depthFormat != depthFormat) {
        pvk_error("%s: the pipeline's depthAttachmentFormat (%d) does not match the depth "
                  "attachment of %s (%d)\n", name, (int)p->depthFormat,
                  cmd->inherits ? "inheritanceInfo" : "pvkCmdBeginRendering", (int)depthFormat);
        cmd->failed = 1;
        return 0;
    }

    if (p->stencilFormat != (cmd->inherits ? cmd->inheritedStencilFormat : cmd->stencilFormat)) {
        pvk_error("%s: the pipeline's stencilAttachmentFormat (%d) does not match the stencil "
                  "attachment of %s (%d)\n", name, (int)p->stencilFormat,
                  cmd->inherits ? "inheritanceInfo" : "pvkCmdBeginRendering",
                  (int)(cmd->inherits ? cmd->inheritedStencilFormat : cmd->stencilFormat));
        cmd->failed = 1;
        return 0;
    }

    missing = p->dynamic & ~cmd->dynamicSet;
    if (!p->blendFix)
        missing &= ~(1u << PVK_DYNAMIC_STATE_BLEND_CONSTANTS);
    if (missing) {
        pvk_error("%s: the pipeline's dynamic state 0x%x was not set (pvkCmdSetViewport, pvkCmdSetScissor, "
                  "pvkCmdSetBlendConstants)\n", name, (unsigned)missing);
        cmd->failed = 1;
        return 0;
    }

    if (!send_uniforms(cmd, name))
        return 0;
    if (p->fs->textured && !bound_set(cmd, name, p->samplerSet, p->samplerSlot, "fragment shader", p->fs->name,
                                      (uint32_t)p->fs->samplerBinding))
        return 0;

    /* A running batch reads the viewport in its first instructions, so wait for
     * VU1 before replacing it. */
    if (p->dynamic & (1u << PVK_DYNAMIC_STATE_VIEWPORT) && cmd->viewportDirty) {
        float context[8];
        uint32_t i;

        if (!cmd_reserve(cmd, 6))
            return 0;
        pvk_viewport_context(&cmd->viewport, p->depthFormat, context);
        cmd_flush(cmd);
        packet2_utils_vu_open_unpack(&cmd->packet, PVK_VU1_CONTEXT_ADDR, 0);
        for (i = 0; i < 8; i++)
            packet2_add_float(&cmd->packet, context[i]);
        packet2_utils_vu_close_unpack(&cmd->packet);
        cmd->viewportDirty = 0;
    }

    if (!cmd->inherits)
        pvk_view_framebuffer(cmd->colorView, &fb);
    if (!cmd_scissor(cmd, cmd->inherits ? NULL : &fb, name, &scissor))
        return 0; /* nothing may be written */
    if (cmd->gsStateDirty) {
        if (!cmd_reserve(cmd, PVK_GS_STATE_QWORDS + PVK_FRAME_MASK_QWORDS))
            return 0;
        /* The pipeline's color write mask, when its GS context holds another. */
        if (cmd->fbmsk[p->zmsk] != p->fbmsk)
            cmd_frame_mask(cmd, p->zmsk, p->fbmsk);
        cmd_gs_state(cmd, &scissor);
    }
    /* FRONT_AND_BACK discards every triangle, a stencil test of NEVER every pixel: nothing to send. */
    return !p->cullAll && !p->stencilNever;
}

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

/* Chain qwords a batch of `vertices` vertices, `indices` indices and
 * `instances` instances needs (alignment lead and tail included): the flush,
 * header and program start, a REF tag per UNPACK chunk of each binding and
 * the copied indices. */
static uint32_t batch_qwords(const PvkPipeline p, uint32_t vertices, uint32_t indices, uint32_t instances)
{
    uint32_t n = 10, b;

    for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++) {
        const struct PvkVertexBinding *vb = &p->vertexBindings[b];

        if (vb->used)
            n += ((vb->instance ? instances : vertices) + 2 * (vb->instance ? p->instanceAlign : p->align))
               / vb->chunk + 2;
    }
    if (indices)
        n += 1 + (8 + 2 * indices + 15) / 16;
    return n;
}

/* Vertices (or instances) every used binding of that rate holds from its bound offset. */
static uint32_t stream_slots(PvkCommandBuffer cmd, int instance)
{
    const PvkPipeline p = cmd->pipeline;
    uint32_t b, slots = UINT32_MAX;

    for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++) {
        const struct PvkVertexBinding *vb = &p->vertexBindings[b];
        const PvkBuffer buf = cmd->vertexBuffers[b];
        size_t n;

        if (!vb->used || vb->instance != instance)
            continue;
        n = buf->size > cmd->vertexOffsets[b] ? (buf->size - cmd->vertexOffsets[b]) / vb->stride : 0;
        if (n < slots)
            slots = (uint32_t)n;
    }
    return slots;
}

/* Instances that fit a batch of `vertices` vertices whose vertex block and
 * indices take `used` input qwords after the header: each needs a GIF tag and
 * its vertices in the output area and its qwords in the input area. */
static uint32_t batch_instances(const PvkPipeline p, uint32_t vertices, uint32_t used, uint32_t left)
{
    uint32_t k = (PVK_VU1_MEMORY_QWORDS - PVK_VU1_OUTPUT_ADDR) / (1 + vertices * p->outputQwords);

    if (p->instanceQwords) {
        uint32_t room = (PVK_BATCH_INPUT_QWORDS - used) / p->instanceQwords;

        room = room > 2 * (p->instanceAlign - 1) ? room - 2 * (p->instanceAlign - 1) : 1;
        if (room < k)
            k = room;
    }
    if (k > left)
        k = left;
    return k ? k : 1;
}

/* The draw's instances must lie in every instance-rate binding's buffer. */
static int draw_instances(PvkCommandBuffer cmd, const char *name, uint32_t firstInstance, uint32_t instanceCount)
{
    if (cmd->pipeline->instanceQwords && (uint64_t)firstInstance + instanceCount > stream_slots(cmd, 1)) {
        pvk_error("%s reads past the end of an instance-rate vertex buffer\n", name);
        cmd->failed = 1;
        return 0;
    }
    return 1;
}

/* 1 when the bound sampler takes texture coordinates in texels (PRIM.FST). */
static uint32_t cmd_fst(PvkCommandBuffer cmd)
{
    const PvkPipeline p = cmd->pipeline;
    return p->fs->textured ? cmd->sets[p->samplerSet]->slots[p->samplerSlot].sampler->fst : 0;
}

/* Batch header: the GIF tag VU1 writes before each instance's vertices, the
 * vertices per instance, for an indexed batch where the index list starts,
 * the winding sign to cull with (flipped for an odd strip triangle at draw
 * position `first`), whether to store UV instead of ST, where the first
 * vertex and the first instance are, how many qwords each takes and the first
 * instance's gl_InstanceIndex. The
 * previous batch must be finished before its memory is overwritten, so a
 * flush comes first. */
static void batch_begin(PvkCommandBuffer cmd, uint32_t count, uint32_t indexOffset, uint32_t first,
                        uint32_t firstQword, uint32_t instances, uint32_t firstInstance, uint32_t instanceQword)
{
    const PvkPipeline p = cmd->pipeline;
    uint32_t fst = cmd_fst(cmd);
    float sign = p->strip && (first & 1) ? -p->cullSign : p->cullSign;
    u64 regs = p->outputQwords == PVK_VU1_OUTPUT_PER_VERTEX_TEXTURED
             ? (fst ? GIF_REG_UV : GIF_REG_ST) | (GIF_REG_RGBAQ << 4) | (GIF_REG_XYZ2 << 8)
             : GIF_REG_RGBAQ | (GIF_REG_XYZ2 << 4);

    cmd_flush(cmd);
    packet2_utils_vu_open_unpack(&cmd->packet, PVK_VU1_INPUT_ADDR, 0);
    packet2_add_2x_s64(&cmd->packet,
                       GIF_SET_TAG(count, 1, 1, p->prim | (fst ? PVK_PRIM_FST : 0), GIF_FLG_PACKED, p->outputQwords),
                       regs);
    packet2_add_u32(&cmd->packet, count);
    packet2_add_u32(&cmd->packet, indexOffset);
    packet2_add_float(&cmd->packet, sign);
    packet2_add_u32(&cmd->packet, fst);
    packet2_add_u32(&cmd->packet, firstQword);
    packet2_add_float(&cmd->packet, sign);
    packet2_add_u32(&cmd->packet, instances);
    packet2_add_u32(&cmd->packet, instanceQword);
    packet2_add_u32(&cmd->packet, p->vertexQwords);
    packet2_add_u32(&cmd->packet, p->instanceQwords);
    packet2_add_float(&cmd->packet, (float)firstInstance);
    packet2_add_u32(&cmd->packet, 0);
    packet2_utils_vu_close_unpack(&cmd->packet);
}

/* VU1 address of the vertex block. */
#define PVK_VERTEX_BLOCK (PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_HEADER)

/* Qwords per element of binding b's stream. */
static uint32_t stream_qwords(const PvkPipeline p, uint32_t b)
{
    return p->vertexBindings[b].instance ? p->instanceQwords : p->vertexQwords;
}

/* VU1 address of binding b's attributes of the element at position `pos` of the block at `base`. */
static uint32_t block_addr(const PvkPipeline p, uint32_t b, uint32_t base, uint32_t pos)
{
    return base + pos * stream_qwords(p, b) + p->vertexBindings[b].slot;
}

static const uint8_t *binding_vertex(PvkCommandBuffer cmd, uint32_t b, uint32_t vertex)
{
    return (const uint8_t *)cmd->vertexBuffers[b]->data + cmd->vertexOffsets[b]
         + (size_t)vertex * cmd->pipeline->vertexBindings[b].stride;
}

/* Binding b's elements [first, first + count) straight from its buffer to
 * position `pos` of the block at `base` (DMA REF, no copy): STCYCL interleaves
 * them with the other bindings' qwords, one UNPACK per chunk. `first` and
 * `count` are multiples of the binding's align, so each REF moves whole qwords. */
static void batch_ref(PvkCommandBuffer cmd, uint32_t b, uint32_t first, uint32_t count, uint32_t base, uint32_t pos)
{
    const PvkPipeline p = cmd->pipeline;
    const struct PvkVertexBinding *vb = &p->vertexBindings[b];
    const uint8_t *src = binding_vertex(cmd, b, first);
    uint32_t qwords = stream_qwords(p, b);

    while (count) {
        uint32_t n = count < vb->chunk ? count : vb->chunk;

        packet2_chain_ref(&cmd->packet, src, n * vb->stride / 16, 0, 0, 0);
        if (vb->count == qwords)
            packet2_vif_stcycl(&cmd->packet, 0, 0x0101, 0);
        else
            packet2_vif_stcycl(&cmd->packet, vb->count, qwords, 0);
        packet2_vif_open_unpack(&cmd->packet, vb->mode, block_addr(p, b, base, pos), 0, 0, vb->usn, 0);
        packet2_vif_close_unpack_manual(&cmd->packet, n * vb->count);
        src   += (size_t)n * vb->stride;
        pos   += n;
        count -= n;
    }
}

/* One vertex of binding b, copied into the chain when a REF cannot move it
 * alone (a stride that is no multiple of 16): the fan hub and the vertices of
 * a primitive sent one by one. */
static void batch_one(PvkCommandBuffer cmd, uint32_t b, uint32_t vertex, uint32_t base, uint32_t pos)
{
    const PvkPipeline p = cmd->pipeline;
    const struct PvkVertexBinding *vb = &p->vertexBindings[b];
    const uint8_t *src = binding_vertex(cmd, b, vertex);
    uint32_t i;

    if (vb->align == 1) {
        batch_ref(cmd, b, vertex, 1, base, pos);
        return;
    }
    packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
    packet2_vif_stcycl(&cmd->packet, 0, 0x0101, 0);
    packet2_vif_open_unpack(&cmd->packet, vb->mode, block_addr(p, b, base, pos), 0, 0, vb->usn, 0);
    for (i = 0; i < vb->stride; i += 4) {
        uint32_t word = 0;
        memcpy(&word, src + i, vb->stride - i < 4 ? vb->stride - i : 4);
        packet2_add_u32(&cmd->packet, word);
    }
    while ((u32)cmd->packet.next & 15)
        packet2_add_u32(&cmd->packet, 0); /* VIF NOPs up to the qword boundary */
    packet2_vif_close_unpack_manual(&cmd->packet, vb->count);
    packet2_chain_close_tag(&cmd->packet);
}

/* The elements [first, first + count) of every binding of one rate, `first`
 * at position `pos` of the block at `base`: each binding starts at its last
 * aligned element at or before `first` (pos is at least that far in) and ends
 * at the next aligned one. */
static void batch_range(PvkCommandBuffer cmd, int instance, uint32_t first, uint32_t count, uint32_t base, uint32_t pos)
{
    const PvkPipeline p = cmd->pipeline;
    uint32_t b;

    for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++) {
        const struct PvkVertexBinding *vb = &p->vertexBindings[b];
        uint32_t l;

        if (!vb->used || vb->instance != instance)
            continue;
        l = first % vb->align;
        batch_ref(cmd, b, first - l, (l + count + vb->align - 1) / vb->align * vb->align, base, pos - l);
    }
}

/* Block elements a range of one rate takes, lead and tails included. */
static uint32_t block_vertices(const PvkPipeline p, int instance, uint32_t first, uint32_t count)
{
    uint32_t lead = first % (instance ? p->instanceAlign : p->align), b, max = lead + count;

    for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++) {
        const struct PvkVertexBinding *vb = &p->vertexBindings[b];
        uint32_t l, end;

        if (!vb->used || vb->instance != instance)
            continue;
        l   = first % vb->align;
        end = lead - l + (l + count + vb->align - 1) / vb->align * vb->align;
        if (end > max)
            max = end;
    }
    return max;
}

/* Qword offset from the input base of instance `firstInstance` in the instance block at `at`. */
static uint32_t instance_qword(const PvkPipeline p, uint32_t firstInstance, uint32_t at)
{
    return at + firstInstance % p->instanceAlign * p->instanceQwords;
}

/* The batch's instance block at input qword `at`, then the program start. */
static void batch_end(PvkCommandBuffer cmd, uint32_t firstInstance, uint32_t instances, uint32_t at)
{
    batch_range(cmd, 1, firstInstance, instances, PVK_VU1_INPUT_ADDR + at,
                firstInstance % cmd->pipeline->instanceAlign);
    packet2_utils_vu_add_start_program(&cmd->packet, 0);
}

/* Index `i` of a batch's list: the carried ones come before the draw's. */
static uint32_t batch_index(const uint16_t *pre, uint32_t preCount, const uint16_t *idx, uint32_t i)
{
    return i < preCount ? pre[i] : idx[i - preCount];
}

/* Indices, less `base` and scaled to qword offsets, copied into the chain:
 * VIF UNPACK S_16 expands each 16-bit value to one VU1 qword, so VU1 reads
 * them with ILW. */
static void batch_indices(PvkCommandBuffer cmd, uint32_t dest, const uint16_t *pre, uint32_t preCount,
                          const uint16_t *idx, uint32_t count, uint32_t base)
{
    uint32_t i, total = preCount + count, qwords = cmd->pipeline->vertexQwords;

    packet2_chain_open_cnt(&cmd->packet, 0, 0, 0);
    packet2_vif_stcycl(&cmd->packet, 0, 0x0101, 0);
    packet2_vif_open_unpack(&cmd->packet, P2_UNPACK_S_16, dest, 0, 0, 1, 0);
    for (i = 0; i < total; i += 2) {
        uint32_t word = (batch_index(pre, preCount, idx, i) - base) * qwords;
        if (i + 1 < total)
            word |= ((batch_index(pre, preCount, idx, i + 1) - base) * qwords) << 16;
        packet2_add_u32(&cmd->packet, word);
    }
    while ((u32)cmd->packet.next & 15)
        packet2_add_u32(&cmd->packet, 0); /* VIF NOPs up to the qword boundary */
    packet2_vif_close_unpack_manual(&cmd->packet, total);
    packet2_chain_close_tag(&cmd->packet);
}

/* pvkCmdDraw, reporting errors under `name`. */
static void draw(PvkCommandBuffer cmd, const char *name, uint32_t vertexCount, uint32_t instanceCount,
                 uint32_t firstVertex, uint32_t firstInstance)
{
    PvkPipeline p = cmd->pipeline;
    uint32_t done, idone, k = 1, b;

    if (!draw_ready(cmd, name) || !draw_instances(cmd, name, firstInstance, instanceCount))
        return;
    if ((uint64_t)firstVertex + vertexCount > stream_slots(cmd, 0)) {
        pvk_error("%s reads past the end of a vertex buffer\n", name);
        cmd->failed = 1;
        return;
    }

    vertexCount -= vertexCount % p->primStep; /* whole primitives only */
    if (vertexCount <= p->primOverlap || !instanceCount)
        return;

    /* Instances draw in order, so a batch holds several only when one holds the whole draw. */
    for (idone = 0; idone < instanceCount; idone += k) {
        done = 0;
        while (done < vertexCount) {
            uint32_t carry[2], c = batch_carry(p, done, carry);
            uint32_t n     = vertexCount - done < p->maxBatch - c ? vertexCount - done : p->maxBatch - c;
            uint32_t first = firstVertex + done - c, fi = firstInstance + idone, lead, vq, at;
            int hub = c && p->fan; /* the hub is not next to the run: it goes over the run's unused lead vertex */

            if (hub)
                first = firstVertex + carry[1];
            lead = first % p->align;
            vq   = (block_vertices(p, 0, first, c + n - hub) + hub) * p->vertexQwords;
            at   = PVK_VU1_INPUT_HEADER + vq;
            k    = vertexCount <= p->maxBatch ? batch_instances(p, c + n, vq, instanceCount - idone) : 1;

            if (!cmd_reserve(cmd, batch_qwords(p, c + n, 0, k) + 2))
                return;
            batch_begin(cmd, c + n, 0, done - c, PVK_VU1_INPUT_HEADER + lead * p->vertexQwords, k, fi,
                        instance_qword(p, fi, at));
            batch_range(cmd, 0, first, c + n - hub, PVK_VERTEX_BLOCK, lead + hub);
            if (hub)
                for (b = 0; b < PVK_MAX_VERTEX_BINDINGS; b++)
                    if (p->vertexBindings[b].used && !p->vertexBindings[b].instance)
                        batch_one(cmd, b, firstVertex, PVK_VERTEX_BLOCK, lead);
            batch_end(cmd, fi, k, at);
            done += n;
        }
    }
}

/* An indexed batch at a draw position: the carried indices, how many of the
 * draw's follow and the vertex range they span; n = 0 is one primitive whose
 * vertices lie too far apart for a block, sent vertex by vertex. */
struct IndexedBatch {
    uint32_t c, n, lo, hi;
    uint16_t pre[2];
};

static void indexed_plan(const PvkPipeline p, const uint16_t *idx, uint32_t indexCount, uint32_t done,
                         struct IndexedBatch *b)
{
    uint32_t carry[2], k;

    b->c = batch_carry(p, done, carry);
    for (k = 0; k < b->c; k++)
        b->pre[k] = idx[carry[k]];
    b->lo = b->hi = b->c ? b->pre[0] : idx[done];
    for (k = 1; k < b->c; k++) {
        if (b->pre[k] < b->lo) b->lo = b->pre[k];
        if (b->pre[k] > b->hi) b->hi = b->pre[k];
    }

    /* Grow the batch a primitive at a time while its indices and the run of
     * vertices they span (sent as one block, so they need no copy) fit. */
    b->n = 0;
    while (done + b->n < indexCount && b->c + b->n + p->primStep <= batch_max_indices(p)) {
        uint32_t l = b->lo, h = b->hi;
        for (k = 0; k < p->primStep; k++) {
            uint32_t v = idx[done + b->n + k];
            if (v < l) l = v;
            if (v > h) h = v;
        }
        if ((h - l + 1 + 2 * (p->align - 1)) * p->vertexQwords + b->c + b->n + p->primStep > p->inputBudget)
            break;
        b->lo = l;
        b->hi = h;
        b->n += p->primStep;
    }
}

/* pvkCmdDrawIndexed, reporting errors under `name`. */
static void draw_indexed(PvkCommandBuffer cmd, const char *name, uint32_t indexCount, uint32_t instanceCount,
                         uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    PvkPipeline p = cmd->pipeline;
    const uint16_t *idx;
    struct IndexedBatch b;
    uint32_t vertexSlots, done, idone, k = 1, i, j;
    int single;

    if (!draw_ready(cmd, name) || !draw_instances(cmd, name, firstInstance, instanceCount))
        return;
    if (!cmd->indexBuffer) {
        pvk_error("%s needs an index buffer bound with pvkCmdBindIndexBuffer\n", name);
        cmd->failed = 1;
        return;
    }
    if (cmd->indexOffset + (size_t)(firstIndex + indexCount) * 2 > cmd->indexBuffer->size) {
        pvk_error("%s reads past the end of the index buffer\n", name);
        cmd->failed = 1;
        return;
    }

    indexCount -= indexCount % p->primStep; /* whole primitives only */
    if (indexCount <= p->primOverlap || !instanceCount)
        return;
    idx         = (const uint16_t *)((const uint8_t *)cmd->indexBuffer->data + cmd->indexOffset) + firstIndex;
    vertexSlots = stream_slots(cmd, 0);
    indexed_plan(p, idx, indexCount, 0, &b);
    single      = (b.n ? b.n : p->primStep) == indexCount; /* one batch holds the whole draw */

    for (idone = 0; idone < instanceCount; idone += k) {
        done = 0;
        while (done < indexCount) {
            uint32_t fi = firstInstance + idone, left = instanceCount - idone, lead, vq, at;
            int64_t first;

            indexed_plan(p, idx, indexCount, done, &b);
            if (b.n == 0) {
                /* One primitive whose vertices lie too far apart for a block:
                 * send its vertices one by one, as an in-order batch. */
                uint32_t total = b.c + p->primStep;

                for (i = 0; i < total; i++) {
                    first = (int64_t)batch_index(b.pre, b.c, idx + done, i) + vertexOffset;
                    if (first < 0 || first >= (int64_t)vertexSlots) {
                        pvk_error("%s: index %u + vertexOffset %d is outside the vertex buffer\n", name,
                                  (unsigned)batch_index(b.pre, b.c, idx + done, i), (int)vertexOffset);
                        cmd->failed = 1;
                        return;
                    }
                }
                vq = total * p->vertexQwords;
                at = PVK_VU1_INPUT_HEADER + vq;
                k  = single ? batch_instances(p, total, vq, left) : 1;
                if (!cmd_reserve(cmd, batch_qwords(p, 0, 0, k) + 2 * total * PVK_MAX_VERTEX_BINDINGS))
                    return;
                batch_begin(cmd, total, 0, done - b.c, PVK_VU1_INPUT_HEADER, k, fi, instance_qword(p, fi, at));
                for (i = 0; i < total; i++)
                    for (j = 0; j < PVK_MAX_VERTEX_BINDINGS; j++)
                        if (p->vertexBindings[j].used && !p->vertexBindings[j].instance)
                            batch_one(cmd, j, (uint32_t)((int64_t)batch_index(b.pre, b.c, idx + done, i) + vertexOffset),
                                      PVK_VERTEX_BLOCK, i);
                batch_end(cmd, fi, k, at);
                done += p->primStep;
                continue;
            }

            first = (int64_t)b.lo + vertexOffset;
            if (first < 0 || (int64_t)b.hi + vertexOffset >= (int64_t)vertexSlots) {
                pvk_error("%s: indices %u..%u + vertexOffset %d reach outside the vertex buffer\n", name,
                          (unsigned)b.lo, (unsigned)b.hi, (int)vertexOffset);
                cmd->failed = 1;
                return;
            }
            lead = (uint32_t)first % p->align;
            vq   = block_vertices(p, 0, (uint32_t)first, b.hi - b.lo + 1) * p->vertexQwords;
            at   = PVK_VU1_INPUT_HEADER + vq + b.c + b.n;
            k    = single ? batch_instances(p, b.c + b.n, vq + b.c + b.n, left) : 1;

            if (!cmd_reserve(cmd, batch_qwords(p, b.hi - b.lo + 1, b.c + b.n, k)))
                return;
            batch_begin(cmd, b.c + b.n, PVK_VU1_INPUT_HEADER + vq, done - b.c,
                        PVK_VU1_INPUT_HEADER + lead * p->vertexQwords, k, fi, instance_qword(p, fi, at));
            batch_range(cmd, 0, (uint32_t)first, b.hi - b.lo + 1, PVK_VERTEX_BLOCK, lead);
            batch_indices(cmd, PVK_VU1_INPUT_ADDR + PVK_VU1_INPUT_HEADER + vq, b.pre, b.c, idx + done, b.n, b.lo);
            batch_end(cmd, fi, k, at);
            done += b.n;
        }
    }
}

void pvkCmdDraw(PvkCommandBuffer cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                uint32_t firstInstance)
{
    draw(cmd, "pvkCmdDraw", vertexCount, instanceCount, firstVertex, firstInstance);
}

void pvkCmdDrawIndexed(PvkCommandBuffer cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                       int32_t vertexOffset, uint32_t firstInstance)
{
    draw_indexed(cmd, "pvkCmdDrawIndexed", indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

/* Checks an indirect call's buffer range and features; returns the first command or NULL. */
static const uint8_t *indirect_commands(PvkCommandBuffer cmd, const char *name, PvkBuffer buffer, size_t offset,
                                        uint32_t drawCount, uint32_t stride, size_t commandSize)
{
    if (!buffer || !buffer->data) {
        pvk_error("%s needs a buffer bound to memory\n", name);
        goto fail;
    }
    if (offset % 4) {
        pvk_error("%s: offset %u is not a multiple of 4\n", name, (unsigned)offset);
        goto fail;
    }
    if (drawCount > 1) {
        if (!cmd->device->features.multiDrawIndirect) {
            pvk_error("%s: drawCount %u needs the multiDrawIndirect feature\n", name, (unsigned)drawCount);
            goto fail;
        }
        if (stride % 4 || stride < commandSize) {
            pvk_error("%s: stride %u is not a multiple of 4 of at least %u\n", name, (unsigned)stride,
                      (unsigned)commandSize);
            goto fail;
        }
    }
    if (drawCount && offset + (size_t)(drawCount - 1) * stride + commandSize > buffer->size) {
        pvk_error("%s reads past the end of the buffer\n", name);
        goto fail;
    }
    return (const uint8_t *)buffer->data + offset;
fail:
    cmd->failed = 1;
    return NULL;
}

/* A nonzero firstInstance in a command needs the drawIndirectFirstInstance feature. */
static int indirect_first_instance(PvkCommandBuffer cmd, const char *name, uint32_t firstInstance)
{
    if (firstInstance && !cmd->device->features.drawIndirectFirstInstance) {
        pvk_error("%s: firstInstance %u needs the drawIndirectFirstInstance feature\n", name, (unsigned)firstInstance);
        cmd->failed = 1;
        return 0;
    }
    return 1;
}

void pvkCmdDrawIndirect(PvkCommandBuffer cmd, PvkBuffer buffer, size_t offset, uint32_t drawCount, uint32_t stride)
{
    const uint8_t *at = indirect_commands(cmd, "pvkCmdDrawIndirect", buffer, offset, drawCount, stride,
                                          sizeof(PvkDrawIndirectCommand));
    uint32_t i;

    if (!at || !draw_ready(cmd, "pvkCmdDrawIndirect"))
        return;
    for (i = 0; i < drawCount && !cmd->failed; i++, at += stride) {
        PvkDrawIndirectCommand c;
        memcpy(&c, at, sizeof c);
        if (!indirect_first_instance(cmd, "pvkCmdDrawIndirect", c.firstInstance))
            return;
        draw(cmd, "pvkCmdDrawIndirect", c.vertexCount, c.instanceCount, c.firstVertex, c.firstInstance);
    }
}

void pvkCmdDrawIndexedIndirect(PvkCommandBuffer cmd, PvkBuffer buffer, size_t offset, uint32_t drawCount,
                               uint32_t stride)
{
    const uint8_t *at = indirect_commands(cmd, "pvkCmdDrawIndexedIndirect", buffer, offset, drawCount, stride,
                                          sizeof(PvkDrawIndexedIndirectCommand));
    uint32_t i;

    if (!at || !draw_ready(cmd, "pvkCmdDrawIndexedIndirect"))
        return;
    for (i = 0; i < drawCount && !cmd->failed; i++, at += stride) {
        PvkDrawIndexedIndirectCommand c;
        memcpy(&c, at, sizeof c);
        if (!indirect_first_instance(cmd, "pvkCmdDrawIndexedIndirect", c.firstInstance))
            return;
        draw_indexed(cmd, "pvkCmdDrawIndexedIndirect", c.indexCount, c.instanceCount, c.firstIndex, c.vertexOffset,
                     c.firstInstance);
    }
}
