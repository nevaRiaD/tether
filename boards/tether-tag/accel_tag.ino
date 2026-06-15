#include <Wire.h>
#include <math.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// --- Configuration ---
#define SAMPLE_INTERVAL_MS  20                  // 50 Hz sampling
#define DEVICE_NAME         "Tether Tag 4"      // name prefix "Tether Tag" -> C6 classifies as accel
#define WINDOW_SIZE         25                  // rolling jitter window (~0.5 s @ 50 Hz)
#define MOTION_THRESHOLD    20                  // avg per-sample jitter in mg; tune empirically

// GATT UUIDs — MUST match the C6 central (ble_tether.c: remote_svc_uuid / remote_chr_uuid)
#define SERVICE_UUID        "59462f12-9543-9999-12c8-58b459a2712d"   // == remote_svc_uuid
#define MOTION_CHAR_UUID    "33333333-2222-2222-1111-111100000001"   // == remote_chr_uuid

//#define XIAO_ESP32_C6           1
#define XIAO_ESP32_C3           1
//#define ESP32_C6_DEVKITC_1      1
//#define ESP32_DOIT_DEVKIT_V1    1

#ifdef XIAO_ESP32_C6
    #define SDA_PIN 22
    #define SCL_PIN 23
#endif
#ifdef XIAO_ESP32_C3
    #define SDA_PIN 8
    #define SCL_PIN 9
#endif
#ifdef ESP32_C6_DEVKITC_1
    #define SDA_PIN 6
    #define SCL_PIN 7
#endif
#ifdef ESP32_DOIT_DEVKIT_V1
    #define SDA_PIN 21
    #define SCL_PIN 22
#endif

Adafruit_MPU6050 mpu;

BLEServer         *pServer          = nullptr;
BLECharacteristic *pMotionChar      = nullptr;
bool               deviceConnected  = false;

// Restart advertising when the central disconnects, so the C6 can reconnect.
class TetherServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *s) override    { deviceConnected = true; }
    void onDisconnect(BLEServer *s) override {
        deviceConnected = false;
        BLEDevice::startAdvertising();
    }
};

void setupBLE() {
    BLEDevice::init(DEVICE_NAME);

    // Max TX power so the C6 reads a strong RSSI (closer to 0 dBm).
    // Set each power type: default (connection) and advertising.
    BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
    BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new TetherServerCallbacks());

    BLEService *pService = pServer->createService(BLEUUID(SERVICE_UUID));

    // Motion flag: READ + NOTIFY. The BLE2902 descriptor is the CCCD (0x2902)
    // the C6 writes 0x0001 to in ble_central_on_disc_complete to subscribe.
    pMotionChar = pService->createCharacteristic(
        MOTION_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pMotionChar->addDescriptor(new BLE2902());

    uint8_t initial = 0;
    pMotionChar->setValue(&initial, 1);

    pService->start();

    // Advertise the 128-bit service UUID; the device name goes in the scan
    // response so name + 128-bit UUID don't overflow the 31-byte adv payload.
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinInterval(0x20);   // 20 ms
    pAdvertising->setMaxInterval(0x40);   // 40 ms
    BLEDevice::startAdvertising();
}

void setup() {
    delay(1000);

    Serial.begin(115200);
    Serial.println("\n[accel_tag] boot");

    Wire.begin(SDA_PIN, SCL_PIN);
    if (!mpu.begin()) {                           // halt if the MPU6050 isn't found
        Serial.println("[accel_tag] MPU6050 not found - halting");
        while (1) delay(100);
    }
    mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

    setupBLE();
}

// Motion via rolling jitter: average the per-sample change in acceleration
// magnitude over WINDOW_SIZE samples and compare to MOTION_THRESHOLD. This
// rejects the constant ~1000 mg gravity bias that a raw-magnitude check trips on.
bool isMoving() {
    static double mag_history[WINDOW_SIZE] = {0};
    static int    history_idx        = 0;
    static double prev_mag           = 1000.0;
    static int    samples_since_boot = 0;

    sensors_event_t accel, gyro, temp;
    mpu.getEvent(&accel, &gyro, &temp);

    int16_t x_mg = (int16_t)(accel.acceleration.x * 101.97);   // m/s^2 -> mg
    int16_t y_mg = (int16_t)(accel.acceleration.y * 101.97);
    int16_t z_mg = (int16_t)(accel.acceleration.z * 101.97);

    int32_t x_sq = (int32_t)x_mg * x_mg;
    int32_t y_sq = (int32_t)y_mg * y_mg;
    int32_t z_sq = (int32_t)z_mg * z_mg;
    double acc_mag = sqrt(x_sq + y_sq + z_sq);

    // Rolling jitter calculation
    double diff = fabs(acc_mag - prev_mag);
    prev_mag = acc_mag;
    mag_history[history_idx] = diff;
    history_idx = (history_idx + 1) % WINDOW_SIZE;

    double avg_diff = 0;
    for (int i = 0; i < WINDOW_SIZE; i++) avg_diff += mag_history[i];
    avg_diff /= WINDOW_SIZE;

    samples_since_boot++;
    bool ready = samples_since_boot >= WINDOW_SIZE;   // ignore the first window while it fills
    bool result = ready && (avg_diff > MOTION_THRESHOLD);

    // DEBUG: print raw jitter periodically so MOTION_THRESHOLD can be tuned.
    // Throttled to ~5 Hz so the UART isn't flooded at the 50 Hz sample rate.
    static uint32_t last_dbg_ms = 0;
    if (millis() - last_dbg_ms >= 200) {
        last_dbg_ms = millis();
        Serial.printf("[accel_tag] avg_jitter=%.1f mg  thresh=%d  ready=%d  moving=%d\n",
                      avg_diff, MOTION_THRESHOLD, ready, result);
    }
    return result;
}

uint8_t last_motion = 0xFF;   // force a notify on first connect

void loop() {
    uint8_t motion = isMoving() ? 1 : 0;

    if (deviceConnected) {
        pMotionChar->setValue(&motion, 1);   // keep value current for READs
        if (motion != last_motion) {          // notify only on change -> less radio traffic
            pMotionChar->notify();
            Serial.printf("[accel_tag] NOTIFY motion=%d (connected)\n", motion);
            last_motion = motion;
        }
    } else {
        if (last_motion != 0xFF) Serial.println("[accel_tag] disconnected - re-arming");
        last_motion = 0xFF;                   // re-arm so reconnect gets a fresh notify
    }

    delay(SAMPLE_INTERVAL_MS);
}