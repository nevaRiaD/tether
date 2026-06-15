# Accel Tag firmware (`accel_tag.ino`)

Arduino sketch for the **motion tag** — a separate ESP32 (C6 or C3 SuperMini) with an
MPU6050 accelerometer. It runs a BLE **GATT server** that exposes a single motion-flag
characteristic (`0`/`1`). The **C6 central** (this repo's ESP-IDF firmware) scans for it,
connects, subscribes to notifications, and reads the motion state.

> This file is **not** part of the `idf.py` build — it lives under `tools/` and is flashed
> separately with the Arduino IDE / `arduino-cli`. The C6's [main/CMakeLists.txt](../../main/CMakeLists.txt)
> only compiles `app_main.c`, so nothing here is picked up.

## ⚠️ This must stay in sync with the C6 central

The tag and the C6 share a hard-coded contract. If you change one side, change the other.
Source of truth on the C6 is [components/ble/ble_tether.c](../../components/ble/ble_tether.c).

| Contract | Tag (`accel_tag.ino`) | C6 central |
|---|---|---|
| **Service UUID** | `SERVICE_UUID` = `59462f12-9543-9999-12c8-58b459a2712d` | `remote_svc_uuid` ([ble_tether.c:22](../../components/ble/ble_tether.c#L22)) |
| **Motion char UUID** | `MOTION_CHAR_UUID` = `33333333-2222-2222-1111-111100000001` | `remote_chr_uuid` ([ble_tether.c:32](../../components/ble/ble_tether.c#L32)) |
| **Device name prefix** | `DEVICE_NAME` must start with `"Tether Tag "` | classified as an accel tag by name prefix ([ble_pair.c:140](../../components/ble/pair/ble_pair.c#L140), [peer.c:925](../../components/ble/lib/peer.c#L925)) |
| **CCCD** | `BLE2902()` descriptor on the char | C6 writes `0x0001` to subscribe |

Notes:
- On the C6 side the UUID bytes are written **little-endian** (the printed string reversed)
  inside `BLE_UUID128_DECLARE`. The string in the table is the canonical/printed form.
- The name prefix check is `strncmp(name, "Tether Tag", 10)` — so `"Tether Tag 1"`,
  `"Tether Tag 2"`, … all classify as accel tags. Use a unique number per physical tag.

## Behavior

- Samples the MPU6050 every `SAMPLE_INTERVAL_MS` (20 ms ≈ 50 Hz).
- `isMoving()` decides motion from the **rolling jitter** (avg per-sample change in
  acceleration magnitude over `WINDOW_SIZE` samples) vs `MOTION_THRESHOLD`, not from raw
  magnitude — this rejects the constant ~1000 mg gravity bias. The first `WINDOW_SIZE`
  samples after boot are ignored while the window fills.
- Notifies the C6 **only on change** (idle/moving transitions) to cut radio traffic; the
  characteristic value is kept current so plain GATT reads also work.

`MOTION_THRESHOLD` (mg) is empirical — tune it on the bench. It was originally set for
100 ms sampling; at 50 Hz per-sample diffs are smaller, so you may need a lower value.

## Board selection

Set one of `ESP32C6` / `ESP32C3` at the top of the sketch to pick the I2C pins:

| Board | SDA | SCL |
|---|---|---|
| ESP32-C6 SuperMini | 22 | 23 |
| ESP32-C3 SuperMini | 8 | 9 |

## Dependencies (Arduino libraries)

- `Adafruit MPU6050`
- `Adafruit Unified Sensor`
- ESP32 BLE (bundled with the Espressif Arduino core)
