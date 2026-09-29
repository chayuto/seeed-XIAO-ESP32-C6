---
name: xiao-debug
description: Build, flash, watch and debug firmware on the Seeed XIAO ESP32-C6 in this repo. Use for any build/flash, reading the serial console, sending a command to the running board, decoding a panic, reading a core dump, or live GDB over the built-in USB-JTAG (task backtraces, variables) — and whenever the board seems dead, crash-loops or "does nothing". Replaces idf.py flash / idf.py monitor, which misbehave or cannot run here.
---

# Flashing and debugging the XIAO ESP32-C6

Everything below was run on the unit in hand on 2026-09-27 (ESP32-C6FH4 rev v0.2,
4 MB in-package flash). Scripts are in `scripts/`, run from anywhere.

## The toolbox

| Need | Command | Resets the board? |
|------|---------|-------------------|
| Which port | `scripts/port.sh` | no |
| Build | `scripts/build.sh <project> [clean]` | no |
| Flash | `scripts/flash.sh <project> [port]` | **yes, once** (at the end) |
| Watch the console | `scripts/attach.sh [secs] [outfile]` | no |
| Send a command byte | `scripts/send.sh <char>` | no |
| Boot banner | `python scripts/capture.py --seconds 15 --out f` | **yes** |
| Live state (tasks, vars) | `scripts/gdb.sh <project> ["gdb cmd" ...]` | no (see below) |
| Decode a panic from a log | `scripts/decode.sh <project> [log]` | no |
| Full crash post-mortem | `scripts/coredump.sh <project>` | **yes, once** |

`python` = `~/.espressif/python_env/idf5.5_py3.14_env/bin/python` (system python has no
pyserial or esptool). Builds live in `/tmp/xiao-c6-build/<project>/`.

## The normal loop

```zsh
S=.claude/skills/xiao-debug/scripts
$S/build.sh 02_airpurifier_coap
$S/flash.sh 02_airpurifier_coap
sleep 3
$S/attach.sh 30 /tmp/attach.log          # everything after ~2 s of boot
$S/send.sh i                             # while attach runs: status line now
```

**Reset budget: one host-driven reset at a time, then wait ≥3 s (≥10 s before another).**
Default to `attach.sh` / `send.sh` / `gdb.sh`, which leave the app and its counters alone.
The S3 sibling wedged after back-to-back resets; the XIAO recovers with a replug (no PMIC),
but nobody should have to replug it for us.

## Facts that cost time to learn

- **No watchdog reset on the C6.** esptool's `--after watchdog_reset` falls back to an RTS
  hard reset. `flash.sh` asks for `hard_reset` explicitly.
- **`sdkconfig` goes to the project dir by default and beats `sdkconfig.defaults`.** A
  partition-table change was silently ignored that way. `build.sh` keeps sdkconfig in the
  build dir and deletes a stray one. After changing `sdkconfig.defaults`: `build.sh <p> clean`.
- **The target comes from `CONFIG_IDF_TARGET="esp32c6"` in sdkconfig.defaults.** No
  `set-target` step needed when that line is present.
- **`capture.py` shows the ROM banner twice.** The port open resets, then its own RTS pulse
  resets again. Expected; it's why it's not the default.
- **`reset=` in the boot line tells you why it restarted:** `usb` (host/esptool RTS),
  `panic`, `jtag` (OpenOCD), `sw` (esp_restart), `task_wdt`/`int_wdt`, `brownout`, `poweron`.
  Check it first when uptime looks too short.
- **OpenOCD resets on GDB attach by default.** esp-openocd's `gdb-attach` handler runs
  `reset halt` when flash support is on and memory protection is enabled (the IDF default).
  `gdb.sh` passes `set ESP_FLASH_SIZE 0` to avoid it (verified: uptime ran 52 → 60 s across
  a session). Cost: no software breakpoints in flash; the 4 hardware triggers still work.
  `XIAO_GDB_FLASH=1 scripts/gdb.sh …` brings flash support (and the reset) back.
- **`esp_coredump -p PORT` resets twice** (two esptool sessions) and can't find GDB unless
  it's on PATH. `coredump.sh` reads the partition in one esptool session, then decodes offline.
- **To see boot-time lines (self-tests) without a host reset**: start `attach.sh`, then
  `send.sh r` if the app has an `r` (esp_restart) command. A software restart keeps the
  USB-Serial-JTAG link up, so the attach keeps reading straight through the reboot.
  Alternatively attach 1 s after `flash.sh` returns (attach doesn't reset, so the
  shorter wait is safe); you'll miss the first ~0.5 s.
- **`SO_RCVTIMEO` of 0 means block forever** (lwIP). Round a remaining-time timeout up to
  whole ms and stop below 1 ms, or a "timed" recv hangs until the next datagram.
- **UDP datagrams over ~1.5 KB need `CONFIG_LWIP_IP4_REASSEMBLY=y`** (off by default);
  without it fragmented replies vanish with no error.
- **An out-of-range Kconfig value in `sdkconfig.defaults` is silently replaced by the
  default.** `CONFIG_ESP_TASK_WDT_TIMEOUT_S=90` (range 1-60) became 5, which would have
  rebooted the board on every 12 s network wait. After changing a numeric option, grep the
  generated `/tmp/xiao-c6-build/<p>/sdkconfig` to confirm it took.
- **The IDF default console caps all output at ~11.5 KB/s.** It is UART0 at 115200 with USB
  as a copy, and every print waits for the UART. Fine for logs; a bulk dump (03's raw CSI)
  blocked its task ~48 ms a line. For heavy output set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
  (USB only; the no-host case drops output after 50 ms instead of blocking).
- **Wi-Fi blob warnings** at start (`ACK_TAB0`, `CTS_TAB0`, `(agc)`, `(trc)`, `<ba-add>`) are
  normal. Anything else at `W`/`E` deserves a look.

## When something's wrong

**Build fails** → `build.sh` prints the error lines and the log path. Missing header
`esp_xxx.h` usually means a missing `REQUIRES` in `main/CMakeLists.txt`
(e.g. `esp_app_desc.h` → `esp_app_format`).

**No port / flash can't connect** → `port.sh`. Nothing listed: charge-only cable or a
dead USB stack in the app. Recover with download mode: hold **BOOT**, tap **RESET**,
release BOOT, then `flash.sh`. (Ask the person at the desk, once.)

**Board runs but "does nothing"** → `attach.sh 35` (a heartbeat lands every 30 s), then
`send.sh i`. Silence from both means the app is hung or crash-looping: use `gdb.sh <p>`
to see where every task is sitting, without resetting it.

**Crash / reboot loop** →
1. `attach.sh 20 /tmp/crash.log`, then `decode.sh <p> /tmp/crash.log`: gives the faulting
   line (MEPC) and caller (RA). The C6 prints no ready-made backtrace, only registers and a
   raw stack dump. The rest of the list is a stack scan, some of it stale.
2. `coredump.sh <p>`: the crashed task, its registers and a real backtrace for every task,
   plus stack used/free per task. Needs `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` and a
   `coredump` row in `partitions.csv` (copy from `01_bringup`). The ELF must match the
   build that crashed: don't rebuild in between.
3. A panic-at-boot loop resets the USB link every second; flash from download mode
   (BOOT + RESET) and fix the code.

**Wi-Fi won't join / weak** → the RF switch: GPIO3 must be LOW, GPIO14 selects antenna
(LOW ceramic). `xiao_board_rf_init()` logs `rf switch=on antenna=…`. Join takes ~1.6–6 s;
healthy RSSI in the house is −54 to −61 dBm.

**Want to look at a variable on a running board** →
`gdb.sh <p> "p my_var" "p *some_struct" "info registers pc"`: the app pauses for a few
hundred ms and resumes. Statics are visible by name; locals only via
`thread N` + `frame M` + `info locals`.

## Proof the crash path works

`01_bringup` has serial command `p` (deliberate NULL store). Flash it, then
`send.sh p` → the console shows `Guru Meditation … Store access fault`, `decode.sh` points at
`main.c` in `console_task`, the next boot says `reset=panic` and `Core dump data checksum is
correct`, and `coredump.sh` names task `console` with `a5 = 0x2a` (the 42 being stored).
Use it to check the tooling after an IDF upgrade.
