# Host build of the rig's line protocol, so the Intel HEX parser and the
# read-back formats can be tested without a Pico or a 6301 attached.

CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O1 -g
SRCDIR  := ../src
ASM     := ../../6301/romdump.asm

.PHONY: all test clean

all: cmdsim

# The firmware's built-in image, generated exactly as the Pico build does it.
romdump.bin: $(ASM)
	tabasm --cpu 6303 -b -c -fFF -q $(ASM) romdump.bin romdump.lst

rom_image.h: romdump.bin ../tools/bin2c.py
	../tools/bin2c.py --name rom_image --addr 0xC000 romdump.bin rom_image.h

cmdsim: cmdsim.c $(SRCDIR)/cmd.c $(SRCDIR)/mem.c rom_image.h
	$(CC) $(CFLAGS) -I$(SRCDIR) -Istubs -I. -o $@ cmdsim.c $(SRCDIR)/cmd.c $(SRCDIR)/mem.c

test: cmdsim
	python3 run_tests.py

clean:
	rm -f cmdsim romdump.bin romdump.lst rom_image.h *.tmp
	rm -rf cmdsim.dSYM
