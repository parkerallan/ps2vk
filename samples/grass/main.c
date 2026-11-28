/*
 * ps2vk grass sample: a field of grass waving in the wind. The application
 * keeps one vec4 per blade in a storage buffer and draws no vertices of its
 * own for the grass: a mesh shader on VU1 turns each vec4 into a bent, tapered
 * blade of 7 vertices and 5 triangles, with the wind as a push constant.
 * VU1 data memory holds 768 blades next to the shader's outputs, so the field
 * is drawn in 5 mesh draws, each with a descriptor set over its part of the
 * buffer. The sky and the ground behind the grass are a vertex pipeline.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2vk.h"

#define WIDTH  640
#define HEIGHT 448

#define DRAWS           5
#define BLADES_PER_DRAW 768
#define BLADES          (DRAWS * BLADES_PER_DRAW)

#define CHECK(call)                                                       \
    do {                                                                  \
        PvkResult res_ = (call);                                          \
        if (res_ != PVK_SUCCESS) {                                        \
            printf("grass: %s failed (%d)\n", #call, (int)res_);          \
            return 1;                                                     \
        }                                                                 \
    } while (0)

/* Reads a .pvs file compiled by ps2vk-shaderc and creates a shader module from its bytes. */
static PvkResult load_shader(PvkDevice device, const char *path, PvkShaderModule *module)
{
    FILE *f = fopen(path, "rb");
    uint32_t *code = NULL;
    long size = 0;
    PvkResult res;

    if (f) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        code = size > 0 ? malloc((size_t)size) : NULL;
        if (code && fread(code, 1, (size_t)size, f) != (size_t)size) {
            free(code);
            code = NULL;
        }
        fclose(f);
    }
    if (!code) {
        printf("cannot read %s\n", path);
        return PVK_ERROR_INVALID_SHADER;
    }

    PvkShaderModuleCreateInfo info = {
        .sType    = PVK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size,
        .pCode    = code,
    };
    res = pvkCreateShaderModule(device, &info, module);
    free(code); /* the module keeps its own copy */
    return res;
}

/* The first memory type allowed by typeBits that has every flag in flags. */
static uint32_t memory_type(PvkPhysicalDevice physicalDevice, uint32_t typeBits, uint32_t flags)
{
    PvkPhysicalDeviceMemoryProperties memory;
    pvkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return 0;
}

static void mat_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_mul(float *out, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a[k * 4 + rw] * b[c * 4 + k];
            r[c * 4 + rw] = s;
        }
    memcpy(out, r, sizeof(r));
}

static void mat_rotate_x(float *m, float angle)
{
    mat_identity(m);
    m[5] = cosf(angle); m[9]  = -sinf(angle);
    m[6] = sinf(angle); m[10] = cosf(angle);
}

static void mat_translate(float *m, float x, float y, float z)
{
    mat_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

/* Vulkan clip space: y points down, depth 0 (near) to 1 (far). */
static void mat_perspective(float *m, float fovY, float aspect, float near, float far)
{
    float f = 1.0f / tanf(fovY * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0]  = f / aspect;
    m[5]  = -f;
    m[10] = far / (near - far);
    m[11] = -1.0f;
    m[14] = near * far / (near - far);
}

/* A number from 0 to 1, the same sequence every run. */
static float random01(void)
{
    static uint32_t seed = 12345;
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8) / 16777216.0f;
}

/* The camera stands in the field and looks slightly down the -z axis. */
#define EYE_HEIGHT 1.5f
#define EYE_Z      4.0f
#define PITCH      0.213f
#define FOV_Y      (50.0f * 3.14159265f / 180.0f)
#define ASPECT     (4.0f / 3.0f)
#define NEAREST    2.0f  /* distance from the camera to the first blades and to the last */
#define FARTHEST   45.0f

typedef struct Push {
    float viewProj[16];
    float wind[4]; /* time, gust strength, direction x, direction z */
} Push;

typedef struct Vertex {
    float pos[4];
    float color[4];
} Vertex;

/* Two clip-space quads as 12 vertices: the sky down to the horizon, the ground below it. */
static void build_backdrop(Vertex *v, float horizon)
{
    static const float skyTop[4] = { 0.20f, 0.42f, 0.80f, 1.0f }, skyLow[4] = { 0.74f, 0.85f, 0.92f, 1.0f };
    static const float far[4] = { 0.30f, 0.46f, 0.24f, 1.0f }, near[4] = { 0.05f, 0.17f, 0.05f, 1.0f };
    const float *colors[2][2] = { { skyTop, skyLow }, { far, near } };
    float y[2][2] = { { -1.0f, horizon }, { horizon, 1.0f } };
    static const int corner[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };

    for (int q = 0; q < 2; q++)
        for (int i = 0; i < 6; i++, v++) {
            v->pos[0] = corner[i][0] ? 1.0f : -1.0f;
            v->pos[1] = y[q][corner[i][1]];
            v->pos[2] = 0.999f;
            v->pos[3] = 1.0f;
            memcpy(v->color, colors[q][corner[i][1]], sizeof(v->color));
        }
}

int main(void)
{
    PvkInstance instance;
    PvkPhysicalDevice physicalDevice;
    uint32_t physicalDeviceCount = 1;
    PvkDevice device;
    PvkQueue queue;
    PvkSwapchain swapchain;
    PvkImage images[2], depth;
    PvkImageView views[2], depthView;
    uint32_t imageCount = 2;
    PvkBuffer bladeBuffer, backdropBuffer;
    PvkDeviceMemory bladeMemory, backdropMemory, depthMemory;
    PvkShaderModule grassMs, colorFs, backdropVs;
    PvkDescriptorSetLayout setLayout;
    PvkDescriptorPool descriptorPool;
    PvkDescriptorSet sets[DRAWS];
    PvkPipelineLayout grassLayout, backdropLayout;
    PvkPipeline grassPipeline, backdropPipeline;
    PvkCommandPool commandPool;
    PvkCommandBuffer cmd;
    PvkFence fence;
    PvkSemaphore imageAvailable, renderDone;
    Push push;
    float proj[16], view[16], tilt[16], move[16];
    float *blades;
    void *mapped;

    PvkInstanceCreateInfo instanceInfo = { .applicationName = "grass" };
    CHECK(pvkCreateInstance(&instanceInfo, &instance));
    CHECK(pvkEnumeratePhysicalDevices(instance, &physicalDeviceCount, &physicalDevice));

    /* Mesh shaders are a feature: ask the device whether it has them, then turn them on. */
    PvkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures = { .sType = PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    PvkPhysicalDeviceFeatures2 features = { .sType = PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &meshFeatures };
    pvkGetPhysicalDeviceFeatures2(physicalDevice, &features);
    if (!meshFeatures.meshShader) {
        printf("grass: the device has no mesh shaders\n");
        return 1;
    }
    PvkPhysicalDeviceMeshShaderFeaturesEXT enabled = {
        .sType = PVK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, .meshShader = 1,
    };
    PvkDeviceQueueCreateInfo queueInfo = { .queueFamilyIndex = 0, .queueCount = 1 };
    PvkDeviceCreateInfo deviceInfo = {
        .sType = PVK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enabled,
        .queueCreateInfoCount = 1, .queueCreateInfos = &queueInfo,
    };
    CHECK(pvkCreateDevice(physicalDevice, &deviceInfo, &device));
    pvkGetDeviceQueue(device, 0, 0, &queue);

    PvkSwapchainCreateInfo swapchainInfo = {
        .width = WIDTH, .height = HEIGHT, .format = PVK_FORMAT_CT32, .imageCount = 2,
        .imageUsage = PVK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .presentMode = PVK_PRESENT_MODE_FIFO,
        .displayMode = PVK_DISPLAY_MODE_NTSC,
    };
    CHECK(pvkCreateSwapchain(device, &swapchainInfo, &swapchain));
    CHECK(pvkGetSwapchainImages(device, swapchain, &imageCount, images));
    for (uint32_t i = 0; i < imageCount; i++) {
        PvkImageViewCreateInfo viewInfo = { .image = images[i], .levelCount = 1 };
        CHECK(pvkCreateImageView(device, &viewInfo, &views[i]));
    }

    PvkImageCreateInfo depthInfo = { .width = WIDTH, .height = HEIGHT, .format = PVK_FORMAT_Z24 };
    CHECK(pvkCreateImage(device, &depthInfo, &depth));
    PvkMemoryRequirements depthReq;
    pvkGetImageMemoryRequirements(device, depth, &depthReq);
    PvkMemoryAllocateInfo depthMemoryInfo = {
        .allocationSize  = depthReq.size,
        .memoryTypeIndex = memory_type(physicalDevice, depthReq.memoryTypeBits, PVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    CHECK(pvkAllocateMemory(device, &depthMemoryInfo, &depthMemory));
    CHECK(pvkBindImageMemory(device, depth, depthMemory, 0));
    PvkImageViewCreateInfo depthViewInfo = { .image = depth, .levelCount = 1 };
    CHECK(pvkCreateImageView(device, &depthViewInfo, &depthView));

    /* The camera, and where it sees the horizon: the direction (0, 0, -1) in clip space. */
    mat_perspective(proj, FOV_Y, ASPECT, 0.5f, 60.0f);
    mat_rotate_x(tilt, PITCH);
    mat_translate(move, 0.0f, -EYE_HEIGHT, -EYE_Z);
    mat_mul(view, tilt, move);
    mat_mul(push.viewProj, proj, view);
    float horizon = -tanf(PITCH) / tanf(FOV_Y * 0.5f);

    /* The field: one vec4 per blade, the only grass data the application ever writes. Blades
     * are spread evenly over the picture, so more of them stand near the camera than far away. */
    PvkBufferCreateInfo bladeInfo = { .size = BLADES * 16 };
    CHECK(pvkCreateBuffer(device, &bladeInfo, &bladeBuffer));
    PvkMemoryRequirements bladeReq;
    pvkGetBufferMemoryRequirements(device, bladeBuffer, &bladeReq);
    PvkMemoryAllocateInfo bladeMemoryInfo = {
        .allocationSize  = bladeReq.size,
        .memoryTypeIndex = memory_type(physicalDevice, bladeReq.memoryTypeBits, PVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
    };
    CHECK(pvkAllocateMemory(device, &bladeMemoryInfo, &bladeMemory));
    CHECK(pvkBindBufferMemory(device, bladeBuffer, bladeMemory, 0));
    CHECK(pvkMapMemory(device, bladeMemory, 0, BLADES * 16, &mapped));
    blades = mapped;
    for (int i = 0; i < BLADES; i++) {
        float distance = NEAREST * powf(FARTHEST / NEAREST, random01());
        float halfWidth = distance * tanf(FOV_Y * 0.5f) * ASPECT * 1.1f;

        blades[i * 4 + 0] = (random01() * 2.0f - 1.0f) * halfWidth; /* x */
        blades[i * 4 + 1] = EYE_Z - distance;                       /* z */
        blades[i * 4 + 2] = 0.35f + 0.3f * random01();              /* height */
        blades[i * 4 + 3] = random01();
    }
    pvkUnmapMemory(device, bladeMemory);

    static Vertex backdrop[12];
    build_backdrop(backdrop, horizon);
    PvkBufferCreateInfo backdropInfo = { .size = sizeof(backdrop) };
    CHECK(pvkCreateBuffer(device, &backdropInfo, &backdropBuffer));
    PvkMemoryRequirements backdropReq;
    pvkGetBufferMemoryRequirements(device, backdropBuffer, &backdropReq);
    PvkMemoryAllocateInfo backdropMemoryInfo = {
        .allocationSize  = backdropReq.size,
        .memoryTypeIndex = memory_type(physicalDevice, backdropReq.memoryTypeBits, PVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
    };
    CHECK(pvkAllocateMemory(device, &backdropMemoryInfo, &backdropMemory));
    CHECK(pvkBindBufferMemory(device, backdropBuffer, backdropMemory, 0));
    CHECK(pvkMapMemory(device, backdropMemory, 0, sizeof(backdrop), &mapped));
    memcpy(mapped, backdrop, sizeof(backdrop));
    pvkUnmapMemory(device, backdropMemory);

    /* Set 0, binding 0: the blades one mesh draw covers. A mesh shader has no vertex input, so
     * a storage buffer is how the blades reach it; each of the 5 sets names its own range. */
    PvkDescriptorSetLayoutBinding binding = {
        .binding = 0, .descriptorType = PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = PVK_SHADER_STAGE_MESH_BIT_EXT,
    };
    PvkDescriptorSetLayoutCreateInfo setLayoutInfo = { .bindingCount = 1, .bindings = &binding };
    CHECK(pvkCreateDescriptorSetLayout(device, &setLayoutInfo, &setLayout));
    PvkDescriptorPoolSize poolSize = { .type = PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = DRAWS };
    PvkDescriptorPoolCreateInfo poolInfo = { .maxSets = DRAWS, .poolSizeCount = 1, .poolSizes = &poolSize };
    CHECK(pvkCreateDescriptorPool(device, &poolInfo, &descriptorPool));
    for (int d = 0; d < DRAWS; d++) {
        PvkDescriptorSetAllocateInfo setInfo = {
            .descriptorPool = descriptorPool, .descriptorSetCount = 1, .setLayouts = &setLayout,
        };
        CHECK(pvkAllocateDescriptorSets(device, &setInfo, &sets[d]));
        PvkDescriptorBufferInfo range = {
            .buffer = bladeBuffer, .offset = (size_t)d * BLADES_PER_DRAW * 16, .range = BLADES_PER_DRAW * 16,
        };
        PvkWriteDescriptorSet write = {
            .dstSet = sets[d], .dstBinding = 0, .descriptorType = PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .bufferInfo = &range,
        };
        pvkUpdateDescriptorSets(device, 1, &write);
    }

    /* The mesh shader's push constant block: the camera matrix and the wind. */
    PvkPushConstantRange pushRange = { .stageFlags = PVK_SHADER_STAGE_MESH_BIT_EXT, .offset = 0, .size = sizeof(Push) };
    PvkPipelineLayoutCreateInfo grassLayoutInfo = {
        .setLayoutCount = 1, .setLayouts = &setLayout,
        .pushConstantRangeCount = 1, .pushConstantRanges = &pushRange,
    };
    CHECK(pvkCreatePipelineLayout(device, &grassLayoutInfo, &grassLayout));
    PvkPipelineLayoutCreateInfo backdropLayoutInfo = { .setLayoutCount = 0 };
    CHECK(pvkCreatePipelineLayout(device, &backdropLayoutInfo, &backdropLayout));

    CHECK(load_shader(device, "host:build/grass_mesh.pvs", &grassMs));
    CHECK(load_shader(device, "host:build/grass_frag.pvs", &colorFs));
    CHECK(load_shader(device, "host:build/backdrop_vert.pvs", &backdropVs));

    /* The grass pipeline: a mesh stage where a vertex stage would be, so it has no vertex
     * bindings, attributes or topology; the mesh shader says it outputs triangles. */
    PvkPipelineShaderStageCreateInfo grassStages[] = {
        { .stage = PVK_SHADER_STAGE_MESH_BIT_EXT, .module = grassMs, .name = "main" },
        { .stage = PVK_SHADER_STAGE_FRAGMENT, .module = colorFs, .name = "main" },
    };
    PvkGraphicsPipelineCreateInfo grassInfo = {
        .stageCount     = 2,
        .stages         = grassStages,
        .layout         = grassLayout,
        .viewport       = { 0.0f, 0.0f, WIDTH, HEIGHT, 0.0f, 1.0f },
        .colorBlend     = { .attachment = { .colorWriteMask = PVK_COLOR_COMPONENT_R_BIT | PVK_COLOR_COMPONENT_G_BIT
                                                             | PVK_COLOR_COMPONENT_B_BIT | PVK_COLOR_COMPONENT_A_BIT } },
        .depthAttachmentFormat = PVK_FORMAT_Z24,
        .rasterization  = { .sType = PVK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO },
        .depthStencil   = { .depthTestEnable = 1, .depthWriteEnable = 1, .depthCompareOp = PVK_COMPARE_OP_LESS },
    };
    CHECK(pvkCreateGraphicsPipeline(device, NULL, &grassInfo, &grassPipeline));

    /* The backdrop pipeline: a vertex stage, drawn first with no depth test. */
    PvkVertexInputBinding vertexBinding = { .binding = 0, .stride = sizeof(Vertex) };
    size_t noOffset = 0;
    PvkVertexInputAttribute attributes[] = {
        { .location = 0, .format = PVK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
        { .location = 1, .format = PVK_FORMAT_R32G32B32A32_SFLOAT, .offset = 16 },
    };
    PvkPipelineShaderStageCreateInfo backdropStages[] = {
        { .stage = PVK_SHADER_STAGE_VERTEX,   .module = backdropVs, .name = "main" },
        { .stage = PVK_SHADER_STAGE_FRAGMENT, .module = colorFs,    .name = "main" },
    };
    PvkGraphicsPipelineCreateInfo backdropPipelineInfo = {
        .stageCount     = 2,
        .stages         = backdropStages,
        .layout         = backdropLayout,
        .bindingCount   = 1,
        .bindings       = &vertexBinding,
        .attributeCount = 2,
        .attributes     = attributes,
        .topology       = PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .viewport       = { 0.0f, 0.0f, WIDTH, HEIGHT, 0.0f, 1.0f },
        .colorBlend     = { .attachment = { .colorWriteMask = PVK_COLOR_COMPONENT_R_BIT | PVK_COLOR_COMPONENT_G_BIT
                                                             | PVK_COLOR_COMPONENT_B_BIT | PVK_COLOR_COMPONENT_A_BIT } },
        .depthAttachmentFormat = PVK_FORMAT_Z24,
        .rasterization  = { .sType = PVK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO },
    };
    CHECK(pvkCreateGraphicsPipeline(device, NULL, &backdropPipelineInfo, &backdropPipeline));

    PvkCommandPoolCreateInfo commandPoolInfo = { .flags = 0 };
    CHECK(pvkCreateCommandPool(device, &commandPoolInfo, &commandPool));
    PvkCommandBufferAllocateInfo cmdInfo = {
        .commandPool = commandPool, .level = PVK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    CHECK(pvkAllocateCommandBuffers(device, &cmdInfo, &cmd));
    CHECK(pvkCreateFence(device, NULL, &fence));
    CHECK(pvkCreateSemaphore(device, NULL, &imageAvailable));
    CHECK(pvkCreateSemaphore(device, NULL, &renderDone));
    PvkCommandBufferBeginInfo beginInfo = { .flags = 0 };

    printf("grass: running, %d blades in %d mesh draws\n", BLADES, DRAWS);

    for (unsigned frame = 0;; frame++) {
        uint32_t imageIndex;

        /* The wind blows across the field and a little away from the camera. */
        push.wind[0] = (float)frame * 0.07f;
        push.wind[1] = 0.45f;
        push.wind[2] = 0.9f;
        push.wind[3] = -0.35f;

        /* The command buffer is re-recorded below, so the previous frame must be done with it. */
        CHECK(pvkWaitForFences(device, 1, &fence));
        CHECK(pvkResetFences(device, 1, &fence));
        CHECK(pvkAcquireNextImage(device, swapchain, UINT64_MAX, imageAvailable, NULL, &imageIndex));

        CHECK(pvkResetCommandPool(device, commandPool, 0));
        CHECK(pvkBeginCommandBuffer(cmd, &beginInfo));
        PvkRenderingInfo rendering = {
            .renderArea = { 0, 0, WIDTH, HEIGHT },
            .colorAttachment = {
                .imageView  = views[imageIndex],
                .loadOp     = PVK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp    = PVK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } },
            },
            .depthAttachment = {
                .imageView  = depthView,
                .loadOp     = PVK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp    = PVK_ATTACHMENT_STORE_OP_DONT_CARE,
                .clearValue = { .depthStencil = { .depth = 1.0f } },
            },
        };
        pvkCmdBeginRendering(cmd, &rendering);

        pvkCmdBindPipeline(cmd, PVK_PIPELINE_BIND_POINT_GRAPHICS, backdropPipeline);
        pvkCmdBindVertexBuffers(cmd, 0, 1, &backdropBuffer, &noOffset);
        pvkCmdDraw(cmd, 12, 1, 0, 0);

        /* The grass: no vertex buffer and no vertex count. Each draw runs one mesh work group
         * per blade of the set bound for it, and the work group's number picks the blade. */
        pvkCmdBindPipeline(cmd, PVK_PIPELINE_BIND_POINT_GRAPHICS, grassPipeline);
        pvkCmdPushConstants(cmd, grassLayout, PVK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(push), &push);
        for (int d = 0; d < DRAWS; d++) {
            pvkCmdBindDescriptorSets(cmd, PVK_PIPELINE_BIND_POINT_GRAPHICS, grassLayout, 0, 1, &sets[d], 0, NULL);
            pvkCmdDrawMeshTasksEXT(cmd, BLADES_PER_DRAW, 1, 1);
        }

        pvkCmdEndRendering(cmd);
        CHECK(pvkEndCommandBuffer(cmd));

        /* The draw waits for the image and signals renderDone; the present waits for renderDone. */
        PvkPipelineStageFlags waitStage = PVK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        PvkSubmitInfo submit = {
            .waitSemaphoreCount   = 1, .waitSemaphores   = &imageAvailable, .waitDstStageMask = &waitStage,
            .commandBufferCount   = 1, .commandBuffers   = &cmd,
            .signalSemaphoreCount = 1, .signalSemaphores = &renderDone,
        };
        CHECK(pvkQueueSubmit(queue, 1, &submit, fence));
        PvkPresentInfo present = {
            .waitSemaphoreCount = 1, .waitSemaphores = &renderDone,
            .swapchainCount     = 1, .swapchains     = &swapchain, .imageIndices = &imageIndex,
        };
        CHECK(pvkQueuePresent(queue, &present));
    }

    return 0;
}
