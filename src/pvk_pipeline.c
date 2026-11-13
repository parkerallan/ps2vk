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

/* VU1 drops a triangle when its screen-space signed area * cullSign < 0. */
static PvkResult setup_cull(PvkPipeline p, const PvkGraphicsPipelineCreateInfo *info)
{
    const PvkPipelineRasterizationStateCreateInfo *rs = &info->rasterization;
    uint32_t *ctx = (uint32_t *)&p->context[PVK_VU1_CTX_CULL * 4];
    int triangles = info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                    info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                    info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    float flip = p->strip ? -1.0f : 1.0f;

    if ((unsigned)rs->cullMode > PVK_CULL_MODE_FRONT_AND_BACK ||
        (unsigned)rs->frontFace > PVK_FRONT_FACE_CLOCKWISE) {
        pvk_error("pipeline: unknown cullMode %d or frontFace %d\n", (int)rs->cullMode, (int)rs->frontFace);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (triangles && rs->cullMode == PVK_CULL_MODE_FRONT_AND_BACK)
        p->cullAll = 1;
    else if (triangles && rs->cullMode != PVK_CULL_MODE_NONE)
        p->cullSign = (rs->cullMode == PVK_CULL_MODE_BACK_BIT) == (rs->frontFace == PVK_FRONT_FACE_COUNTER_CLOCKWISE)
                    ? 1.0f : -1.0f;

    memcpy(&ctx[0], &flip, sizeof(flip));
    ctx[1] = p->fan ? 1 : 0;
    ctx[2] = p->cullSign != 0.0f ? p->primStep : 0;
    ctx[3] = 0x8000;
    return PVK_SUCCESS;
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

/* Vertex attribute formats: the VIF UNPACK that expands one to a qword, and
 * what VU1 then does with it. */
enum { CONV_FLOAT, CONV_UNORM, CONV_SNORM, CONV_SCALED };

typedef struct VertexFormat {
    PvkFormat format;
    uint8_t   mode;       /* P2_UNPACK_* */
    uint8_t   components;
    uint8_t   bytes;
    uint8_t   conv;
    uint8_t   usn;        /* unsigned integers */
} VertexFormat;

#define INT_FORMATS(prefix, mode, n, bytes)                               \
    { PVK_FORMAT_##prefix##_UNORM,   mode, n, bytes, CONV_UNORM,  1 },    \
    { PVK_FORMAT_##prefix##_SNORM,   mode, n, bytes, CONV_SNORM,  0 },    \
    { PVK_FORMAT_##prefix##_USCALED, mode, n, bytes, CONV_SCALED, 1 },    \
    { PVK_FORMAT_##prefix##_SSCALED, mode, n, bytes, CONV_SCALED, 0 },

static const VertexFormat vertex_formats[] = {
    { PVK_FORMAT_R32G32B32A32_SFLOAT, P2_UNPACK_V4_32, 4, 16, CONV_FLOAT, 0 },
    { PVK_FORMAT_R32G32B32_SFLOAT,    P2_UNPACK_V3_32, 3, 12, CONV_FLOAT, 0 },
    { PVK_FORMAT_R32G32_SFLOAT,       P2_UNPACK_V2_32, 2,  8, CONV_FLOAT, 0 },
    { PVK_FORMAT_R32_SFLOAT,          P2_UNPACK_S_32,  1,  4, CONV_FLOAT, 0 },
    INT_FORMATS(R16G16B16A16, P2_UNPACK_V4_16, 4, 8)
    INT_FORMATS(R16G16B16,    P2_UNPACK_V3_16, 3, 6)
    INT_FORMATS(R16G16,       P2_UNPACK_V2_16, 2, 4)
    INT_FORMATS(R16,          P2_UNPACK_S_16,  1, 2)
    INT_FORMATS(R8G8B8A8,     P2_UNPACK_V4_8,  4, 4)
    INT_FORMATS(R8G8B8,       P2_UNPACK_V3_8,  3, 3)
    INT_FORMATS(R8G8,         P2_UNPACK_V2_8,  2, 2)
    INT_FORMATS(R8,           P2_UNPACK_S_8,   1, 1)
};

static const VertexFormat *vertex_format(PvkFormat format)
{
    size_t i;
    for (i = 0; i < sizeof(vertex_formats) / sizeof(vertex_formats[0]); i++)
        if (vertex_formats[i].format == format)
            return &vertex_formats[i];
    return NULL;
}

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Fills p->vertexBindings from the bindings and attributes; fmt[i] gets the
 * format of shader input i. VU1 reads a vertex as one qword per shader input in
 * location order, and the VIF unpacks a binding with one UNPACK code, so a
 * binding's attributes are consecutive inputs of one format, packed. */
static PvkResult setup_vertex_input(PvkPipeline p, const PvkGraphicsPipelineCreateInfo *info,
                                    const VertexFormat **fmt)
{
    const PvkShaderCode *vs = p->vs;
    uint32_t i, j;

    for (i = 0; i < info->bindingCount; i++) {
        const PvkVertexInputBinding *b = &info->bindings[i];

        if (b->binding >= PVK_MAX_VERTEX_BINDINGS || !b->stride || p->vertexBindings[b->binding].stride) {
            pvk_error("pipeline: vertex binding %u is out of range, declared twice or has stride 0\n",
                      (unsigned)b->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        p->vertexBindings[b->binding].stride = b->stride;
    }

    p->align = 1;
    for (i = 0; i < vs->inputCount; i++) {
        const PvkVertexInputAttribute *attr = NULL;
        const VertexFormat *f;
        struct PvkVertexBinding *vb;

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
        f = vertex_format(attr->format);
        if (!f) {
            pvk_error("pipeline: format %d of the attribute at location %u is not a vertex attribute format\n",
                      (int)attr->format, (unsigned)attr->location);
            return PVK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        if (attr->binding >= PVK_MAX_VERTEX_BINDINGS || !p->vertexBindings[attr->binding].stride) {
            pvk_error("pipeline: the attribute at location %u reads vertex binding %u, which is not declared\n",
                      (unsigned)attr->location, (unsigned)attr->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        vb = &p->vertexBindings[attr->binding];
        if (!vb->used) {
            vb->used = 1;
            vb->slot = i;
            vb->mode = f->mode;
            vb->usn  = f->usn;
        } else if (vb->slot + vb->count != i) {
            pvk_error("pipeline: the attributes of vertex binding %u must feed consecutive shader inputs\n",
                      (unsigned)attr->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        } else if (vb->mode != f->mode || vb->usn != f->usn || fmt[vb->slot]->conv != f->conv) {
            pvk_error("pipeline: the attributes of vertex binding %u must share one format "
                      "(the VIF unpacks a binding with one UNPACK code)\n", (unsigned)attr->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (vb->count && f->bytes != 16) {
            pvk_error("pipeline: an attribute of format %d needs a vertex binding of its own; binding %u "
                      "holds more than one\n", (int)attr->format, (unsigned)attr->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (attr->offset != vb->count * f->bytes) {
            pvk_error("pipeline: the attribute at location %u must be at offset %u of vertex binding %u "
                      "(attributes are packed in shader input order)\n",
                      (unsigned)attr->location, (unsigned)(vb->count * f->bytes), (unsigned)attr->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        vb->count++;
        fmt[i] = f;
    }

    for (i = 0; i < PVK_MAX_VERTEX_BINDINGS; i++) {
        struct PvkVertexBinding *vb = &p->vertexBindings[i];

        if (!vb->used)
            continue;
        if (vb->stride != vb->count * fmt[vb->slot]->bytes) {
            pvk_error("pipeline: vertex binding %u has stride %u but its attributes take %u bytes "
                      "(a binding is tightly packed)\n", (unsigned)i, (unsigned)vb->stride,
                      (unsigned)(vb->count * fmt[vb->slot]->bytes));
            return PVK_ERROR_VALIDATION_FAILED;
        }
        vb->align = 16 / gcd(16, vb->stride);
        vb->chunk = 255 / vb->count;
        vb->chunk -= vb->chunk % vb->align;
        p->align  = p->align / gcd(p->align, vb->align) * vb->align;
    }
    return PVK_SUCCESS;
}

/* VU1 instruction words the conversion block is patched with. */
#define VU_UPPER_NOP    0x000002FFu
#define VU_LOWER_NOP    0x8000033Cu
#define VU_LOWER_B      0x40000000u /* b imm11: target = this pair + 1 + imm11 */
#define VU_DEST_SHIFT   21          /* xyzw write mask, x in bit 24 */

static void vu_nop(uint32_t *pair)
{
    pair[0] = VU_LOWER_NOP;
    pair[1] = VU_UPPER_NOP;
}

/* The pipeline's copy of the vertex shader, with the conversion block (see
 * PvkShaderCode::vu1Convert) patched: per input the pairs `itof0` + `loi`
 * scale, `muli` + `loi` -1, `maxi` and `mul.mask VFn,VF00,VF00` (0 and 1 for
 * the components the format lacks), each NOP'd when the format does not need
 * it (a `loi` feeds the next pair), and the branch skips the leading inputs
 * that need nothing. */
static PvkResult copy_code(PvkPipeline p, const VertexFormat *const *fmt)
{
    const PvkShaderCode *vs = p->vs;
    size_t words = (size_t)(vs->vu1CodeEnd - vs->vu1CodeStart);
    uint32_t *blk, i, first = vs->inputCount, target;

    if (!vs->vu1Convert || vs->vu1Convert < vs->vu1CodeStart ||
        vs->vu1Convert + 2 * (2 + 4 * vs->inputCount) > vs->vu1CodeEnd) {
        pvk_error("pipeline: vertex shader '%s' has no input conversion block; rebuild it with this shaderc\n",
                  vs->name);
        return PVK_ERROR_INVALID_SHADER;
    }
    p->code = memalign(16, words * 4);
    if (!p->code)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(p->code, vs->vu1CodeStart, words * 4);
    p->codeEnd = p->code + words;
    blk = p->code + (vs->vu1Convert - vs->vu1CodeStart);

    for (i = 0; i < vs->inputCount; i++) {
        uint32_t *s = blk + 2 * (2 + 4 * i);
        const VertexFormat *f = fmt[i];
        uint32_t bits = f->bytes * 8 / f->components;
        float scale = f->conv == CONV_UNORM ? 1.0f / (float)((1u << bits) - 1)
                    : 1.0f / (float)((1u << (bits - 1)) - 1);
        int needed = f->conv != CONV_FLOAT || f->components < 4;

        if (f->conv == CONV_FLOAT)
            vu_nop(s);
        else
            memcpy(&s[0], &scale, 4);
        if (f->conv == CONV_FLOAT || f->conv == CONV_SCALED)
            vu_nop(s + 2);
        if (f->conv != CONV_SNORM)
            vu_nop(s + 4);
        if (f->components == 4)
            vu_nop(s + 6);
        else
            s[7] = (s[7] & ~(0xFu << VU_DEST_SHIFT)) | (((1u << (4 - f->components)) - 1) << VU_DEST_SHIFT);
        if (needed && first == vs->inputCount)
            first = i;
    }
    target = 2 + 4 * first;
    blk[0] = VU_LOWER_B | ((target - 1) & 0x7FF);
    return PVK_SUCCESS;
}

static uint32_t max_batch_vertices(const PvkPipeline p, uint32_t step)
{
    /* Up to align - 1 unused vertices come before and after a batch's. */
    uint32_t byInput  = (PVK_VU1_OUTPUT_ADDR - PVK_VU1_INPUT_ADDR - PVK_VU1_INPUT_HEADER) / p->inputQwords
                      - 2 * (p->align - 1);
    uint32_t byOutput = (PVK_VU1_MEMORY_QWORDS - PVK_VU1_OUTPUT_ADDR - 1) / p->outputQwords;
    uint32_t n        = byInput < byOutput ? byInput : byOutput;

    return n - n % step; /* whole primitives per batch */
}

/* GS PRIM type and batch splitting of each topology. */
static const struct {
    uint32_t prim, step, overlap, fan;
} topologies[] = {
    [PVK_PRIMITIVE_TOPOLOGY_POINT_LIST]     = { PRIM_POINT, 1, 0, 0 },
    [PVK_PRIMITIVE_TOPOLOGY_LINE_LIST]      = { PRIM_LINE, 2, 0, 0 },
    [PVK_PRIMITIVE_TOPOLOGY_LINE_STRIP]     = { PRIM_LINE_STRIP, 1, 1, 0 },
    [PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST]  = { PRIM_TRIANGLE, 3, 0, 0 },
    [PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP] = { PRIM_TRIANGLE_STRIP, 1, 2, 0 },
    [PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN]   = { PRIM_TRIANGLE_FAN, 1, 2, 1 },
    [PVK_PRIMITIVE_TOPOLOGY_SPRITE_LIST]    = { PRIM_SPRITE, 2, 0, 0 },
};

PvkResult pvkCreateGraphicsPipeline(PvkDevice device, const PvkGraphicsPipelineCreateInfo *info, PvkPipeline *pipeline)
{
    const PvkShaderCode *vs = info->vertexShader ? info->vertexShader->code : NULL;
    const PvkShaderCode *fs = info->fragmentShader ? info->fragmentShader->code : NULL;
    const PvkViewport *vp   = &info->viewport;
    const VertexFormat *fmt[PVK_MAX_VERTEX_INPUTS];
    PvkPipeline p;
    PvkResult res;
    float *ctx, depthMax;

    (void)device;

    if ((res = validate_shaders(vs, fs)) != PVK_SUCCESS)
        return res;
    if ((unsigned)info->topology >= sizeof(topologies) / sizeof(topologies[0])) {
        pvk_error("pipeline: unknown topology %d\n", (int)info->topology);
        return PVK_ERROR_VALIDATION_FAILED;
    }

    p = memalign(16, sizeof(struct PvkPipeline_T));
    if (!p)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    memset(p, 0, sizeof(*p));

    p->vs           = vs;
    p->fs           = fs;
    p->layout       = info->layout;
    if ((res = validate_layout(p, info)) != PVK_SUCCESS || (res = setup_vertex_input(p, info, fmt)) != PVK_SUCCESS ||
        (res = copy_code(p, fmt)) != PVK_SUCCESS) {
        pvkDestroyPipeline(device, p);
        return res;
    }
    p->inputQwords  = vs->inputCount;
    /* A vertex shader with a texture coordinate output stores ST before RGBAQ, XYZ2. */
    p->outputQwords = vs->texcoordOutputLocation >= 0 ? PVK_VU1_OUTPUT_PER_VERTEX_TEXTURED : PVK_VU1_OUTPUT_PER_VERTEX;
    p->primStep     = topologies[info->topology].step;
    p->primOverlap  = topologies[info->topology].overlap;
    p->fan          = topologies[info->topology].fan;
    p->strip        = info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    p->maxBatch     = max_batch_vertices(p, p->primStep);
    /* ST holds S/w, T/w and Q = 1/w (FST = 0), so the GS divides per pixel. */
    p->prim         = GS_SET_PRIM(topologies[info->topology].prim, fs->gouraud, fs->textured, 0,
                                  info->colorBlend.attachment.blendEnable ? 1 : 0, 0, 0, 0, 0);

    if ((res = setup_depth(p, info)) != PVK_SUCCESS || (res = setup_blend(p, info)) != PVK_SUCCESS ||
        (res = setup_cull(p, info)) != PVK_SUCCESS) {
        pvkDestroyPipeline(device, p);
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
    if (!pipeline)
        return;
    free(pipeline->code);
    free(pipeline);
}
