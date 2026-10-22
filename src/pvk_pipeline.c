/* Graphics pipelines: compiled vertex shader (VU1) + GS state from the fragment shader. */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "pvk_internal.h"

/* Largest 24-bit depth value; depth is flipped so near = large (GS tests are >=). */
#define PVK_GS_DEPTH_MAX 16777215.0f

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

static uint32_t max_batch_vertices(uint32_t inputQwords)
{
    uint32_t byInput  = (PVK_VU1_OUTPUT_ADDR - PVK_VU1_INPUT_ADDR - PVK_VU1_INPUT_HEADER) / inputQwords;
    uint32_t byOutput = (PVK_VU1_MEMORY_QWORDS - PVK_VU1_OUTPUT_ADDR - 1) / PVK_VU1_OUTPUT_PER_VERTEX;
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
    float *ctx;

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
    p->inputQwords  = vs->inputCount;
    p->vertexStride = info->vertexStride;
    p->maxBatch     = max_batch_vertices(p->inputQwords);
    p->prim         = GS_SET_PRIM(PRIM_TRIANGLE, fs->gouraud, fs->textured, 0, 0, 0, 0, 0, 0);

    /* Vulkan viewport transform, in GS primitive coordinates: x and y go
     * from NDC [-1, 1] (y down) to pixels, z from [0, 1] to flipped 24-bit depth. */
    ctx = p->context;
    ctx[PVK_VU1_CTX_VIEWPORT_SCALE * 4 + 0]  = vp->width * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_SCALE * 4 + 1]  = vp->height * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_SCALE * 4 + 2]  = -PVK_GS_DEPTH_MAX * (vp->maxDepth - vp->minDepth);
    ctx[PVK_VU1_CTX_VIEWPORT_OFFSET * 4 + 0] = PVK_GS_ORIGIN + vp->x + vp->width * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_OFFSET * 4 + 1] = PVK_GS_ORIGIN + vp->y + vp->height * 0.5f;
    ctx[PVK_VU1_CTX_VIEWPORT_OFFSET * 4 + 2] = PVK_GS_DEPTH_MAX * (1.0f - vp->minDepth);
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
