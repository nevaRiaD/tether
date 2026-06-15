# Tether

A multi-board ESP32 system that keeps a "tag" within range of a base station and alerts when the link is broken or motion is detected.

Tether was made as a capstone project for my UCLA 180DW capstone course. I worked with 4 other members to develop this project. I primarily worked on the BLE and SDIO.

## Description

Tether is firmware for a three-board proximity and motion monitoring system. A central
base station (the **P4 main** board) drives a touchscreen UI, audio alerts, and proximity
sensing, while a small wireless **tag** reports motion over Bluetooth Low Energy. A
dedicated radio coprocessor (the **C6**) handles the BLE/SDIO link so the main processor
stays free for the UI and application logic.

The three boards each live in their own folder under [boards/](boards/):

| Board | Folder | Role |
|---|---|---|
| **Tether P4 Main** | [boards/tether-p4-main/](boards/tether-p4-main/) | Base station: 7" touch LCD (LVGL UI), audio alerts, VL53L1X time-of-flight + HMMD mmWave proximity sensing, and SDIO master to the C6. |
| **Tether C6 BLE** | [boards/tether-c6-ble/](boards/tether-c6-ble/) | BLE radio coprocessor on the P4 board. Runs the NimBLE stack and an SDIO slave; owns scanning, pairing, and bonding, exposing them to the P4 over a small TLV protocol (and to developers over a UART console). |
| **Tether Tag** | [boards/tether-tag/](boards/tether-tag/) | Wireless motion tag: a separate ESP32 (C6/C3 SuperMini) with an MPU6050 accelerometer. Runs a BLE GATT server that advertises a single motion flag the C6 central subscribes to. |

See each board's own `README.md` for board-specific details.

## Getting Started

### Dependencies

* **ESP-IDF v5.3.5** (required for the ESP32-P4) — for the `tether-p4-main` and `tether-c6-ble` boards.
* **Arduino IDE / arduino-cli** — for the `tether-tag` sketch.
* **Hardware:**
  * Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (P4 host + onboard C6) — [product page](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-7b.htm) · [wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B)
  * HMMD mmWave motion sensor (UART) — [product page](https://www.waveshare.com/hmmd-mmwave-sensor.htm)
  * VL53L1X time-of-flight distance sensor
  * ESP32-C6 / C3 SuperMini + MPU6050 accelerometer (for the tag)
* **Frameworks / libraries:** ESP-IDF (FreeRTOS), LVGL via `esp_lvgl_port`, MIPI-DSI display driver. Component dependencies for the P4 are listed in [boards/tether-p4-main/README.md](boards/tether-p4-main/README.md).

### Installing

* Clone the repository.
* Each board is a self-contained project under `boards/`. Build them independently:
  * `tether-p4-main` and `tether-c6-ble` are ESP-IDF projects (`idf.py` / the ESP-IDF VS Code extension).
  * `tether-tag` is an Arduino sketch flashed separately.
* For the P4, pull the managed components (see the dependency list in
  [boards/tether-p4-main/README.md](boards/tether-p4-main/README.md)) before the first build.

### Executing program

Build and flash the **P4 main** board:

```bash
cd boards/tether-p4-main
idf.py set-target esp32p4
idf.py build flash monitor
```

Build and flash the **C6 BLE** coprocessor (note its special wiring/download-mode steps in
[boards/tether-c6-ble/README.md](boards/tether-c6-ble/README.md)):

```bash
cd boards/tether-c6-ble
idf.py set-target esp32c6
idf.py build flash monitor
```

Flash the **tag** with the Arduino IDE / arduino-cli (select `ESP32C6` or `ESP32C3` at the
top of [boards/tether-tag/accel_tag.ino](boards/tether-tag/accel_tag.ino) to match your board).

## Help

* The C6 exposes a UART console (`tether>` prompt at 115200 baud, CR line ending, DTR/RTS
  unchecked) for scanning, pairing, and disconnecting BLE devices manually. Type `help`
  for the command list — see [boards/tether-c6-ble/README.md](boards/tether-c6-ble/README.md).
* For general ESP-IDF help:

```bash
idf.py --help
```

## Authors

Tether project team.

## Version History

* 0.1
    * Initial Release

## License

This project is licensed under the [NAME HERE] License - see the LICENSE.md file for details

## Acknowledgments

* [Waveshare ESP32-P4-WIFI6-Touch-LCD-7B wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B)
* [Commit message convention](https://gist.github.com/qoomon/5dfcdf8eec66a051ecd85625518cfd13)
* [awesome-readme](https://github.com/matiassingers/awesome-readme)
</content>
</invoke>
