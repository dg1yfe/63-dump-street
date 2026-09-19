# 63 Dump Street

Reading the internal mask ROM out of a Hitachi HD6301V1.

The HD6301V1 hides 4 KB of mask ROM at `$F000–$FFFF` that no pin exposes. The
way in is **mode 0** (multiplexed test), where the internal ROM stays enabled
but the reset vector at `$FFFE` is fetched *externally* for the first three or
four cycles after RES rises. Feed the part a vector and a program from outside,
and it will happily read its own ROM out and send it down a serial line.

That takes two halves, and this repository is both of them.

## The two parts

**`6301/` — the program the chip runs.** 31 bytes of HD6301 assembly at
`$C000`: bring up the SCI, walk `$F000` to `$FFFF`, poll TDRE for each byte,
then `SLP`. The loop ends on the `$FFFF → $0000` wrap because `INX` touches
only Z, so it needs no counter and no RAM. Assembled with
[tabasm](https://github.com/dg1yfe/tabasm).

**`pico2/` — the rig that makes it run.** A Raspberry Pi Pico 2 generates
EXTAL and RES, emulates the whole 64 KB address space over the multiplexed bus
with PIO, captures the SCI output into a buffer, and exposes it all over one
USB CDC: Intel HEX in to load programs, commands to halt, reset and `g <addr>`,
and Intel HEX or raw binary out. `k <hz>` retunes the target's clock at
runtime — unclamped, so it can be walked below the datasheet floor to watch a
dynamic core lose its state.

See [`pico2/README.md`](pico2/README.md) for the wiring table, the protocol,
and why RES and EXTAL are open-drain while everything else is direct.

## Quick start

```sh
make -C 6301 verify hex              # assemble and check part 1
cd pico2 && cmake -B build -G Ninja && cmake --build build
make -C test test                    # protocol tests, no hardware needed

python3 host/capture.py --port /dev/cu.usbmodem* \
        --hex ../6301/romdump.hex --go c000 --out dump.bin
```

4096 bytes in about 2.6 s at 15625 baud.

## Layout

| | |
|---|---|
| `6301/` | HD6301 assembly, Makefile, `make verify` |
| `pico2/src/` | firmware: `bus.pio`, `main.c`, `cmd.c`, `mem.c` |
| `pico2/test/` | the protocol built for the host and driven over a pipe |
| `pico2/host/` | `capture.py`, the pyserial front end |
| `pico2/tools/` | `bin2c.py`, part 1's image into the firmware |
| `doc/` | datasheets and the pinout (PDFs are gitignored — large) |

## Status

Both parts build clean and the protocol tests pass. Nothing has been run
against real hardware yet: the EXTAL duty wants measuring at its 3.5 V
threshold rather than at 50 %, and the "target not running" diagnostic is worth
confirming with no chip attached before anything depends on it.
