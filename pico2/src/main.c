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

static PIO  bus_pio = pio0;
static uint sm_clk, sm_bus;
static uint off_clk, off_bus;

static volatile bool     running;
static volatile bool     resync_req;
static volatile uint32_t bus_cycles;   // core1 only writes, core0 only reads

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

bool target_running(void) { return running; }

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

    off_clk = pio_add_program(bus_pio, &extal_clk_program);
    sm_clk  = (uint)pio_claim_unused_sm(bus_pio, true);
    extal_clk_program_init(bus_pio, sm_clk, off_clk, PIN_EXTAL, EXTAL_HZ);

    off_bus = pio_add_program(bus_pio, &bus_slave_program);
    sm_bus  = (uint)pio_claim_unused_sm(bus_pio, true);
    bus_slave_program_init(bus_pio, sm_bus, off_bus);

    multicore_launch_core1(core1_main);

    gpio_set_function(PIN_SCI_RX, GPIO_FUNC_UART);
    // 150 MHz / (16 * 15625) = 600 exactly, so this lands on the nominal rate
    // with a zero fractional divisor. `s` reports what it actually got.
    rig.sci_baud = uart_init(uart0, SCI_BAUD);
    rig.extal_hz = EXTAL_HZ;
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(uart0, false, false);
    uart_set_fifo_enabled(uart0, true);

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
