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
| GP20 | open-drain | EXTAL, 1 MHz, **470 Ω to +5 V** | 3 |
| GP21 | open-drain | RES, **pull-up to +5 V** | 6 |
| GP22 | push-pull | NMI | 4 |

NMI is push-pull, unlike the other two Pico outputs: it is an "Other Input" at
V<sub>IH</sub> = 2.0 V, which 3.3 V clears easily, so it needs no pull-up and —
more to the point — can never be left floating into a spurious interrupt.

These go nowhere near the Pico but decide whether the part runs at all:

| HD6301 pin | | |
|---|---|---|
| 7 | STBY | **to Vcc.** Low stops every clock and holds the part in the reset state (§2.12). Like RES it wants Vcc−0.5 = 4.5 V, so it cannot be driven from the Pico — tie it high. |
| 21 | Vcc | +5 V |
| 1 | Vss | ground |
| 2 | XTAL | leave open, as §2.9 requires when EXTAL is driven externally |
| 5 | IRQ1 | pull up. Harmless while I is set, which reset does and this firmware never undoes — but do not leave it floating for a target running its own code. |
| 8, 9, 10 | P20, P21, P22 | to GND, strapping mode 0. **Check this ground is actually connected.** |
| 12 | P24 | SCI TX. Give it a pull-up, or a pull-down — anything but floating |

Pin numbers throughout are from `../doc/HD6301V-pinout.png` (DP-40).

**The mode straps are the thing to check first when nothing works.** The mode
is latched afresh at *every* reset, so if those pull-downs are not solidly
grounded the part comes up in a different, random mode each time. Only mode 0
fetches `$FFFE` externally; in any other mode the chip boots its own ROM and
the dumper never runs. A floating strap rail cost an evening here, and its
signature is distinctive: the boot trace shows one garbage address and then
silence, with AS stuck high — because the dice came up mode 7, single-chip,
where ports 3 and 4 are plain I/O and there is no external bus at all.

P24 floats until the dumper sets TE and the 6301 takes the pin over, and a
floating line reads as a start bit. A pull-down works — the PL011 treats a
low line as a break and latches it once rather than reporting continuously —
though a pull-up is marginally tidier, since an idle-high line produces no
event at all.

### Why those two lines are open-drain

At Vcc = 5 V the 6301 needs V<sub>IH</sub> = Vcc−0.5 = **4.5 V** on RES and
Vcc×0.7 = **3.5 V** on EXTAL. A 3.3 V push-pull output reaches neither, so the
Pico only ever sinks those pins and the pull-up supplies the high level.
Everything else it drives (D0–D7) is an "Other Input" at 2.0 V and goes direct.
The other direction needs nothing: RP2350 GPIOs are 5 V tolerant on non-ADC
pins with VIO powered, and this map uses GP0–GP22 only.

The pull-up also sets the clock ceiling. Time above threshold is `T/2 − t_rise`,
and `t_rise` is fixed by the RC, so duty falls as EXTAL rises. The resistor is
what buys headroom:

| EXTAL | 1 kΩ | 680 Ω | 470 Ω |
|---|---|---|---|
| 500 kHz | 47.9 % | 48.4 % | 48.9 % |
| 1 MHz | **45.8 %** | 46.7 % | 47.7 % |
| 2 MHz | 41.6 % | 43.4 % | 45.4 % |
| 4 MHz | 33.2 % | 36.8 % | 40.8 % |

1 kΩ at 1 MHz is only just inside the 45 % limit and falls outside it with a
little more wire capacitance, which is reason enough not to use it. 470 Ω also
brings 2 MHz within spec. The cost is sink current: 5 V / 470 Ω is 10.6 mA
while the pin is held low, close to the RP2350's 12 mA per-pin guidance, so
this is about as low as the resistor should go without a buffer. Beyond ~1 MHz this scheme has to be replaced by
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
| `n [cyc]` | pulse NMI low for `<cyc>` E cycles (default 4) |
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

## NMI

`n` drives NMI low for a number of **E cycles**, not microseconds, so the pulse
tracks `k`: 16 µs at E = 250 kHz, 14 ms at the 286 Hz floor, where any fixed
microsecond figure would simply vanish. §2.7 makes the width matter — NMI is
edge sensitive on the falling edge, but the line is "sampled by internal
clock", so the low time has to span a sample. NMI idles high from before reset
is released, since a part that comes out of reset with NMI already low takes
the interrupt immediately.

What it reaches depends on the mode, and in mode 0 it is not a control path:
the NMI vector at `$FFFC/$FFFD` lives inside the enabled internal ROM, so an
NMI runs *the chip's own* handler. Only `$FFFE` is fetched externally, and only
for those first few cycles after RES. In a mode where the vectors are external
— which is the case for variants that have no mode 0 — the Pico supplies
`$FFFC` and `n` becomes a genuine break-in: interrupt the target at will and
land in code you wrote, without losing its state to a reset the way `h` does.

**The register dump it does not yet give you.** NMI stacks PC, X, A, B and CC.
Point SP at external memory and those seven bytes land in the Pico's image —
an exact snapshot of where a wandering CPU was and what it held, which is
precisely what the slow-clock experiment wants. That needs the emulator to
*capture* writes, and it currently does not: the PIO deliberately never drives
on a write cycle and does not sample one either. Adding it means carrying R/W
into the captured word so core1 can tell a read request from write data.

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

### How fast it will actually go

Measured against a real HD6301V1 with the 470 Ω fitted, six dumps at each rate,
every one verified against sha256 `6321af44`:

| EXTAL | E | baud | duty | dump | result |
|---|---|---|---|---|---|
| 1.00 MHz | 250 kHz | 15,625 | 47.7 % | 2.6 s | default |
| 1.97 MHz | 493 kHz | 30,843 | 46.1 % | 1.3 s | 6/6 |
| 2.50 MHz | 625 kHz | 39,064 | 45.0 % | 1.0 s | 6/6 |
| 3.00 MHz | 750 kHz | 46,875 | 44.0 % | 0.9 s | 6/6 |
| 3.57 MHz | 893 kHz | 55,813 | 42.9 % | 0.7 s | 6/6 |
| 3.95 MHz | 987 kHz | 61,696 | 42.1 % | 0.7 s | 6/6 |

Two things that says. The 45–55 % duty requirement has margin in it — the part
reads perfectly at 42.1 %, so it is not the cliff the arithmetic implies. And
the ceiling that stopped the sweep is the **part's own rating**, not anything
here: tcyc ≥ 1 µs puts E at 1.0 MHz, and 987 kHz is already there. The Pico
still holds ~2.8× timing margin at that point and would not become the limit
until roughly E = 1.5–2 MHz.

**The default stays at 1 MHz regardless.** That sweep was taken on a breadboard
with decoupled rails and a 470 Ω pull-up. A scrappier one, or the 1 kΩ, has
less signal integrity to spend — and a dumper that works on bad wiring is worth
more than one that is fast on good wiring. `k` is there when the rig in front of
you justifies it.

## Building

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build          # -> build/src/hd6301dump.uf2
```

No environment setup is needed: the extension block at the top of
`CMakeLists.txt` points the build at `~/.pico-sdk/sdk/2.3.0` itself, so
`PICO_SDK_PATH` does not have to be exported. `tabasm` does have to be on
PATH — the build assembles part 1 (`../6301/romdump.asm`) and converts it with
`tools/bin2c.py` into the firmware's built-in memory image, so editing the
assembly rebuilds the firmware.

Flashing, either way round:

```sh
# hold BOOTSEL while plugging in, then
cp build/src/hd6301dump.uf2 /Volumes/RP2350

# or, with the board already running
~/.pico-sdk/picotool/2.3.0/picotool/picotool load -fx build/src/hd6301dump.uf2
```

`picotool` is not on PATH; it ships inside the VS Code extension's SDK.

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
