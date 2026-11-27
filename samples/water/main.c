/*
 * ps2vk water sample: ripples on a 160x78 water surface whose edges lie out of
 * view. Every frame a compute shader on VU1 steps the wave equation over the
 * grid's heights and works out each cell's light and slope, one dispatch per
 * band of 3 rows; the graphics
 * pipeline then draws the grid as a depth-tested triangle mesh whose vertices
 * read those values straight from a storage buffer and reflect the sky and the
 * moon, bent by the slope. The heights live in two buffers that swap roles each
 * frame through two sets of descriptor sets. Raindrops land at random places
 * as a push constant the compute shader adds to the surface.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2vk.h"

#define WIDTH  640
#define HEIGHT 448

#define COLS    160                        /* cells across */
#define ROWS    78                         /* cells from far to near */
#define CELLS   (COLS * ROWS)
#define CELL    (2.4f / 63.0f)             /* a cell's size */
#define BAND    3                          /* rows one dispatch steps: its storage fits VU1 memory */
#define BANDS   (ROWS / BAND)
#define MAX_INDICES ((COLS - 1) * (ROWS - 1) * 6)

#define SKY_SIZE 64                        /* the sky the water reflects is SKY_SIZE x SKY_SIZE */
#define MOON_U   0.5f                      /* the moon's centre in the sky texture */
#define MOON_V   0.8f
#define MOON_R   0.06f                     /* and its radius there */
#define MOON_SEGMENTS 16                   /* triangles of the moon drawn in the sky */

#define CHECK(call)                                                       \
    do {                                                                  \
        PvkResult res_ = (call);                                          \
        if (res_ != PVK_SUCCESS) {                                        \
            printf("water: %s failed (%d)\n", #call, (int)res_);          \
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

/* A buffer in EE RAM of its own, mapped so the EE can fill it. */
static PvkResult make_buffer(PvkPhysicalDevice physicalDevice, PvkDevice device, size_t size, PvkBuffer *buffer,
                             void **mapped)
{
    PvkBufferCreateInfo info = { .size = size };
    PvkMemoryRequirements req;
    PvkDeviceMemory memory;
    PvkResult res;

    if ((res = pvkCreateBuffer(device, &info, buffer)) != PVK_SUCCESS)
        return res;
    pvkGetBufferMemoryRequirements(device, *buffer, &req);
    PvkMemoryAllocateInfo alloc = {
        .allocationSize  = req.size,
        .memoryTypeIndex = memory_type(physicalDevice, req.memoryTypeBits, PVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
    };
    if ((res = pvkAllocateMemory(device, &alloc, &memory)) != PVK_SUCCESS ||
        (res = pvkBindBufferMemory(device, *buffer, memory, 0)) != PVK_SUCCESS)
        return res;
    return pvkMapMemory(device, memory, 0, size, mapped);
}

/* Column-major 4x4 matrices: m[col * 4 + row]. */
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

/* The sky the water reflects, as CT32 texels (red in the low byte, full alpha 0x80): dark
 * overhead (v = 0) as the background, lighter towards the horizon (v = 1), with the moon. */
static void build_sky(uint32_t *texels)
{
    for (int y = 0; y < SKY_SIZE; y++)
        for (int x = 0; x < SKY_SIZE; x++) {
            float u = (float)x / (SKY_SIZE - 1), v = (float)y / (SKY_SIZE - 1), fade = v * v;
            float r = 0.04f + 0.36f * fade, g = 0.06f + 0.44f * fade, b = 0.12f + 0.58f * fade;
            float d = sqrtf((u - MOON_U) * (u - MOON_U) + (v - MOON_V) * (v - MOON_V)) / MOON_R;
            float moon = d < 0.8f ? 1.0f : d < 1.0f ? (1.0f - d) * 5.0f : 0.0f; /* soft edge */
            r += (0.95f - r) * moon; g += (0.93f - g) * moon; b += (0.85f - b) * moon;
            texels[y * SKY_SIZE + x] = 0x80000000u | (uint32_t)(b * 255.0f) << 16 | (uint32_t)(g * 255.0f) << 8 |
                                       (uint32_t)(r * 255.0f);
        }
}

/* The push constants: the compute shader's drop, then the vertex shader's matrix. */
typedef struct {
    float drop[4]; /* x, y: the cell; z: strength, 0 for no drop; w: 1 / radius^2 */
    float mvp[16];
} Push;

int main(void)
{
    PvkInstance instance;
    PvkPhysicalDevice physicalDevice;
    uint32_t physicalDeviceCount = 1, imageCount = 2;
    PvkDevice device;
    PvkQueue queue;
    PvkSwapchain swapchain;
    PvkImage images[2], depth, sky;
    PvkImageView views[2], depthView, skyView;
    PvkDeviceMemory depthMemory, skyMemory;
    PvkSampler skySampler;
    PvkBuffer heights[2], surface, grid, indexBuffer, skyTexels, moonGrid, moonSurface;
    uint32_t *skyData;
    float *mapped[2], *surfaceData, *gridData, *moonGridData, *moonSurfaceData, moonModel[16], moonMvp[16];
    uint16_t *indexData;
    uint32_t indexCount = 0;
    PvkShaderModule cs, vs, fs;
    PvkDescriptorSetLayout setLayout, skyLayout;
    PvkPipelineLayout layout;
    PvkPipeline computePipeline, drawPipeline;
    PvkDescriptorPool pool;
    PvkDescriptorSet sets[2][BANDS], skySet;
    PvkCommandPool commandPool;
    PvkCommandBuffer cmd;
    PvkFence fence;
    PvkSemaphore imageAvailable, renderDone;
    float proj[16];
    Push push;

    PvkInstanceCreateInfo instanceInfo = { .applicationName = "water" };
    CHECK(pvkCreateInstance(&instanceInfo, &instance));
    CHECK(pvkEnumeratePhysicalDevices(instance, &physicalDeviceCount, &physicalDevice));
    /* Queue family 0 has PVK_QUEUE_GRAPHICS_BIT and PVK_QUEUE_COMPUTE_BIT: dispatches and draws share it. */
    PvkDeviceQueueCreateInfo queueInfo = { .queueFamilyIndex = 0, .queueCount = 1 };
    PvkDeviceCreateInfo deviceInfo = { .sType = PVK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .queueCreateInfos = &queueInfo };
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

    /* The sky the water reflects; its texels are built in a buffer and uploaded before the first frame. */
    PvkImageCreateInfo skyInfo = { .width = SKY_SIZE, .height = SKY_SIZE, .format = PVK_FORMAT_CT32 };
    CHECK(pvkCreateImage(device, &skyInfo, &sky));
    PvkMemoryRequirements skyReq;
    pvkGetImageMemoryRequirements(device, sky, &skyReq);
    PvkMemoryAllocateInfo skyMemoryInfo = {
        .allocationSize  = skyReq.size,
        .memoryTypeIndex = memory_type(physicalDevice, skyReq.memoryTypeBits, PVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    CHECK(pvkAllocateMemory(device, &skyMemoryInfo, &skyMemory));
    CHECK(pvkBindImageMemory(device, sky, skyMemory, 0));
    PvkImageViewCreateInfo skyViewInfo = { .image = sky, .levelCount = 1 };
    CHECK(pvkCreateImageView(device, &skyViewInfo, &skyView));
    /* Clamp to edge: a wave moves a texel coordinate past the edge, which reads the edge texel. */
    PvkSamplerCreateInfo skySamplerInfo = {
        .magFilter = PVK_FILTER_LINEAR, .minFilter = PVK_FILTER_LINEAR,
        .addressModeU = PVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = PVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    CHECK(pvkCreateSampler(device, &skySamplerInfo, &skySampler));
    CHECK(make_buffer(physicalDevice, device, SKY_SIZE * SKY_SIZE * 4, &skyTexels, (void **)&skyData));
    build_sky(skyData);

    /* Two height buffers (still water: all 0) with a padding row above and below the grid, the
     * surface buffer (height, light and two slopes per cell; still water: 0, 0.35, 0, 0), the grid's
     * x and z per vertex, and the mesh's indices. */
    for (int b = 0; b < 2; b++) {
        CHECK(make_buffer(physicalDevice, device, (ROWS + 2) * COLS * sizeof(float), &heights[b], (void **)&mapped[b]));
        memset(mapped[b], 0, (ROWS + 2) * COLS * sizeof(float));
    }
    CHECK(make_buffer(physicalDevice, device, CELLS * 4 * sizeof(float), &surface, (void **)&surfaceData));
    memset(surfaceData, 0, CELLS * 4 * sizeof(float));
    for (int i = 0; i < CELLS; i++)
        surfaceData[i * 4 + 1] = 0.35f;
    /* Per vertex: x and z (the last row at z = 1.2, under the camera's view), then the sky texel the
     * still surface reflects there: further away (smaller z), the nearer the horizon (v = 1). */
    CHECK(make_buffer(physicalDevice, device, CELLS * 4 * sizeof(float), &grid, (void **)&gridData));
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            float *g = &gridData[(y * COLS + x) * 4];
            g[0] = (x - (COLS - 1) * 0.5f) * CELL;
            g[1] = 1.2f - (ROWS - 1 - y) * CELL;
            g[2] = 0.5f + g[0] / 3.0f;
            g[3] = 0.5f - g[1] / 3.0f;
        }
    CHECK(make_buffer(physicalDevice, device, MAX_INDICES * sizeof(uint16_t), &indexBuffer, (void **)&indexData));

    /* The moon in the sky: a disc of triangles on the water's grid inputs, every vertex showing the
     * moon's centre texel, at height 0, light 0.5 (the texel unchanged) and no slope. */
    CHECK(make_buffer(physicalDevice, device, MOON_SEGMENTS * 3 * 4 * sizeof(float), &moonGrid, (void **)&moonGridData));
    CHECK(make_buffer(physicalDevice, device, MOON_SEGMENTS * 3 * 4 * sizeof(float), &moonSurface, (void **)&moonSurfaceData));
    memset(moonSurfaceData, 0, MOON_SEGMENTS * 3 * 4 * sizeof(float));
    for (int s = 0; s < MOON_SEGMENTS; s++)
        for (int k = 0; k < 3; k++) {
            float a = (float)(s + (k == 2)) * 6.2831853f / MOON_SEGMENTS, radius = k ? 0.28f : 0.0f;
            float *g = &moonGridData[(s * 3 + k) * 4];
            g[0] = radius * cosf(a);
            g[1] = radius * sinf(a);
            g[2] = MOON_U;
            g[3] = MOON_V;
            moonSurfaceData[(s * 3 + k) * 4 + 1] = 0.5f;
        }
    /* The disc stands upright 8 units in front of the camera, 8 degrees above the water's horizon. */
    mat_identity(moonModel);
    moonModel[5] = 0.0f; moonModel[6] = 1.0f;
    moonModel[9] = 1.0f; moonModel[10] = 0.0f;
    moonModel[13] = 1.735f; moonModel[14] = -6.11f;

    CHECK(load_shader(device, "host:build/water_comp.pvs", &cs));
    CHECK(load_shader(device, "host:build/water_vert.pvs", &vs));
    CHECK(load_shader(device, "host:build/water_frag.pvs", &fs));

    /* The compute shader's three storage blocks; one layout serves both pipelines. */
    PvkDescriptorSetLayoutBinding bindings[] = {
        { .binding = 0, .descriptorType = PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = PVK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = PVK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = PVK_SHADER_STAGE_COMPUTE_BIT },
    };
    PvkDescriptorSetLayoutCreateInfo setLayoutInfo = { .bindingCount = 3, .bindings = bindings };
    CHECK(pvkCreateDescriptorSetLayout(device, &setLayoutInfo, &setLayout));
    /* Set 1: the sky the water's fragment shader samples. */
    PvkDescriptorSetLayoutBinding skyBinding = {
        .binding = 0, .descriptorType = PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
        .stageFlags = PVK_SHADER_STAGE_FRAGMENT };
    PvkDescriptorSetLayoutCreateInfo skyLayoutInfo = { .bindingCount = 1, .bindings = &skyBinding };
    CHECK(pvkCreateDescriptorSetLayout(device, &skyLayoutInfo, &skyLayout));
    /* The drop (bytes 0..15) and the matrix (16..79) overlap no stage's block, but one range keeps them both set across the two pipelines. */
    PvkPushConstantRange pushRange = { .stageFlags = PVK_SHADER_STAGE_VERTEX | PVK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(Push) };
    PvkDescriptorSetLayout layoutSets[2] = { setLayout, skyLayout };
    PvkPipelineLayoutCreateInfo layoutInfo = {
        .setLayoutCount = 2, .setLayouts = layoutSets, .pushConstantRangeCount = 1, .pushConstantRanges = &pushRange,
    };
    CHECK(pvkCreatePipelineLayout(device, &layoutInfo, &layout));

    PvkComputePipelineCreateInfo computeInfo = {
        .sType             = PVK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage             = { .stage = PVK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .name = "main" },
        .layout            = layout,
        .basePipelineIndex = -1,
    };
    CHECK(pvkCreateComputePipelines(device, NULL, 1, &computeInfo, NULL, &computePipeline));

    /* sets[i][b] gives band b's rows of heights[i] (with the row above and below) to read, and its
     * rows of heights[1 - i] and of the surface to write: the frames alternate between the heights. */
    PvkDescriptorPoolSize poolSizes[] = {
        { PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 * BANDS }, { PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
    };
    PvkDescriptorPoolCreateInfo poolInfo = { .maxSets = 2 * BANDS + 1, .poolSizeCount = 2, .poolSizes = poolSizes };
    CHECK(pvkCreateDescriptorPool(device, &poolInfo, &pool));
    PvkDescriptorSetLayout setLayouts[BANDS];
    for (int b = 0; b < BANDS; b++)
        setLayouts[b] = setLayout;
    PvkDescriptorSetAllocateInfo allocInfo = { .descriptorPool = pool, .descriptorSetCount = BANDS, .setLayouts = setLayouts };
    for (int s = 0; s < 2; s++) {
        CHECK(pvkAllocateDescriptorSets(device, &allocInfo, sets[s]));
        for (int b = 0; b < BANDS; b++) {
            size_t row = COLS * sizeof(float);
            PvkDescriptorBufferInfo current = { heights[s], b * BAND * row, (BAND + 2) * row };
            PvkDescriptorBufferInfo next = { heights[1 - s], (b * BAND + 1) * row, BAND * row };
            PvkDescriptorBufferInfo out = { surface, b * BAND * row * 4, BAND * row * 4 };
            PvkWriteDescriptorSet writes[] = {
                { .dstSet = sets[s][b], .dstBinding = 0, .descriptorType = PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .bufferInfo = &current },
                { .dstSet = sets[s][b], .dstBinding = 1, .descriptorType = PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .bufferInfo = &next },
                { .dstSet = sets[s][b], .dstBinding = 2, .descriptorType = PVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .bufferInfo = &out },
            };
            pvkUpdateDescriptorSets(device, 3, writes);
        }
    }
    PvkDescriptorSetAllocateInfo skyAllocInfo = { .descriptorPool = pool, .descriptorSetCount = 1, .setLayouts = &skyLayout };
    CHECK(pvkAllocateDescriptorSets(device, &skyAllocInfo, &skySet));
    PvkDescriptorImageInfo skyDescriptor = { .sampler = skySampler, .imageView = skyView };
    PvkWriteDescriptorSet skyWrite = {
        .dstSet = skySet, .dstBinding = 0, .descriptorType = PVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .imageInfo = &skyDescriptor,
    };
    pvkUpdateDescriptorSets(device, 1, &skyWrite);

    /* The mesh reads two bindings per vertex: its x, z and sky texel, and its height, light and two slopes. */
    PvkVertexInputBinding vertexBindings[] = {
        { .binding = 0, .stride = 16 },
        { .binding = 1, .stride = 16 },
    };
    PvkVertexInputAttribute attributes[] = {
        { .location = 0, .binding = 0, .format = PVK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 1, .format = PVK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
    };
    PvkPipelineShaderStageCreateInfo stages[] = {
        { .stage = PVK_SHADER_STAGE_VERTEX,   .module = vs, .name = "main" },
        { .stage = PVK_SHADER_STAGE_FRAGMENT, .module = fs, .name = "main" },
    };
    PvkGraphicsPipelineCreateInfo drawInfo = {
        .stageCount     = 2,
        .stages         = stages,
        .layout         = layout,
        .bindingCount   = 2,
        .bindings       = vertexBindings,
        .attributeCount = 2,
        .attributes     = attributes,
        .topology       = PVK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .viewport       = { 0.0f, 0.0f, WIDTH, HEIGHT, 0.0f, 1.0f },
        .depthAttachmentFormat = PVK_FORMAT_Z24,
        .depthStencil   = { .depthTestEnable = 1, .depthWriteEnable = 1, .depthCompareOp = PVK_COMPARE_OP_LESS },
        .colorBlend     = { .attachment = { .colorWriteMask = PVK_COLOR_COMPONENT_R_BIT | PVK_COLOR_COMPONENT_G_BIT
                                                             | PVK_COLOR_COMPONENT_B_BIT | PVK_COLOR_COMPONENT_A_BIT } },
        .rasterization  = { .sType = PVK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO },
    };
    CHECK(pvkCreateGraphicsPipeline(device, NULL, &drawInfo, &drawPipeline));

    PvkCommandPoolCreateInfo commandPoolInfo = { .flags = 0, .maxQwords = 65535 };
    CHECK(pvkCreateCommandPool(device, &commandPoolInfo, &commandPool));
    PvkCommandBufferAllocateInfo cmdInfo = {
        .commandPool = commandPool, .level = PVK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    CHECK(pvkAllocateCommandBuffers(device, &cmdInfo, &cmd));
    CHECK(pvkCreateFence(device, NULL, &fence));
    CHECK(pvkCreateSemaphore(device, NULL, &imageAvailable));
    CHECK(pvkCreateSemaphore(device, NULL, &renderDone));
    PvkCommandBufferBeginInfo beginInfo = { .flags = 0 };

    /* Upload the sky once, before the first frame. */
    CHECK(pvkBeginCommandBuffer(cmd, &beginInfo));
    PvkImageSubresourceRange wholeSky = { .aspectMask = PVK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 };
    PvkImageMemoryBarrier toTransfer = {
        .dstAccessMask = PVK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = PVK_IMAGE_LAYOUT_UNDEFINED, .newLayout = PVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = PVK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = PVK_QUEUE_FAMILY_IGNORED,
        .image = sky, .subresourceRange = wholeSky,
    };
    pvkCmdPipelineBarrier(cmd, PVK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, PVK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toTransfer);
    PvkBufferImageCopy copy = {
        .imageSubresource = { .aspectMask = PVK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .layerCount = 1 },
        .imageExtent      = { SKY_SIZE, SKY_SIZE, 1 },
    };
    pvkCmdCopyBufferToImage(cmd, skyTexels, sky, PVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    PvkImageMemoryBarrier toShader = {
        .srcAccessMask = PVK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = PVK_ACCESS_SHADER_READ_BIT,
        .oldLayout = PVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = PVK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = PVK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = PVK_QUEUE_FAMILY_IGNORED,
        .image = sky, .subresourceRange = wholeSky,
    };
    pvkCmdPipelineBarrier(cmd, PVK_PIPELINE_STAGE_TRANSFER_BIT, PVK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &toShader);
    CHECK(pvkEndCommandBuffer(cmd));
    PvkSubmitInfo uploadSubmit = { .commandBufferCount = 1, .commandBuffers = &cmd };
    CHECK(pvkQueueSubmit(queue, 1, &uploadSubmit, fence));

    /* For a 16:9 TV, which stretches the 640x448 picture to that shape. */
    mat_perspective(proj, 50.0f * 3.14159265f / 180.0f, 16.0f / 9.0f, 0.5f, 10.0f);
    srand(1);

    /* The mesh's indices: the quads on or near the screen. The camera stays still, so the rest are
     * left out once here; their corners would land outside the GS's 4096-pixel space. */
    {
        float rx[16], t[16], m[16];
        mat_rotate_x(rx, 0.2f);
        mat_translate(t, 0.0f, -0.25f, -1.9f);
        mat_mul(m, t, rx);
        mat_mul(m, proj, m);
        for (int y = 0; y < ROWS - 1; y++)
            for (int x = 0; x < COLS - 1; x++) {
                uint16_t a = (uint16_t)(y * COLS + x), b = (uint16_t)(a + 1), c = (uint16_t)(a + COLS), d = (uint16_t)(c + 1);
                uint16_t corner[4] = { a, b, c, d };
                float lx = 1e9f, hx = -1e9f, ly = 1e9f, hy = -1e9f;
                int keep = 1;
                for (int k = 0; k < 8 && keep; k++) {
                    const float *g = &gridData[corner[k & 3] * 4];
                    float h = k & 4 ? 0.1f : -0.1f;
                    float cx = m[0] * g[0] + m[4] * h + m[8] * g[1] + m[12];
                    float cy = m[1] * g[0] + m[5] * h + m[9] * g[1] + m[13];
                    float cw = m[3] * g[0] + m[7] * h + m[11] * g[1] + m[15];
                    float sx = 320.0f + 320.0f * cx / cw, sy = 224.0f + 224.0f * cy / cw;
                    if (cw < 0.5f || sx < -1000.0f || sx > 1600.0f || sy < -1000.0f || sy > 1400.0f)
                        keep = 0;
                    lx = sx < lx ? sx : lx; hx = sx > hx ? sx : hx;
                    ly = sy < ly ? sy : ly; hy = sy > hy ? sy : hy;
                }
                if (!keep || hx < -16.0f || lx > 656.0f || hy < -16.0f || ly > 464.0f)
                    continue;
                indexData[indexCount++] = a; indexData[indexCount++] = c; indexData[indexCount++] = b;
                indexData[indexCount++] = b; indexData[indexCount++] = c; indexData[indexCount++] = d;
            }
    }

    printf("water: running, %u of %u quads drawn\n", (unsigned)(indexCount / 6), (unsigned)((COLS - 1) * (ROWS - 1)));

    for (unsigned frame = 0;; frame++) {
        uint32_t imageIndex, current = frame & 1;
        size_t offsets[2] = { 0, 0 };
        float rx[16], t[16], dropRow;

        /* A small drop every 20 frames at a random cell (as many per area as every 60 frames on the
         * 64x64 grid), the first one in front of the camera. */
        memset(push.drop, 0, sizeof(push.drop));
        if (frame % 20 == 0) {
            push.drop[0] = frame ? (float)(3 + rand() % (COLS - 6)) : COLS / 2;
            push.drop[1] = frame ? (float)(3 + rand() % (ROWS - 6)) : 46.0f;
            push.drop[2] = frame ? 0.6f + 0.2f * (float)(rand() % 4) : 1.5f;
            push.drop[3] = 1.0f / 3.0f;
        }
        dropRow = push.drop[1];
        /* The surface stays still; the camera stands low over the water, looking out across it. */
        mat_rotate_x(rx, 0.2f);
        mat_translate(t, 0.0f, -0.25f, -1.9f);
        mat_mul(push.mvp, t, rx);
        mat_mul(push.mvp, proj, push.mvp);
        mat_mul(moonMvp, push.mvp, moonModel);

        /* The command buffer is re-recorded below, so the previous frame (or the upload) must be done with it. */
        CHECK(pvkWaitForFences(device, 1, &fence));
        CHECK(pvkResetFences(device, 1, &fence));
        CHECK(pvkAcquireNextImage(device, swapchain, UINT64_MAX, imageAvailable, NULL, &imageIndex));

        CHECK(pvkResetCommandPool(device, commandPool, 0));
        CHECK(pvkBeginCommandBuffer(cmd, &beginInfo));

        /* One step of the waves, band by band: 5 x BAND work groups of 8 invocations, 5 per row. The
         * drop's row is given from each band's first row. */
        pvkCmdBindPipeline(cmd, PVK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        pvkCmdPushConstants(cmd, layout, PVK_SHADER_STAGE_VERTEX | PVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
        for (int b = 0; b < BANDS; b++) {
            push.drop[1] = dropRow - (float)(b * BAND);
            pvkCmdPushConstants(cmd, layout, PVK_SHADER_STAGE_VERTEX | PVK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push.drop);
            pvkCmdBindDescriptorSets(cmd, PVK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[current][b], 0, NULL);
            pvkCmdDispatch(cmd, COLS / 4 / 8, BAND, 1);
        }

        /* The mesh reads what the dispatch wrote as vertex attributes. */
        PvkBufferMemoryBarrier toVertices = {
            .srcAccessMask = PVK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = PVK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
            .srcQueueFamilyIndex = PVK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = PVK_QUEUE_FAMILY_IGNORED,
            .buffer = surface, .offset = 0, .size = PVK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(cmd, PVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, PVK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                              0, NULL, 1, &toVertices, 0, NULL);

        PvkRenderingInfo rendering = {
            .renderArea = { 0, 0, WIDTH, HEIGHT },
            .colorAttachment = {
                .imageView  = views[imageIndex],
                .loadOp     = PVK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp    = PVK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = { .color = { { 0.02f, 0.03f, 0.06f, 1.0f } } },
            },
            .depthAttachment = {
                .imageView  = depthView,
                .loadOp     = PVK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp    = PVK_ATTACHMENT_STORE_OP_DONT_CARE,
                .clearValue = { .depthStencil = { .depth = 1.0f } },
            },
        };
        pvkCmdBeginRendering(cmd, &rendering);
        pvkCmdBindPipeline(cmd, PVK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline);
        pvkCmdBindDescriptorSets(cmd, PVK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &skySet, 0, NULL);
        PvkBuffer vertexBuffers[2] = { grid, surface };
        pvkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);
        pvkCmdBindIndexBuffer(cmd, indexBuffer, 0, PVK_INDEX_TYPE_UINT16);
        pvkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
        /* The moon, with its own matrix (bytes 16..79 of the push constants). */
        PvkBuffer moonBuffers[2] = { moonGrid, moonSurface };
        pvkCmdBindVertexBuffers(cmd, 0, 2, moonBuffers, offsets);
        pvkCmdPushConstants(cmd, layout, PVK_SHADER_STAGE_VERTEX | PVK_SHADER_STAGE_COMPUTE_BIT, 16, 64, moonMvp);
        pvkCmdDraw(cmd, MOON_SEGMENTS * 3, 1, 0, 0);
        pvkCmdEndRendering(cmd);
        CHECK(pvkEndCommandBuffer(cmd));

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
