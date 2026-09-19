#include <string.h>

#include "mem.h"
#include "rom_image.h"

uint8_t mem[MEM_SIZE];
uint8_t capture[CAPTURE_SIZE];

// Written by core0 (command parser) and by the main loop; read by both.
static volatile uint32_t cap_len;
static volatile uint32_t cap_lost;

void mem_init(void) {
    // $FF rather than $00: it is what an erased device reads as, and it keeps
    // an unloaded address from decoding as opcode $00.
    memset(mem, 0xFF, sizeof mem);
    memcpy(&mem[ROM_IMAGE_ADDR], rom_image, sizeof rom_image);
    capture_reset();
}

void capture_reset(void) {
    cap_len  = 0;
    cap_lost = 0;
}

void capture_put(uint8_t b) {
    // Drop the tail rather than wrapping: a silently wrapped ROM dump looks
    // plausible and is wrong, which is the worst way to fail here.
    if (cap_len < CAPTURE_SIZE) capture[cap_len++] = b;
    else                        cap_lost++;
}

uint32_t capture_len(void)  { return cap_len; }
uint32_t capture_lost(void) { return cap_lost; }
