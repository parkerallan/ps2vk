# ps2vk cube sample: a textured, depth-tested, spinning cube. The texture is
# texture.png, read at run time from the folder the ELF was booted from (host:).
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> .pvs files the sample loads from host:.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc

EE_BIN  = cube.elf
EE_OBJS = main.o
SHADERS = build/cube_vert.pvs build/cube_frag.pvs
EE_INCS = -I$(PS2VK)/include -I$(PS2SDK)/ports/include
# libpng and zlib come from ps2sdk-ports.
EE_LIBS = -L$(PS2VK)/lib -L$(PS2SDK)/ports/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma -lpng -lz -lm
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN) $(SHADERS)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

build/cube_vert.pvs: build/cube.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/cube_vert

build/cube_frag.pvs: build/cube.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/cube_frag

main.o: $(PS2VK)/include/ps2vk.h

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
