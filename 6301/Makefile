# Build the HD6301V1 ROM-dumper image.
#
# Assembler: tabasm - a Rust reimplementation of the Telemark Assembler (TASM).
# It finds its instruction tables automatically (share/tabasm/tables), so no
# environment setup is needed. The source is TASM dialect, not Motorola syntax.
#
# tabasm flags: --cpu 6303 (the HD6303 table is the HD6301 instruction set,
# including SLP; the generic 6800 table hides the 6801/6301 additions behind
# class bits), -b binary, -c contiguous block, -fFF fill gaps with 0xFF,
# -l append the label table to the listing.
#
# The image spans $C000..$FFFF, so it is 16384 bytes and loads at $C000.
#
# `make hex` produces Intel HEX instead, for the Pico 2 loader in ../pico2.
# Without -c that emits records only for the regions actually written -
# $C000..$C01E and $FFFE..$FFFF - rather than 16 KB of mostly $FF.

SRC      := romdump.asm
BIN      := romdump.bin
HEX      := romdump.hex
LST      := romdump.lst
ASM      ?= tabasm
ASMOPTS  ?= --cpu 6303 -b -c -fFF -l
HEXOPTS  ?= --cpu 6303 -g0 -l

.PHONY: all hex verify clean

all: $(BIN)

$(BIN): $(SRC)
	$(ASM) $(ASMOPTS) $(SRC) $(BIN) $(LST)

hex: $(HEX)

$(HEX): $(SRC)
	$(ASM) $(HEXOPTS) $(SRC) $(HEX) $(LST)

# Cheap sanity checks: image size, reset vector byte order, and the baud
# divisor. tabasm emits .word little endian by default, so a byte-swapped
# vector is the most likely silent failure in this image; the RMCR byte is
# the one that decides whether the host hears anything intelligible.
verify: $(BIN)
	@test "$$(wc -c < $(BIN))" -eq 16384 \
	  && echo "OK   size 16384 bytes (\$$C000..\$$FFFF)" \
	  || (echo "FAIL size $$(wc -c < $(BIN)), expected 16384"; exit 1)
	@test "$$(dd if=$(BIN) bs=1 skip=16382 count=2 2>/dev/null | xxd -p)" = "c000" \
	  && echo "OK   reset vector \$$FFFE = \$$C000" \
	  || (echo "FAIL reset vector byte order - was .msfirst lost?"; exit 1)
	@test "$$(dd if=$(BIN) bs=1 skip=5 count=1 2>/dev/null | xxd -p)" = "04" \
	  && echo "OK   RMCR = \$$04 (E/16 -> 15625 baud at E = 250 kHz)" \
	  || (echo "FAIL RMCR byte at offset 5 is not \$$04"; exit 1)

clean:
	rm -f $(BIN) $(HEX) $(LST) $(SRC:.asm=.exp) $(SRC:.asm=.sym)
