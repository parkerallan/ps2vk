/*
 * ps2vk step 1: clear the screen through a command buffer and present it.
 * No shaders or VU1. The clear color cycles so a hung frame is easy to spot.
 * Two timestamps around the clear give the time the GS spent on it, printed
 * every 60 frames.
 */
#include <stdio.h>

#include "ps2vk.h"

#define WIDTH  640
#define HEIGHT 448

#define CHECK(call)                                                       \
    do {                                                                  \
        PvkResult res_ = (call);                                          \
        if (res_ != PVK_SUCCESS) {                                        \
            printf("clear: %s failed (%d)\n", #call, (int)res_);          \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main(void)
{
    PvkInstance instance;
    PvkPhysicalDevice physicalDevice;
    uint32_t physicalDeviceCount = 1;
    PvkDevice device;
    PvkPhysicalDeviceProperties properties;
    PvkPhysicalDeviceFeatures features;
    PvkPhysicalDeviceMemoryProperties memory;
    PvkQueue queue;
    PvkSwapchain swapchain;
    PvkImage images[2];
    PvkImageView views[2];
    uint32_t imageCount = 2;
    PvkCommandPool commandPool;
    PvkCommandBuffer cmd;
    PvkFence fence;
    PvkSemaphore imageAvailable, renderDone;
    PvkQueryPool queryPool;
    uint32_t frame = 0;

    PvkInstanceCreateInfo instanceInfo = { .applicationName = "clear" };
    CHECK(pvkCreateInstance(&instanceInfo, &instance));
    CHECK(pvkEnumeratePhysicalDevices(instance, &physicalDeviceCount, &physicalDevice));
    pvkGetPhysicalDeviceProperties(physicalDevice, &properties);
    pvkGetPhysicalDeviceFeatures(physicalDevice, &features);
    pvkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
    printf("clear: %s, API %u.%u, textures up to %u with %u mip levels, %u bytes of push constants, "
           "%u descriptor sets, %u vertex bindings, robustBufferAccess %u\n",
           properties.deviceName, (unsigned)(properties.apiVersion >> 22), (unsigned)((properties.apiVersion >> 12) & 0x3ff),
           (unsigned)properties.limits.maxImageDimension2D, (unsigned)properties.limits.maxMipLevels,
           (unsigned)properties.limits.maxPushConstantsSize, (unsigned)properties.limits.maxBoundDescriptorSets,
           (unsigned)properties.limits.maxVertexInputBindings, (unsigned)features.robustBufferAccess);
    for (uint32_t i = 0; i < memory.memoryHeapCount; i++)
        printf("clear: heap %u: %u KB%s\n", (unsigned)i, (unsigned)(memory.memoryHeaps[i].size >> 10),
               memory.memoryHeaps[i].flags & PVK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (GS VRAM)" : "");
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
        printf("clear: memory type %u: heap %u, flags 0x%x\n", (unsigned)i, (unsigned)memory.memoryTypes[i].heapIndex,
               (unsigned)memory.memoryTypes[i].propertyFlags);
    /* Queue family 0 is the graphics queue (pvkGetPhysicalDeviceQueueFamilyProperties lists it). */
    PvkDeviceQueueCreateInfo queueInfo = { .queueFamilyIndex = 0, .queueCount = 1 };
    PvkDeviceCreateInfo deviceInfo = { .queueCreateInfoCount = 1, .queueCreateInfos = &queueInfo };
    CHECK(pvkCreateDevice(physicalDevice, &deviceInfo, &device));
    pvkGetDeviceQueue(device, 0, 0, &queue);

    PvkSwapchainCreateInfo swapchainInfo = {
        .width = WIDTH, .height = HEIGHT, .format = PVK_FORMAT_CT32, .imageCount = 2,
    };
    CHECK(pvkCreateSwapchain(device, &swapchainInfo, &swapchain));
    CHECK(pvkGetSwapchainImages(device, swapchain, &imageCount, images));
    for (uint32_t i = 0; i < imageCount; i++) {
        PvkImageViewCreateInfo viewInfo = { .image = images[i], .levelCount = 1 };
        CHECK(pvkCreateImageView(device, &viewInfo, &views[i]));
    }
    PvkCommandPoolCreateInfo commandPoolInfo = { .flags = 0 };
    CHECK(pvkCreateCommandPool(device, &commandPoolInfo, &commandPool));
    PvkCommandBufferAllocateInfo cmdInfo = {
        .commandPool = commandPool, .level = PVK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    CHECK(pvkAllocateCommandBuffers(device, &cmdInfo, &cmd));
    CHECK(pvkCreateFence(device, NULL, &fence));
    CHECK(pvkCreateSemaphore(device, NULL, &imageAvailable));
    CHECK(pvkCreateSemaphore(device, NULL, &renderDone));
    /* Two timestamp slots: the point before the clear and the point after it. */
    PvkQueryPoolCreateInfo queryPoolInfo = { .queryType = PVK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
    CHECK(pvkCreateQueryPool(device, &queryPoolInfo, &queryPool));
    PvkCommandBufferBeginInfo beginInfo = { .flags = 0 };

    printf("clear: running\n");

    for (;; frame++) {
        uint32_t imageIndex;
        float t = (float)(frame % 120) / 120.0f;

        /* The command buffer is re-recorded below, so the previous frame must be done with it. */
        CHECK(pvkWaitForFences(device, 1, &fence));
        CHECK(pvkResetFences(device, 1, &fence));
        /* The previous frame's timestamps are in once its fence is; every 60th frame, print the GS time. */
        if (frame > 0 && frame % 60 == 0) {
            uint32_t ticks[2];
            CHECK(pvkGetQueryPoolResults(device, queryPool, 0, 2, sizeof(ticks), ticks, sizeof(ticks[0]), 0));
            printf("clear: frame %u cleared in %u us on the GS\n", (unsigned)(frame - 1),
                   (unsigned)((ticks[1] - ticks[0]) * properties.limits.timestampPeriod / 1000.0f));
        }
        /* imageAvailable is signaled when the image leaves the screen; the draw waits for it. */
        CHECK(pvkAcquireNextImage(device, swapchain, UINT64_MAX, imageAvailable, NULL, &imageIndex));

        CHECK(pvkResetCommandPool(device, commandPool, 0));
        CHECK(pvkBeginCommandBuffer(cmd, &beginInfo));
        /* A slot must be reset before each write; timestamp 0 goes before the clear, 1 after it. */
        pvkCmdResetQueryPool(cmd, queryPool, 0, 2);
        pvkCmdWriteTimestamp(cmd, PVK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
        PvkRenderingInfo rendering = {
            .renderArea = { 0, 0, WIDTH, HEIGHT },
            .colorAttachment = {
                .imageView  = views[imageIndex],
                .loadOp     = PVK_ATTACHMENT_LOAD_OP_CLEAR,
                .clearValue = { .color = { { t, 0.3f, 1.0f - t, 1.0f } } },
            },
        };
        pvkCmdBeginRendering(cmd, &rendering);
        pvkCmdEndRendering(cmd);
        pvkCmdWriteTimestamp(cmd, PVK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
        CHECK(pvkEndCommandBuffer(cmd));

        /* The draw waits for the image and signals renderDone; the present waits for renderDone. */
        /* The chain waits whole, so the stage only documents what the wait protects: the color writes. */
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
