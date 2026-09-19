// Host harness for the rig's line protocol.
//
// Runs the real cmd.c and mem.c against stdin/stdout so the Intel HEX parser
// and both read-back formats can be tested without a Pico or a 6301. The
// target-control calls become flags; everything else is the shipping code.
//
//   cmdsim [capture-preload|-] [memory-dump-out]

#include <stdio.h>
#include <stdlib.h>

#include "cmd.h"
#include "mem.h"

static bool running_flag;

void target_halt(void)    { running_flag = false; }
void target_run(void)     { running_flag = true; capture_reset(); }
bool target_running(void) { return running_flag; }

uint32_t target_bus_cycles(void) { return 0; }
uint16_t target_last_addr(void)  { return 0; }

// Mirrors the firmware's arithmetic without any hardware: EXTAL = 75 MHz / N
// for integer N, and the matching PL011 divisor is exactly 8N.
uint32_t target_set_extal(uint32_t hz) {
    if (hz == 0u) return rig.extal_hz;
    running_flag = false;
    uint32_t n = (75000000u + hz / 2u) / hz;
    if (n < 1u)     n = 1u;
    if (n > 65535u) n = 65535u;
    rig.extal_div   = n;
    rig.extal_hz    = 75000000u / n;
    rig.sci_baud    = rig.extal_hz / 64u;
    rig.sci_clamped = (8u * n) > 65535u;
    return rig.extal_hz;
}

int main(int argc, char **argv) {
    mem_init();
    cmd_init();

    // Preload the capture buffer, standing in for bytes the 6301 would have
    // sent, so the `d` and `b` read-backs have something to return.
    if (argc > 1 && argv[1][0] != '-') {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        int c;
        while ((c = fgetc(f)) != EOF) capture_put((uint8_t)c);
        fclose(f);
    }

    int c;
    while ((c = getchar()) != EOF) cmd_feed((uint8_t)c);

    if (argc > 2) {
        FILE *f = fopen(argv[2], "wb");
        if (!f) { perror(argv[2]); return 2; }
        fwrite(mem, 1, MEM_SIZE, f);
        fclose(f);
    }
    return 0;
}
