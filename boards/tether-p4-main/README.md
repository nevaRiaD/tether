# Repo Guide

Read this before pushing any code.

---

## Tech Stack

- **Primary Board:** 
- **Testing Board:** DOIT ESP32 DEVKIT V1 (PlatformIO)
- **Framework:** Espidf (required for ESP32-P4)
- **UI:** LVGL (via esp_lvgl_port)
- **Display Driver:** MIPI-DSI driver (built into ESP-IDF)
- **RTOS:** FreeRTOS (included in ESP-IDF)

---

## Dependencies

```bash
# copy paste all for P4 (ensure you are on esp-idf v5.3.5 for P4)
idf.py add-dependency "waveshare/esp32_p4_wifi6_touch_lcd_7b^1.0.2"
idf.py add-dependency "espressif/esp_lcd_ek79007"
idf.py add-dependency "espressif/esp_lvgl_port"
idf.py add-dependency "espressif/esp_lcd_touch_gt911"
idf.py add-dependency "espressif/esp_codec_dev"
idf.py add-dependency "espressif/es8311"
```



## Folder Structure

```
project-root/
├── platformio.ini
├── .gitignore
│
├── src/
│   └── main.c
│
├── components/
│   ├── ble/
│   │   ├── src/
│   │   ├── include/
│   │   └── test/
│   │
│   ├── mmwave/
│   │   ├── src/
│   │   ├── include/
│   │   └── test/
│   │
│   ├── display/
│   │   ├── src/
│   │   ├── include/
│   │   └── test/
│   │
│   └── rtos/
│       ├── src/
│       ├── include/
│       └── test/
│
└── docs/
```

- **`src/main.c`** — the main program that wires everything together
- **`components/`** — each subteam's code lives here
  - `src/` — your `.c` files
  - `include/` — your `.h` headers (other teams `#include` these to use your code)
  - `test/` — standalone test program to verify your part works by itself
- **`docs/`** — documentation, diagrams, datasheets
- **`shared/`** — code shared between components

---

## Who Works Where

| Subteam | Folder |
|---------|--------|
| Hardware & Sensors | `components/ble/`, `components/mmwave/` |
| Display & Frontend | `components/display/` |
| RTOS | `components/rtos/` |
| Application & Logic | `components/logic/` |

---

## Git Workflow

**Branches:**

| Branch | Purpose |
|--------|---------|
| `main` | Stable, tested code only |
| `dev` | Integration — all subteam work merges here |
| `team/feature` | Your day-to-day work |

**Branch naming examples:** `hardware/ble-pairing`, `display/ui-layout`, `rtos/task-scheduling`

**Steps:**

```bash
# Start new work (always branch from dev)
git checkout dev
git pull origin dev
git checkout -b hardware/ble-pairing

# Commit and push
git add .
git commit -m "Add BLE scan and connect"
git push origin hardware/ble-pairing

# Then open a Merge Request into dev on GitLab
```

**Rules:**
- Never push directly to `main` or `dev` — always use Merge Requests
- Always pull `dev` before creating a new branch
- Delete your branch after it's merged

---

## Commit Message Convention

## Commit Messages
- When committing messages, please adhere to the [commit message convention](https://gist.github.com/qoomon/5dfcdf8eec66a051ecd85625518cfd13)
- Only commit related items with prefixes related to related items

### Types
- Changes relevant to the API or UI:
    - `feat` Commits that add, adjust or remove a new feature to the API or UI
    - `fix` Commits that fix an API or UI bug of a preceded `feat` commit
- `refactor` Commits that rewrite or restructure code without altering API or UI behavior
    - `perf` Commits are special type of `refactor` commits that specifically improve performance
- `style` Commits that address code style (e.g., white-space, formatting, missing semi-colons) and do not affect application behavior
- `test` Commits that add missing tests or correct existing ones
- `docs` Commits that exclusively affect documentation
- `build` Commits that affect build-related components such as build tools, dependencies, project version, ...
- `ops` Commits that affect operational aspects like infrastructure (IaC), deployment scripts, CI/CD pipelines, backups, monitoring, or recovery procedures, ...
- `chore` Commits that represent tasks like initial commit, modifying `.gitignore`, ...

---
## Getting Started

1. TODO: Add here

---

## Hardware
- **board:** ESP32-P4-WIFI6
	- [product page](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-7b.htm?srsltid=AfmBOopjKFRQeYlBdgiozdVEvXocaN9YvudQJKqS6X639Ct8jHlUdhCC)
	- [wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B)
  - [dependencies](https://components.espressif.com/components/waveshare/esp32_p4_wifi6_touch_lcd_7b/versions/1.0.2/readme?language=en)
  - [flashing-c6-chip-using-p4-chip](https://github.com/lboshuizen/crowpanel-p4-c6-sdio-ota)

- **mmWave Motion Sensor:** HMMD_mmWave_Sensor
	- communication protocol: UART
	- [product page](https://www.waveshare.com/hmmd-mmwave-sensor.htm?srsltid=AfmBOop5wLOV6OTvkNYN8VJNB9K_mb6IOMyQ69yI2wphHusvhHGw5z-2)
	- [wiki](http://www.waveshare.com/wiki/HMMD_mmWave_Sensor)
- **BLE:** Built-In
# C6 BLE/SDIO code (for ESP32-P4-WIFI6)

Note: Repo is currently designed in mind of the C6 chip on the P4-WIFI6 board, NOT the ESP32-C6-DevKitC-1-N8 Development Board

The C6 runs the BLE stack and an SDIO slave. The P4 (host) drives the BLE feature set entirely over SDIO; the same commands are also exposed as a UART console for development. Both transports call into the same `ble_pair_*` core, so a feature added to one is visible to the other without code duplication.

## How to Flash C6 Chip for Waveshare ESP32-P4-WIFI6-Touch-LCD-7B

1. Ensure that P4-Board is off
2. Connect USB-C cable from PC to P4-Board USB port
3. Connect C6-UART to USB->TTL Converter
	- C6-UART: TXD -> USB->TTL: RXD
	- C6-UART: RXD -> USB->TTL: TXD
	- C6-UART: GND -> USB->TTL: GND
	- C6-UART: IO9 -> P4-Board: GND (puts C6 in download mode for flashing firmware)
4. Turn on P4-Board
5. Press "Build, Flash, and Monitor" from ESP-IDF
6. After code has successfully flashed, unplug cable for C6-UART: IO9 -> P4-Board: GND
7. Turn off P4-Board and turn it on again

## BLE Console Commands

The C6 exposes a console REPL over UART0 for discovering, pairing, and disconnecting BLE peripherals. The same operations are also available to the P4 over SDIO — see [SDIO Command Channel](#sdio-command-channel) below.

### Connecting to the console

| Setting | Value |
| --- | --- |
| Port | `/dev/ttyUSB0` (CP2102 bridge on the C6-UART header) |
| Baud rate | `115200` |
| Line ending | `CR` |
| DTR / RTS | **uncheck both** (avoids spurious resets on connect) |

After connecting you should see the prompt:
```
tether>
```

### Available commands

| Command | What it does |
| --- | --- |
| `help` | List all registered commands. |
| `show` | Active-scan for `ADV_SCAN_WINDOW_MS` (default 5 s) and print every advertising BLE device. Devices broadcasting the priority name (`Holy-IOT*`) are listed first, then other named devices, then `<unknown>` advertisers (privacy-randomized phones, beacons). RSSI is printed with every entry. |
| `connected` | List all currently connected devices with RSSI. |
| `pair <index>` | Initiate a connection to the device at `<index>` from the most recent `show` output. The C6 caches the snapshot, so you can pair without re-scanning. |
| `disconnect <index>` | Terminate the connection to the device at `<index>` from the most recent `show` output. Fails if the device is not currently connected. |
| `read_adv <index>` | Rescan the device at `<index>` from the most recent `show` output. Prints out advertised data as hex. |
| `parse_adv <index>` | Decode ESP32 advertisement data, exposing manufacturer ID and accelerometer readings. |

### Typical pairing session

```
tether> show
Scanning for 5000 ms...
Found 12 devices:
  [0]  60:74:F4:83:9E:C7  Holy-IOT-A1B2
  [1]  C2:39:01:55:7A:8E  ihoment_H7170_9EC7
  [2]  47:4C:24:07:48:4E  <unknown>
  ...
tether> pair 0
tether> connected
  [0] Holy-IOT-A1B2  60:74:F4:83:9E:C7  -42 dBm
Connected: 1 device(s).
tether> disconnect 0
```

### Notes

- The auto-connect logic also runs continuously in the background for advertisers whose Complete Local Name matches `BLE_PEER_NAME` (defined in [components/ble/ble_tether.h](components/ble/ble_tether.h)). The `show` / `pair` commands are for manually connecting to anything else.
- iOS / Android devices broadcasting in the background appear as `<unknown>` with random MACs by design; they don't respond to scan requests. This does **not** affect your iPhone connecting *to* the C6 as a peripheral (advertised name `"Tether"`) — that flow is unaffected.
- Discovery snapshot capacity is `MAX_ADVERTISED_DEVICES` (default 128) in [components/tether_config/tether_slave_cfg.h](components/tether_config/tether_slave_cfg.h). The priority prefix the snapshot sorts by is `BLE_PEER_NAME` in [components/ble/ble_tether.h](components/ble/ble_tether.h).

## SDIO Command Channel

The P4 host issues the same BLE operations to the C6 over SDIO using a small TLV protocol. The C6 is the "smart" side: it owns scanning, deduplication, RSSI smoothing, snapshot sorting, and pairing. The P4 receives a ready-to-display list of devices and never sees raw advertising packets.

### Wire format

Every packet (P4 → C6 and C6 → P4) is a TLV frame. The header is 5 bytes:

| offset | size | field    | description |
| ------ | ---- | -------- | ----------- |
| 0      | 1 B  | `op`     | opcode: command, response (`0x80 \| cmd`), or event (`0xC0 \| kind`) |
| 1      | 1 B  | `status` | error code; meaningful only on the **last** fragment of a logical message |
| 2      | 1 B  | `flags`  | bit 0 = `MORE` (more fragments follow for this logical message) |
| 3      | 2 B  | `len`    | bytes of payload in **this** fragment, little-endian |
| 5      | N    | payload  | opcode-specific (see below) |

Both chips are little-endian, so the header can be `memcpy`'d to/from a `tether_frame_hdr_t` struct directly. Definitions and constants are in [components/sdio-slave/tether_sdio_proto.h](components/sdio-slave/tether_sdio_proto.h) — that file is intentionally free of ESP-IDF dependencies so it can be shared verbatim with the P4 code.

Each frame fits in one SDIO buffer (`TETHER_MAX_FRAME_SIZE` = 128 B). Logical messages larger than that — currently only the slim scan list — span multiple frames with the same opcode, all carrying `MORE=1` until the final one. SDIO preserves frame order, so the host concatenates payloads in arrival order.

### Commands (P4 → C6)

| op   | name           | req payload          | response payload |
| ---- | -------------- | -------------------- | ---------------- |
| `0x01` | `SHOW`       | empty                | slim per-device list (idx, mac, rssi, name) |
| `0x02` | `PAIR`       | `u8 index`           | empty (ACK only — pair completion arrives as `EVT_PAIR_COMPLETE`) |
| `0x03` | `READ_ADV`   | `u8 index`           | `u8 idx, u8 mac[6], u8 name_len, char name[name_len], u8 data_len, u8 data[data_len]` |
| `0x04` | `CONNECTED`  | empty                | `u8 count, {u8 mac[6], i8 rssi} × count` |
| `0x05` | `DISCONNECT` | `u8 index`           | empty (ACK only — disconnect arrives as `EVT_PEER_DISCONN`) |
| `0x06` | `REMOVE`     | empty                | not yet implemented — returns `ST_INTERNAL` |

Indices throughout are snapshot indices (same as `pair`), so the P4 can `SHOW` once and then `PAIR` / `DISCONNECT` by index without re-scanning.

`SHOW` returns a **slim** per-entry record: 1 B index, 6 B MAC, 1 B RSSI, 1 B name length, up to 20 B truncated name. Full per-device detail (full name + raw manufacturer data) is fetched via `READ_ADV` for a single index. `READ_ADV` does **not** trigger a live rescan when invoked via SDIO; it returns cached snapshot data, so it does not block the SDIO task on a scan window. P4 calls `SHOW` first if it wants fresh data.

### Events (C6 → P4, unsolicited)

| op   | name             | payload |
| ---- | ---------------- | ------- |
| `0xC1` | `SCAN_UPDATED` | empty — pure notification, P4 pulls a fresh snapshot via `SHOW` when ready |
| `0xC2` | `PAIR_COMPLETE`| `u8 mac[6], u8 success` |
| `0xC3` | `PEER_DISCONN` | `u8 mac[6], u16 reason` |

`SCAN_UPDATED` is rate-limited inside the C6 to at most one notification per `ADV_SCAN_WINDOW_MS` regardless of how many advertisement packets arrive in that window. P4 can ignore it while it's busy and pull whenever idle.

### Status codes

| code | name           | meaning |
| ---- | -------------- | ------- |
| `0x00` | `OK`         | request handled |
| `0x01` | `BAD_OPCODE` | unknown opcode |
| `0x02` | `BAD_LEN`    | payload length didn't match opcode |
| `0x03` | `BAD_INDEX`  | index out of range for the current snapshot |
| `0x04` | `NO_SNAPSHOT`| no scan data available yet |
| `0x05` | `BLE_ERR`    | NimBLE-side failure (e.g. pair / disconnect) |
| `0x06` | `BUSY`       | reserved, not currently emitted |
| `0xFF` | `INTERNAL`   | unimplemented opcode or unexpected error |

### Host-side notifications

The SDIO slave driver automatically asserts `SEND_NEW_PACKET` on the host interrupt line whenever a TLV frame is queued for the host, so the P4 doesn't have to poll — it can sleep on the interrupt and read the frame when it wakes.

## Architecture

```
P4 ──RX──▶  tether_sdio_slave  ──▶  tether_sdio_cmd  ──▶  ble_pair_*
                  task                 (dispatcher)         (accessors)
P4 ◀──TX──  tether_sdio_slave  ◀──  tether_sdio_events  ◀──  NimBLE
                                       (event queue)           callbacks
P4 ◀─INT──  SDIO driver (auto on each TX)
```

| Component | Files | Responsibility |
| --------- | ----- | -------------- |
| Wire protocol | [tether_sdio_proto.h](components/sdio-slave/tether_sdio_proto.h) | TLV header + opcode/status constants. Shared verbatim with P4. |
| SDIO transport | [tether_sdio_slave.{c,h}](components/sdio-slave/tether_sdio_slave.c) | Brings up the slave, runs the RX/dispatch task, exposes `tether_sdio_send_frame()`. |
| Command dispatcher | [tether_sdio_cmd.{c,h}](components/sdio-slave/tether_sdio_cmd.c) | Parses inbound TLV, routes to `ble_pair_*`, serializes responses. |
| Event queue | [tether_sdio_events.{c,h}](components/sdio-slave/tether_sdio_events.c) | FreeRTOS queue between NimBLE callbacks and SDIO TX; rate-limits `SCAN_UPDATED`. |
| BLE pair core | [ble_pair.{c,h}](components/ble/pair/ble_pair.c) | Snapshot state, per-index accessors used by both transports, pair/disconnect actions. |
| Console REPL | [ble_pair_console.{c,h}](components/ble/pair/ble_pair_console.c) | UART command handlers; thin layer over `ble_pair_*`. |
| BLE host | [ble_tether.c](components/ble/ble_tether.c) | NimBLE init and GAP callbacks; posts events on connect/disconnect/scan. |
