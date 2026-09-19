# 63 Dump Street — the Pico 2 side

A Raspberry Pi Pico 2 generates the HD6301V1's clock and reset, emulates its
whole 64 KB external address space over the multiplexed bus, captures whatever
the target sends on its SCI, and exposes all of it over one USB CDC.

The target is strapped permanently in **mode 0** (multiplexed test): internal
ROM enabled at $F000–$FFFF, and the reset vector at $FFFE fetched *externally*
for the first 3–4 cycles after RES rises. That is what makes the rig work —
the Pico supplies the vector and the program, and what comes back is the chip's
own mask ROM.

## Wiring

| Pico | Dir | Signal | HD6301 pin |
|---|---|---|---|
| GP0–GP7 | bidir | P30–P37 = D0–D7 muxed with A0–A7 | 37,36,35,34,33,32,31,30 |
| GP8–GP15 | in | P40–P47 = A8–A15 | 29,28,27,26,25,24,23,22 |
| GP16 | in | SC1 = AS | 39 |
| GP17 | in | P24 = SCI TX → UART0 RX | 12 |
| GP18 | in | E | 40 |
| GP19 | in | SC2 = R/W | 38 |
| GP20 | open-drain | EXTAL, 1 MHz, **680 Ω to +5 V** | 3 |
| GP21 | open-drain | RES, **pull-up to +5 V** | 6 |

Straps: P20/P21/P22 (pins 8, 9, 10) to GND for mode 0. XTAL (pin 2) left open,
as §2.9 requires when EXTAL is driven externally. Verify Vcc/Vss/STBY against
the datasheet before powering anything.

### Why those two lines are open-drain

At Vcc = 5 V the 6301 needs V<sub>IH</sub> = Vcc−0.5 = **4.5 V** on RES and
Vcc×0.7 = **3.5 V** on EXTAL. A 3.3 V push-pull output reaches neither, so the
Pico only ever sinks those pins and the pull-up supplies the high level.
Everything else it drives (D0–D7) is an "Other Input" at 2.0 V and goes direct.
The other direction needs nothing: RP2350 GPIOs are 5 V tolerant on non-ADC
pins with VIO powered, and this map uses GP0–GP21 only.

The pull-up also sets the clock ceiling. Time above threshold is `T/2 − t_rise`
while `t_rise` stays fixed at ~30–48 ns, so duty falls as EXTAL rises: 46.7 % at
1 MHz with 680 Ω, but 42.8 % at 2 MHz and 35.6 % at 4 MHz — both outside the
45–55 % the datasheet requires. Beyond ~1 MHz this scheme has to be replaced by
a 5 V HCT buffer. E = 250 kHz also keeps tcyc at 4 µs, mid-range in the 1–10 µs
window, rather than the 8 µs that E = 125 kHz would give against a 10 µs limit.

### Why R/W is wired

The 6301 puts *internal* accesses on the external bus too, so the Pico sees
cycles it must not answer into. Handbook §III.4.3: address and control lines
are always output regardless of internal or external access; during writes to
internal space the same data is driven onto the data bus, and **during reads the
data bus goes high impedance**. So an internal write would collide — R/W stops
the PIO driving — while an internal read is harmless, the CPU ignoring whatever
the emulator put there. That is why the Pico can answer every read cycle
uniformly and needs no address map at all.

## Host protocol

One USB CDC, line-oriented. Everything emitted is ASCII lines; the only binary
is the payload after `b`'s `LEN <n>` line, read as exactly n bytes. The dump is
buffered on the Pico rather than streamed, so binary never shares the wire with
a response and no framing or escaping is needed anywhere.

| Input | Action |
|---|---|
| `:...` | Intel HEX record, loaded into emulated memory |
| `h` | halt — assert RES and hold it |
| `r` | reset and run from the vector at $FFFE; clears the capture |
| `g <addr>` | set the vector at $FFFE to `<addr>` (hex), then run |
| `s` | status: state, capture, load totals, UART errors, bus, clock |
| `d [addr]` | read the capture back as Intel HEX, default base `F000` |
| `b` | read the capture back as binary after a `LEN <n>` line |
| `c` | clear the capture buffer |
| `k <hz>` | retune EXTAL (decimal Hz) and halt; the SCI rate follows |
| `?` | command summary |

Addresses are hex, frequencies decimal.

`h` is a reset hold, not a resumable halt: the HD6301V1 has no HALT or MR pin
and cannot have its clock stopped (100 kHz minimum), so register state is lost.

`g` works because of mode 0 — patching $FFFE and pulsing reset starts the CPU
anywhere, and after those first cycles $FFFE reverts to internal ROM so the
patch cannot disturb the dump.

Watch the **framing** counter in `s`. Non-zero is the signature of a baud
mismatch, which otherwise looks exactly like a chip returning garbage.

`s` also reports bus cycles and the last address the target fetched. Together
they say whether it is alive: a running CPU advances both, and one that has
reached `SLP` stops issuing cycles entirely, so a frozen count is the normal
end of a dump rather than a fault.

## Changing the clock at runtime

`k <hz>` rewrites the PIO divider, retunes the UART to match, and leaves the
target halted for a following `g`. EXTAL = 75 MHz / N for integer N only — a
fractional divider would stretch occasional cycles, which is exactly the duty
asymmetry the 45–55 % spec is about.

The arithmetic works out unusually well: the PL011 divisor comes to **exactly
8N** for every N, so the target's clock and the receiver's baud stay exactly
matched at any setting, with no fractional divisor on either side.

| N | EXTAL | E | SCI baud | IBRD |
|---|---|---|---|---|
| 75 | 1.000 MHz | 250 kHz | 15625 | 600 |
| 150 | 500 kHz | 125 kHz | 7812.5 | 1200 |
| 1500 | 50 kHz | 12.5 kHz | 781.25 | 12000 |
| 8191 | 9.16 kHz | 2.29 kHz | 143.1 | 65528 |
| 65535 | 1.14 kHz | 286 Hz | 17.9 | (clamped) |

RMCR stays `$04` throughout — E/16 is a fixed ratio, only E moves under it, so
part 1 never changes.

### Running it below spec on purpose

`k` is deliberately **not** clamped to the datasheet. The HD6301V1's 100 kHz
floor (tcyc ≤ 10 µs) exists because parts of the core are dynamic rather than
static, and watching that fail is a legitimate thing to want to do. `s` says
how far out of spec a setting is instead of refusing it.

The divider bottoms out at N = 65535, i.e. **E ≈ 286 Hz — a 3.5 ms bus cycle,
some 350× slower than the minimum.** Expect charge to leak off dynamic nodes
somewhere well above that: corrupt bytes first, then the CPU wandering off.
This is a logical failure, not an electrical one — the EXTAL edges stay fast
whatever the frequency, so there is no overvoltage and no extra current, and
the part should come back on a reset at a valid rate.

Two things make the experiment legible. The threshold is temperature
dependent, because leakage roughly doubles every 10 °C — cooling the part
should let it run slower, warming it should raise the floor. And the rig
already knows what the answer should be: load the bring-up variant below,
whose expected output is known exactly, then walk `k` down and watch where the
capture stops matching.

One instrumentation limit: below N ≈ 8191 (EXTAL 9.16 kHz) the UART can no
longer reach the implied rate, and `s` says so. Past that point the observable
is bus cycles and the last address, not the SCI.

## Building

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build          # -> build/src/hd6301dump.uf2
```

The built-in memory image is part 1 (`../6301/romdump.asm`), assembled by the
build with `tabasm` and converted by `tools/bin2c.py`. Editing the assembly
rebuilds the firmware.

## Testing without hardware

`test/` builds `cmd.c` and `mem.c` for the host and drives the real line
protocol over a pipe:

```sh
make -C test test
```

This covers Intel HEX in and out, checksum rejection, the type-04 path when a
capture runs past $FFFF, binary payload integrity, and the empty-buffer cases.

## Running a dump

```sh
make -C ../6301 hex
python3 host/capture.py --port /dev/cu.usbmodem* \
        --hex ../6301/romdump.hex --go c000 --out dump.bin
```

Expect 4096 bytes in about 2.6 s at 15625 baud.

**Bring-up run that verifies itself:** assemble a variant with
`DUMPBEG .equ $C000` and `g c000` it. The capture then starts with the
Pico-served memory, whose contents are known exactly from `romdump.lst`, before
crossing into internal ROM at $F000 — and it exercises A14/A15, which a
$F000-only dump never toggles. It is the one way to show the address and data
paths are right rather than merely plausible, since in mode 0 no test pattern
can be planted at $F000.
