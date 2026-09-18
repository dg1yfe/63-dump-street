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

SRC      := romdump.asm
BIN      := romdump.bin
LST      := romdump.lst
ASM      ?= tabasm
ASMOPTS  ?= --cpu 6303 -b -c -fFF -l

.PHONY: all verify clean

all: $(BIN)

$(BIN): $(SRC)
	$(ASM) $(ASMOPTS) $(SRC) $(BIN) $(LST)

# Cheap sanity checks: image size and reset vector byte order. tabasm emits
# .word little endian by default, so a byte-swapped vector is the most likely
# silent failure in this image.
verify: $(BIN)
	@test "$$(wc -c < $(BIN))" -eq 16384 \
	  && echo "OK   size 16384 bytes (\$$C000..\$$FFFF)" \
	  || (echo "FAIL size $$(wc -c < $(BIN)), expected 16384"; exit 1)
	@test "$$(dd if=$(BIN) bs=1 skip=16382 count=2 2>/dev/null | xxd -p)" = "c000" \
	  && echo "OK   reset vector \$$FFFE = \$$C000" \
	  || (echo "FAIL reset vector byte order - was .msfirst lost?"; exit 1)

clean:
	rm -f $(BIN) $(LST) $(SRC:.asm=.exp) $(SRC:.asm=.sym)
