# ps2vk proof of concept: one Gouraud-shaded triangle.
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> VU1 code / GS settings.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc
EE_DVP  = dvp-as

EE_BIN  = triangle.elf
EE_OBJS = main.o build/tri_vert.o build/tri_vert_vu1.o build/tri_frag.o
EE_INCS = -I$(PS2VK)/include
EE_LIBS = -L$(PS2VK)/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

# Vertex shaders produce VU1 code (.vsm) and a PvkShaderCode (.c).
build/tri_vert.c build/tri_vert.vsm: build/tri.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/tri_vert

build/tri_frag.c: build/tri.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/tri_frag

build/%_vu1.o: build/%.vsm
	$(EE_DVP) $< -o $@

main.o: build/tri_vert.c build/tri_frag.c

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
