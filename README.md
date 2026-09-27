# seeed-XIAO-ESP32-C6

ESP-IDF firmware projects for the **Seeed Studio XIAO ESP32-C6**, a thumb-sized
ESP32-C6 board with Wi-Fi 6, BLE 5 and 802.15.4 (Thread / Zigbee).

Board facts, build rules and lessons learned live in [`CLAUDE.md`](CLAUDE.md); each
non-trivial project has a design doc in [`docs/design/`](docs/design/) written before
its code.

## Board

| Feature | Detail |
|---|---|
| Chip | ESP32-C6, RISC-V 160 MHz HP core + 20 MHz LP core |
| Memory | 512 KB SRAM, 4 MB flash |
| Wireless | Wi-Fi 6 (2.4 GHz), BLE 5, IEEE 802.15.4 |
| Antenna | On-board ceramic or u.FL, chosen by an RF switch (GPIO3 / GPIO14) |
| I/O | 11 pads (D0–D10): ADC, I2C, UART, SPI |
| USB | USB-C, native USB-Serial-JTAG console |
| Power | 3.7 V LiPo pads with on-board charging |

## Projects

| # | Project | One-liner |
|---|---|---|
| 01 | [Bring-up](projects/01_bringup/) | Board census: chip, flash, LED, BOOT button, RF switch, Wi-Fi join |
| 02 | [Air purifier CoAP](docs/design/02_airpurifier_coap.md) | Read a Philips air purifier's live status over its local encrypted CoAP protocol |

## Build & flash

Requires [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/).

```zsh
. ~/esp/esp-idf/export.sh
idf.py -C projects/01_bringup -B /tmp/xiao-c6-build/01_bringup set-target esp32c6
idf.py -C projects/01_bringup -B /tmp/xiao-c6-build/01_bringup \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local" build
.claude/skills/flash/scripts/flash.sh 01_bringup
```

Wi-Fi credentials go in `projects/<name>/sdkconfig.defaults.local` (gitignored):

```
CONFIG_XIAO_WIFI_SSID="your-ssid"
CONFIG_XIAO_WIFI_PASSWORD="your-password"
```

## License

MIT — see [LICENSE](LICENSE).
