# ps2vk-shaderc: runs on the PC (WSL), built with the host compiler.

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -I../../include

shaderc: shaderc.c ../../include/ps2vk_vu1_layout.h
	$(CC) $(CFLAGS) -o $@ shaderc.c

clean:
	rm -f shaderc

.PHONY: clean
