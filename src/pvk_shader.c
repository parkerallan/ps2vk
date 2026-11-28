/* Shader modules: a PvkShaderCode built from the bytes of a .pvs file written by tools/shaderc. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "pvk_internal.h"

/* One block: the shader, then the names, clamp pointers, specialization constants and
 * microcode its pointers refer to. Modules, pipelines and programs each hold a reference. */
struct PvkShaderData {
    uint32_t      refs;
    PvkShaderCode code;
};

static struct PvkShaderData *shader_data(const PvkShaderCode *code)
{
    return (struct PvkShaderData *)((char *)code - offsetof(struct PvkShaderData, code));
}

void pvk_shader_retain(const PvkShaderCode *code)
{
    if (code)
        shader_data(code)->refs++;
}

void pvk_shader_release(const PvkShaderCode *code)
{
    if (code && !--shader_data(code)->refs)
        free(shader_data(code));
}

/* Nonzero if [offset, offset + bytes) lies inside the file. */
static int inside(const PvkPvsHeader *h, uint32_t offset, size_t bytes)
{
    return offset <= h->fileBytes && bytes <= h->fileBytes - offset;
}

/* A NUL-terminated string at that offset, or NULL. */
static const char *string_at(const PvkPvsHeader *h, uint32_t offset)
{
    const char *s = (const char *)h + offset;

    return offset < h->fileBytes && memchr(s, 0, h->fileBytes - offset) ? s : NULL;
}

/* What is wrong with the file, or NULL. */
static const char *check_pvs(const PvkPvsHeader *h, size_t codeSize)
{
    uint32_t i;

    if (codeSize < sizeof(*h) || h->magic != PVK_PVS_MAGIC)
        return "the bytes are not a .pvs file";
    if (h->version != PVK_PVS_VERSION)
        return "the .pvs file is of another version; recompile it with this ps2vk's shaderc";
    if (h->fileBytes < sizeof(*h) || h->fileBytes > codeSize || h->fileBytes % 4)
        return "the .pvs file is cut short";
    if (h->stage != PVK_SHADER_STAGE_VERTEX && h->stage != PVK_SHADER_STAGE_FRAGMENT &&
        h->stage != PVK_SHADER_STAGE_COMPUTE_BIT && h->stage != PVK_SHADER_STAGE_TASK_BIT_EXT &&
        h->stage != PVK_SHADER_STAGE_MESH_BIT_EXT)
        return "the .pvs file names no shader stage";
    if (!string_at(h, h->nameOffset) || !string_at(h, h->entryPointOffset))
        return "the .pvs file's names run past its end";
    if (h->stage == PVK_SHADER_STAGE_FRAGMENT)
        return NULL;
    if (!h->codeWords)
        return "the vertex or compute shader has no microcode";
    if (h->codeOffset % 4 || h->clampsOffset % 4 || h->specsOffset % 4 ||
        !inside(h, h->codeOffset, (size_t)h->codeWords * 4) ||
        !inside(h, h->clampsOffset, (size_t)h->clampCount * 4) ||
        !inside(h, h->specsOffset, (size_t)h->specCount * sizeof(PvkPvsSpecConstant)))
        return "the .pvs file's microcode runs past its end";
    if (h->convertWord >= h->codeWords || h->inputCount > PVK_MAX_VERTEX_INPUTS ||
        h->uniformBlockCount > PVK_MAX_UNIFORM_BLOCKS || h->storageBlockCount > PVK_MAX_STORAGE_BLOCKS ||
        (h->stage != PVK_SHADER_STAGE_VERTEX && (h->inputCount ||
         !h->localSize[0] || !h->localSize[1] || !h->localSize[2])))
        return "the .pvs file's header is corrupt";
    if (h->stage == PVK_SHADER_STAGE_MESH_BIT_EXT &&
        (h->meshPrimitiveVertices < 1 || h->meshPrimitiveVertices > 3 || !h->meshMaxVertices || !h->meshMaxPrimitives ||
         h->meshOutputAddress < PVK_VU1_PAYLOAD_ADDR || h->meshOutputAddress >= h->meshKickAddress ||
         h->meshKickAddress >= PVK_VU1_MEMORY_QWORDS))
        return "the .pvs file's header is corrupt";
    for (i = 0; i < h->clampCount; i++)
        if (((const uint32_t *)((const char *)h + h->clampsOffset))[i] >= h->codeWords)
            return "the .pvs file's header is corrupt";
    for (i = 0; i < h->specCount; i++)
        if (((const PvkPvsSpecConstant *)((const char *)h + h->specsOffset))[i].word >= h->codeWords)
            return "the .pvs file's header is corrupt";
    return NULL;
}

PvkResult pvkCreateShaderModule(PvkDevice device, const PvkShaderModuleCreateInfo *info, PvkShaderModule *module)
{
    const PvkPvsHeader *h;
    const PvkPvsSpecConstant *fileSpecs;
    const uint32_t *fileClamps;
    const char *name, *entry, *problem;
    struct PvkShaderData *data;
    PvkShaderSpecConstant *specs;
    const uint32_t **clamps;
    uint32_t *code, i;
    char *strings;
    size_t nameLen, entryLen, stringBytes, total;
    PvkShaderCode sc;
    PvkShaderModule mod;

    (void)device;

    if (!info || info->sType != PVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
        pvk_error("pvkCreateShaderModule: sType is not PVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (info->flags) {
        pvk_error("pvkCreateShaderModule: flags must be 0\n");
        return PVK_ERROR_VALIDATION_FAILED;
    }
    if (!info->pCode) {
        pvk_error("pvkCreateShaderModule: no shader code\n");
        return PVK_ERROR_INVALID_SHADER;
    }
    h = (const PvkPvsHeader *)info->pCode;
    problem = check_pvs(h, info->codeSize);
    if (problem) {
        pvk_error("pvkCreateShaderModule: %s\n", problem);
        return PVK_ERROR_INVALID_SHADER;
    }
    name        = string_at(h, h->nameOffset);
    entry       = string_at(h, h->entryPointOffset);
    nameLen     = strlen(name) + 1;
    entryLen    = strlen(entry) + 1;
    stringBytes = (nameLen + entryLen + 7) & ~(size_t)7;
    fileClamps  = (const uint32_t *)((const char *)h + h->clampsOffset);
    fileSpecs   = (const PvkPvsSpecConstant *)((const char *)h + h->specsOffset);

    total = sizeof(*data) + stringBytes + h->clampCount * sizeof(*clamps) + h->specCount * sizeof(*specs) +
            (size_t)h->codeWords * 4;
    data = calloc(1, total);
    mod  = calloc(1, sizeof(struct PvkShaderModule_T));
    if (!data || !mod) {
        free(data);
        free(mod);
        return PVK_ERROR_OUT_OF_HOST_MEMORY;
    }
    strings = (char *)(data + 1);
    clamps  = (const uint32_t **)(strings + stringBytes);
    specs   = (PvkShaderSpecConstant *)(clamps + h->clampCount);
    code    = (uint32_t *)(specs + h->specCount);
    memcpy(strings, name, nameLen);
    memcpy(strings + nameLen, entry, entryLen);

    memset(&sc, 0, sizeof(sc));
    sc.stage      = (PvkShaderStage)h->stage;
    sc.name       = strings;
    sc.entryPoint = strings + nameLen;
    if (h->stage != PVK_SHADER_STAGE_FRAGMENT) {
        memcpy(code, (const char *)h + h->codeOffset, (size_t)h->codeWords * 4);
        for (i = 0; i < h->clampCount; i++)
            clamps[i] = code + fileClamps[i];
        for (i = 0; i < h->specCount; i++) {
            specs[i].constantID = fileSpecs[i].constantID;
            specs[i].vu1Word    = code + fileSpecs[i].word;
            specs[i].type       = (PvkSpecConstantType)fileSpecs[i].type;
        }
        sc.vu1CodeStart         = code;
        sc.vu1CodeEnd           = code + h->codeWords;
        sc.vu1Convert           = h->stage == PVK_SHADER_STAGE_VERTEX ? code + h->convertWord : NULL;
        sc.vu1ClampCount        = h->clampCount;
        sc.vu1Clamps            = clamps;
        sc.vu1SpecConstantCount = h->specCount;
        sc.vu1SpecConstants     = specs;
    }
    sc.inputCount = h->inputCount;
    memcpy(sc.inputLocations, h->inputLocations, sizeof(sc.inputLocations));
    sc.colorOutputLocation    = h->colorOutputLocation;
    sc.texcoordOutputLocation = h->texcoordOutputLocation;
    sc.fogOutputLocation      = h->fogOutputLocation;
    sc.pushConstantBytes      = h->pushConstantBytes;
    sc.uniformBlockCount      = h->uniformBlockCount;
    memcpy(sc.uniformBlocks, h->uniformBlocks, sizeof(sc.uniformBlocks));
    sc.colorInputLocation     = h->colorInputLocation;
    sc.texcoordInputLocation  = h->texcoordInputLocation;
    sc.gouraud                = h->gouraud;
    sc.textured               = h->textured;
    sc.textureFunction        = h->textureFunction;
    sc.samplerBinding         = h->samplerBinding;
    sc.samplerSet             = h->samplerSet;
    sc.alphaTest              = h->alphaTest;
    sc.alphaTestFunction      = h->alphaTestFunction;
    sc.alphaTestReference     = h->alphaTestReference;
    sc.fogInputLocation       = h->fogInputLocation;
    sc.fog                    = h->fog;
    sc.fogColor               = h->fogColor;
    memcpy(sc.localSize, h->localSize, sizeof(sc.localSize));
    sc.storageBlockCount      = h->storageBlockCount;
    memcpy(sc.storageBlocks, h->storageBlocks, sizeof(sc.storageBlocks));
    sc.meshMaxVertices        = h->meshMaxVertices;
    sc.meshMaxPrimitives      = h->meshMaxPrimitives;
    sc.meshPrimitiveVertices  = h->meshPrimitiveVertices;
    sc.meshOutputAddress      = h->meshOutputAddress;
    sc.meshKickAddress        = h->meshKickAddress;
    sc.payloadBytes           = h->payloadBytes;
    sc.sharedBytes            = h->sharedBytes;

    data->refs = 1;
    data->code = sc;
    mod->code  = &data->code;
    *module = mod;
    return PVK_SUCCESS;
}

void pvkDestroyShaderModule(PvkDevice device, PvkShaderModule module)
{
    (void)device;
    if (!module)
        return;
    pvk_shader_release(module->code);
    free(module);
}
