import os, termios, time, select, re

DEV = "/dev/cu.usbmodem11101"

class Rig:
    def __init__(self, dev=DEV):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[6] = list(a[6]); a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        time.sleep(0.2); self.drain()

    def drain(self):
        try: os.read(self.fd, 65536)
        except BlockingIOError: pass

    def cmd(self, s, wait=0.25):
        os.write(self.fd, s.encode() + b"\n")
        buf, end = b"", time.time() + wait
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try: buf += os.read(self.fd, 65536)
                except BlockingIOError: pass
        return buf.decode("ascii", "replace")

    def status(self):
        t = self.cmd("s")
        d = {}
        m = re.search(r"captured (\d+) bytes, (\d+) lost", t)
        if m: d["captured"], d["lost"] = int(m[1]), int(m[2])
        m = re.search(r"uart\s+(\d+) framing, (\d+) overrun", t)
        if m: d["framing"], d["overrun"] = int(m[1]), int(m[2])
        m = re.search(r"bus\s+(\w+), (\d+) cycles, last addr ([0-9A-F]{4})", t)
        if m: d["bus"], d["cycles"], d["addr"] = m[1], int(m[2]), int(m[3], 16)
        m = re.search(r"state\s+(\w+)", t)
        if m: d["state"] = m[1]
        return d


# Used for all of the hardware testing. Example:
#
#   python3 -c "
#   from rig import Rig
#   import time
#   r = Rig()
#   r.cmd('c'); r.cmd('g c000')
#   time.sleep(3.2)
#   print(r.status())
#   "
#
# capture.py needs pyserial; this does not, which is why the testing used it.
