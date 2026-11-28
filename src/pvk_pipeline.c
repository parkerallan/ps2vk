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

/* What a stencil op does to the one stencil bit, and which pixels a stencil compare passes. */
enum { ST_KEEP, ST_SET0, ST_SET1, ST_INVERT };
enum { ST_ALWAYS, ST_NEVER, ST_BIT0, ST_BIT1 };

/* The op on a 1-bit stencil; -1 for no PvkStencilOp. A clear write mask bit keeps the bit. */
static int stencil_action(PvkStencilOp op, const PvkStencilOpState *s)
{
    int action;

    switch (op) {
    case PVK_STENCIL_OP_KEEP:                action = ST_KEEP; break;
    case PVK_STENCIL_OP_ZERO:
    case PVK_STENCIL_OP_DECREMENT_AND_CLAMP: action = ST_SET0; break;
    case PVK_STENCIL_OP_REPLACE:             action = s->reference & 1 ? ST_SET1 : ST_SET0; break;
    case PVK_STENCIL_OP_INCREMENT_AND_CLAMP: action = ST_SET1; break;
    case PVK_STENCIL_OP_INVERT:
    case PVK_STENCIL_OP_INCREMENT_AND_WRAP:
    case PVK_STENCIL_OP_DECREMENT_AND_WRAP:  action = ST_INVERT; break;
    default:                                 return -1;
    }
    return s->writeMask & 1 ? action : ST_KEEP;
}

/* (reference & compareMask) compareOp (stencil & compareMask) on a 1-bit stencil; -1 for no PvkCompareOp. */
static int stencil_passes(const PvkStencilOpState *s)
{
    uint32_t m = s->compareMask & 1, r = s->reference & m;

    switch (s->compareOp) {
    case PVK_COMPARE_OP_NEVER:            return ST_NEVER;
    case PVK_COMPARE_OP_ALWAYS:           return ST_ALWAYS;
    case PVK_COMPARE_OP_EQUAL:            return !m ? ST_ALWAYS : r ? ST_BIT1 : ST_BIT0;
    case PVK_COMPARE_OP_NOT_EQUAL:        return !m ? ST_NEVER : r ? ST_BIT0 : ST_BIT1;
    case PVK_COMPARE_OP_LESS:             return !m || r ? ST_NEVER : ST_BIT1;
    case PVK_COMPARE_OP_LESS_OR_EQUAL:    return !m || !r ? ST_ALWAYS : ST_BIT1;
    case PVK_COMPARE_OP_GREATER:          return !m || !r ? ST_NEVER : ST_BIT0;
    case PVK_COMPARE_OP_GREATER_OR_EQUAL: return !m || r ? ST_ALWAYS : ST_BIT0;
    default:                              return -1;
    }
}

/* The action on a pixel whose stencil bit is known (0 or 1; -1 = either). */
static int stencil_resolve(int action, int bit)
{
    if (bit == 0)
        return action == ST_SET0 ? ST_KEEP : action == ST_INVERT ? ST_SET1 : action;
    if (bit == 1)
        return action == ST_SET1 ? ST_KEEP : action == ST_INVERT ? ST_SET0 : action;
    return action;
}

/* One face's stencil state as the GS does it: `passes` and what a passing pixel gets. 0 if the GS cannot. */
static int stencil_face(const PvkStencilOpState *s, const PvkPipelineDepthStencilStateCreateInfo *ds,
                        const char *face, int *passes, int *passAction)
{
    int fail = stencil_action(s->failOp, s), pass = stencil_action(s->passOp, s);
    int depthFail = stencil_action(s->depthFailOp, s), known;
    int depthCanFail = ds->depthTestEnable && ds->depthCompareOp != PVK_COMPARE_OP_ALWAYS;
    int depthCanPass = !ds->depthTestEnable || ds->depthCompareOp != PVK_COMPARE_OP_NEVER;

    *passes = stencil_passes(s);
    if (fail < 0 || pass < 0 || depthFail < 0 || *passes < 0) {
        pvk_error("pipeline: %s stencil state holds a value that is no PvkStencilOp or PvkCompareOp\n", face);
        return 0;
    }
    /* The GS writes nothing to a pixel that fails the stencil test or the depth test. */
    known = *passes == ST_BIT0 ? 1 : *passes == ST_BIT1 ? 0 : -1;
    if (*passes != ST_ALWAYS && stencil_resolve(fail, known) != ST_KEEP) {
        pvk_error("pipeline: %s failOp %d changes the stencil of a pixel that fails the stencil test; the GS "
                  "writes nothing to such a pixel, so only an op that keeps the bit works\n", face, (int)s->failOp);
        return 0;
    }
    known = *passes == ST_BIT0 ? 0 : *passes == ST_BIT1 ? 1 : -1;
    if (*passes != ST_NEVER && depthCanFail && stencil_resolve(depthFail, known) != ST_KEEP) {
        pvk_error("pipeline: %s depthFailOp %d changes the stencil of a pixel that fails the depth test; the GS "
                  "writes nothing to such a pixel, so only an op that keeps the bit works\n", face, (int)s->depthFailOp);
        return 0;
    }
    *passAction = *passes != ST_NEVER && depthCanPass ? stencil_resolve(pass, known) : ST_KEEP;
    if (*passAction != ST_KEEP && *passAction != ST_SET1) {
        pvk_error("pipeline: %s passOp %d turns the stencil bit off; GS FBA can only turn the bit on, so a "
                  "draw keeps the bit or sets it, and a clear turns it off\n", face, (int)s->passOp);
        return 0;
    }
    return 1;
}

/* The stencil is the top alpha bit of the color attachment's pixels: tested by GS TEST.DATE/DATM,
 * set by FBA, kept by FRAME.FBMSK bit 31. Runs after setup_blend, which sets fbmsk. */
static PvkResult setup_stencil(PvkPipeline p, const PvkGraphicsPipelineCreateInfo *info)
{
    const PvkPipelineDepthStencilStateCreateInfo *ds = &info->depthStencil;
    const PvkPipelineRasterizationStateCreateInfo *rs = &info->rasterization;
    int triangles = info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                    info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                    info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    int passes, action, backPasses, backAction;

    if (info->stencilAttachmentFormat != PVK_FORMAT_UNDEFINED && info->stencilAttachmentFormat != PVK_FORMAT_CT32 &&
        info->stencilAttachmentFormat != PVK_FORMAT_CT16) {
        pvk_error("pipeline: stencilAttachmentFormat must be PVK_FORMAT_CT32, PVK_FORMAT_CT16 (the color "
                  "attachment's format) or PVK_FORMAT_UNDEFINED\n");
        return PVK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    p->stencilFormat = info->stencilAttachmentFormat;
    /* Without a stencil attachment every pixel passes and FBA and alpha bit 31 are the color's. */
    if (p->stencilFormat == PVK_FORMAT_UNDEFINED)
        return PVK_SUCCESS;
    p->fbmsk |= 0x80000000u;
    if (!ds->stencilTestEnable)
        return PVK_SUCCESS;

    if (!stencil_face(&ds->front, ds, "front", &passes, &action) ||
        !stencil_face(&ds->back, ds, "back", &backPasses, &backAction))
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    if (triangles && rs->cullMode == PVK_CULL_MODE_NONE && (passes != backPasses || action != backAction)) {
        pvk_error("pipeline: front and back stencil state differ and no face is culled; the GS has one "
                  "stencil state for both faces\n");
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (triangles && rs->cullMode == PVK_CULL_MODE_FRONT_BIT) {
        passes = backPasses;
        action = backAction;
    }
    p->stencilNever = passes == ST_NEVER;
    p->date = passes == ST_BIT0 || passes == ST_BIT1;
    p->datm = passes == ST_BIT1;
    if (action == ST_SET1) {
        p->fba = 1;
        p->fbmsk &= ~0x80000000u;
    }
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

    if (bs->colorWriteMask & ~0xFu) {
        pvk_error("pipeline: colorWriteMask 0x%x has bits that are no PvkColorComponentFlagBits\n",
                  (unsigned)bs->colorWriteMask);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    /* FBMSK protects the bits it sets: a byte per channel the mask leaves out. */
    p->fbmsk = (bs->colorWriteMask & PVK_COLOR_COMPONENT_R_BIT ? 0 : 0x000000FFu)
             | (bs->colorWriteMask & PVK_COLOR_COMPONENT_G_BIT ? 0 : 0x0000FF00u)
             | (bs->colorWriteMask & PVK_COLOR_COMPONENT_B_BIT ? 0 : 0x00FF0000u)
             | (bs->colorWriteMask & PVK_COLOR_COMPONENT_A_BIT ? 0 : 0xFF000000u);

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
    p->blendFix = src == 2;
    if (src == 2 && !(p->dynamic & (1u << PVK_DYNAMIC_STATE_BLEND_CONSTANTS))) {
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

static PvkResult setup_dynamic(PvkPipeline p, const PvkGraphicsPipelineCreateInfo *info)
{
    uint32_t i;

    for (i = 0; i < info->dynamicState.dynamicStateCount; i++) {
        PvkDynamicState s = info->dynamicState.dynamicStates[i];

        if (s == PVK_DYNAMIC_STATE_LINE_STIPPLE) {
            p->dynamic |= PVK_DYNAMIC_LINE_STIPPLE_BIT;
            continue;
        }
        if (s != PVK_DYNAMIC_STATE_VIEWPORT && s != PVK_DYNAMIC_STATE_SCISSOR &&
            s != PVK_DYNAMIC_STATE_BLEND_CONSTANTS) {
            pvk_error("pipeline: dynamic state %d; only VIEWPORT, SCISSOR, BLEND_CONSTANTS and LINE_STIPPLE "
                      "change between draws\n", (int)s);
            return PVK_ERROR_FEATURE_NOT_PRESENT;
        }
        p->dynamic |= 1u << s;
    }
    p->scissor = info->scissor;
    return PVK_SUCCESS;
}

/* The line and polygon smooth states chained to the rasterization state: PRIM.AA1. */
static PvkResult setup_lines(PvkPipeline p, PvkDevice device, const PvkGraphicsPipelineCreateInfo *info)
{
    const PvkPhysicalDeviceLineRasterizationFeatures *on = &device->lineFeatures;
    const PvkPipelineRasterizationLineStateCreateInfo *ls = NULL;
    const PvkPipelineRasterizationPolygonStateCreateInfo *ps = NULL;
    const struct PvkBaseStructure *s;
    int line = info->topology == PVK_PRIMITIVE_TOPOLOGY_LINE_LIST || info->topology == PVK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    int triangle = info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                   info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                   info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;

    if (info->rasterization.sType != PVK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO) {
        pvk_error("pipeline: rasterization.sType is %d, not "
                  "PVK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO\n", (int)info->rasterization.sType);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    for (s = info->rasterization.pNext; s; s = s->pNext)
        if (s->sType == PVK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO)
            ls = (const PvkPipelineRasterizationLineStateCreateInfo *)s;
        else if (s->sType == PVK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_POLYGON_STATE_CREATE_INFO)
            ps = (const PvkPipelineRasterizationPolygonStateCreateInfo *)s;

    if (ps && ps->polygonRasterizationMode != PVK_POLYGON_RASTERIZATION_MODE_DEFAULT &&
        ps->polygonRasterizationMode != PVK_POLYGON_RASTERIZATION_MODE_RECTANGULAR_SMOOTH) {
        pvk_error("pipeline: polygonRasterizationMode %d is no PvkPolygonRasterizationMode\n",
                  (int)ps->polygonRasterizationMode);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    /* Blended, PRIM.AA1 takes the coverage only where alpha is 1.0 and draws the other borders at full alpha. */
    if (ps && ps->polygonRasterizationMode == PVK_POLYGON_RASTERIZATION_MODE_RECTANGULAR_SMOOTH && triangle &&
        !info->colorBlend.attachment.blendEnable) {
        p->prim |= PVK_PRIM_AA1;
        /* The coverage is the alpha of every pixel: (Cs - Cd) * As + Cd. */
        p->alpha = GS_SET_ALPHA(0, 1, 0, 1, 0x80);
    }
    if (!ls)
        return PVK_SUCCESS;

    if (ls->stippledLineEnable) {
        pvk_error("pipeline: stippledLineEnable; the GS has no line pattern\n");
        return PVK_ERROR_FEATURE_NOT_PRESENT;
    }
    switch (ls->lineRasterizationMode) {
    case PVK_LINE_RASTERIZATION_MODE_DEFAULT:
        return PVK_SUCCESS;
    case PVK_LINE_RASTERIZATION_MODE_RECTANGULAR:
        if (on->rectangularLines)
            return PVK_SUCCESS;
        pvk_error("pipeline: PVK_LINE_RASTERIZATION_MODE_RECTANGULAR needs the rectangularLines feature\n");
        return PVK_ERROR_VALIDATION_FAILED;
    case PVK_LINE_RASTERIZATION_MODE_BRESENHAM:
        if (on->bresenhamLines)
            return PVK_SUCCESS;
        pvk_error("pipeline: PVK_LINE_RASTERIZATION_MODE_BRESENHAM needs the bresenhamLines feature\n");
        return PVK_ERROR_VALIDATION_FAILED;
    case PVK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH:
        if (!on->smoothLines) {
            pvk_error("pipeline: PVK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH needs the smoothLines feature\n");
            return PVK_ERROR_VALIDATION_FAILED;
        }
        /* Unblended, PRIM.AA1 would still blend the border with the ALPHA register. */
        if (line && info->colorBlend.attachment.blendEnable)
            p->prim |= PVK_PRIM_AA1;
        return PVK_SUCCESS;
    default:
        pvk_error("pipeline: lineRasterizationMode %d is no PvkLineRasterizationMode\n",
                  (int)ls->lineRasterizationMode);
        return PVK_ERROR_VALIDATION_FAILED;
    }
}

void pvk_viewport_context(const PvkViewport *vp, PvkFormat depthFormat, float *ctx)
{
    float depthMax = pvk_depth_max(depthFormat);

    /* Vulkan viewport transform, in GS primitive coordinates: x and y go from
     * NDC [-1, 1] (y down) to pixels, z from [0, 1] to the depth format's
     * flipped integer range. */
    ctx[PVK_VU1_CTX_VIEWPORT_SCALE * 4 + 0]  = vp->width * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_SCALE * 4 + 1]  = vp->height * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_SCALE * 4 + 2]  = -depthMax * (vp->maxDepth - vp->minDepth);
    ctx[PVK_VU1_CTX_VIEWPORT_OFFSET * 4 + 0] = PVK_GS_ORIGIN + vp->x + vp->width * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_OFFSET * 4 + 1] = PVK_GS_ORIGIN + vp->y + vp->height * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_OFFSET * 4 + 2] = depthMax * (1.0f - vp->minDepth);
}

static PvkResult validate_shaders(const PvkShaderCode *vs, const PvkShaderCode *fs)
{
    if (!vs || (vs->stage != PVK_SHADER_STAGE_VERTEX && vs->stage != PVK_SHADER_STAGE_MESH_BIT_EXT)) {
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
    if (fs->fogInputLocation >= 0 && fs->fogInputLocation != vs->fogOutputLocation) {
        pvk_error("pipeline: fragment shader '%s' fogs by the float at location %d, "
                  "but vertex shader '%s' does not write one there\n",
                  fs->name, (int)fs->fogInputLocation, vs->name);
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
    return !memcmp(a->pushWords, b->pushWords, sizeof(a->pushWords));
}

PvkResult pvkCreatePipelineLayout(PvkDevice device, const PvkPipelineLayoutCreateInfo *info, PvkPipelineLayout *layout)
{
    PvkPipelineLayout l;
    uint32_t i, used = 0;

    (void)device;

    if (info->setLayoutCount > pvk_limits.maxBoundDescriptorSets) {
        pvk_error("pipeline layout: %u descriptor set layouts; at most %u (maxBoundDescriptorSets)\n",
                  (unsigned)info->setLayoutCount, (unsigned)pvk_limits.maxBoundDescriptorSets);
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

        if ((r->offset | r->size) & 3 || r->size == 0 || r->offset + r->size > pvk_limits.maxPushConstantsSize) {
            pvk_error("pipeline layout: push constant range %u (offset %u, size %u) must be multiples "
                      "of 4 within %u bytes (maxPushConstantsSize)\n", (unsigned)i, (unsigned)r->offset,
                      (unsigned)r->size, (unsigned)pvk_limits.maxPushConstantsSize);
            free(l);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (!r->stageFlags || r->stageFlags & ~PVK_ALL_SHADER_STAGES) {
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
        if (r->stageFlags & PVK_SHADER_STAGE_COMPUTE_BIT)
            l->pushWords[2] |= mask;
        if (r->stageFlags & PVK_SHADER_STAGE_TASK_BIT_EXT)
            l->pushWords[3] |= mask;
        if (r->stageFlags & PVK_SHADER_STAGE_MESH_BIT_EXT)
            l->pushWords[4] |= mask;
    }

    *layout = l;
    return PVK_SUCCESS;
}

void pvkDestroyPipelineLayout(PvkDevice device, PvkPipelineLayout layout)
{
    (void)device;
    free(layout);
}

/* A compute, task or mesh shader's push constant block and its uniform and storage blocks must
 * be in the layout for its stage. Fills in the slots. */
static PvkResult validate_blocks(const PvkShaderCode *cs, PvkPipelineLayout layout, uint32_t stageBit, uint32_t pushIndex,
                                 const char *who, const char *stage, uint32_t *uniformSlots, uint32_t *storageSlots)
{
    uint32_t needed = pvk_push_word_mask(0, cs->pushConstantBytes), i;

    if ((layout->pushWords[pushIndex] & needed) != needed) {
        pvk_error("%s: %s shader '%s' reads %u bytes of push constants, but the layout's "
                  "%s stage ranges do not cover them\n", who, stage, cs->name, (unsigned)cs->pushConstantBytes, stage);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    for (i = 0; i < cs->uniformBlockCount + cs->storageBlockCount; i++) {
        int uniform = i < cs->uniformBlockCount;
        uint32_t set = uniform ? cs->uniformBlocks[i].set : cs->storageBlocks[i - cs->uniformBlockCount].set;
        uint32_t binding = uniform ? cs->uniformBlocks[i].binding : cs->storageBlocks[i - cs->uniformBlockCount].binding;
        PvkDescriptorType type = uniform ? PVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        const char *what = uniform ? "uniform" : "storage";
        int slot = set < layout->setLayoutCount ? pvk_set_layout_find(&layout->setLayouts[set], binding) : -1;

        if (slot < 0 || layout->setLayouts[set].bindings[slot].descriptorType != type ||
            !(layout->setLayouts[set].bindings[slot].stageFlags & stageBit)) {
            pvk_error("%s: %s shader '%s' has a %s block at set %u, binding %u, but the layout "
                      "has no PVK_DESCRIPTOR_TYPE_%s_BUFFER binding there for the %s stage\n", who, stage, cs->name, what,
                      (unsigned)set, (unsigned)binding, uniform ? "UNIFORM" : "STORAGE", stage);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (uniform)
            uniformSlots[i] = (uint32_t)slot;
        else
            storageSlots[i - cs->uniformBlockCount] = (uint32_t)slot;
    }
    return PVK_SUCCESS;
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
    if (p->mesh) {
        PvkResult res = validate_blocks(vs, info->layout, PVK_SHADER_STAGE_MESH_BIT_EXT, 4, "pipeline", "mesh",
                                        p->uniformSlots, p->storageSlots);
        if (res == PVK_SUCCESS && p->ts)
            res = validate_blocks(p->ts, info->layout, PVK_SHADER_STAGE_TASK_BIT_EXT, 3, "pipeline", "task",
                                  p->taskUniformSlots, p->taskStorageSlots);
        if (res != PVK_SUCCESS)
            return res;
    }
    needed = p->mesh ? 0 : pvk_push_word_mask(0, vs->pushConstantBytes);
    if ((info->layout->pushWords[0] & needed) != needed) {
        pvk_error("pipeline: vertex shader '%s' reads %u bytes of push constants, but the layout's "
                  "vertex stage ranges do not cover them\n", vs->name, (unsigned)vs->pushConstantBytes);
        return PVK_ERROR_VALIDATION_FAILED;
    }

    for (i = 0; !p->mesh && i < vs->uniformBlockCount; i++) {
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
 * format of shader input i, qword[i] its qword within a vertex or an instance
 * and instance[i] its binding's rate. VU1 reads a vertex (or an instance) as
 * one qword per shader input of that rate in location order, and the VIF
 * unpacks a binding with one UNPACK code, so a binding's attributes are
 * consecutive inputs of one format, packed. */
static PvkResult setup_vertex_input(PvkPipeline p, const PvkGraphicsPipelineCreateInfo *info,
                                    const VertexFormat **fmt, uint32_t *qword, int *instance)
{
    const PvkShaderCode *vs = p->vs;
    uint32_t i, j, next[2] = { 0, 0 }, first[PVK_MAX_VERTEX_BINDINGS] = { 0 };

    for (i = 0; i < info->bindingCount; i++) {
        const PvkVertexInputBinding *b = &info->bindings[i];

        if (b->binding >= pvk_limits.maxVertexInputBindings || !b->stride || p->vertexBindings[b->binding].stride) {
            pvk_error("pipeline: vertex binding %u is out of range, declared twice or has stride 0\n",
                      (unsigned)b->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if ((unsigned)b->inputRate > PVK_VERTEX_INPUT_RATE_INSTANCE) {
            pvk_error("pipeline: vertex binding %u has inputRate %d; use a PvkVertexInputRate value\n",
                      (unsigned)b->binding, (int)b->inputRate);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        p->vertexBindings[b->binding].stride   = b->stride;
        p->vertexBindings[b->binding].instance = b->inputRate == PVK_VERTEX_INPUT_RATE_INSTANCE;
    }

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
        if (attr->binding >= pvk_limits.maxVertexInputBindings || !p->vertexBindings[attr->binding].stride) {
            pvk_error("pipeline: the attribute at location %u reads vertex binding %u, which is not declared\n",
                      (unsigned)attr->location, (unsigned)attr->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        vb = &p->vertexBindings[attr->binding];
        if (!vb->used) {
            vb->used = 1;
            vb->slot = next[vb->instance];
            vb->mode = f->mode;
            vb->usn  = f->usn;
            first[attr->binding] = i;
        } else if (first[attr->binding] + vb->count != i) {
            pvk_error("pipeline: the attributes of vertex binding %u must feed consecutive shader inputs\n",
                      (unsigned)attr->binding);
            return PVK_ERROR_VALIDATION_FAILED;
        } else if (vb->mode != f->mode || vb->usn != f->usn || fmt[first[attr->binding]]->conv != f->conv) {
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
        qword[i]    = vb->slot + vb->count;
        instance[i] = vb->instance;
        next[vb->instance]++;
        vb->count++;
        fmt[i] = f;
    }

    p->align = p->instanceAlign = 1;
    for (i = 0; i < PVK_MAX_VERTEX_BINDINGS; i++) {
        struct PvkVertexBinding *vb = &p->vertexBindings[i];
        uint32_t *align = vb->instance ? &p->instanceAlign : &p->align;

        if (!vb->used)
            continue;
        if (vb->stride != vb->count * fmt[first[i]]->bytes) {
            pvk_error("pipeline: vertex binding %u has stride %u but its attributes take %u bytes "
                      "(a binding is tightly packed)\n", (unsigned)i, (unsigned)vb->stride,
                      (unsigned)(vb->count * fmt[first[i]]->bytes));
            return PVK_ERROR_VALIDATION_FAILED;
        }
        vb->align = 16 / gcd(16, vb->stride);
        vb->chunk = 255 / vb->count;
        vb->chunk -= vb->chunk % vb->align;
        *align = *align / gcd(*align, vb->align) * vb->align;
    }
    p->vertexQwords   = next[0];
    p->instanceQwords = next[1];
    p->inputBudget    = PVK_BATCH_INPUT_QWORDS - p->instanceQwords * (1 + 2 * (p->instanceAlign - 1));
    return PVK_SUCCESS;
}

/* VU1 instruction words the conversion block is patched with. */
#define VU_UPPER_NOP    0x000002FFu
#define VU_LOWER_NOP    0x8000033Cu
#define VU_LOWER_B      0x40000000u /* b imm11: target = this pair + 1 + imm11 */
#define VU_DEST_SHIFT   21          /* xyzw write mask, x in bit 24 */
#define VU_LQ_BASE_SHIFT 11         /* lq ft,imm11(is): is in bits 15-11, imm11 in bits 10-0 */
#define VU_VERTEX_REG   3           /* VI03: the vertex being processed */
#define VU_INSTANCE_REG 15          /* VI15: the instance being processed */

static void vu_nop(uint32_t *pair)
{
    pair[0] = VU_LOWER_NOP;
    pair[1] = VU_UPPER_NOP;
}

/* The pipeline's copy of the vertex shader, with the input loads before the
 * conversion block (see PvkShaderCode::vu1Convert) pointed at each input's
 * qword of the vertex or the instance, and the block patched: per input the
 * pairs `itof0` + `loi` scale, `muli` + `loi` -1, `maxi` and `mul.mask
 * VFn,VF00,VF00` (0 and 1 for the components the format lacks), each NOP'd
 * when the format does not need it (a `loi` feeds the next pair), and the
 * branch skips the leading inputs that need nothing. The run-time index
 * clamps (vu1Clamps) are NOP'd unless the device has robustBufferAccess. */
static PvkResult copy_code(PvkPipeline p, const VertexFormat *const *fmt, const uint32_t *qword, const int *instance,
                           int robust)
{
    const PvkShaderCode *vs = p->vs;
    size_t words = (size_t)(vs->vu1CodeEnd - vs->vu1CodeStart);
    uint32_t *blk, i, first = vs->inputCount, target;

    if (!vs->vu1Convert || vs->vu1Convert < vs->vu1CodeStart + 2 * vs->inputCount ||
        vs->vu1Convert + 2 * (2 + 4 * vs->inputCount) > vs->vu1CodeEnd) {
        pvk_error("pipeline: vertex shader '%s' has no input conversion block; rebuild it with this shaderc\n",
                  vs->name);
        return PVK_ERROR_INVALID_SHADER;
    }
    if (words / 2 > PVK_VU1_FRAME_ROUTINE) {
        pvk_error("pipeline: vertex shader '%s' is %u VU1 instructions; micro memory holds %u\n", vs->name,
                  (unsigned)(words / 2), (unsigned)PVK_VU1_FRAME_ROUTINE);
        return PVK_ERROR_INVALID_SHADER;
    }
    p->program = calloc(1, sizeof(*p->program));
    if (!p->program)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    p->program->vs   = vs;
    pvk_shader_retain(vs);
    p->program->refs = 1;
    p->program->code = memalign(16, words * 4);
    if (!p->program->code)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(p->program->code, vs->vu1CodeStart, words * 4);
    p->program->codeEnd = p->program->code + words;
    blk = p->program->code + (vs->vu1Convert - vs->vu1CodeStart);

    for (i = 0; i < vs->inputCount; i++) {
        uint32_t *load = blk - 2 * (vs->inputCount - i);
        uint32_t *s = blk + 2 * (2 + 4 * i);

        load[0] = (load[0] & ~0xFFFFu) | ((instance[i] ? VU_INSTANCE_REG : VU_VERTEX_REG) << VU_LQ_BASE_SHIFT) | qword[i];
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

    for (i = 0; i < vs->vu1ClampCount && !robust; i++) {
        const uint32_t *c = vs->vu1Clamps[i];
        if (c < vs->vu1CodeStart || c + 6 > vs->vu1CodeEnd) {
            pvk_error("pipeline: vertex shader '%s' has a run-time index clamp outside its code\n", vs->name);
            return PVK_ERROR_INVALID_SHADER;
        }
        vu_nop(p->program->code + (c - vs->vu1CodeStart));
        vu_nop(p->program->code + (c - vs->vu1CodeStart) + 2);
        vu_nop(p->program->code + (c - vs->vu1CodeStart) + 4);
    }
    return PVK_SUCCESS;
}

/* What a mesh pipeline's two shaders share: the uniform area, the payload and VU1 data memory. */
static PvkResult mesh_check(const PvkShaderCode *ms, const PvkShaderCode *ts)
{
    uint32_t meshEnd = 0, taskStart = PVK_VU1_UNIFORM_QWORDS, i;

    for (i = 0; i < ms->uniformBlockCount; i++)
        if (ms->uniformBlocks[i].qword + (ms->uniformBlocks[i].bytes + 15) / 16 > meshEnd)
            meshEnd = ms->uniformBlocks[i].qword + (ms->uniformBlocks[i].bytes + 15) / 16;
    for (i = 0; ts && i < ts->uniformBlockCount; i++)
        if (ts->uniformBlocks[i].qword < taskStart)
            taskStart = ts->uniformBlocks[i].qword;
    if (meshEnd > taskStart) {
        pvk_error("pipeline: the uniform blocks of mesh shader '%s' and task shader '%s' take more than %u bytes "
                  "together (maxUniformBufferRange)\n", ms->name, ts->name, (unsigned)PVK_MAX_UNIFORM_BYTES);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (ms->payloadBytes && (!ts || ts->payloadBytes != ms->payloadBytes)) {
        pvk_error("pipeline: mesh shader '%s' reads a task payload of %u bytes, but %s\n", ms->name,
                  (unsigned)ms->payloadBytes, ts ? "the task shader's payload is of another size"
                                                 : "the pipeline has no task stage");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (ts && PVK_VU1_PAYLOAD_ADDR + (ts->payloadBytes + 15) / 16 + ts->sharedBytes / 16 >
              ms->meshOutputAddress - ms->sharedBytes / 16) {
        pvk_error("pipeline: the payload and shared variables of task shader '%s' and the shared variables and "
                  "outputs of mesh shader '%s' take more than %u bytes together "
                  "(maxMeshPayloadAndOutputMemorySize)\n", ts->name, ms->name, (unsigned)PVK_MAX_MESH_OUTPUT_BYTES);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    return PVK_SUCCESS;
}

/* A mesh pipeline's program: the task shader's code, which calls what follows it, then the mesh
 * shader's. The run-time index clamps are NOP'd unless the device has robustBufferAccess. */
static PvkResult mesh_code(PvkPipeline p, int robust)
{
    const PvkShaderCode *part[2] = { p->ts, p->vs };
    size_t words = 0, at = 0;
    uint32_t i, k;

    for (k = 0; k < 2; k++)
        if (part[k])
            words += (size_t)(part[k]->vu1CodeEnd - part[k]->vu1CodeStart);
    if (words / 2 > PVK_VU1_FRAME_ROUTINE) {
        pvk_error("pipeline: mesh shader '%s' and its task shader are %u VU1 instructions together; micro memory "
                  "holds %u\n", p->vs->name, (unsigned)(words / 2), (unsigned)PVK_VU1_FRAME_ROUTINE);
        return PVK_ERROR_INVALID_SHADER;
    }
    p->program = calloc(1, sizeof(*p->program));
    if (!p->program)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    p->program->vs   = p->vs;
    pvk_shader_retain(p->vs);
    p->program->refs = 1;
    p->program->code = memalign(16, words * 4);
    if (!p->program->code)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    p->program->codeEnd = p->program->code + words;
    for (k = 0; k < 2; k++) {
        const PvkShaderCode *c = part[k];
        size_t n = c ? (size_t)(c->vu1CodeEnd - c->vu1CodeStart) : 0;

        if (!c)
            continue;
        memcpy(p->program->code + at, c->vu1CodeStart, n * 4);
        for (i = 0; i < c->vu1ClampCount && !robust; i++) {
            const uint32_t *clamp = c->vu1Clamps[i];
            if (clamp < c->vu1CodeStart || clamp + 6 > c->vu1CodeEnd) {
                pvk_error("pipeline: shader '%s' has a run-time index clamp outside its code\n", c->name);
                return PVK_ERROR_INVALID_SHADER;
            }
            vu_nop(p->program->code + at + (clamp - c->vu1CodeStart));
            vu_nop(p->program->code + at + (clamp - c->vu1CodeStart) + 2);
            vu_nop(p->program->code + at + (clamp - c->vu1CodeStart) + 4);
        }
        at += n;
    }
    p->meshEntry = p->ts ? 0 : PVK_VU1_MESH_ENTRY;
    return PVK_SUCCESS;
}

/* Vulkan's rules for a stage's PvkSpecializationInfo: every entry lies within
 * data. Which constants exist is the shader's business (specialize). */
static PvkResult check_specialization(const PvkSpecializationInfo *info, uint32_t stage)
{
    uint32_t i;

    if (!info)
        return PVK_SUCCESS;
    if ((info->mapEntryCount && !info->mapEntries) || (info->dataSize && !info->data)) {
        pvk_error("pipeline: stage %u has specialization entries or data without the pointer to them\n",
                  (unsigned)stage);
        return PVK_ERROR_VALIDATION_FAILED;
    }
    for (i = 0; i < info->mapEntryCount; i++) {
        const PvkSpecializationMapEntry *e = &info->mapEntries[i];

        if (e->size == 0 || e->offset > info->dataSize || e->size > info->dataSize - e->offset) {
            pvk_error("pipeline: stage %u specialization entry %u (offset %u, size %u) lies outside its "
                      "%u bytes of data\n", (unsigned)stage, (unsigned)i, (unsigned)e->offset,
                      (unsigned)e->size, (unsigned)info->dataSize);
            return PVK_ERROR_VALIDATION_FAILED;
        }
    }
    return PVK_SUCCESS;
}

/* Writes the value each entry gives one of the vertex shader's specialization
 * constants over the LOI immediate holding its default in the pipeline's copy
 * of the code, as the float VU1 computes with. */
static PvkResult specialize(PvkPipeline p, const PvkShaderCode *vs, const PvkSpecializationInfo *info, size_t at)
{
    const char *stage = vs->stage == PVK_SHADER_STAGE_COMPUTE_BIT ? "compute" : vs->stage == PVK_SHADER_STAGE_MESH_BIT_EXT
                      ? "mesh" : vs->stage == PVK_SHADER_STAGE_TASK_BIT_EXT ? "task" : "vertex";
    uint32_t i, j;

    for (i = 0; i < vs->vu1SpecConstantCount; i++) {
        const PvkShaderSpecConstant *c = &vs->vu1SpecConstants[i];

        if (c->vu1Word < vs->vu1CodeStart || c->vu1Word >= vs->vu1CodeEnd) {
            pvk_error("pipeline: %s shader '%s' has a specialization constant outside its code\n", stage, vs->name);
            return PVK_ERROR_INVALID_SHADER;
        }
        for (j = 0; info && j < info->mapEntryCount; j++) {
            const PvkSpecializationMapEntry *e = &info->mapEntries[j];
            uint32_t raw;
            float v;

            if (e->constantID != c->constantID)
                continue;
            if (e->size != 4) {
                pvk_error("pipeline: specialization entry %u sets constant %u of %s shader '%s' with %u "
                          "bytes; it is 4 bytes\n", (unsigned)j, (unsigned)e->constantID, stage, vs->name,
                          (unsigned)e->size);
                return PVK_ERROR_VALIDATION_FAILED;
            }
            memcpy(&raw, (const uint8_t *)info->data + e->offset, sizeof(raw));
            switch (c->type) {
            case PVK_SPEC_CONSTANT_TYPE_INT:  v = (float)(int32_t)raw;      break;
            case PVK_SPEC_CONSTANT_TYPE_UINT: v = (float)raw;               break;
            case PVK_SPEC_CONSTANT_TYPE_BOOL: v = raw ? 1.0f : 0.0f;        break;
            default:                          memcpy(&v, &raw, sizeof(v)); break;
            }
            if (v != v || v - v != 0.0f) {
                pvk_error("pipeline: specialization constant %u of %s shader '%s' is infinite or NaN, "
                          "which VU1 cannot represent\n", (unsigned)e->constantID, stage, vs->name);
                return PVK_ERROR_VALIDATION_FAILED;
            }
            memcpy(p->program->code + at + (c->vu1Word - vs->vu1CodeStart), &v, sizeof(v));
            break;
        }
    }
    return PVK_SUCCESS;
}

/* Memory from the callbacks, or memalign() without them. */
static void *host_alloc(const PvkAllocationCallbacks *alloc, size_t size)
{
    return alloc ? alloc->pfnAllocation(alloc->pUserData, size, 16, PVK_SYSTEM_ALLOCATION_SCOPE_OBJECT)
                 : memalign(16, size);
}

static void host_free(const PvkAllocationCallbacks *alloc, void *memory)
{
    if (alloc)
        alloc->pfnFree(alloc->pUserData, memory);
    else
        free(memory);
}

static void program_release(struct PvkProgram *prog)
{
    if (!prog || --prog->refs)
        return;
    pvk_shader_release(prog->vs);
    host_free(prog->hasAlloc ? &prog->alloc : NULL, prog->code);
    host_free(prog->hasAlloc ? &prog->alloc : NULL, prog);
}

/* Replaces the pipeline's freshly patched program with the cache's identical
 * one when there is one, else adds the pipeline's to the cache. */
static void program_intern(PvkPipeline p, PvkPipelineCache cache)
{
    struct PvkProgram *mine = p->program, *c;
    size_t bytes = (size_t)(mine->codeEnd - mine->code) * 4;

    for (c = cache->programs; c; c = c->next) {
        if (c->vs == mine->vs && (size_t)(c->codeEnd - c->code) * 4 == bytes && !memcmp(c->code, mine->code, bytes)) {
            program_release(mine);
            c->refs++;
            p->program = c;
            return;
        }
    }
    mine->refs++;
    mine->next      = cache->programs;
    cache->programs = mine;
}

static uint32_t max_batch_vertices(const PvkPipeline p, uint32_t step)
{
    /* Up to align - 1 unused vertices come before and after a batch's. */
    uint32_t byInput  = p->vertexQwords ? p->inputBudget / p->vertexQwords - 2 * (p->align - 1) : UINT32_MAX;
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

PvkResult pvkCreateGraphicsPipeline(PvkDevice device, PvkPipelineCache cache, const PvkGraphicsPipelineCreateInfo *info,
                                    PvkPipeline *pipeline)
{
    const PvkShaderCode *vs = NULL, *fs = NULL, *ts = NULL, *ms = NULL;
    const PvkSpecializationInfo *vsSpec = NULL, *tsSpec = NULL, *msSpec = NULL;
    const VertexFormat *fmt[PVK_MAX_VERTEX_INPUTS];
    uint32_t qword[PVK_MAX_VERTEX_INPUTS], i;
    int instance[PVK_MAX_VERTEX_INPUTS];
    PvkGraphicsPipelineCreateInfo meshInfo;
    PvkPipeline p;
    PvkResult res;
    float *ctx;

    for (i = 0; i < info->stageCount; i++) {
        const PvkPipelineShaderStageCreateInfo *s = &info->stages[i];
        const PvkShaderCode **slot = s->stage == PVK_SHADER_STAGE_VERTEX ? &vs
                                   : s->stage == PVK_SHADER_STAGE_FRAGMENT ? &fs
                                   : s->stage == PVK_SHADER_STAGE_TASK_BIT_EXT ? &ts
                                   : s->stage == PVK_SHADER_STAGE_MESH_BIT_EXT ? &ms : NULL;

        if (!slot || *slot || !s->module) {
            pvk_error("pipeline: stage %u must be a PVK_SHADER_STAGE_VERTEX, PVK_SHADER_STAGE_FRAGMENT, "
                      "PVK_SHADER_STAGE_TASK_BIT_EXT or PVK_SHADER_STAGE_MESH_BIT_EXT stage, each at most once, with a "
                      "module\n", (unsigned)i);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (s->module->code->stage != s->stage) {
            pvk_error("pipeline: stage %u names stage %d, but its module '%s' is not that stage's shader\n",
                      (unsigned)i, (int)s->stage, s->module->code->name);
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (!s->name || !s->module->code->entryPoint || strcmp(s->name, s->module->code->entryPoint)) {
            pvk_error("pipeline: stage %u names entry point '%s', but module '%s' has '%s'\n", (unsigned)i,
                      s->name ? s->name : "(null)", s->module->code->name,
                      s->module->code->entryPoint ? s->module->code->entryPoint : "(none; rebuild it with this shaderc)");
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if ((res = check_specialization(s->specializationInfo, i)) != PVK_SUCCESS)
            return res;
        *slot = s->module->code;
        if (s->stage == PVK_SHADER_STAGE_VERTEX)
            vsSpec = s->specializationInfo;
        else if (s->stage == PVK_SHADER_STAGE_TASK_BIT_EXT)
            tsSpec = s->specializationInfo;
        else if (s->stage == PVK_SHADER_STAGE_MESH_BIT_EXT)
            msSpec = s->specializationInfo;
    }
    if (ms || ts) {
        /* The mesh shader's output layout stands in for the vertex input and the topology. */
        static const PvkPrimitiveTopology lists[] = { PVK_PRIMITIVE_TOPOLOGY_POINT_LIST, PVK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                      PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };

        if (vs || !ms) {
            pvk_error("pipeline: a task stage needs a mesh stage, and neither goes with a vertex stage\n");
            return PVK_ERROR_VALIDATION_FAILED;
        }
        if (!device->meshFeatures.meshShader || (ts && !device->meshFeatures.taskShader)) {
            pvk_error("pipeline: a %s stage needs the %s feature turned on at pvkCreateDevice\n",
                      device->meshFeatures.meshShader ? "task" : "mesh",
                      device->meshFeatures.meshShader ? "taskShader" : "meshShader");
            return PVK_ERROR_FEATURE_NOT_PRESENT;
        }
        if ((res = mesh_check(ms, ts)) != PVK_SUCCESS)
            return res;
        vs       = ms;
        vsSpec   = msSpec;
        meshInfo = *info;
        meshInfo.topology = lists[ms->meshPrimitiveVertices - 1];
        info     = &meshInfo;
    }
    if (!vs || !fs) {
        pvk_error("pipeline: stages must hold a fragment stage and a vertex or a mesh stage\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
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
    pvk_shader_retain(vs);
    p->fs           = fs;
    pvk_shader_retain(fs);
    p->ate          = fs->alphaTest;
    p->atst         = fs->alphaTestFunction;
    p->aref         = fs->alphaTestReference;
    p->layout       = info->layout;
    p->mesh         = ms != NULL;
    p->ts           = ts;
    if (ts)
        pvk_shader_retain(ts);
    res = validate_layout(p, info);
    if (res == PVK_SUCCESS && p->mesh) {
        size_t taskWords = ts ? (size_t)(ts->vu1CodeEnd - ts->vu1CodeStart) : 0;

        res = mesh_code(p, device->features.robustBufferAccess != 0);
        if (res == PVK_SUCCESS && ts)
            res = specialize(p, ts, tsSpec, 0);
        if (res == PVK_SUCCESS)
            res = specialize(p, vs, vsSpec, taskWords);
    } else if (res == PVK_SUCCESS) {
        if ((res = setup_vertex_input(p, info, fmt, qword, instance)) == PVK_SUCCESS &&
            (res = copy_code(p, fmt, qword, instance, device->features.robustBufferAccess != 0)) == PVK_SUCCESS)
            res = specialize(p, vs, vsSpec, 0);
    }
    if (res != PVK_SUCCESS) {
        pvkDestroyPipeline(device, p, NULL);
        return res;
    }
    if (cache)
        program_intern(p, cache);
    /* A vertex shader with a texture coordinate output stores ST before RGBAQ, XYZ2. */
    p->texcoordOut  = vs->texcoordOutputLocation >= 0;
    p->fogOut       = vs->fogOutputLocation >= 0;
    p->fogcol       = fs->fogColor;
    p->outputQwords = PVK_VU1_OUTPUT_PER_VERTEX + p->texcoordOut + p->fogOut;
    p->primStep     = topologies[info->topology].step;
    p->primOverlap  = topologies[info->topology].overlap;
    p->fan          = topologies[info->topology].fan;
    p->strip        = info->topology == PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    p->maxBatch     = max_batch_vertices(p, p->primStep);
    /* ST holds S/w, T/w and Q = 1/w (FST = 0), so the GS divides per pixel. */
    p->prim         = GS_SET_PRIM(topologies[info->topology].prim, fs->gouraud, fs->textured, fs->fog,
                                  info->colorBlend.attachment.blendEnable ? 1 : 0, 0, 0, 0, 0);

    if ((res = setup_dynamic(p, info)) != PVK_SUCCESS || (res = setup_depth(p, info)) != PVK_SUCCESS ||
        (res = setup_blend(p, info)) != PVK_SUCCESS || (res = setup_cull(p, info)) != PVK_SUCCESS ||
        (res = setup_stencil(p, info)) != PVK_SUCCESS || (res = setup_lines(p, device, info)) != PVK_SUCCESS) {
        pvkDestroyPipeline(device, p, NULL);
        return res;
    }
    /* Depth writes are ZBUF.ZMSK, set once per context by pvkCmdBeginRendering: no writes = context 2. */
    if (p->zmsk)
        p->prim |= PVK_PRIM_CTXT;

    ctx = p->context;
    pvk_viewport_context(&info->viewport, p->depthFormat, ctx);
    /* Colors 0..1 to GS 0..255; GS alpha 128 means 1.0. */
    ctx[PVK_VU1_CTX_COLOR_SCALE * 4 + 0] = 255.0f;
    ctx[PVK_VU1_CTX_COLOR_SCALE * 4 + 1] = 255.0f;
    ctx[PVK_VU1_CTX_COLOR_SCALE * 4 + 2] = 255.0f;
    ctx[PVK_VU1_CTX_COLOR_SCALE * 4 + 3] = 128.0f;

    *pipeline = p;
    return PVK_SUCCESS;
}

/* The compute shader's push constant block must lie in the layout's compute ranges, each
 * uniform block be a uniform buffer binding and each storage block a storage buffer binding,
 * visible to the compute stage, in the set layout at its set index. Fills in the slots. */
static PvkResult validate_compute_layout(PvkPipeline p, PvkPipelineLayout layout)
{
    if (!layout) {
        pvk_error("compute pipeline: layout is NULL; create one with pvkCreatePipelineLayout\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    return validate_blocks(p->vs, layout, PVK_SHADER_STAGE_COMPUTE_BIT, 2, "compute pipeline", "compute",
                           p->uniformSlots, p->storageSlots);
}

/* The pipeline's copy of the compute shader, with the run-time index clamps NOP'd unless the
 * device has robustBufferAccess. */
static PvkResult compute_code(PvkPipeline p, int robust)
{
    const PvkShaderCode *cs = p->vs;
    size_t words = (size_t)(cs->vu1CodeEnd - cs->vu1CodeStart);
    uint32_t i;

    if (words / 2 > PVK_VU1_FRAME_ROUTINE) {
        pvk_error("compute pipeline: compute shader '%s' is %u VU1 instructions; micro memory holds %u\n", cs->name,
                  (unsigned)(words / 2), (unsigned)PVK_VU1_FRAME_ROUTINE);
        return PVK_ERROR_INVALID_SHADER;
    }
    p->program = host_alloc(p->hasAlloc ? &p->alloc : NULL, sizeof(*p->program));
    if (!p->program)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    memset(p->program, 0, sizeof(*p->program));
    p->program->alloc    = p->alloc;
    p->program->hasAlloc = p->hasAlloc;
    p->program->vs   = cs;
    pvk_shader_retain(cs);
    p->program->refs = 1;
    p->program->code = host_alloc(p->hasAlloc ? &p->alloc : NULL, words * 4);
    if (!p->program->code)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(p->program->code, cs->vu1CodeStart, words * 4);
    p->program->codeEnd = p->program->code + words;
    for (i = 0; i < cs->vu1ClampCount && !robust; i++) {
        const uint32_t *c = cs->vu1Clamps[i];
        if (c < cs->vu1CodeStart || c + 6 > cs->vu1CodeEnd) {
            pvk_error("compute pipeline: compute shader '%s' has a run-time index clamp outside its code\n", cs->name);
            return PVK_ERROR_INVALID_SHADER;
        }
        vu_nop(p->program->code + (c - cs->vu1CodeStart));
        vu_nop(p->program->code + (c - cs->vu1CodeStart) + 2);
        vu_nop(p->program->code + (c - cs->vu1CodeStart) + 4);
    }
    return PVK_SUCCESS;
}

/* One pipeline of pvkCreateComputePipelines(). */
static PvkResult create_compute_pipeline(PvkDevice device, PvkPipelineCache cache,
                                         const PvkComputePipelineCreateInfo *info,
                                         const PvkAllocationCallbacks *alloc, PvkPipeline *pipeline)
{
    const PvkPipelineShaderStageCreateInfo *s = &info->stage;
    const PvkShaderCode *cs = s->module ? s->module->code : NULL;
    PvkPipeline p;
    PvkResult res;

    if (s->stage != PVK_SHADER_STAGE_COMPUTE_BIT || !cs || cs->stage != PVK_SHADER_STAGE_COMPUTE_BIT) {
        pvk_error("compute pipeline: the stage must be PVK_SHADER_STAGE_COMPUTE_BIT with a compute shader's module\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (!s->name || !cs->entryPoint || strcmp(s->name, cs->entryPoint)) {
        pvk_error("compute pipeline: the stage names entry point '%s', but module '%s' has '%s'\n",
                  s->name ? s->name : "(null)", cs->name, cs->entryPoint ? cs->entryPoint : "(none)");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (info->sType != PVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO) {
        pvk_error("pvkCreateComputePipelines: sType is not PVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (info->flags) {
        pvk_error("pvkCreateComputePipelines: flags must be 0\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if ((res = check_specialization(s->specializationInfo, 0)) != PVK_SUCCESS)
        return res;

    p = host_alloc(alloc, sizeof(struct PvkPipeline_T));
    if (!p)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    memset(p, 0, sizeof(*p));
    if (alloc) {
        p->alloc    = *alloc;
        p->hasAlloc = 1;
    }
    p->compute = 1;
    p->vs      = cs;
    pvk_shader_retain(cs);
    p->layout  = info->layout;
    if ((res = validate_compute_layout(p, info->layout)) != PVK_SUCCESS ||
        (res = compute_code(p, device->features.robustBufferAccess != 0)) != PVK_SUCCESS ||
        (res = specialize(p, cs, s->specializationInfo, 0)) != PVK_SUCCESS) {
        pvkDestroyPipeline(device, p, alloc);
        return res;
    }
    if (cache)
        program_intern(p, cache);
    *pipeline = p;
    return PVK_SUCCESS;
}

PvkResult pvkCreateComputePipelines(PvkDevice device, PvkPipelineCache pipelineCache, uint32_t createInfoCount,
                                    const PvkComputePipelineCreateInfo *pCreateInfos,
                                    const PvkAllocationCallbacks *pAllocator, PvkPipeline *pPipelines)
{
    PvkResult first = PVK_SUCCESS, res;
    uint32_t i;

    for (i = 0; i < createInfoCount; i++) {
        pPipelines[i] = NULL;
        res = create_compute_pipeline(device, pipelineCache, &pCreateInfos[i], pAllocator, &pPipelines[i]);
        if (res != PVK_SUCCESS && first == PVK_SUCCESS)
            first = res;
    }
    return first;
}

void pvkDestroyPipeline(PvkDevice device, PvkPipeline pipeline, const PvkAllocationCallbacks *pAllocator)
{
    (void)device;
    if (!pipeline)
        return;
    if ((pAllocator != NULL) != (pipeline->hasAlloc != 0)) {
        pvk_error("pvkDestroyPipeline: the pipeline was created %s callbacks but is destroyed %s; it is freed the "
                  "way it was allocated\n", pipeline->hasAlloc ? "with" : "without", pAllocator ? "with" : "without");
        pAllocator = pipeline->hasAlloc ? &pipeline->alloc : NULL;
    }
    program_release(pipeline->program);
    pvk_shader_release(pipeline->vs);
    pvk_shader_release(pipeline->fs);
    pvk_shader_release(pipeline->ts);
    host_free(pAllocator, pipeline);
}

PvkResult pvkCreatePipelineCache(PvkDevice device, const PvkPipelineCacheCreateInfo *info, PvkPipelineCache *cache)
{
    PvkPipelineCache c;

    (void)device;
    if (info && info->flags) {
        pvk_error("pvkCreatePipelineCache: flags must be 0\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (info && info->initialDataSize && !info->initialData) {
        pvk_error("pvkCreatePipelineCache: initialDataSize without initialData\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    c = calloc(1, sizeof(*c));
    if (!c)
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    *cache = c;
    return PVK_SUCCESS;
}

void pvkDestroyPipelineCache(PvkDevice device, PvkPipelineCache cache)
{
    struct PvkProgram *prog, *next;

    (void)device;
    if (!cache)
        return;
    for (prog = cache->programs; prog; prog = next) {
        next = prog->next;
        program_release(prog);
    }
    free(cache);
}
