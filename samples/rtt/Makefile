# ps2vk render-to-texture sample: a cube rendered into a texture that a second
# cube is drawn with.
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> VU1 code / GS settings.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc
EE_DVP  = dvp-as

EE_BIN  = rtt.elf
EE_OBJS = main.o build/rtt_vert.o build/rtt_vert_vu1.o build/color_frag.o build/texture_frag.o
EE_INCS = -I$(PS2VK)/include
EE_LIBS = -L$(PS2VK)/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma -lm
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

build/rtt_vert.c build/rtt_vert.vsm: build/rtt.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/rtt_vert

build/color_frag.c: build/color.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/color_frag

build/texture_frag.c: build/texture.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/texture_frag

build/%_vu1.o: build/%.vsm
	$(EE_DVP) $< -o $@

main.o: build/rtt_vert.c build/color_frag.c build/texture_frag.c $(PS2VK)/include/ps2vk.h

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
