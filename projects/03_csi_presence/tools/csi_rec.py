#!/usr/bin/env python3
"""Record the 03_csi_presence console on the Mac without resetting the board.

    tools/csi_rec.py OUT [--seconds N] [--raw] [--port /dev/cu.usbmodemXXXX]

Every line is written as "<unix time> <console line>". To label a stretch of the recording,
append "<unix time> # <label>" to OUT from anywhere, e.g.
    echo "$(date +%s) # empty" >> OUT
(OUT is opened for append, so those lines land in order). --raw switches the per-frame I/Q
dump on for the recording and off again at the end. Stops after --seconds, or on Ctrl-C /
SIGTERM, and prints a summary to stderr.

Not pyserial: opening the port with it pulses RTS/DTR, which resets the C6 (see the
xiao-debug skill). This opens the tty the way attach.sh does and clears HUPCL, so closing it
doesn't reset the board either.
"""
import argparse
import glob
import os
import re
import select
import signal
import sys
import termios
import time

RAW_STATE = re.compile(rb"raw_dump=(\d)")
RAW_SEQ = re.compile(rb"^raw s=(\d+) ")


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no /dev/cu.usbmodem* port: is the board plugged in?")
    return ports[0]


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0  # iflag: no CR/LF translation, no software flow control
    a[1] = 0  # oflag: raw output
    a[2] = (a[2] & ~termios.HUPCL) | termios.CLOCAL | termios.CREAD | termios.CS8
    a[3] = 0  # lflag: no echo (an echo would feed our own output back as commands)
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


class Recorder:
    def __init__(self, fd, out):
        self.fd = fd
        self.out = out
        self.buf = b""
        self.raw_state = None  # last raw_dump=N the board reported
        self.n_lines = self.n_raw = self.gaps = 0
        self.last_seq = None

    def pump(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return
        try:
            chunk = os.read(self.fd, 65536)
        except BlockingIOError:
            return
        now = time.time()
        self.buf += chunk
        *lines, self.buf = self.buf.split(b"\n")
        for line in lines:
            line = line.rstrip(b"\r")
            self.n_lines += 1
            m = RAW_SEQ.match(line)
            if m:
                self.n_raw += 1
                seq = int(m.group(1))
                if self.last_seq is not None and seq > self.last_seq + 1:
                    self.gaps += seq - self.last_seq - 1
                self.last_seq = seq
            else:
                m = RAW_STATE.search(line)
                if m:
                    self.raw_state = int(m.group(1))
            self.out.write(f"{now:.3f} {line.decode('utf-8', 'replace')}\n")

    def set_raw(self, want):
        """'c' toggles the dump; send it until the board reports the state we want."""
        for _ in range(3):
            self.raw_state = None
            os.write(self.fd, b"c")
            deadline = time.time() + 2
            while self.raw_state is None and time.time() < deadline:
                self.pump(0.1)
            if self.raw_state == want:
                return True
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=0, help="0 = until Ctrl-C / SIGTERM")
    ap.add_argument("--raw", action="store_true", help="per-frame I/Q dump while recording")
    ap.add_argument("--port")
    args = ap.parse_args()

    stop = False

    def on_signal(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    fd = open_port(args.port or find_port())
    t0 = time.time()
    with open(args.out, "a", buffering=1) as out:
        rec = Recorder(fd, out)
        out.write(f"{t0:.3f} # rec start raw={int(args.raw)}\n")
        if args.raw and not rec.set_raw(1):
            print("warning: the board never confirmed raw_dump=1", file=sys.stderr)
        while not stop and (args.seconds <= 0 or time.time() - t0 < args.seconds):
            rec.pump(0.2)
        if args.raw and not rec.set_raw(0):
            print("warning: the board never confirmed raw_dump=0", file=sys.stderr)
        rec.pump(0.3)
        out.write(f"{time.time():.3f} # rec stop\n")
    os.close(fd)
    print(f"{time.time() - t0:.0f} s, {rec.n_lines} lines, {rec.n_raw} raw, "
          f"{rec.gaps} raw lines missing -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
