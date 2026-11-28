# ps2vk grass sample: a field of grass, each blade built by a mesh shader on VU1.
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> .pvs files the sample loads from host:.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc

EE_BIN  = grass.elf
EE_OBJS = main.o
SHADERS = build/grass_mesh.pvs build/grass_frag.pvs build/backdrop_vert.pvs
EE_INCS = -I$(PS2VK)/include
EE_LIBS = -L$(PS2VK)/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma -lm
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN) $(SHADERS)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

build/grass.mesh.spv: grass.mesh
	@mkdir -p build
	glslangValidator --target-env vulkan1.2 -g -o $@ $<

build/grass_mesh.pvs: build/grass.mesh.spv $(SHADERC)
	$(SHADERC) $< -o build/grass_mesh

build/grass_frag.pvs: build/grass.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/grass_frag

build/backdrop_vert.pvs: build/backdrop.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/backdrop_vert

main.o: $(PS2VK)/include/ps2vk.h

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
