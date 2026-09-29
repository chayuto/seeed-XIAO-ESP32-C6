#!/usr/bin/env python3
"""Score csi_rec.py recordings offline.

    tools/csi_motion.py REC... [--window S] [--trim S] [--per-window]
    tools/csi_motion.py REC... --map [--bin S] [--hours H] [--rows N]

Default: one line per labelled stretch (between "# label" lines) with the median, p90 and
max of the motion score, next to the board's own 1 Hz "m" lines. The score is recomputed
from raw I/Q lines with the same maths as csi.c (|H| per subcarrier, normalised by the
frame's mean over non-null tones; per window, the mean over subcarriers of std/mean,
x1000), or read from the "band" lines that csi_rec.py --bands writes.

--map draws the recording as text: one column per time bin (auto: the whole span in at
most 100 columns), one row per slice of the channel shaded by how much it fluctuated,
plus the motion score and the board's present state. Standard library only.
"""
import argparse
import base64
import math
import re
import statistics
import sys
import time
from array import array

M_LINE = re.compile(r"main: m t=\d+ motion=(?P<motion>[\d.]+) thr=[\d.]+ present=(?P<present>\d)"
                    r" fps=(?P<fps>\d+)")
BANDS = 16  # slices of the channel in a "band" line


def amplitudes(iq_b64, first_invalid):
    """|H_k| normalised by the frame's mean over non-null tones, as csi.c does."""
    b = array("b", base64.b64decode(iq_b64))  # int8 pairs, imaginary first
    n = len(b) // 2
    amp = [0.0] * n
    for k in range(2 if first_invalid else 0, n):
        amp[k] = math.hypot(b[2 * k], b[2 * k + 1])
    nonzero = [a for a in amp if a > 0]
    if not nonzero:
        return None
    mean = sum(nonzero) / len(nonzero)
    return [a / mean for a in amp]


def window_motion(amps):
    """Mean over subcarriers of std/mean across the window, x1000 (population std)."""
    n = len(amps)
    if n < 2:
        return 0.0
    acc, used = 0.0, 0
    for col in zip(*amps):
        mean = sum(col) / n
        if mean < 0.05:
            continue  # null / guard tone
        var = max(sum(x * x for x in col) / n - mean * mean, 0.0)
        acc += math.sqrt(var) / mean
        used += 1
    return 1000 * acc / used if used else 0.0


class Bands:
    """Per-second fluctuation (std/mean x1000) of each of BANDS slices of the channel,
    averaged over the slice's non-null subcarriers, plus the overall motion score. Feed
    frames in time order; add() returns the second it just closed, or None."""

    def __init__(self):
        self.sec, self.n, self.s, self.s2 = None, 0, None, None

    def tick(self, t_us):
        """Close the current second if t_us is past it (for frames this one skips)."""
        if self.sec is not None and t_us // 1_000_000 != self.sec:
            return self.close()
        return None

    def add(self, t_us, amp):
        sec = t_us // 1_000_000
        done = None
        if self.sec is not None and (sec != self.sec or len(amp) != len(self.s)):
            done = self.close()
        if self.s is None:
            self.s, self.s2 = [0.0] * len(amp), [0.0] * len(amp)
        self.sec = sec
        s, s2 = self.s, self.s2
        for k, a in enumerate(amp):
            s[k] += a
            s2[k] += a * a
        self.n += 1
        return done

    def close(self):
        n, s, s2, sec = self.n, self.s, self.s2, self.sec
        self.sec, self.n, self.s, self.s2 = None, 0, None, None
        if n < 2:
            return None
        cvs = []
        for k in range(len(s)):
            m = s[k] / n
            cvs.append(math.sqrt(max(s2[k] / n - m * m, 0.0)) / m if m >= 0.05 else None)
        used = [c for c in cvs if c is not None]
        w = len(cvs) // BANDS
        bands = []
        for i in range(BANDS):
            cs = [c for c in cvs[i * w:(i + 1) * w] if c is not None]
            bands.append(round(1000 * sum(cs) / len(cs)) if cs else -1)
        motion = 1000 * sum(used) / len(used) if used else 0.0
        return {"sec": sec, "n": n, "motion": motion, "bands": bands}


def band_line(sec, locked=None, kinds=None):
    """One second: all frames (n, motion, b), then only the frames of the locked kind
    (nk, mk, bk) and the kinds seen, as fmt/group/MPDU-length:count, most common first."""
    line = (f"band t={sec['sec']} n={sec['n']} motion={sec['motion']:.1f} b="
            + ",".join(str(v) for v in sec["bands"]))
    if locked is not None:
        line += (f" nk={locked['n']} mk={locked['motion']:.1f} bk="
                 + ",".join(str(v) for v in locked["bands"]))
    if kinds:
        line += " kinds=" + ",".join(f"{k}:{c}" for k, c in kinds.most_common(4))
    return line


def parse(paths, locked_only=False):
    labels, frames, mlines, seconds = [], [], [], []
    wrap, prev = 0, None
    for path in paths:
        for line in open(path, errors="replace"):
            ht, _, body = line.rstrip("\n").partition(" ")
            try:
                ht = float(ht)
            except ValueError:
                continue
            if body.startswith("# "):
                labels.append((ht, body[2:].strip()))
            elif body.startswith("raw s="):
                kv = dict(x.split("=", 1) for x in body.split()[1:] if "=" in x)
                if "iq" not in kv:
                    continue  # cut short by the end of a capture
                t = int(kv["t"])  # rx timestamp, us, 32-bit
                if prev is not None and t < prev - 2**31:
                    wrap += 2**32
                prev = t
                frames.append((t + wrap, ht, int(kv["len"]), kv["fi"] == "1", kv["iq"]))
            elif body.startswith("band t="):
                kv = dict(x.split("=", 1) for x in body.split()[1:])
                if locked_only:
                    if "mk" not in kv:
                        continue
                    kv["motion"], kv["n"], kv["b"] = kv["mk"], kv["nk"], kv["bk"]
                # host time of the line is when the second closed: centre is ~0.5 s before
                seconds.append((ht - 0.5, float(kv["motion"]), int(kv["n"]),
                                [int(v) for v in kv["b"].split(",")]))
            else:
                m = M_LINE.search(body)
                if m:
                    mlines.append((ht, float(m["motion"]), int(m["fps"]), int(m["present"])))
    return labels, frames, mlines, seconds


def seconds_from_frames(frames):
    """Band seconds recomputed from raw frames, on the host clock."""
    if not frames:
        return []
    lock = statistics.mode(f[2] for f in frames)
    offset = statistics.median(f[1] - f[0] / 1e6 for f in frames)
    out, bands = [], Bands()
    for t_us, _, ln, fi, iq in frames:
        a = amplitudes(iq, fi) if ln == lock else None
        done = bands.add(t_us, a) if a else None
        if done:
            out.append(done)
    last = bands.close()
    if last:
        out.append(last)
    return [(s["sec"] + 0.5 + offset, s["motion"], s["n"], s["bands"]) for s in out]


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))] if xs else float("nan")


def stretches_table(args, labels, frames, mlines, seconds):
    windows = []  # (host time of window centre, motion, frames)
    if frames:
        lock = statistics.mode(f[2] for f in frames)
        offset = statistics.median(f[1] - f[0] / 1e6 for f in frames)
        w_us = int(args.window * 1e6)
        cur, amps = None, []
        for t_us, _, ln, fi, iq in frames:
            if ln != lock:
                continue
            w = t_us // w_us
            if w != cur and amps:
                windows.append(((cur + 0.5) * args.window + offset, window_motion(amps), len(amps)))
                amps = []
            cur = w
            a = amplitudes(iq, fi)
            if a:
                amps.append(a)
        if amps:
            windows.append(((cur + 0.5) * args.window + offset, window_motion(amps), len(amps)))
    else:
        windows = [(t, motion, n) for t, motion, n, _ in seconds]

    t_first = min(x[0] for x in windows + mlines)
    t_end = max([x[0] for x in windows + mlines] + [l[0] for l in labels])
    if not labels or labels[0][0] > t_first:
        labels.insert(0, (float("-inf"), "(before first label)"))
    stretches = [(t, labels[i + 1][0] if i + 1 < len(labels) else t_end + 1, name)
                 for i, (t, name) in enumerate(labels)]

    print(f"window={args.window:g}s trim={args.trim:g}s frames={len(frames)} "
          f"windows={len(windows)} m_lines={len(mlines)}")
    print(f"{'stretch':28} {'secs':>5} {'win':>4} {'median':>7} {'p90':>7} {'max':>7} "
          f"{'fps':>4} | {'fw_med':>7} {'fw_p90':>7}")
    for a, b, name in stretches:
        lo, hi = a + args.trim, b - args.trim
        ws = [w for w in windows if lo <= w[0] < hi]
        ms = [m for m in mlines if lo <= m[0] < hi]
        if not ws and not ms:
            continue
        secs = min(hi, t_end) - max(lo, min(x[0] for x in ws + ms))
        mv = [w[1] for w in ws]
        fw = [m[1] for m in ms]
        fps = statistics.median([w[2] / args.window for w in ws]) if ws else float("nan")
        print(f"{name[:28]:28} {secs:5.0f} {len(ws):4d} {pct(mv, .5):7.1f} {pct(mv, .9):7.1f} "
              f"{max(mv, default=float('nan')):7.1f} {fps:4.0f} | {pct(fw, .5):7.1f} {pct(fw, .9):7.1f}")
        if args.per_window:
            for w in ws:
                print(f"    {w[0]:.1f} motion={w[1]:.1f} n={w[2]}")


LADDER = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600]
BARS = "▁▂▃▄▅▆▇█"


def shade(v):
    if v is None:
        return "·"
    return " " if v < 25 else "░" if v < 50 else "▒" if v < 100 else "▓" if v < 200 else "█"


def bar(v):  # log scale, 15 .. 300
    if v is None:
        return "·"
    x = (math.log(max(v, 15)) - math.log(15)) / (math.log(300) - math.log(15))
    return BARS[max(0, min(7, int(x * 8)))]


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def draw_map(args, labels, mlines, seconds):
    if not seconds:
        sys.exit("no raw or band lines to map")
    t_end = max(s[0] for s in seconds)
    t_start = min(s[0] for s in seconds)
    if args.hours:
        t_start = max(t_start, t_end - args.hours * 3600)
    span = t_end - t_start
    bin_s = args.bin or next((b for b in LADDER if span / b <= 100), 3600)
    t_start -= t_start % bin_s
    ncol = int((t_end - t_start) // bin_s) + 1
    cols = [{"m": [], "b": [[] for _ in range(BANDS)], "p": []} for _ in range(ncol)]
    for t, motion, _, bands in seconds:
        if t >= t_start:
            c = cols[int((t - t_start) // bin_s)]
            c["m"].append(motion)
            for i, v in enumerate(bands):
                if v >= 0:
                    c["b"][i].append(v)
    for t, _, _, present in mlines:
        i = int((t - t_start) // bin_s)
        if 0 <= i < ncol:
            cols[i]["p"].append(present)

    fmt = "%H:%M:%S" if bin_s < 60 else "%H:%M"
    axis = [" "] * (ncol + 10)
    step = 15 if bin_s < 60 else 12
    for i in range(0, ncol, step):
        for j, ch in enumerate(time.strftime(fmt, time.localtime(t_start + i * bin_s))):
            axis[i + j] = ch
    rows = max(1, min(args.rows, BANDS))
    day = time.strftime("%a %d %b", time.localtime(t_start))
    print(f"{day}, {bin_s} s per column, {len(seconds)} s of data")
    print(f"{'time':11} " + "".join(axis).rstrip())
    print(f"{'motion':11} " + "".join(bar(mean(c["m"])) for c in cols))

    def pres(ps):
        if not ps:
            return "·"
        f = sum(ps) / len(ps)
        return " " if f == 0 else "░" if f < 0.5 else "▓" if f < 1 else "█"

    print(f"{'present':11} " + "".join(pres(c["p"]) for c in cols))
    width = 256 // BANDS
    for r in range(rows):
        lo, hi = r * BANDS // rows, (r + 1) * BANDS // rows
        vals = [mean([v for i in range(lo, hi) for v in c["b"][i]]) for c in cols]
        print(f"sc {lo * width:3d}-{hi * width - 1:3d} " + "".join(shade(v) for v in vals))
    print(f"{'':11} blank <25  ░ <50  ▒ <100  ▓ <200  █ 200+   · no data   "
          f"(std/mean x1000; present: blank none, ░ some, ▓ most, █ all)")

    # Coverage, gaps and where the movement was
    ts = sorted(s[0] for s in seconds if s[0] >= t_start)
    gaps = [(a, b) for a, b in zip(ts, ts[1:]) if b - a > 60]
    motion = [s[1] for s in seconds if s[0] >= t_start]
    pres_all = [p for c in cols for p in c["p"]]
    hm = lambda t: time.strftime("%H:%M:%S", time.localtime(t))
    summary = (f"span {hm(ts[0])}-{hm(ts[-1])}  coverage {len(ts) / max(1, ts[-1] - ts[0] + 1):.0%}"
               f"  motion median {statistics.median(motion):.1f} p90 {pct(motion, .9):.1f}")
    if pres_all:
        summary += f"  present {sum(pres_all) / len(pres_all):.0%} of the time"
    print(summary)
    for a, b in gaps[:10]:
        print(f"  gap {hm(a)}-{hm(b)} ({b - a:.0f} s)")
    colv = [(i, mean(c["m"])) for i, c in enumerate(cols) if c["m"]]
    runs, cur = [], None
    for i, v in colv:
        active = v >= 60
        if cur and cur[0] == active and cur[2] == i - 1:
            cur[2] = i
            cur[3].append(v)
        else:
            cur = [active, i, i, [v]]
            runs.append(cur)
    if len(runs) <= 24:
        for active, a, b, vs in runs:
            print(f"  {'MOVING' if active else 'quiet '} {hm(t_start + a * bin_s)}-"
                  f"{hm(t_start + (b + 1) * bin_s - 1)}  mean {mean(vs):6.1f}  max {max(vs):6.1f}")
    else:
        top = sorted(colv, key=lambda x: -x[1])[:8]
        print("  busiest columns: " + ", ".join(f"{hm(t_start + i * bin_s)} ({v:.0f})"
                                                for i, v in sorted(top)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("rec", nargs="+")
    ap.add_argument("--window", type=float, default=1.0, help="seconds (firmware: 1)")
    ap.add_argument("--trim", type=float, default=0.0, help="seconds off each stretch end")
    ap.add_argument("--per-window", action="store_true")
    ap.add_argument("--map", action="store_true", help="draw the recording as text")
    ap.add_argument("--bin", type=int, default=0, help="seconds per map column (auto)")
    ap.add_argument("--hours", type=float, default=0, help="map only the last H hours")
    ap.add_argument("--rows", type=int, default=8, help="channel slices in the map (<=16)")
    ap.add_argument("--locked", action="store_true",
                    help="band lines: use only the frames of the locked kind (mk/bk)")
    args = ap.parse_args()

    labels, frames, mlines, seconds = parse(sorted(args.rec), args.locked)
    if not frames and not mlines and not seconds:
        sys.exit("no raw, band or m lines in " + " ".join(args.rec))
    if args.map:
        draw_map(args, labels, mlines, seconds or seconds_from_frames(frames))
    else:
        stretches_table(args, labels, frames, mlines, seconds)


if __name__ == "__main__":
    main()
