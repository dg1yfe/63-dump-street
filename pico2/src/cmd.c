// Line parser for the USB CDC: commands plus Intel HEX in and out.
//
// Everything emitted here is ASCII lines. The single exception is the payload
// after `b`'s "LEN n" line, which the host reads as exactly n raw bytes before
// returning to line mode. Because the dump is captured into a buffer rather
// than streamed, no binary ever shares the wire with a response, so there is
// no framing or escaping anywhere in this protocol.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "cmd.h"
#include "mem.h"

rig_stats_t rig;

// A maximal Intel HEX record is ':' + (255 + 5) * 2 = 521 characters.
#define LINE_MAX 550

static char   line[LINE_MAX];
static size_t line_len;
static bool   line_overflow;

// Set by the first record of a load and cleared by its EOF record, so the
// counters `s` reports are the last complete load's rather than a running
// total across every load since power-on.
static bool   load_active;

// --- helpers ----------------------------------------------------------------

static int hexdig(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hexbyte(const char *s) {
    int h = hexdig(s[0]);
    int l = hexdig(s[1]);
    return (h < 0 || l < 0) ? -1 : (h << 4) | l;
}

// Parse an optional whitespace-separated hex argument. Returns false when
// there is no argument at all, so callers can apply a default.
static bool hex_arg(const char *s, uint32_t *out) {
    while (*s == ' ' || *s == '\t') s++;
    if (hexdig(*s) < 0) return false;
    uint32_t v = 0;
    while (hexdig(*s) >= 0) v = (v << 4) | (uint32_t)hexdig(*s++);
    *out = v;
    return true;
}

static bool dec_arg(const char *s, uint32_t *out) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return false;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    *out = v;
    return true;
}

// --- Intel HEX in -----------------------------------------------------------

static void hex_record(const char *s, size_t len) {
    // Reset before validating, not after: a corrupt first record must still
    // be counted, not wiped by the next good one.
    if (!load_active) {
        rig.hex_bytes = rig.hex_records = rig.hex_errors = 0;
        load_active = true;
    }

    // ':' LL AAAA TT <data> CC - every field is two hex digits.
    if (len < 11 || ((len - 1) & 1)) {
        rig.hex_errors++;
        puts("ERR hex malformed");
        return;
    }
    int count = hexbyte(s + 1);
    if (count < 0 || len != 1 + 2 * (size_t)(count + 5)) {
        rig.hex_errors++;
        puts("ERR hex length");
        return;
    }

    // The checksum covers LL through CC inclusive and must sum to zero.
    unsigned sum = 0;
    for (size_t i = 1; i < len; i += 2) {
        int b = hexbyte(s + i);
        if (b < 0) {
            rig.hex_errors++;
            puts("ERR hex digit");
            return;
        }
        sum += (unsigned)b;
    }
    if ((sum & 0xFFu) != 0) {
        rig.hex_errors++;
        puts("ERR hex checksum");
        return;
    }

    int hi = hexbyte(s + 3), lo = hexbyte(s + 5), type = hexbyte(s + 7);
    uint16_t addr = (uint16_t)((hi << 8) | lo);

    switch (type) {
    case 0x00:
        // Written straight into the emulated memory, no staging copy. The
        // 16-bit wrap is the 6301's own behaviour.
        for (int i = 0; i < count; i++)
            mem[(uint16_t)(addr + i)] = (uint8_t)hexbyte(s + 9 + 2 * i);
        rig.hex_bytes += (uint32_t)count;
        rig.hex_records++;
        break;

    case 0x01:
        load_active = false;
        printf("OK loaded %u bytes in %u records, %u errors\n",
               (unsigned)rig.hex_bytes, (unsigned)rig.hex_records,
               (unsigned)rig.hex_errors);
        break;

    case 0x02: case 0x03: case 0x04: case 0x05:
        // Meaningless for a 64K target: there is nothing above $FFFF to reach,
        // and the start address is set with `g`, not by the file.
        puts("WARN extended/start-address record ignored");
        break;

    default:
        rig.hex_errors++;
        puts("ERR hex record type");
    }
}

// --- Intel HEX out ----------------------------------------------------------

static void hex_out(uint32_t base) {
    uint32_t n = capture_len();
    uint32_t cur_upper = 0xFFFFFFFFu;

    for (uint32_t off = 0; off < n; off += 16) {
        uint32_t len = (n - off < 16) ? n - off : 16;
        uint32_t a   = base + off;
        uint32_t upper = a >> 16;

        // A capture longer than the space left below $FFFF needs type 04
        // rather than a silent 16-bit wrap.
        if (upper != cur_upper) {
            unsigned s4 = 2 + 4 + ((upper >> 8) & 0xFFu) + (upper & 0xFFu);
            printf(":02000004%04X%02X\n", (unsigned)(upper & 0xFFFFu),
                   (unsigned)((0u - s4) & 0xFFu));
            cur_upper = upper;
        }

        uint16_t a16 = (uint16_t)a;
        unsigned sum = len + ((a16 >> 8) & 0xFFu) + (a16 & 0xFFu);
        printf(":%02X%04X00", (unsigned)len, (unsigned)a16);
        for (uint32_t i = 0; i < len; i++) {
            uint8_t b = capture[off + i];
            sum += b;
            printf("%02X", b);
        }
        printf("%02X\n", (unsigned)((0u - sum) & 0xFFu));
    }
    puts(":00000001FF");
}

// --- binary out -------------------------------------------------------------

static void bin_out(void) {
    uint32_t n = capture_len();
    printf("LEN %u\n", (unsigned)n);
    fflush(stdout);
    if (n) {
        fwrite(capture, 1, n, stdout);
        fflush(stdout);
    }
}

// --- status and help --------------------------------------------------------

static void status(void) {
    printf("state    %s\n", target_running() ? "running" : "halted");
    printf("captured %u bytes, %u lost\n",
           (unsigned)capture_len(), (unsigned)capture_lost());
    printf("loaded   %u bytes, %u records, %u errors\n",
           (unsigned)rig.hex_bytes, (unsigned)rig.hex_records,
           (unsigned)rig.hex_errors);
    printf("uart     %u framing, %u overrun\n",
           (unsigned)rig.uart_framing, (unsigned)rig.uart_overrun);
    // Cycles and the last address together say whether the target is alive:
    // a running CPU advances both, and one that has reached SLP stops issuing
    // cycles entirely, so a frozen count is the normal end of a dump.
    printf("bus      %s, %u cycles, last addr %04X\n",
           rig.as_seen ? "active" : "no activity",
           (unsigned)target_bus_cycles(), (unsigned)target_last_addr());
    // The achieved rate, not the requested one: everything downstream depends
    // on it, and a divisor that did not come out exact shows up here.
    uint32_t e = rig.extal_hz / 4u;
    printf("clock    EXTAL %u Hz (div %u), E %u Hz, SCI %u baud\n",
           (unsigned)rig.extal_hz, (unsigned)rig.extal_div,
           (unsigned)e, (unsigned)rig.sci_baud);
    if (e > 1000000u)
        puts("warn     E above the 1 MHz maximum for HD6301V1");
    else if (e < 100000u && e > 0u)
        printf("warn     E below the 100 kHz minimum - tcyc %u us, the core's "
               "dynamic nodes may not hold\n", (unsigned)(1000000u / e));
    if (rig.extal_hz > 1000000u)
        puts("warn     EXTAL above 1 MHz - open-drain duty falls below 45%");
    if (rig.sci_clamped)
        puts("warn     SCI rate outside the UART's range - capture will not decode");
}

static void help(void) {
    puts(":...      Intel HEX record, loaded into emulated memory");
    puts("h         halt - assert RES and hold it");
    puts("r         reset and run from the vector at $FFFE");
    puts("g <addr>  set the vector at $FFFE to <addr>, then run");
    puts("s         status");
    puts("d [addr]  read the capture back as Intel HEX (default base F000)");
    puts("b         read the capture back as binary after a LEN <n> line");
    puts("c         clear the capture buffer");
    puts("k <hz>    retune EXTAL (decimal Hz) and halt; the SCI rate follows");
}

// --- dispatch ---------------------------------------------------------------

static void do_line(void) {
    if (line_overflow) {
        rig.hex_errors++;
        puts("ERR line too long");
        return;
    }
    if (line_len == 0) return;

    if (line[0] == ':') {
        hex_record(line, line_len);
        return;
    }

    uint32_t a;
    switch (line[0]) {
    case 'h':
        target_halt();
        puts("OK halted");
        break;

    case 'r':
        target_run();
        puts("OK running");
        break;

    case 'g':
        if (!hex_arg(line + 1, &a)) {
            puts("ERR g needs a hex address");
            break;
        }
        // In mode 0 the 6301 fetches $FFFE externally for 3 or 4 cycles after
        // RES rises, so patching it here is what makes "go" possible at all.
        mem[0xFFFE] = (uint8_t)(a >> 8);
        mem[0xFFFF] = (uint8_t)a;
        target_run();
        printf("OK running from %04X\n", (unsigned)(a & 0xFFFFu));
        break;

    case 'k':
        if (!dec_arg(line + 1, &a) || a == 0u) {
            puts("ERR k needs a frequency in Hz");
            break;
        }
        printf("OK EXTAL %u Hz, halted\n", (unsigned)target_set_extal(a));
        status();
        break;

    case 's': status(); break;
    case 'd': hex_out(hex_arg(line + 1, &a) ? a : 0xF000u); break;
    case 'b': bin_out(); break;

    case 'c':
        capture_reset();
        puts("OK capture cleared");
        break;

    case '?': help(); break;

    default:
        puts("ERR unknown command, try ?");
    }
}

void cmd_init(void) {
    memset(&rig, 0, sizeof rig);
    line_len = 0;
    line_overflow = false;
    load_active = false;
}

void cmd_feed(uint8_t ch) {
    if (ch == '\r' || ch == '\n') {
        line[line_len] = '\0';
        do_line();
        line_len = 0;
        line_overflow = false;
        return;
    }
    if (line_len + 1 < LINE_MAX) line[line_len++] = (char)ch;
    else                         line_overflow = true;
}
