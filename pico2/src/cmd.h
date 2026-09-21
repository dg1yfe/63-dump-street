#ifndef CMD_H
#define CMD_H

#include <stdbool.h>
#include <stdint.h>

// Counters shown by the `s` command. The HEX fields are owned by cmd.c, the
// UART and bus fields by main.c.
typedef struct {
    uint32_t hex_bytes;     // data bytes written by the current load
    uint32_t hex_records;
    uint32_t hex_errors;    // malformed or bad-checksum records, each skipped
    uint32_t uart_framing;  // non-zero means the baud rate is wrong
    uint32_t uart_overrun;
    bool     as_seen;       // bus activity observed since the last start
    uint32_t sci_baud;      // rate the UART actually achieved, set by main.c
    uint32_t extal_hz;      // clock fed to the target, set by main.c
    uint32_t extal_div;     // PIO divider behind it
    bool     sci_clamped;   // the UART could not reach the rate E/16 implies
    uint32_t nmi_count;     // NMI pulses issued since power-on
    uint32_t restarts;      // resets that had to be repeated to take
} rig_stats_t;

extern rig_stats_t rig;

// Implemented in main.c: the parser drives the target through these.
void     target_halt(void);
void     target_run(void);
void     target_run_nmi(int32_t nmi_after_cycles);  // <0 = no NMI
bool     target_running(void);
uint32_t target_set_extal(uint32_t hz);   // returns the rate actually set
uint32_t target_bus_cycles(void);
uint16_t target_last_addr(void);
uint32_t target_nmi(uint32_t e_cycles);   // returns the pulse width in us
void     target_pin_survey(void);          // measure what the input pins are doing
void     target_trace(void);               // addresses around an interrupt frame
void     target_trace_arm(uint16_t addr);  // start the trace at this address

void cmd_init(void);
void cmd_feed(uint8_t ch);

#endif
