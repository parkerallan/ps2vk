# ps2vk runtime library (libps2vk.a), built with the ps2dev toolchain.
# Needs PS2SDK set and the ps2dev bin directories on PATH.

EE_LIB  = lib/libps2vk.a
EE_OBJS = src/pvk_device.o src/pvk_swapchain.o src/pvk_memory.o src/pvk_shader.o \
          src/pvk_pipeline.o src/pvk_cmd.o src/pvk_queue.o
EE_INCS = -Iinclude
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_LIB) shaderc clear triangle

shaderc:
	$(MAKE) -C tools/shaderc

clear: $(EE_LIB)
	$(MAKE) -C samples/clear

triangle: $(EE_LIB) shaderc
	$(MAKE) -C samples/triangle

docs:
	doxygen Doxyfile

clean:
	rm -f $(EE_LIB) $(EE_OBJS)
	$(MAKE) -C tools/shaderc clean
	$(MAKE) -C samples/clear clean
	$(MAKE) -C samples/triangle clean

.PHONY: all shaderc clear triangle docs clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
