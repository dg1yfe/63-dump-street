// HD6301 bus emulator, loader and dump capture for the Raspberry Pi Pico 2.
//
// The Pico generates the 6301's clock and reset, answers every read cycle out
// of a 64 KB memory image, captures whatever the target sends on its SCI, and
// exposes all of it over one USB CDC.
//
// The target is strapped in mode 0 (multiplexed test), where internal ROM is
// enabled at $F000-$FFFF but the reset vector at $FFFE is fetched externally
// for the first 3 or 4 cycles after RES rises. That is what lets this rig
// start the CPU anywhere and then read the chip's own mask ROM back out.

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"

#include "bus.pio.h"
#include "cmd.h"
#include "mem.h"

#define EXTAL_HZ        1000000u   // -> E = EXTAL/4 = 250 kHz
#define SCI_BAUD          15625u   // E/16, the fastest the 6301 SCI offers
#define RESET_POR_MS         50    // 2.8 wants at least 20 ms at power-on
#define RESET_PULSE_MS        1    // 2.8 wants 3 E cycles (12 us) to re-reset
#define AS_TIMEOUT_MS       200
#define NMI_CYCLES_DEFAULT    4    // E cycles to hold NMI low

static PIO  bus_pio = pio0;
static uint sm_clk, sm_bus;
static uint off_clk, off_bus;

static volatile bool     running;
static volatile bool     resync_req;
static volatile uint32_t bus_cycles;   // core1 only writes, core0 only reads
static volatile uint16_t last_addr;    // ditto: where the target last looked

static uint32_t        as_mark;
static absolute_time_t as_deadline;
static bool            as_pending;

// --- core1: answer bus cycles -----------------------------------------------
//
// Polling rather than blocking, so a halt or run can resynchronise the state
// machine. That resync matters: the SM does `in` and `pull` as separate steps,
// and a reset landing between them would leave the two FIFOs one entry out of
// step for good, handing every cycle the answer to its predecessor.

static void core1_main(void) {
    while (true) {
        if (resync_req) {
            pio_sm_set_enabled(bus_pio, sm_bus, false);
            pio_sm_clear_fifos(bus_pio, sm_bus);
            pio_sm_restart(bus_pio, sm_bus);
            pio_sm_exec(bus_pio, sm_bus, pio_encode_jmp(off_bus));
            pio_sm_set_enabled(bus_pio, sm_bus, true);
            resync_req = false;
        }
        if (!pio_sm_is_rx_fifo_empty(bus_pio, sm_bus)) {
            uint32_t a = pio_sm_get(bus_pio, sm_bus);
            pio_sm_put(bus_pio, sm_bus, mem[a & 0xFFFFu]);
            last_addr = (uint16_t)a;
            bus_cycles++;
        }
    }
}

// --- target control ---------------------------------------------------------
//
// RES is open drain: the Pico only ever sinks it. The 6301 needs Vcc-0.5 =
// 4.5 V for a high, which a 3.3 V push-pull drive cannot reach, so the high
// level comes from the external pull-up to 5 V.

static void res_assert(void) {
    gpio_put(PIN_RES, 0);
    gpio_set_dir(PIN_RES, GPIO_OUT);
}

static void res_release(void) {
    gpio_set_dir(PIN_RES, GPIO_IN);
}

static void bus_resync(void) {
    resync_req = true;
    while (resync_req) tight_loop_contents();
}

void target_halt(void) {
    res_assert();
    running    = false;
    as_pending = false;
    bus_resync();
}

void target_run(void) {
    res_assert();
    bus_resync();
    capture_reset();
    rig.as_seen = false;
    sleep_ms(RESET_PULSE_MS);

    // The bus state machine is already running, which it must be: in mode 0
    // the vector fetch happens within 3 or 4 cycles of RES rising.
    as_mark    = bus_cycles;
    as_deadline = make_timeout_time_ms(AS_TIMEOUT_MS);
    as_pending = true;
    res_release();
    running = true;
}

bool     target_running(void)  { return running; }
uint32_t target_bus_cycles(void) { return bus_cycles; }
uint16_t target_last_addr(void)  { return last_addr; }

// NMI is edge sensitive on the falling edge and, per 2.7, "sampled by internal
// clock" - so the low time has to span at least one sample. The width is given
// in E cycles rather than microseconds precisely because `k` can take E down
// to 286 Hz, where any fixed microsecond figure would vanish entirely.
//
// Push-pull, unlike RES and EXTAL. Those need Vcc-0.5 and Vcc*0.7, which 3.3 V
// cannot reach; NMI is an "Other Input" at VIH = 2.0 V, so driving it directly
// is in spec and cannot leave the line floating into a spurious interrupt.
uint32_t target_nmi(uint32_t e_cycles) {
    if (e_cycles == 0u)    e_cycles = NMI_CYCLES_DEFAULT;
    if (e_cycles > 1000u)  e_cycles = 1000u;

    uint32_t e_hz = (rig.extal_hz ? rig.extal_hz : EXTAL_HZ) / 4u;
    uint32_t us = (uint32_t)(((uint64_t)e_cycles * 1000000u + e_hz - 1u) / e_hz);
    if (us < 2u) us = 2u;

    gpio_put(PIN_NMI, 0);
    busy_wait_us(us);
    gpio_put(PIN_NMI, 1);

    rig.nmi_count++;
    return us;
}

// The SCI rate follows the clock: baud = E/16 = EXTAL/64 = clk_sys/(128*N).
// The PL011 divisor works out to exactly 8*N whenever clk_peri equals clk_sys,
// which is an integer for every N - so the target's clock and the receiver
// stay exactly matched at any setting, with no fractional divisor on either
// side. Below about N = 8191 the divisor no longer fits and capture stops
// decoding, which `s` reports rather than hiding.
static void sci_retune(uint32_t n) {
    uint64_t div64 = ((uint64_t)8u * n * clock_get_hz(clk_peri) * 64u)
                   / clock_get_hz(clk_sys);
    uint32_t ibrd = (uint32_t)(div64 >> 6);
    uint32_t fbrd = (uint32_t)(div64 & 0x3Fu);

    rig.sci_clamped = false;
    if (ibrd > 65535u) { ibrd = 65535u; fbrd = 63u; rig.sci_clamped = true; }
    if (ibrd == 0u)    { ibrd = 1u;     fbrd = 0u;  rig.sci_clamped = true; }

    uart_get_hw(uart0)->ibrd = ibrd;
    uart_get_hw(uart0)->fbrd = fbrd;
    // The PL011 latches the divisor on the next LCR_H write, so write it back.
    uart_get_hw(uart0)->lcr_h = uart_get_hw(uart0)->lcr_h;

    rig.sci_baud = (uint32_t)(((uint64_t)clock_get_hz(clk_peri) * 4u)
                              / (64u * ibrd + fbrd));
}

// Retune EXTAL at runtime. The target is halted first: changing E mid-run
// would leave the SCI part-way through a character at the old rate, and the
// divider restart can emit a short period.
//
// Deliberately unclamped. The HD6301V1's 100 kHz floor exists because parts of
// the core are dynamic rather than static, and walking below it to watch that
// happen is a legitimate thing to want; `s` says how far out of spec a setting
// is instead of refusing it. The only hard limits here are the PIO divider's
// own 1..65535, which bottom out around 1.14 kHz EXTAL - E of 286 Hz, a 3.5 ms
// bus cycle, some 350x slower than the datasheet minimum.
uint32_t target_set_extal(uint32_t hz) {
    if (hz == 0u) return rig.extal_hz;
    target_halt();

    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t n = (sys / 2u + hz / 2u) / hz;    // two instructions per period
    if (n < 1u)     n = 1u;
    if (n > 65535u) n = 65535u;

    pio_sm_set_enabled(bus_pio, sm_clk, false);
    pio_sm_set_clkdiv_int_frac(bus_pio, sm_clk, (uint16_t)n, 0);
    pio_sm_clkdiv_restart(bus_pio, sm_clk);    // integer only: a fractional
    pio_sm_set_enabled(bus_pio, sm_clk, true); // divider would jitter the duty

    rig.extal_div = n;
    rig.extal_hz  = sys / (2u * n);
    sci_retune(n);
    return rig.extal_hz;
}

// --- main -------------------------------------------------------------------

static void drain_uart(void) {
    while (!(uart_get_hw(uart0)->fr & UART_UARTFR_RXFE_BITS)) {
        uint32_t dr = uart_get_hw(uart0)->dr;
        // The PL011 reports per-character errors in the top of the data
        // register. A framing count above zero is the signature of a baud
        // mismatch, which otherwise looks exactly like a chip returning junk.
        if (dr & UART_UARTDR_FE_BITS) rig.uart_framing++;
        if (dr & UART_UARTDR_OE_BITS) rig.uart_overrun++;
        capture_put((uint8_t)dr);
    }
}

int main(void) {
    set_sys_clock_khz(150000, true);

    stdio_init_all();
    // Without this the USB driver turns every $0A in a binary read-back into
    // $0D $0A - silently, and only for images that happen to contain one.
    stdio_set_translate_crlf(&stdio_usb, false);

    mem_init();
    cmd_init();

    // Hold the target down before anything else comes up.
    gpio_init(PIN_RES);
    gpio_disable_pulls(PIN_RES);   // an internal pull would fight the 5 V one
    res_assert();

    // NMI idles high and must already be there at reset, or the part takes an
    // interrupt the moment it starts.
    gpio_init(PIN_NMI);
    gpio_disable_pulls(PIN_NMI);
    gpio_put(PIN_NMI, 1);
    gpio_set_dir(PIN_NMI, GPIO_OUT);

    off_clk = pio_add_program(bus_pio, &extal_clk_program);
    sm_clk  = (uint)pio_claim_unused_sm(bus_pio, true);
    extal_clk_program_init(bus_pio, sm_clk, off_clk, PIN_EXTAL, EXTAL_HZ);

    off_bus = pio_add_program(bus_pio, &bus_slave_program);
    sm_bus  = (uint)pio_claim_unused_sm(bus_pio, true);
    bus_slave_program_init(bus_pio, sm_bus, off_bus);

    multicore_launch_core1(core1_main);

    gpio_set_function(PIN_SCI_RX, GPIO_FUNC_UART);
    uart_init(uart0, SCI_BAUD);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(uart0, false, false);
    uart_set_fifo_enabled(uart0, true);

    // Sets the PIO divider and the matching UART divisor from one place, so
    // the two can never disagree about what rate the link is running at.
    target_set_extal(EXTAL_HZ);

    sleep_ms(RESET_POR_MS);
    target_run();                  // power-on run of the built-in image

    while (true) {
        drain_uart();

        int c;
        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
            cmd_feed((uint8_t)c);

        if (as_pending && time_reached(as_deadline)) {
            as_pending  = false;
            rig.as_seen = (bus_cycles != as_mark);
            if (!rig.as_seen)
                puts("ERR target not running - no bus activity");
        }
    }
}
