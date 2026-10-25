# ps2vk

ps2vk is a Vulkan-like graphics API for the PlayStation 2. It carries over Vulkan's object model and explicit control, mapped onto the EE, VU1 and GS as they actually work. Additional features include a GLSL shader compiler that compiles to VU1 microcode.

## Concept mapping

Those coming from Vulkan will find the same design pattern, where we write a command list of drawing instructions into memory, then tell the graphics hardware to draw it. Here's some examples of the object model:

| Vulkan | ps2vk | PS2 hardware |
| --- | --- | --- |
| `VkInstance`, `VkDevice`, `VkQueue` | `PvkInstance`, `PvkDevice`, `PvkQueue` | DMA channel setup; one fixed device and queue |
| `VkBuffer` | `PvkBuffer` | EE main RAM, sent to VU1 with `UNPACK` |
| `VkImage` | `PvkImage` | GS VRAM with a GS pixel format |
| `VkShaderModule` | `PvkShaderModule` | GLSL compiled offline to VU1 microcode (vertex) or GS settings (fragment) |
| `VkPipeline` | `PvkPipeline` | Compiled shaders + GS state |
| `VkCommandBuffer` | `PvkCommandBuffer` | A VIF1 DMA chain in main RAM |
| `vkCmdBeginRendering` | `pvkCmdBeginRendering` | GS `FRAME`, `ZBUF`, `SCISSOR`, `XYOFFSET`; a clear is a rectangle draw |
| `VkFence` | `PvkFence` | GS `FINISH` signal |
| `VkSwapchainKHR` | `PvkSwapchain` | Framebuffers in VRAM, switched with `DISPFB` at vsync |

## Status

Draws a Gouraud-shaded triangle from GLSL vertex and fragment shaders, running in PCSX2.

## Setup

Install the basics:

```bash
sudo apt install build-essential glslang-tools
```

Download the prebuilt ps2dev toolchain:

```bash
curl -L -o ps2dev.tar.gz https://github.com/ps2dev/ps2dev/releases/download/latest/ps2dev-ubuntu-latest.tar.gz
```

Unpack it into your home folder (it creates `~/ps2dev`):

```bash
tar xzf ps2dev.tar.gz -C ~
```

Save these lines to the end of `~/.bashrc`:

```bash
export PS2DEV=$HOME/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
export GSKIT=$PS2DEV/gsKit
export PATH=$PATH:$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PS2SDK/bin
```

make builds `lib/libps2vk.a`, the shader compiler, and `samples/triangle/triangle.elf`. Boot the ELF in PCSX2 with System > Boot ELF.
