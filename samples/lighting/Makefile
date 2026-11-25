# ps2vk lighting sample: a sphere lit per vertex by a directional light read
# from a uniform buffer.
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> .pvs files the sample loads from host:.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc

EE_BIN  = lighting.elf
EE_OBJS = main.o
SHADERS = build/lighting_vert.pvs build/lighting_frag.pvs
EE_INCS = -I$(PS2VK)/include
EE_LIBS = -L$(PS2VK)/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma -lm
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN) $(SHADERS)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

build/lighting_vert.pvs: build/lighting.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/lighting_vert

build/lighting_frag.pvs: build/lighting.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/lighting_frag

main.o: $(PS2VK)/include/ps2vk.h

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
