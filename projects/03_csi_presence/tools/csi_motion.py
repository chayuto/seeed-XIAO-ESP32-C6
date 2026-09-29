#!/usr/bin/env python3
"""Score a csi_rec.py recording offline, one line per labelled stretch.

    tools/csi_motion.py REC [--window S] [--trim S] [--per-window]

Recomputes the firmware's motion score from the raw I/Q lines with the same maths as
csi.c (|H| per subcarrier, normalised by the frame's mean over non-null tones; per window,
the mean over subcarriers of std/mean, x1000) and summarises each stretch between
"# label" lines next to the board's own 1 Hz "m" lines. --window tries other window
lengths; --trim drops the first and last seconds of each stretch (people moving in and
out). Standard library only.
"""
import argparse
import base64
import math
import re
import statistics
import sys
from array import array

M_LINE = re.compile(r"main: m t=(?P<t>\d+) motion=(?P<motion>[\d.]+) .*fps=(?P<fps>\d+)")


def parse(path):
    labels, frames, mlines = [], [], []
    wrap, prev = 0, None
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
        else:
            m = M_LINE.search(body)
            if m:
                mlines.append((ht, float(m["motion"]), int(m["fps"])))
    return labels, frames, mlines


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


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))] if xs else float("nan")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("rec")
    ap.add_argument("--window", type=float, default=1.0, help="seconds (firmware: 1)")
    ap.add_argument("--trim", type=float, default=0.0, help="seconds off each stretch end")
    ap.add_argument("--per-window", action="store_true")
    args = ap.parse_args()

    labels, frames, mlines = parse(args.rec)
    if not frames and not mlines:
        sys.exit("no raw or m lines in " + args.rec)

    # Windows by the board's rx clock; host time = rx time + a fixed offset (median of
    # arrival minus rx, so USB batching doesn't shift the labels).
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

    t_end = max([w[0] for w in windows] + [m[0] for m in mlines] + [l[0] for l in labels])
    if not labels or labels[0][0] > min([w[0] for w in windows] + [m[0] for m in mlines]):
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
        secs = (min(hi, t_end) - max(lo, min(x[0] for x in ws + ms)))
        mv = [w[1] for w in ws]
        fw = [m[1] for m in ms]
        fps = statistics.median([w[2] / args.window for w in ws]) if ws else float("nan")
        print(f"{name[:28]:28} {secs:5.0f} {len(ws):4d} {pct(mv, .5):7.1f} {pct(mv, .9):7.1f} "
              f"{max(mv, default=float('nan')):7.1f} {fps:4.0f} | {pct(fw, .5):7.1f} {pct(fw, .9):7.1f}")
        if args.per_window:
            for w in ws:
                print(f"    {w[0]:.1f} motion={w[1]:.1f} n={w[2]}")


if __name__ == "__main__":
    main()
