#!/usr/bin/env python3
"""Record the 03_csi_presence console on the Mac without resetting the board.

    tools/csi_rec.py OUT [--seconds N] [--raw | --bands] [--port /dev/cu.usbmodemXXXX]

Every line is written as "<unix time> <console line>". OUT may hold strftime codes
("logs/csi-%Y%m%d.rec" gives one file a day). To label a stretch, append
"<unix time> # <label>" to the current file from anywhere:
    echo "$(date +%s) # empty" >> logs/csi-20260929.rec

--raw    turn the board's per-frame I/Q dump on while recording (~170 MB an hour).
--bands  turn it on too, but keep the raw lines out of the file: one "band" line per second
         instead, the fluctuation of each of 16 slices of the channel (~20 MB a day).

Meant to run for days: it reopens the port when it goes away (unplug, Mac sleep) and turns
the dump back on when the board stops sending it (a reboot turns it off). Stops after
--seconds, or on Ctrl-C / SIGTERM, and prints a summary to stderr.

Not pyserial: opening the port with it pulses RTS/DTR, which resets the C6 (see the
xiao-debug skill). This opens the tty the way attach.sh does and clears HUPCL, so closing it
doesn't reset the board either. Stop it before flashing or attaching: two readers split the
bytes between them.
"""
import argparse
import collections
import glob
import os
import re
import select
import signal
import sys
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from csi_motion import Bands, amplitudes, band_line  # noqa: E402

RAW_STATE = re.compile(rb"raw_dump=(\d)")
RAW_LINE = re.compile(rb"^raw s=(\d+) t=(\d+) rssi=-?\d+ nf=-?\d+ fmt=(\d+) rate=\d+ sl=(\d+)"
                      rb" g=(\d) siga1=[0-9a-f]+ fi=(\d) len=(\d+) iq=(\S+)$")
RELOCK = 100  # consecutive frames of another length before the reducer follows them


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise OSError("no /dev/cu.usbmodem* port")
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
    termios.tcflush(fd, termios.TCIFLUSH)  # bytes left over from the last reader
    return fd


class Out:
    """Append-only writer that moves to a new file when the strftime pattern says so."""

    def __init__(self, pattern):
        self.pattern, self.name, self.f = pattern, None, None

    def write(self, t, text):
        name = time.strftime(self.pattern, time.localtime(t))
        if name != self.name:
            if self.f:
                self.f.close()
            if os.path.dirname(name):
                os.makedirs(os.path.dirname(name), exist_ok=True)
            self.f, self.name = open(name, "a", buffering=1), name
        self.f.write(f"{t:.3f} {text}\n")

    def close(self):
        if self.f:
            self.f.close()


class Recorder:
    def __init__(self, out, port, want_raw, bands):
        self.out, self.port, self.want_raw = out, port, want_raw
        self.bands = Bands() if bands else None
        self.bands_k = Bands() if bands else None  # only frames of the locked kind
        self.kind_lock, self.kind_mismatch = None, 0
        self.kinds, self.kinds_sec, self.kinds_done = collections.Counter(), None, None
        self.fd, self.buf = None, b""
        self.raw_state = None
        self.last_raw = self.last_fix = 0.0
        self.lock_len, self.mismatch = None, 0
        self.n_lines = self.n_raw = self.gaps = self.reopens = 0
        self.last_seq = None
        self.down = False

    def note(self, text):
        self.out.write(time.time(), "# " + text)

    def open(self):
        try:
            path = self.port or find_port()
            self.fd = open_port(path)
        except OSError as e:
            if not self.down:
                self.note(f"port unavailable: {e}")
                self.down = True
            return False
        self.note(f"port open {path}")
        self.down = False
        self.reopens += 1
        return True

    def lost(self, why):
        try:
            os.close(self.fd)
        except OSError:
            pass
        self.fd, self.buf, self.last_seq = None, b"", None
        if self.bands:
            self.bands, self.bands_k = Bands(), Bands()  # drop the half-finished second
            self.kinds, self.kinds_sec = collections.Counter(), None
        self.note(why)
        self.down = True

    def pump(self, timeout):
        try:
            r, _, _ = select.select([self.fd], [], [], timeout)
            if not r:
                return
            chunk = os.read(self.fd, 65536)
        except BlockingIOError:
            return
        except OSError as e:
            self.lost(f"port lost: {e}")
            return
        if not chunk:
            self.lost("port lost: end of file")
            return
        now = time.time()
        self.buf += chunk
        *lines, self.buf = self.buf.split(b"\n")
        for line in lines:
            self.handle(now, line.rstrip(b"\r"))

    def handle(self, now, line):
        self.n_lines += 1
        m = RAW_LINE.match(line)
        if not m:
            s = RAW_STATE.search(line)
            if s:
                self.raw_state = int(s.group(1))
            self.out.write(now, line.decode("utf-8", "replace"))
            return
        self.n_raw += 1
        self.last_raw = now
        seq = int(m.group(1))
        if self.last_seq is not None and seq > self.last_seq + 1:
            self.gaps += seq - self.last_seq - 1
        self.last_seq = seq
        if self.bands is None:
            self.out.write(now, line.decode("ascii", "replace"))
            return
        t_us, ln = int(m.group(2)), int(m.group(7))
        if ln != self.lock_len:
            self.mismatch += 1
            if self.lock_len is not None and self.mismatch < RELOCK:
                return
            self.note(f"reducer locked on len={ln}")
            self.lock_len = ln
        self.mismatch = 0
        # Kind of frame: format / group-addressed / MPDU length. The ping reply is 4/0/90.
        kind = f"{int(m.group(3))}/{int(m.group(5))}/{int(m.group(4))}"
        if kind != self.kind_lock:
            self.kind_mismatch += 1
            if self.kind_lock is None or self.kind_mismatch >= RELOCK:
                self.note(f"reducer kind lock {kind}")
                self.kind_lock, self.kind_mismatch = kind, 0
        else:
            self.kind_mismatch = 0
        sec = t_us // 1_000_000
        if sec != self.kinds_sec:
            self.kinds_done, self.kinds, self.kinds_sec = self.kinds, collections.Counter(), sec
        self.kinds[kind] += 1
        amp = amplitudes(m.group(8).decode("ascii"), m.group(6) == b"1")
        if not amp:
            return
        done = self.bands.add(t_us, amp)
        done_k = self.bands_k.add(t_us, amp) if kind == self.kind_lock else self.bands_k.tick(t_us)
        if done:
            self.out.write(now, band_line(done, done_k or {"n": 0, "motion": 0.0,
                                                         "bands": [-1] * 16}, self.kinds_done))

    def set_raw(self, want):
        """'c' toggles the dump; send it until the board reports the state we want."""
        self.last_fix = time.time()
        for _ in range(3):
            if self.fd is None:
                return False
            self.raw_state = None
            try:
                os.write(self.fd, b"c")
            except OSError as e:
                self.lost(f"port lost: {e}")
                return False
            deadline = time.time() + 2
            while self.fd is not None and self.raw_state is None and time.time() < deadline:
                self.pump(0.1)
            if self.raw_state == want:
                return True
        self.note(f"board never confirmed raw_dump={want}")
        return False

    def stats(self):
        return (f"rec stats lines={self.n_lines} raw={self.n_raw} raw_missing={self.gaps} "
                f"port_opens={self.reopens}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", help="file; strftime codes allowed")
    ap.add_argument("--seconds", type=float, default=0, help="0 = until Ctrl-C / SIGTERM")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--raw", action="store_true", help="per-frame I/Q dump in the file")
    mode.add_argument("--bands", action="store_true", help="dump on, 1 band line/s in the file")
    ap.add_argument("--port")
    args = ap.parse_args()
    want_raw = args.raw or args.bands

    stop = False

    def on_signal(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    out = Out(args.out)
    rec = Recorder(out, args.port, want_raw, args.bands)
    t0 = last_stats = time.time()
    rec.note(f"rec start mode={'bands' if args.bands else 'raw' if args.raw else 'console'}")
    while not stop and (args.seconds <= 0 or time.time() - t0 < args.seconds):
        if rec.fd is None:
            if not rec.open():
                time.sleep(2)
                continue
            if want_raw:
                rec.set_raw(1)
            if rec.fd is None:
                continue
        rec.pump(0.2)
        now = time.time()
        if (want_raw and rec.fd is not None and now - rec.last_raw > 5
                and now - rec.last_fix > 10):
            rec.note("raw dump silent: turning it back on")
            rec.set_raw(1)
        if now - last_stats >= 600:
            rec.note(rec.stats())
            last_stats = now
    if want_raw and rec.fd is not None:
        rec.set_raw(0)
    if rec.fd is not None:
        rec.pump(0.3)
        os.close(rec.fd)
    rec.note(rec.stats())
    rec.note("rec stop")
    out.close()
    print(f"{time.time() - t0:.0f} s, {rec.n_lines} lines, {rec.n_raw} raw, "
          f"{rec.gaps} raw lines missing -> {out.name}", file=sys.stderr)


if __name__ == "__main__":
    main()
