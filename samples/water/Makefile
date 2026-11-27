# ps2vk water sample: ripples stepped by a compute shader on VU1, drawn as a mesh.
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> .pvs files the sample loads from host:.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc

EE_BIN  = water.elf
EE_OBJS = main.o
SHADERS = build/water_comp.pvs build/water_vert.pvs build/water_frag.pvs
EE_INCS = -I$(PS2VK)/include
EE_LIBS = -L$(PS2VK)/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma -lm
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN) $(SHADERS)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

build/water_comp.pvs: build/water.comp.spv $(SHADERC)
	$(SHADERC) $< -o build/water_comp

build/water_vert.pvs: build/water.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/water_vert

build/water_frag.pvs: build/water.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/water_frag

main.o: $(PS2VK)/include/ps2vk.h

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
