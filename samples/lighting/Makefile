# ps2vk lighting sample: a sphere lit per vertex by a directional light read
# from a uniform buffer.
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> VU1 code / GS settings.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc
EE_DVP  = dvp-as

EE_BIN  = lighting.elf
EE_OBJS = main.o build/lighting_vert.o build/lighting_vert_vu1.o build/lighting_frag.o
EE_INCS = -I$(PS2VK)/include
EE_LIBS = -L$(PS2VK)/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma -lm
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

# Vertex shaders produce VU1 code (.vsm) and a PvkShaderCode (.c).
build/lighting_vert.c build/lighting_vert.vsm: build/lighting.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/lighting_vert

build/lighting_frag.c: build/lighting.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/lighting_frag

build/%_vu1.o: build/%.vsm
	$(EE_DVP) $< -o $@

main.o: build/lighting_vert.c build/lighting_frag.c $(PS2VK)/include/ps2vk.h

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
