# 03_csi_presence — room presence from Wi-Fi channel state

## Goal

Tell whether someone is in the room using nothing but the Wi-Fi link between the XIAO and
the home router. A body in the room changes the multipath: the per-subcarrier amplitude of
every received frame wobbles when someone moves, and sits still when the room is empty.
The ESP32-C6 exposes this per-frame channel estimate as **CSI** (channel state
information). No camera, no PIR, no second board.

Output: one `motion` score per second, a `present` state with hysteresis, and (on demand)
the raw per-frame amplitudes for offline analysis on the Mac.

## What CSI sensing can and can't do

- **Motion is easy.** Walking, sitting down or waving moves many paths at once; the score
  jumps by an order of magnitude.
- **Still presence is hard.** Someone reading on the sofa only moves with breathing
  (~0.2–0.3 Hz, millimetres). Detecting that needs a quiet link and a longer window;
  it is a stretch goal, not M3. Until then "present" means "moved within the hold time".
- **It's per link, not per room.** Only the paths between the router and the XIAO matter.
  Placement (the person between or near the two) decides sensitivity more than code does.

## Facts from ESP-IDF 5.5 (checked in the headers, not the web)

- `CONFIG_ESP_WIFI_CSI_ENABLED=y` (off by default). Costs about
  `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` KB of RAM.
- The C6 is Wi-Fi 6 with `SOC_WIFI_MAC_VERSION_NUM=2`, so `wifi_csi_config_t` is
  `wifi_csi_acquire_config_t` (bitfields `enable`, `acquire_csi_legacy`, `_ht20`, `_ht40`,
  `_su`, `_mu`, `_dcm`, `_beamformed`, `_he_stbc`, `val_scale_cfg`, `dump_ack_en`), **not**
  the older `lltf_en`/`htltf_en` struct the ESP32/S3 examples use.
- Frame metadata is `esp_wifi_rxctrl_t` (`rssi`, `noise_floor`, `channel`,
  `cur_bb_format`, `rx_channel_estimate_len`, `timestamp`).
- `wifi_csi_info_t.buf` holds int8 pairs per subcarrier, **imaginary first, then real**;
  `len` bytes in total. `first_word_invalid` flags the first 4 bytes as garbage.
- The CSI callback runs in the Wi-Fi task: copy and return, never compute or log there.
- Sequence: `esp_wifi_start()` → `esp_wifi_set_csi_config()` → `esp_wifi_set_csi_rx_cb()`
  → `esp_wifi_set_csi(true)`.

## Where the frames come from

CSI is measured on frames the XIAO **receives**. Beacons alone give ~10 Hz, unevenly. So the
XIAO pings its gateway (lwIP `esp_ping`, 20 ms interval) and every ICMP reply from the
router is a CSI frame at a steady ~50 Hz. Only frames whose source MAC is the associated
BSSID are used; the rest are counted (`other=`) and dropped, because a phone across the
house has a completely different channel.

The router may answer in legacy, HT or HE format; each has a different subcarrier count.
The pipeline locks onto the first `len` it sees from the AP and counts mismatches
(`skip_len=`), so the score is never computed across formats. If the log shows the lock
flapping, pin the format.

Measured on the home router (2026-09-29, raw dump):

- Every AP frame is a ping reply (MPDU length 90) in HE SU format (`fmt=4`), `len=512`
  (256 subcarriers), MCS 0–3. No beacon produced a CSI frame (`skip_len=0`), so the ping is
  the only source.
- ~61 frames/s from 50 pings/s: about one reply in five arrives twice within 5 ms, a
  retransmission (the router misses some of the XIAO's ACKs). Harmless for the score.
- `esp_ping` waits out its timeout on a lost reply, then `vTaskDelayUntil` catches up with
  back-to-back pings. At the 1000 ms timeout that was a 1 s gap, then a ~50-frame burst that
  overflowed the queue (`dropped=14` in the first minute). The timeout is now 100 ms; a late
  reply still gives a CSI frame.

*Rejected: a second XIAO as a dedicated transmitter.* Cleaner (fixed rate, fixed format,
placement under our control) and the natural next step, but it needs two boards on a desk
before the first one has shown a single CSI frame. The router link is free and already
positioned across a real room.

## Signal pipeline

```
Wi-Fi task                    csi task (1 frame at a time)                1 Hz
csi_cb ──copy──► queue(16) ──► amplitude |H_k| per subcarrier ──► per-k sum, sum² ──► motion
  filter: src == BSSID          skip guard/null tones (|H|=0)          (1 s window)     present
  drop on full queue (dropped=)  normalise by frame mean amplitude
```

- **Normalise each frame by its mean amplitude.** The receiver's AGC rescales frames
  from moment to moment; dividing it out leaves only the *shape* across subcarriers,
  which is what a body changes.
- **Motion score** = mean over subcarriers of the coefficient of variation (std / mean)
  of the normalised amplitude across the 1 s window, ×1000. The memory cost doesn't grow
  with the window: two float arrays of per-subcarrier sums.
- **Presence**: `motion > threshold` in 2 of the last 3 seconds sets `present=1`; it
  clears after `hold_s` (default 60) seconds below threshold.
- **Threshold**: `b` on the console records a 30 s baseline (room empty, nobody moving)
  → threshold = mean + 4·sd, floor at 1.5× mean, stored in NVS. Until calibrated, a Kconfig
  default is used and the heartbeat says `cal=0`.

## Logging (agentic-logging)

```
boot project=03_csi_presence ... reset=usb
csi_lock len=… fmt=… n_sub=… src=<ap bssid>
m t=… motion=… thr=… present=0|1 fps=… rssi=… nf=…      (1 Hz)
state present=1 motion=… after_s=…                    (on change)
hb up=… heap=… heap_min=… rssi=… frames=… other=… dropped=… skip_len=… last_frame_ms=…
   raw=… raw_lines=… raw_drop=… cal=…
raw s=<seq> t=<rx µs> rssi=… nf=… fmt=… rate=… sl=<MPDU len> g=… siga1=<hex> fi=… len=… iq=<base64>
```

`last_frame_ms` makes a silent CSI source visible (pings stopped, lock lost, Wi-Fi down).

Serial commands: `i` status, `r` restart, `b` calibrate baseline (30 s), `c` toggle the raw
dump, `m` toggle the 1 Hz `m` lines (keeps a long attach readable).

The raw dump is one line per AP frame with the exact I/Q bytes (int8 pairs, imaginary
first) in base64: ~780 B a line, ~48 KB/s at 62 frames/s. `s=` counts every frame offered to
the console, so a gap in it is a dropped line. The `m` lines keep running alongside, which
lets the Mac check its own scoring against the board's.

## Recording on the Mac (M4)

```zsh
PY=~/.espressif/python_env/idf5.5_py3.14_env/bin/python
tools/rec.sh start                                        # long capture, detached; stop | status
$PY tools/csi_motion.py logs/csi-*.rec --map --hours 6    # text map of the last 6 h
$PY tools/csi_rec.py /tmp/walk.rec --raw --seconds 600    # a short raw capture instead
echo "$(date +%s) # empty" >> /tmp/walk.rec                # label from anywhere, any time
$PY tools/csi_motion.py /tmp/walk.rec --trim 5            # per label: median, p90, max
```

`csi_rec.py` opens the port like `attach.sh` (pyserial would reset the board), stamps
every line with the Mac's clock, and carries on through the port going away (unplug, Mac
sleep) and the board rebooting (it turns the dump back on). With `--bands` it keeps the
~170 MB/h raw stream out of the file and writes one line a second instead: the
fluctuation (std/mean x1000) of each of 16 slices of the channel, computed on the Mac from
the raw I/Q, about 17 MB a day.

`tools/rec.sh` runs that detached, one file a day in `logs/`, which is gitignored: a
presence log is a record of when people are home. It holds `caffeinate -i` so the Mac
doesn't idle-sleep; a closed lid still pauses it. **It holds the port: `rec.sh stop` before
`flash.sh` or `attach.sh`**, `rec.sh start` after.

`csi_motion.py` recomputes the firmware's score from the I/Q (standard library only); on
the first 30 s recording it agreed with the board (median 24.7 vs 25.0, p90 116 vs 119).
`--map` draws any span as text, one column per time bin, a row per slice of the channel.

## Milestones

| | Milestone | Done when |
|---|---|---|
| M1 | CSI flowing | `csi_lock` line; `fps≈50` steady in the heartbeat; `dropped=0` |
| M2 | Motion score | Empty room vs someone walking differ ≥5× in `motion`, seen in a log |
| M3 | Presence | `b` calibration, `present` toggles on entry, clears after hold; 1 h empty with no false `present=1` |
| M4 | Record | Raw dumps captured on the Mac (`tools/`), labelled, to tune the window and threshold |
| M5 | Later | Supabase rows like 02; breathing band (0.1–0.5 Hz) for still presence; second XIAO as TX |

M1 done 2026-09-29 (~61 frames/s, see the measured facts above). M4's tools are in `tools/`;
a long band capture has run on the Mac since 2026-09-29 19:15; no labelled recording yet.

### First night on the band capture (2026-09-29 19:15 to 09-30 08:05)

- 12.8 h: the board never rebooted or dropped Wi-Fi. `dropped` rose 120 to 164, all in
  bursts between 21:34 and 22:15. The Mac stopped reading three times between 07:08 and
  07:47 (asleep or lid shut), so 96% coverage.
- **Calibrated with the house empty** (09-30 08:07, `b`): mean 20.0, sd 3.8, threshold 35.1
  (was the default 60). Replaying the night's 1 Hz scores through the presence rules: with
  people home in the evening, present 92% of the time in 8 episodes (61% in 40 at 60); the
  empty house from 08:04, 0%.
- **The night was not people.** For 1.9 h of the night the router sent 85+ frames/s instead
  of ~62, and those seconds scored 150–800 (97% over 100) with one fixed shape across the
  channel (the top slice ~780). At the normal frame rate the night's median was 19–20 and
  no second went over 100. Bodies don't change the frame rate: the likely cause is extra
  frames of another kind with the same `len`, which the `len` lock lets through, so the
  score is computed across kinds after all. `csi_rec.py` now logs each second's frame kinds
  (`kinds=` format/group/MPDU length) and a score from the locked kind alone (`nk`, `mk`,
  `bk`; the ping reply is `4/0/90`), to confirm this before the firmware lock changes.
- The empty house also shows short bursts in the lowest quarter of the channel only
  (slices 0–3 up to ~90 while the rest sit at ~17): something narrowband nearby. Movement
  lights up every slice at once, so a median across slices would ignore these.

### The empty house (2026-09-30 08:04 to 16:03)

- **11:34 to 15:40, 4 h with no presence at all** at threshold 35.1.
- **08:10 to 11:35: ~20 short presence episodes** (scores up to 258) at the normal frame
  rate, with a smooth shape across the channel (strongest at the low end, dipping in the
  middle). That is something physically moving, not the router; what, is still open.
- **15:40: one router burst**: 118 and 94 frames/s, every frame a ping reply of the normal
  kind (4/0/90), same step-and-spike shape as the night (top slice ~900–1080). So the extra
  frames are the router sending the replies again in a way format and length don't show.
  The kind now includes the HE-SIG-A1 settings that shape the channel estimate (bandwidth,
  GI+LTF size, streams, beam change: `4/0/90/b0l1s0c1` normally), and `rec.sh` keeps the
  raw lines from 5 s before to 5 s after any second with 85+ frames or a score of 150+
  (`logs/csi-burst-*.rec`, ~30 min a day at most) to see exactly what differs.
- **The board dropped ~350 frames from 15:00** while `raw_drop` rose with it: the Mac was
  reading slowly and the CSI task waits on the console, because it prints the 1 Hz `m` line
  itself. Fix to make with the kind lock: log from a separate low-priority task.
- **The recorder died at 16:03** on a raw line damaged in transit (bad base64). Damaged
  lines are now counted (`bad=`) and skipped, and `rec.sh` restarts the recorder if it ever
  exits with an error. 16:03 to 18:07 is lost.

## Decisions

- **Amplitude only, no phase.** Phase on a single-antenna receiver has a random offset per
  frame (CFO/SFO/packet detection delay); sanitising it is real work for small gain at M2.
- **Ping, not UDP to our own host.** Needs nothing running on the LAN.
- **Power save off** (as `wifi_sta` already does): modem sleep would bunch up the replies.
- **Console on the USB port only** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`). The IDF default,
  UART0 at 115200 with USB as a copy, makes every line wait for the UART (~11.5 KB/s). The
  first raw dump held the CSI task ~48 ms a line, which starved the ping task and cut the
  frame rate from ~55/s to ~18/s.
- **Nothing downstream can slow the source.** The ping task runs above the CSI task, so a
  busy CSI task drops frames (`dropped=`) rather than delaying pings; raw lines go through
  a message buffer to their own low-priority task, so a slow reader drops lines
  (`raw_drop=`) rather than stalling the score.
