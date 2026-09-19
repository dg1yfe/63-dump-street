#!/usr/bin/env python3
"""Protocol tests for the rig, run against the host build of cmd.c/mem.c.

These are the checks the plan calls for with no target attached - Intel HEX in,
Intel HEX and binary out, and the degenerate empty cases, which is where
length-prefixed protocols usually break. Running them here rather than over a
CDC means they actually get run.
"""

import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).parent
SIM = HERE / "cmdsim"
HEXFILE = HERE.parent.parent / "6301" / "romdump.hex"
BINFILE = HERE / "romdump.bin"

failures = []


def check(name, cond, detail=""):
    if cond:
        print(f"  ok    {name}")
    else:
        print(f"  FAIL  {name}  {detail}")
        failures.append(name)


def run(stdin_text, preload=None, memout=None):
    args = [str(SIM), str(preload) if preload else "-"]
    if memout:
        args.append(str(memout))
    p = subprocess.run(args, input=stdin_text.encode(), capture_output=True)
    return p.stdout


def parse_ihex(text):
    """Return {addr: byte} from Intel HEX text, verifying every checksum."""
    out, upper = {}, 0
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        raw = bytes.fromhex(line[1:])
        if (sum(raw) & 0xFF) != 0:
            raise ValueError(f"bad checksum: {line}")
        count, addr, rtype = raw[0], (raw[1] << 8) | raw[2], raw[3]
        data = raw[4:4 + count]
        if rtype == 0:
            for i, b in enumerate(data):
                out[(upper << 16) + addr + i] = b
        elif rtype == 1:
            break
        elif rtype == 4:
            upper = (data[0] << 8) | data[1]
    return out


def main():
    if not SIM.exists():
        sys.exit("cmdsim not built - run `make` first")
    if not HEXFILE.exists():
        sys.exit(f"{HEXFILE} missing - run `make hex` in ../../6301")

    image = BINFILE.read_bytes()
    hex_text = HEXFILE.read_text()

    print("Intel HEX in")
    memout = HERE / "mem.tmp"
    out = run(hex_text, memout=memout).decode()
    check("EOF record reports totals",
          "OK loaded 33 bytes in 3 records, 0 errors" in out, out.strip())
    check("no errors reported", "ERR" not in out, out.strip())
    mem = memout.read_bytes()
    check("code landed at $C000", mem[0xC000:0xC01F] == image[0:0x1F])
    check("vector landed at $FFFE", mem[0xFFFE:0x10000] == b"\xC0\x00")
    check("untouched space stays $FF", mem[0xD000:0xD100] == b"\xFF" * 256)

    print("bad checksum is rejected, not partially applied")
    # Same first record with its checksum decremented by one.
    good = [l for l in hex_text.splitlines() if l.startswith(":")][0]
    bad = good[:-2] + f"{(int(good[-2:], 16) - 1) & 0xFF:02X}"
    out = run(bad + "\n:00000001FF\n", memout=memout).decode()
    check("checksum error reported", "ERR hex checksum" in out, out.strip())
    check("counted as an error", "0 bytes in 0 records, 1 errors" in out, out.strip())
    mem = memout.read_bytes()
    check("no bytes written", mem[0xC000:0xC018] == image[0:0x18],
          "the built-in image at $C000 must be untouched")

    print("malformed records")
    out = run(":xx\n").decode()
    check("short line rejected", "ERR hex malformed" in out, out.strip())
    out = run(":10C00000ZZ\n").decode()
    check("bad length rejected", "ERR hex length" in out, out.strip())
    out = run(":00000009FF\n").decode()
    check("unknown type rejected", "ERR hex" in out, out.strip())

    print("Intel HEX out round-trips")
    payload = bytes((i * 7 + 3) & 0xFF for i in range(4096))
    pre = HERE / "cap.tmp"
    pre.write_bytes(payload)
    out = run("d\n", preload=pre).decode()
    got = parse_ihex(out)
    check("ends with an EOF record", out.strip().endswith(":00000001FF"))
    check("4096 bytes emitted", len(got) == 4096, f"got {len(got)}")
    expect = {0xF000 + i: b for i, b in enumerate(payload)}
    check("bytes and addresses match", got == expect)

    print("Intel HEX out honours an explicit base")
    out = run("d 8000\n", preload=pre).decode()
    got = parse_ihex(out)
    check("based at $8000", min(got) == 0x8000 and max(got) == 0x8FFF)

    print("a capture past $FFFF uses type 04, not a silent wrap")
    big = HERE / "big.tmp"
    big.write_bytes(bytes(range(256)) * 32)          # 8192 bytes
    out = run("d F000\n", preload=big).decode()
    check("type 04 record emitted", ":02000004" in out, out.splitlines()[0])
    got = parse_ihex(out)
    check("addresses continue past $FFFF", max(got) == 0xF000 + 8191,
          f"max {max(got):#x}")

    print("binary out")
    raw = run("b\n", preload=pre)
    head, _, body = raw.partition(b"\n")
    check("LEN line correct", head == b"LEN 4096", head)
    check("payload is exact and unescaped", body == payload,
          f"{len(body)} bytes")

    print("degenerate empty cases")
    out = run("c\nd\n").decode()
    check("empty hex is just the EOF record",
          out.strip().endswith(":00000001FF") and out.count(":") == 1,
          out.strip())
    raw = run("c\nb\n")
    check("empty binary is LEN 0 with no payload after it",
          raw.endswith(b"LEN 0\n"), raw)

    print("runtime clock retuning")
    out = run("k 1000000\n").decode()
    check("1 MHz lands on divider 75",
          "EXTAL 1000000 Hz (div 75), E 250000 Hz, SCI 15625 baud" in out,
          out.strip())
    check("retuning halts the target", "state    halted" in out, out.strip())
    check("in spec, no warning", "warn" not in out, out.strip())

    out = run("k 500000\n").decode()
    check("500 kHz lands on divider 150",
          "EXTAL 500000 Hz (div 150), E 125000 Hz" in out, out.strip())

    out = run("k 0\n").decode()
    check("zero rejected", "ERR k needs a frequency" in out, out.strip())
    out = run("k\n").decode()
    check("missing argument rejected", "ERR k needs a frequency" in out, out.strip())

    print("below the datasheet floor - allowed, but flagged")
    out = run("k 50000\n").decode()
    check("sub-spec E accepted", "EXTAL 50000 Hz (div 1500), E 12500 Hz" in out,
          out.strip())
    check("sub-spec E warns with tcyc",
          "warn     E below the 100 kHz minimum - tcyc 80 us" in out, out.strip())

    out = run("k 1144\n").decode()
    check("divider clamps at 65535", "(div 65535)" in out, out.strip())
    check("slowest setting is ~286 Hz E", "E 286 Hz" in out, out.strip())
    check("unreachable SCI rate is reported",
          "warn     SCI rate outside the UART's range" in out, out.strip())

    out = run("k 2000000\n").decode()
    check("above 1 MHz warns about duty",
          "warn     EXTAL above 1 MHz" in out, out.strip())

    print("NMI pulses, width in E cycles so it tracks the clock")
    out = run("k 1000000\nn\n").decode()
    check("default is 4 E cycles at 250 kHz = 16 us",
          "OK NMI pulsed 16 us" in out, out.strip())
    check("halted target is called out",
          "target halted - it will not be seen" in out, out.strip())

    out = run("k 1000000\nn 100\n").decode()
    check("explicit width honoured", "OK NMI pulsed 400 us" in out, out.strip())

    out = run("k 1144\nn\n").decode()
    check("width scales to the 286 Hz floor",
          "OK NMI pulsed 13987 us" in out, out.strip())

    out = run("k 1000000\nn\nn\ns\n").decode()
    check("pulses are counted", "nmi      2 issued" in out, out.strip())

    print("status and unknown commands")
    out = run("s\n").decode()
    check("status reports halted", "state    halted" in out, out.strip())
    out = run("q\n").decode()
    check("unknown command rejected", "ERR unknown command" in out, out.strip())

    for f in (memout, pre, big):
        f.unlink(missing_ok=True)

    print()
    if failures:
        print(f"{len(failures)} failed: {', '.join(failures)}")
        return 1
    print("all protocol tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
