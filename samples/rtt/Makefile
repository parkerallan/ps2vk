# ps2vk render-to-texture sample: a cube rendered into a texture that a second
# cube is drawn with.
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> .pvs files the sample loads from host:.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc

EE_BIN  = rtt.elf
EE_OBJS = main.o
SHADERS = build/rtt_vert.pvs build/color_frag.pvs build/texture_frag.pvs
EE_INCS = -I$(PS2VK)/include
EE_LIBS = -L$(PS2VK)/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma -lm
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN) $(SHADERS)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

build/rtt_vert.pvs: build/rtt.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/rtt_vert

build/color_frag.pvs: build/color.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/color_frag

build/texture_frag.pvs: build/texture.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/texture_frag

main.o: $(PS2VK)/include/ps2vk.h

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
