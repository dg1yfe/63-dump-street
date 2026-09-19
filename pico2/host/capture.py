#!/usr/bin/env python3
"""Drive the Pico 2 HD6301 rig: halt, load, go, capture, read back.

The rig speaks one line-oriented protocol over its USB CDC. Everything it
emits is ASCII lines except the payload after `b`'s "LEN <n>" line, which is
exactly n raw bytes. Nothing is streamed while the target runs - the dump is
buffered on the Pico and fetched afterwards - so no framing is needed.

  python3 capture.py --port /dev/cu.usbmodem1101 \
                     --hex ../../6301/romdump.hex --go c000 --out dump.bin
"""

import argparse
import hashlib
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")


class Rig:
    def __init__(self, port, timeout=2.0):
        # USB CDC ignores the line rate; the value is a formality.
        self.ser = serial.Serial(port, 115200, timeout=timeout)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def send(self, line):
        self.ser.write((line + "\n").encode("ascii"))
        self.ser.flush()

    def line(self, timeout=2.0):
        self.ser.timeout = timeout
        raw = self.ser.readline()
        return raw.decode("ascii", "replace").strip() if raw else None

    def expect(self, prefix, timeout=2.0):
        """Read lines until one starts with prefix. Reports ERR lines as they
        appear - including the spontaneous 'target not running' diagnostic."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            ln = self.line(timeout=max(0.05, deadline - time.time()))
            if ln is None:
                continue
            if ln.startswith("ERR"):
                print(f"  rig: {ln}", file=sys.stderr)
            if ln.startswith(prefix):
                return ln
        raise TimeoutError(f"no line starting with {prefix!r}")

    def status(self):
        self.send("s")
        fields = {}
        for _ in range(5):
            ln = self.line()
            if not ln:
                break
            key, _, rest = ln.partition(" ")
            fields[key] = rest.strip()
        return fields

    def captured(self):
        st = self.status()
        # "captured 4096 bytes, 0 lost"
        try:
            return int(st.get("captured", "0").split()[0])
        except (ValueError, IndexError):
            return 0

    def read_binary(self, timeout=10.0):
        self.send("b")
        ln = self.expect("LEN", timeout=timeout)
        n = int(ln.split()[1])
        if n == 0:
            return b""
        self.ser.timeout = timeout
        data = self.ser.read(n)
        if len(data) != n:
            raise IOError(f"short read: {len(data)} of {n} bytes")
        return data


def load_hex(rig, path):
    sent = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            rig.send(line)
            sent += 1
            if line.upper().startswith(":00000001"):
                print(f"  {rig.expect('OK loaded')}")
                return sent
            # Records only answer on error, so drain anything waiting.
            while rig.ser.in_waiting:
                ln = rig.line(timeout=0.2)
                if ln and ln.startswith(("ERR", "WARN")):
                    print(f"  rig: {ln}", file=sys.stderr)
    return sent


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial device of the rig")
    ap.add_argument("--hex", help="Intel HEX file to load before running")
    ap.add_argument("--go", metavar="ADDR", help="run from this hex address")
    ap.add_argument("--out", help="write the captured bytes here")
    ap.add_argument("--expect", type=int, default=4096,
                    help="bytes expected; capture ends early once reached")
    ap.add_argument("--idle", type=float, default=1.0,
                    help="seconds without new bytes that end the capture")
    ap.add_argument("--settle", type=float, default=30.0,
                    help="overall capture timeout in seconds")
    args = ap.parse_args()

    rig = Rig(args.port)

    print("halting target")
    rig.send("h")
    print(f"  {rig.expect('OK')}")

    if args.hex:
        print(f"loading {args.hex}")
        n = load_hex(rig, args.hex)
        print(f"  {n} records sent")

    if args.go:
        print(f"starting at {args.go}")
        rig.send(f"g {args.go}")
        print(f"  {rig.expect('OK')}")
    else:
        print("running")
        rig.send("r")
        print(f"  {rig.expect('OK')}")

    print("capturing")
    last, last_change = 0, time.time()
    deadline = time.time() + args.settle
    while time.time() < deadline:
        n = rig.captured()
        if n != last:
            last, last_change = n, time.time()
            print(f"  {n} bytes", end="\r", flush=True)
        if n >= args.expect:
            break
        if n and time.time() - last_change > args.idle:
            print(f"\n  idle for {args.idle}s, stopping at {n} bytes")
            break
        time.sleep(0.1)
    print(f"  {last} bytes captured")

    data = rig.read_binary()
    print(f"read back {len(data)} bytes")
    if len(data) != last:
        print(f"  WARNING: status said {last}", file=sys.stderr)

    if data:
        print(f"  sha256 {hashlib.sha256(data).hexdigest()}")
        preview = " ".join(f"{b:02x}" for b in data[:16])
        print(f"  first  {preview}")
        preview = " ".join(f"{b:02x}" for b in data[-16:])
        print(f"  last   {preview}")

    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
        print(f"wrote {args.out}")

    return 0 if data else 1


if __name__ == "__main__":
    sys.exit(main())
