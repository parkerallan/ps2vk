# ps2vk rain sample: a night rain scene drawn with every primitive topology.
# Shaders: GLSL -> SPIR-V (glslangValidator) -> ps2vk-shaderc -> VU1 code / GS settings.

PS2VK   = ../..
SHADERC = $(PS2VK)/tools/shaderc/shaderc
EE_DVP  = dvp-as

EE_BIN  = rain.elf
EE_OBJS = main.o build/rain_vert.o build/rain_vert_vu1.o build/rain_frag.o
EE_INCS = -I$(PS2VK)/include
EE_LIBS = -L$(PS2VK)/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma -lm
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN)

build/%.spv: %
	@mkdir -p build
	glslangValidator -V -g -o $@ $<

build/rain_vert.c build/rain_vert.vsm: build/rain.vert.spv $(SHADERC)
	$(SHADERC) $< -o build/rain_vert

build/rain_frag.c: build/rain.frag.spv $(SHADERC)
	$(SHADERC) $< -o build/rain_frag

build/%_vu1.o: build/%.vsm
	$(EE_DVP) $< -o $@

main.o: build/rain_vert.c build/rain_frag.c $(PS2VK)/include/ps2vk.h

clean:
	rm -rf build $(EE_BIN) main.o

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
