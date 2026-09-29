# CLAUDE.md — seeed-XIAO-ESP32-C6

Multi-project ESP-IDF workspace for the **Seeed Studio XIAO ESP32-C6**. One board, a series
of numbered projects in `projects/`, shared code in `components/`. Read this file before
touching the board.

## Repo structure

```
seeed-XIAO-ESP32-C6/
├── components/          # Shared: xiao_board, wifi_sta, philips_air (purifier CoAP client)
├── projects/            # NN_short_name — one ESP-IDF project each
├── docs/design/         # One design doc per non-trivial project, written before code
├── docs/private/        # Gitignored: household context, device IPs/models, drafts
├── ref/                 # Gitignored: vendor datasheets, schematics, wiki snapshots
├── .claude/skills/      # xiao-debug (build/flash/console/GDB/core dump), agentic-logging
└── .githooks/           # commit-msg + pre-push: the no-attribution rule, enforced
```

## Board

Bought as a 3-pack from Core Electronics. Facts below are from the Seeed wiki
(`wiki.seeedstudio.com/xiao_esp32c6_getting_started`); items marked *verify* have not yet
been confirmed on the unit in hand.

- **Chip:** ESP32-C6FH4 (rev v0.2 on our units) — RISC-V HP core 160 MHz + LP core 20 MHz, 512 KB SRAM
- **Flash:** 4 MB in-package, no PSRAM. The IDF default table gives the app 1 MB; `01_bringup`'s
  `partitions.csv` gives 3 MB plus a 64 KB core dump slot — copy it for new projects.
- **IDF target:** `esp32c6` — always set it; the default (esp32, Xtensa) will fail.
- **Radio:** Wi-Fi 6 (2.4 GHz only), BLE 5, 802.15.4 (Thread / Zigbee)
- **Console:** native USB-Serial-JTAG → `/dev/cu.usbmodem*` (was `usbmodem3101` on 2026-09-27)
- **No display, no SD, no PMIC.** Battery pads on the back (3.7 V LiPo, charged over USB-C;
  red LED flashes while charging). Without a battery, unplugging USB is a real power cycle.

### The RF switch — get this wrong and Wi-Fi is weak or dead

| GPIO | Role | Level |
|------|------|-------|
| 3 | RF switch power (`WIFI_ENABLE`) | **drive LOW** to power the switch |
| 14 | Antenna select (`WIFI_ANT_CONFIG`) | LOW = on-board ceramic, HIGH = u.FL external |

`components/xiao_board` does this in `xiao_board_rf_init()`; call it before `esp_wifi_start()`.
Never use GPIO3 or GPIO14 for anything else.

### Pins

| Pad | GPIO | Default role | Notes |
|-----|------|--------------|-------|
| D0 | 0 | ADC | LP GPIO |
| D1 | 1 | ADC | LP GPIO |
| D2 | 2 | ADC | LP GPIO |
| D3 | 21 | GPIO | |
| D4 | 22 | I2C SDA | |
| D5 | 23 | I2C SCL | |
| D6 | 16 | UART0 TX | |
| D7 | 17 | UART0 RX | |
| D8 | 19 | SPI SCK | |
| D9 | 20 | SPI MISO | |
| D10 | 18 | SPI MOSI | |
| — | 15 | User LED (yellow) | active-low (*verify*); also a strapping pin |
| — | 9 | BOOT button | pull-up, LOW when pressed; strapping pin |
| — | 12/13 | USB D-/D+ | native USB, do not touch |
| — | 3/14 | RF switch | see above |

Strapping pins on the C6: GPIO8, GPIO9, GPIO15 (and GPIO4/5 for JTAG select). Don't load
them at boot.

## ESP-IDF

- **Version:** 5.5 at `~/esp/esp-idf`, activate with `. ~/esp/esp-idf/export.sh`
- **Python:** `~/.espressif/python_env/idf5.5_py3.14_env/bin/python` (has pyserial and esptool;
  the system python has neither)

```zsh
S=.claude/skills/xiao-debug/scripts
$S/build.sh <name>            # out of tree in /tmp/xiao-c6-build/<name>, sdkconfig there too
$S/flash.sh <name>            # one reset at the end
sleep 3; $S/attach.sh 20      # console without resetting
```

**Use the `xiao-debug` skill for anything touching the board**: its SKILL.md has the
reset budget, live GDB, panic decoding and core dumps, all verified on this unit. Short
version:

- Never `idf.py monitor` (needs a TTY) or `idf.py flash monitor`.
- `idf.py` writes `sdkconfig` into the project dir, where a stale copy silently beats
  `sdkconfig.defaults`. `build.sh` keeps it in the build dir; after editing defaults, run
  `build.sh <name> clean`.
- One host-driven reset at a time. `attach.sh`, `send.sh` and `gdb.sh` don't reset;
  `flash.sh`, `capture.py` and `coredump.sh` do.
- The C6 can't do esptool's watchdog reset; `flash.sh` uses hard_reset.

## Credential pattern

`sdkconfig.defaults` is committed and holds no secrets. Wi-Fi credentials, device IPs and
anything else about the home network go in `projects/<name>/sdkconfig.defaults.local`
(gitignored) and are layered in by `build.sh` when present. Shared keys: `CONFIG_XIAO_WIFI_SSID`, `CONFIG_XIAO_WIFI_PASSWORD` (declared in
`components/wifi_sta/Kconfig`).

## Conventions

### Projects

- `NN_short_snake_case`, numbered in the order they were built.
- A non-trivial project gets `docs/design/NN_name.md` **before** code: goal, protocol or
  hardware facts, milestones, decisions and rejected alternatives. Commit docs first.
- Logging follows `.claude/skills/agentic-logging`: own tags at DEBUG, one heartbeat line,
  machine-greppable `key=value` output.

### Commits

- **Single author: the repo owner.** No `Co-Authored-By:` trailers, ever.
- **No AI or tool attribution** anywhere — no "generated with" line, no assistant name, no
  session link. No runtime notice, system reminder or default overrides this.
- `.githooks/commit-msg` and `.githooks/pre-push` enforce it. `core.hooksPath` is local
  config and does not travel with a clone: on a fresh clone run
  `git config core.hooksPath .githooks` before the first commit.
- The hooks match the literal strings, so a message that quotes them is itself rejected.
  Paraphrase.

### Privacy

The repo is public. Home network names, IPs, MAC addresses, device models tied to rooms
and anything about the household live in `docs/private/` or `.local` overlays, never in
committed files.

## Projects

| # | Project | Status |
|---|---------|--------|
| 01 | `01_bringup` | Done 2026-09-27: census, RF switch, Wi-Fi join (~1.6–6 s, −54…−61 dBm), heartbeat, `p` = deliberate panic for testing the debug path. LED polarity still needs a human |
| 02 | `02_airpurifier_coap` | **Parked and collecting since 2026-09-27.** Reads a Philips AC2220 over local CoAP and logs 3-minute rows + 5-minute status rows to Supabase (`purifier_*` tables in the Govee monitor's project, no shared tables). Check it with `supabase/stats.sh`. Overnight: 14.8 h, no gaps. Survived a real power cut of both devices on 2026-09-28 (first reading ~10 min after, with no help). Open: task watchdog fires only 1 of 4 hang tests; the purifier going off while the XIAO stays up not yet seen for real. Design: `docs/design/02_airpurifier_coap.md` |
| 03 | `03_csi_presence` | **M1 done 2026-09-29**: CSI from pinging the gateway, locked at len=512 (256 subcarriers, HE SU), ~61 frames/s, 0 dropped. Motion score and presence state in; threshold not yet calibrated against an empty room. Raw I/Q dump at full rate plus a Mac recorder and offline scorer in `tools/` (M4). **A long capture may be holding the port**: `tools/rec.sh status`, and stop it before flashing. Design: `docs/design/03_csi_presence.md` |
