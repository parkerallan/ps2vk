/* Graphics pipelines: compiled vertex shader (VU1) + GS state from the fragment shader. */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "pvk_internal.h"

/* Depth test and writes. Depth is stored flipped (near = large) because the GS
 * only compares with >= and >, so Vulkan's LESS becomes GS GREATER. */
static PvkResult setup_depth(PvkPipeline p, const PvkGraphicsPipelineCreateInfo *info)
{
    const PvkPipelineDepthStencilStateCreateInfo *ds = &info->depthStencil;

    switch (info->depthAttachmentFormat) {
    case PVK_FORMAT_UNDEFINED:
    case PVK_FORMAT_Z32:
    case PVK_FORMAT_Z24:
    case PVK_FORMAT_Z16:
        break;
    default:
        pvk_error("pipeline: depthAttachmentFormat must be a depth format or PVK_FORMAT_UNDEFINED\n");
        return PVK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    p->depthFormat = info->depthAttachmentFormat;

    if (!ds->depthTestEnable) {
        /* Vulkan: no test means no writes either. GS ZTE = 0 is not allowed,
         * so an always-passing test with writes masked does the same. */
        p->ztst = ZTEST_METHOD_ALLPASS;
        p->zmsk = 1;
        return PVK_SUCCESS;
    }
    if (p->depthFormat == PVK_FORMAT_UNDEFINED) {
        pvk_error("pipeline: depthTestEnable needs a depthAttachmentFormat\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }

    switch (ds->depthCompareOp) {
    case PVK_COMPARE_OP_NEVER:         p->ztst = ZTEST_METHOD_ALLFAIL;       break;
    case PVK_COMPARE_OP_LESS:          p->ztst = ZTEST_METHOD_GREATER;       break;
    case PVK_COMPARE_OP_LESS_OR_EQUAL: p->ztst = ZTEST_METHOD_GREATER_EQUAL; break;
    case PVK_COMPARE_OP_ALWAYS:        p->ztst = ZTEST_METHOD_ALLPASS;       break;
    default:
        pvk_error("pipeline: depthCompareOp %d is not one the GS can do; use LESS, "
                  "LESS_OR_EQUAL, ALWAYS or NEVER\n", (int)ds->depthCompareOp);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    p->zmsk = ds->depthWriteEnable ? 0 : 1;
    return PVK_SUCCESS;
}

/* A factor as k0 + k1 * alpha; src is the GS ALPHA.C source (As, Ad, FIX) or -1. */
static int blend_factor(PvkBlendFactor f, int *k0, int *k1, int *src)
{
    *k0 = 0; *k1 = 0; *src = -1;
    switch (f) {
    case PVK_BLEND_FACTOR_ZERO:                     return 1;
    case PVK_BLEND_FACTOR_ONE:                      *k0 = 1; return 1;
    case PVK_BLEND_FACTOR_SRC_ALPHA:                *k1 = 1; *src = 0; return 1;
    case PVK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:      *k0 = 1; *k1 = -1; *src = 0; return 1;
    case PVK_BLEND_FACTOR_DST_ALPHA:                *k1 = 1; *src = 1; return 1;
    case PVK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:      *k0 = 1; *k1 = -1; *src = 1; return 1;
    case PVK_BLEND_FACTOR_CONSTANT_ALPHA:           *k1 = 1; *src = 2; return 1;
    case PVK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: *k0 = 1; *k1 = -1; *src = 2; return 1;
    default:                                        return 0;
    }
}

/* Rewrites src * sf (op) dst * df as the GS's (A - B) * C + D. */
static PvkResult setup_blend(PvkPipeline p, const PvkGraphicsPipelineCreateInfo *info)
{
    const PvkPipelineColorBlendAttachmentState *bs = &info->colorBlend.attachment;
    int s0, s1, ssrc, d0, d1, dsrc, src, fix = 0x80;
    int a, b, c, d;
    float k;

    p->alpha = GS_SET_ALPHA(0, 2, 2, 1, 0x80);
    if (!bs->blendEnable)
        return PVK_SUCCESS;

    if (!blend_factor(bs->srcColorBlendFactor, &s0, &s1, &ssrc)
        || !blend_factor(bs->dstColorBlendFactor, &d0, &d1, &dsrc)) {
        pvk_error("pipeline: blend factors %d, %d; the GS only scales by an alpha, so use ZERO, ONE "
                  "or a *_ALPHA factor\n", (int)bs->srcColorBlendFactor, (int)bs->dstColorBlendFactor);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    switch (bs->colorBlendOp) {
    case PVK_BLEND_OP_ADD:                                      break;
    case PVK_BLEND_OP_SUBTRACT:         d0 = -d0; d1 = -d1;     break;
    case PVK_BLEND_OP_REVERSE_SUBTRACT: s0 = -s0; s1 = -s1;     break;
    default:
        pvk_error("pipeline: colorBlendOp %d; the GS only adds and subtracts\n", (int)bs->colorBlendOp);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (ssrc >= 0 && dsrc >= 0 && ssrc != dsrc) {
        pvk_error("pipeline: blend factors %d, %d use two different alphas; the GS has one per draw\n",
                  (int)bs->srcColorBlendFactor, (int)bs->dstColorBlendFactor);
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    src = ssrc >= 0 ? ssrc : dsrc;
    if (src == 2) {
        k = info->colorBlend.blendConstants[3];
        k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
        fix = (int)(k * 128.0f + 0.5f);
    }

    for (c = 0; c < 3; c++) {
        if (src >= 0 && c != src)
            continue;
        for (a = 0; a < 3; a++)
            for (b = 0; b < 3; b++)
                for (d = 0; d < 3; d++) {
                    int gs1 = (a == 0) - (b == 0), gd1 = (a == 1) - (b == 1);
                    int gs0 = d == 0, gd0 = d == 1;

                    if (src < 0) {
                        if (c != 2 || gs0 + gs1 != s0 || gd0 + gd1 != d0)
                            continue;
                    } else if (gs0 != s0 || gs1 != s1 || gd0 != d0 || gd1 != d1) {
                        continue;
                    }
                    p->alpha = GS_SET_ALPHA(a, b, c, d, fix);
                    return PVK_SUCCESS;
                }
    }
    pvk_error("pipeline: blend factors %d, %d with op %d do not fit the GS's (A - B) * C + D\n",
              (int)bs->srcColorBlendFactor, (int)bs->dstColorBlendFactor, (int)bs->colorBlendOp);
    return PVK_ERROR_FEATURE_NOT_PRESENT;
}

static PvkResult validate_shaders(const PvkShaderCode *vs, const PvkShaderCode *fs)
{
    if (!vs || vs->stage != PVK_SHADER_STAGE_VERTEX) {
        pvk_error("pipeline: vertexShader is not a vertex shader\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (!fs || fs->stage != PVK_SHADER_STAGE_FRAGMENT) {
        pvk_error("pipeline: fragmentShader is not a fragment shader\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (fs->colorInputLocation >= 0 && fs->colorInputLocation != vs->colorOutputLocation) {
        pvk_error("pipeline: fragment shader '%s' reads a color at location %d, "
                  "but vertex shader '%s' does not write one there\n",
                  fs->name, (int)fs->colorInputLocation, vs->name);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (fs->texcoordInputLocation >= 0 && fs->texcoordInputLocation != vs->texcoordOutputLocation) {
        pvk_error("pipeline: fragment shader '%s' samples with a texture coordinate at "
                  "location %d, but vertex shader '%s' does not write one there\n",
                  fs->name, (int)fs->texcoordInputLocation, vs->name);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    return PVK_SUCCESS;
}

uint32_t pvk_push_word_mask(uint32_t offset, uint32_t size)
{
    uint32_t first = offset / 4, count = size / 4;

    return (count >= 32 ? 0xffffffffu : (1u << count) - 1) << first;
}

int pvk_layout_push_compatible(const struct PvkPipelineLayout_T *a, const struct PvkPipelineLayout_T *b)
{
    return a->pushWords[0] == b->pushWords[0] && a->pushWords[1] == b->pushWords[1];
}

PvkResult pvkCreatePipelineLayout(PvkDevice device, const PvkPipelineLayoutCreateInfo *info, PvkPipelineLayout *layout)
{
    PvkPipelineLayout l;
    uint32_t i, used = 0;

    (void)device;

    if (info->setLayoutCount > PVK_MAX_DESCRIPTOR_SETS) {
        pvk_error("pipeline layout: %u descriptor set layouts; at most %d\n", (unsigned)info->setLayoutCount,
                  PVK_MAX_DESCRIPTOR_SETS);
        return PVK_ERROR_VALIDATION_FAILED;
    }

    l = malloc(sizeof(struct PvkPipelineLayout_T));
    if (!l)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    memset(l, 0, sizeof(*l));

    /* Copies, so the set layouts may be destroyed after this. */
    l->setLayoutCount = info->setLayoutCount;
    for (i = 0; i < info->setLayoutCount; i++)
        l->setLayouts[i] = *info->setLayouts[i];

    for (i = 0; i < info->pushConstantRangeCount; i++) {
        const PvkPushConstantRange *r = &info->pushConstantRanges[i];
        uint32_t mask;

        if ((r->offset | r->size) & 3 || r->size == 0 || r->offset + r->size > PVK_MAX_PUSH_CONSTANT_BYTES) {
            pvk_error("pipeline layout: push constant range %u (offset %u, size %u) must be multiples "
                      "of 4 within %d bytes\n", (unsigned)i, (unsigned)r->offset, (unsigned)r->size,
                      PVK_MAX_PUSH_CONSTANT_BYTES);
            free(l);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (!r->stageFlags || r->stageFlags & ~(PVK_SHADER_STAGE_VERTEX | PVK_SHADER_STAGE_FRAGMENT)) {
            pvk_error("pipeline layout: push constant range %u has stage flags 0x%x; use "
                      "PvkShaderStage values\n", (unsigned)i, (unsigned)r->stageFlags);
            free(l);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        mask = pvk_push_word_mask(r->offset, r->size);
        if (used & mask) {
            pvk_error("pipeline layout: push constant range %u overlaps an earlier one\n", (unsigned)i);
            free(l);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        used |= mask;
        if (r->stageFlags & PVK_SHADER_STAGE_VERTEX)
            l->pushWords[0] |= mask;
        if (r->stageFlags & PVK_SHADER_STAGE_FRAGMENT)
            l->pushWords[1] |= mask;
    }

    *layout = l;
    return PVK_SUCCESS;
}

void pvkDestroyPipelineLayout(PvkDevice device, PvkPipelineLayout layout)
{
    (void)device;
    free(layout);
}

/* The vertex shader's push constant block, bytes 0 to pushConstantBytes, must
 * lie in the layout's vertex-stage ranges, each of its uniform blocks must be
 * a uniform buffer binding, visible to the vertex stage, in the set layout at
 * its set index, and the fragment shader's sampler2D must be a combined image
 * sampler binding, visible to the fragment stage, in the set layout at its
 * set index. Fills in the pipeline's uniform and sampler slots. */
static PvkResult validate_layout(PvkPipeline p, const PvkGraphicsPipelineCreateInfo *info)
{
    const PvkShaderCode *vs = p->vs, *fs = p->fs;
    const struct PvkDescriptorSetLayout_T *set;
    uint32_t needed, i;
    int slot;

    if (!info->layout) {
        pvk_error("pipeline: layout is NULL; create one with pvkCreatePipelineLayout\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    needed = pvk_push_word_mask(0, vs->pushConstantBytes);
    if ((info->layout->pushWords[0] & needed) != needed) {
        pvk_error("pipeline: vertex shader '%s' reads %u bytes of push constants, but the layout's "
                  "vertex stage ranges do not cover them\n", vs->name, (unsigned)vs->pushConstantBytes);
        return PVK_ERROR_VALIDATION_FAILED;
    }

    for (i = 0; i < vs->uniformBlockCount; i++) {
        const PvkShaderUniformBlock *u = &vs->uniformBlocks[i];

        if (u->set >= info->layout->setLayoutCount) {
            pvk_error("pipeline: vertex shader '%s' has a uniform block at set %u, binding %u, but the "
                      "layout has %u descriptor set layouts\n", vs->name, (unsigned)u->set,
                      (unsigned)u->binding, (unsigned)info->layout->setLayoutCount);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        set  = &info->layout->setLayouts[u->set];
        slot = pvk_set_layout_find(set, u->binding);
        if (slot < 0 || set->bindings[slot].descriptorType != PVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
            || !(set->bindings[slot].stageFlags & PVK_SHADER_STAGE_VERTEX)) {
            pvk_error("pipeline: vertex shader '%s' has a uniform block at set %u, binding %u, but set "
                      "layout %u has no PVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER binding %u for the vertex stage\n",
                      vs->name, (unsigned)u->set, (unsigned)u->binding, (unsigned)u->set, (unsigned)u->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        p->uniformSlots[i] = (uint32_t)slot;
    }

    if (!fs->textured)
        return PVK_SUCCESS;
    if (fs->samplerSet < 0 || (uint32_t)fs->samplerSet >= info->layout->setLayoutCount) {
        pvk_error("pipeline: fragment shader '%s' samples set %d, binding %d, but the layout has %u "
                  "descriptor set layouts\n", fs->name, (int)fs->samplerSet, (int)fs->samplerBinding,
                  (unsigned)info->layout->setLayoutCount);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    set  = &info->layout->setLayouts[fs->samplerSet];
    slot = pvk_set_layout_find(set, (uint32_t)fs->samplerBinding);
    if (slot < 0 || set->bindings[slot].descriptorType != PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
        || !(set->bindings[slot].stageFlags & PVK_SHADER_STAGE_FRAGMENT)) {
        pvk_error("pipeline: fragment shader '%s' samples set %d, binding %d, but set layout %d has no "
                  "PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER binding %d for the fragment stage\n",
                  fs->name, (int)fs->samplerSet, (int)fs->samplerBinding, (int)fs->samplerSet,
                  (int)fs->samplerBinding);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    p->samplerSet  = (uint32_t)fs->samplerSet;
    p->samplerSlot = (uint32_t)slot;
    return PVK_SUCCESS;
}

static PvkResult validate_vertex_input(const PvkShaderCode *vs, const PvkGraphicsPipelineCreateInfo *info)
{
    uint32_t i, j;

    /* VU1 reads each vertex as consecutive qwords, one per shader input in
     * location order, so the buffer layout has to match exactly for now. */
    if (info->vertexStride != vs->inputCount * 16) {
        pvk_error("pipeline: vertexStride must be %u (one vec4 per shader input)\n",
                  (unsigned)(vs->inputCount * 16));
        return PVK_ERROR_VALIDATION_FAILED;
    }

    for (i = 0; i < vs->inputCount; i++) {
        const PvkVertexInputAttribute *attr = NULL;

        for (j = 0; j < info->attributeCount; j++) {
            if (info->attributes[j].location == vs->inputLocations[i]) {
                attr = &info->attributes[j];
                break;
            }
        }
        if (!attr) {
            pvk_error("pipeline: no vertex attribute for shader input location %u\n",
                      (unsigned)vs->inputLocations[i]);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (attr->format != PVK_FORMAT_R32G32B32A32_SFLOAT) {
            pvk_error("pipeline: attribute at location %u must be PVK_FORMAT_R32G32B32A32_SFLOAT\n",
                      (unsigned)attr->location);
            return PVK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        if (attr->offset != i * 16) {
            pvk_error("pipeline: attribute at location %u must have offset %u\n",
                      (unsigned)attr->location, (unsigned)(i * 16));
            return PVK_ERROR_VALIDATION_FAILED;
        }
    }
    return PVK_SUCCESS;
}

static uint32_t max_batch_vertices(uint32_t inputQwords, uint32_t outputQwords)
{
    uint32_t byInput  = (PVK_VU1_OUTPUT_ADDR - PVK_VU1_INPUT_ADDR - PVK_VU1_INPUT_HEADER) / inputQwords;
    uint32_t byOutput = (PVK_VU1_MEMORY_QWORDS - PVK_VU1_OUTPUT_ADDR - 1) / outputQwords;
    uint32_t n        = byInput < byOutput ? byInput : byOutput;

    return n - n % 3; /* whole triangles per batch */
}

PvkResult pvkCreateGraphicsPipeline(PvkDevice device, const PvkGraphicsPipelineCreateInfo *info, PvkPipeline *pipeline)
{
    const PvkShaderCode *vs = info->vertexShader ? info->vertexShader->code : NULL;
    const PvkShaderCode *fs = info->fragmentShader ? info->fragmentShader->code : NULL;
    const PvkViewport *vp   = &info->viewport;
    PvkPipeline p;
    PvkResult res;
    float *ctx, depthMax;

    (void)device;

    if ((res = validate_shaders(vs, fs)) != PVK_SUCCESS)
        return res;
    if ((res = validate_vertex_input(vs, info)) != PVK_SUCCESS)
        return res;
    if (info->topology != PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
        pvk_error("pipeline: only PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST is supported\n");
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }

    p = memalign(16, sizeof(struct PvkPipeline_T));
    if (!p)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    memset(p, 0, sizeof(*p));

    p->vs           = vs;
    p->fs           = fs;
    p->layout       = info->layout;
    if ((res = validate_layout(p, info)) != PVK_SUCCESS) {
        free(p);
        return res;
    }
    p->inputQwords  = vs->inputCount;
    /* A vertex shader with a texture coordinate output stores ST before RGBAQ, XYZ2. */
    p->outputQwords = vs->texcoordOutputLocation >= 0 ? PVK_VU1_OUTPUT_PER_VERTEX_TEXTURED : PVK_VU1_OUTPUT_PER_VERTEX;
    p->vertexStride = info->vertexStride;
    p->maxBatch     = max_batch_vertices(p->inputQwords, p->outputQwords);
    /* ST holds S/w, T/w and Q = 1/w (FST = 0), so the GS divides per pixel. */
    p->prim         = GS_SET_PRIM(PRIM_TRIANGLE, fs->gouraud, fs->textured, 0,
                                  info->colorBlend.attachment.blendEnable ? 1 : 0, 0, 0, 0, 0);

    if ((res = setup_depth(p, info)) != PVK_SUCCESS || (res = setup_blend(p, info)) != PVK_SUCCESS) {
        free(p);
        return res;
    }
    depthMax = pvk_depth_max(p->depthFormat);

    /* Vulkan viewport transform, in GS primitive coordinates: x and y go from
     * NDC [-1, 1] (y down) to pixels, z from [0, 1] to the depth format's
     * flipped integer range. */
    ctx = p->context;
    ctx[PVK_VU1_CTX_VIEWPORT_SCALE * 4 + 0]  = vp->width * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_SCALE * 4 + 1]  = vp->height * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_SCALE * 4 + 2]  = -depthMax * (vp->maxDepth - vp->minDepth);
    ctx[PVK_VU1_CTX_VIEWPORT_OFFSET * 4 + 0] = PVK_GS_ORIGIN + vp->x + vp->width * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_OFFSET * 4 + 1] = PVK_GS_ORIGIN + vp->y + vp->height * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_OFFSET * 4 + 2] = depthMax * (1.0f - vp->minDepth);
    /* Colors 0..1 to GS 0..255; GS alpha 128 means 1.0. */
    ctx[PVK_VU1_CTX_COLOR_SCALE * 4 + 0] = 255.0f;
    ctx[PVK_VU1_CTX_COLOR_SCALE * 4 + 1] = 255.0f;
    ctx[PVK_VU1_CTX_COLOR_SCALE * 4 + 2] = 255.0f;
    ctx[PVK_VU1_CTX_COLOR_SCALE * 4 + 3] = 128.0f;

    *pipeline = p;
    return PVK_SUCCESS;
}

void pvkDestroyPipeline(PvkDevice device, PvkPipeline pipeline)
{
    (void)device;
    free(pipeline);
}
