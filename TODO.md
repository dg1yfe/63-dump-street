# Open items

## When you reassemble the fixture

The fixture was taken apart after the second ROM dump. These need the board
back together, an HD6301 in the socket, and the Pico on USB. Everything below
is verification that is written and builds but has not been run on hardware.

1. Reflash and confirm it enumerates. `HD6301Y0-dev` carries the NMI work.
   ```
   cd pico2 && cmake --build build
   picotool load -f -x build/src/hd6301dump.uf2      # ~/.pico-sdk/.../picotool
   ```
   The port may come back as `usbmodem1101` or `usbmodem11101`; `host/rig.py`
   globs for it now, so that no longer matters.

2. Confirm a clean dump still works, as a baseline:
   `python3 host/rig.py`-driven `g c000`, expect 4096 bytes, sha256 `6321af44`,
   `restart 0`, zero framing errors.

3. **Verify the hostile-handler detector (commit eba9c69, unrun).** This is the
   one piece committed without a hardware run.
   - Load `6301/test/sptest.hex`, `g c000` to plant SP = $B0B0, then
     `g c010 nmi`.
   - Wait past the 500 ms watch window, then `s`.
   - Expect the line `nmi-entry captured (handler kept control)` — the EZA9
     handler resets SP with `LDS #$00FF` and loops, so it must classify as
     CAPTURED. If it says "not taken", the NMI edge was not seen (check the
     NMI wiring, GP22 -> pin 4). If it says "reached entry", the handler came
     back via a reset (Port 1 bit 1 toggling) - real, but not the expected
     path; note it and look at the trace.
   - Push `HD6301Y0-dev` once the classification is confirmed.

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
