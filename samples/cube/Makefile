# ps2vk cube sample: a textured, depth-tested, spinning cube. The texture is
# texture.png, read at run time from the folder the ELF was booted from (host:).
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> VU1 code / GS settings.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc
EE_DVP  = dvp-as

EE_BIN  = cube.elf
EE_OBJS = main.o build/cube_vert.o build/cube_vert_vu1.o build/cube_frag.o
EE_INCS = -I$(PS2VK)/include -I$(PS2SDK)/ports/include
# libpng and zlib come from ps2sdk-ports.
EE_LIBS = -L$(PS2VK)/lib -L$(PS2SDK)/ports/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma -lpng -lz -lm
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

# Vertex shaders produce VU1 code (.vsm) and a PvkShaderCode (.c).
build/cube_vert.c build/cube_vert.vsm: build/cube.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/cube_vert

build/cube_frag.c: build/cube.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/cube_frag

build/%_vu1.o: build/%.vsm
	$(EE_DVP) $< -o $@

main.o: build/cube_vert.c build/cube_frag.c $(PS2VK)/include/ps2vk.h

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
