/*
 * ps2vk step 1: clear the screen through a command buffer and present it.
 * No shaders or VU1. The clear color cycles so a hung frame is easy to spot.
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
    PvkDevice device;
    PvkQueue queue;
    PvkSwapchain swapchain;
    PvkImage images[2];
    uint32_t imageCount = 2;
    PvkCommandBuffer cmd;
    PvkFence fence;
    uint32_t frame = 0;

    PvkInstanceCreateInfo instanceInfo = { .applicationName = "clear" };
    CHECK(pvkCreateInstance(&instanceInfo, &instance));
    CHECK(pvkCreateDevice(instance, NULL, &device));
    pvkGetDeviceQueue(device, &queue);

    PvkSwapchainCreateInfo swapchainInfo = {
        .width = WIDTH, .height = HEIGHT, .format = PVK_FORMAT_CT32, .imageCount = 2,
    };
    CHECK(pvkCreateSwapchain(device, &swapchainInfo, &swapchain));
    CHECK(pvkGetSwapchainImages(device, swapchain, &imageCount, images));
    CHECK(pvkCreateCommandBuffer(device, NULL, &cmd));
    CHECK(pvkCreateFence(device, NULL, &fence));

    printf("clear: running\n");

    for (;; frame++) {
        uint32_t imageIndex;
        float t = (float)(frame % 120) / 120.0f;

        CHECK(pvkAcquireNextImage(device, swapchain, &imageIndex));

        CHECK(pvkBeginCommandBuffer(cmd));
        PvkRenderingInfo rendering = {
            .renderArea = { 0, 0, WIDTH, HEIGHT },
            .colorAttachment = {
                .image      = images[imageIndex],
                .loadOp     = PVK_ATTACHMENT_LOAD_OP_CLEAR,
                .clearValue = { { t, 0.3f, 1.0f - t, 1.0f } },
            },
        };
        pvkCmdBeginRendering(cmd, &rendering);
        pvkCmdEndRendering(cmd);
        CHECK(pvkEndCommandBuffer(cmd));

        CHECK(pvkResetFences(device, 1, &fence));
        CHECK(pvkQueueSubmit(queue, cmd, fence));
        CHECK(pvkWaitForFences(device, 1, &fence));
        CHECK(pvkQueuePresent(queue, swapchain, imageIndex));
    }

    return 0;
}
