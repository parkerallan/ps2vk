# ps2vk step 1: clear the screen, no shaders.

PS2VK   = ../..

EE_BIN  = clear.elf
EE_OBJS = main.o
EE_INCS = -I$(PS2VK)/include
EE_LIBS = -L$(PS2VK)/lib -lps2vk -ldraw -lgraph -lpacket2 -ldma
EE_WARNFLAGS = -Wall -Wextra

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

.PHONY: all clean

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
