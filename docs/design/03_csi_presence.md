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
hb up=… heap=… heap_min=… rssi=… frames=… other=… dropped=… skip_len=… last_frame_ms=… cal=…
```

`last_frame_ms` makes a silent CSI source visible (pings stopped, lock lost, Wi-Fi down).

Serial commands: `i` status, `r` restart, `b` calibrate baseline (30 s), `c` toggle raw
dump (`raw t=… rssi=… a=<hex amplitude per subcarrier>` per frame, for the Mac), `m` toggle
the 1 Hz `m` lines (keeps a long attach readable).

## Milestones

| | Milestone | Done when |
|---|---|---|
| M1 | CSI flowing | `csi_lock` line; `fps≈50` steady in the heartbeat; `dropped=0` |
| M2 | Motion score | Empty room vs someone walking differ ≥5× in `motion`, seen in a log |
| M3 | Presence | `b` calibration, `present` toggles on entry, clears after hold; 1 h empty with no false `present=1` |
| M4 | Record | Raw dumps captured on the Mac (`tools/`), labelled, to tune the window and threshold |
| M5 | Later | Supabase rows like 02; breathing band (0.1–0.5 Hz) for still presence; second XIAO as TX |

## Decisions

- **Amplitude only, no phase.** Phase on a single-antenna receiver has a random offset per
  frame (CFO/SFO/packet detection delay); sanitising it is real work for small gain at M2.
- **Ping, not UDP to our own host.** Needs nothing running on the LAN.
- **Power save off** (as `wifi_sta` already does): modem sleep would bunch up the replies.
