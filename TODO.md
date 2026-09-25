# Open items

## NMI hostile-handler detector — verified

Done. `g <addr> nmi` classifies the target's NMI handler: CAPTURED (handler kept
control), REACHED (returned to the injected entry), or NOT TAKEN ($FFFC never
fetched). Confirmed live on the EZA9 mask ROM, which reports CAPTURED — its
handler runs `LDS #$00FF` first and never returns through the stack.

Only CAPTURED and the idle-clear case are exercisable on this hardware. REACHED
needs a cooperative handler and NOT TAKEN needs NMI unwired, neither reachable
with a fixed hostile mask ROM whose NMI vector is internal.

## Part 1 (`6301/`)

`SLP` can truncate the final character. One dump in ~150 ended at 4095 bytes.
This SCI has no transmit-complete flag, so the fix is a delay of about one
character time before `SLP`, or not sleeping at all.

## Part 2 (`pico2/`)

`host/capture.py` has never been run. It needs pyserial, which is not installed
here, so all of the hardware testing used `host/rig.py` instead — same
protocol, but it talks to the port through termios directly.

The `restart` counter in `s` should stay at 0. It counts resets that had to be
repeated, and it was the canary that found the floating mode-strap ground. If
it starts climbing again, something in the strapping or the reset is coming
loose.

## Hardware

The EXTAL kick is worth doing and is not done. Instead of releasing the pin and
letting the pull-up do the whole rise, drive it high for about two PIO cycles
first, then release. That roughly halves `t_rise` and brings 4 MHz EXTAL inside
the 45–55 % duty window, which 470 Ω alone does not. It needs
`GPIO_DRIVE_STRENGTH_12MA` and a scope on the 3.5 V threshold to tune: too
short and it buys nothing, too long and the pin sits at 3.3 V — below
threshold — and the duty gets worse than doing nothing.

## `HD6301Y0-dev`

The entry sequence is written and has never met a Y0. What is still missing is
the re-pinning for the non-multiplexed bus:

- port 1 = A0–A7, port 4 = A8–A15, port 3 = data, port 7 carries R/W
- MP0/MP1 are dedicated mode pins, so the chip can never drive them and they
  are safe on the ADC pins along with NMI
- only 10 address bits fit: 23 tolerant pins cover 8 data + 10 address + E +
  R/W + SCI TX + EXTAL + RES, leaving GP26–28 for NMI and the two mode pins
- the dumper, its slide and the stack fill must then share a 1 KB window
  without colliding mod 1024

## Reference

| | |
|---|---|
| dump | 4096 bytes, sha256 `6321af4498939a4264aa32ccb6c5d6088fb8c238aeb06536889f6acbd6053776` |
| default clock | EXTAL 1 MHz, E 250 kHz, 15625 baud, 2.6 s per dump |
| tested to | EXTAL 3.95 MHz, E 987 kHz, 61 696 baud, 0.7 s per dump |
| release | `26.9.1` |
