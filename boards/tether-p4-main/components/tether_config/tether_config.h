#ifndef TETHER_CONFIG_H
#define TETHER_CONFIG_H

/* ========== FREERTOS TASK PRIORITY ========== */
/*
 * Higher values mean higher priorities. Latency-critical tasks require higher values.
 *
 * Priority (1-24) typical use:
 * 1-4   Background / housekeeping
 * 5     Default for most app tasks
 * 10-15 Time-sensitive sensor / control loops
 * 18+   Wi-Fi / Bluetooth / IDF system tasks
 */
#define BLE_DISPLAY_RSSI_TASK_PRIO      5
#define VL53L1X_DETECT_TASK_PRIO        5
#define SDIO_MASTER_TASK_PRIO           10  // transport from C6; higher than consumers

/* TASK STACK MEMORY (B) */
#define BLE_DISPLAY_RSSI_TASK_STACK     4096
#define VL53L1X_DETECT_TASK_STACK       4096
#define SDIO_MASTER_TASK_STACK          4096

/* TASK DELAY INTERVAL (MS) */
#define VL53L1X_DETECT_TASK_DELAY       100
#define SCHEDULE_TASK_DELAY             30000

/* ========== CLOCK CONFIGURATION ========== */
// #define SCHEDULE_LOGS_MODE              1

/* ========== BLUETOOTH LOW ENERGY ========== */

/* BLE */
#define BLE_POLL_INTERVAL_MS            20
#define MAX_DEVICES                     30
#define BLE_SUPERVISION_TIMEOUT_FACTOR  30

/* RSSI Buffer */
#define MAX_RSSI_BUFF_SIZE              8
#define MIN_RSSI_ENTRIES                4
#define RSSI_VAR_THRESHOLD              10
#define RSSI_THRESHOLD                  -45

/* Mode Config */
#define T_BLE_LEGACY_MODE               1
// #define T_PRINTSCAN_TEST_MODE           1    // to print out ble_events in device_scan.c
// #define BLE_PAIRING_MODE             1
// #define TEST_RSSI_MODE               1
// #define T_BLE_TEST_MOVE_CLOSE        1

/* ========== SDIO MASTER (P4 host -> C6 BLE slave) ========== */
#define SDIO_MASTER_PIN_CLK     18
#define SDIO_MASTER_PIN_CMD     19
#define SDIO_MASTER_PIN_D0      14
#define SDIO_MASTER_PIN_D1      15
#define SDIO_MASTER_PIN_D2      16
#define SDIO_MASTER_PIN_D3      17

#define SDIO_MASTER_USE_4BIT        1   /* 1 = 4-bit bus, 0 = 1-bit bus */
#define SDIO_MASTER_HIGHSPEED       0   /* 1 = 40 MHz, 0 = default ~20 MHz */
#define SDIO_MASTER_INPUT_DELAY     0   /* sdmmc_delay_phase_t value */
#define SDIO_MASTER_RECV_BUFFER_SZ  128 /* slave per-buffer size; must match slave */

#define SDIO_MASTER_HOST_SLOT_1     SDMMC_HOST_SLOT_1

/* ========== TIME OF FLIGHT ========== */

/* TOF Proximity Threshold (mm) */
#define TOF_THRESHOLD_MM                1000

/* PIN Configuration */
#define ESP32_P4 1

#ifdef ESP32_P4 // ESP32_P4 (main board)
    #define SDA_PIN             7
    #define SCL_PIN             8
    #define GLITCH_IGNORE_CNT   7
#else           // ESP32_C6_DEVELOPMENT_BOARD
    #define SDA_PIN             6
    #define SCL_PIN             7
#endif

/* I2C Configuration */
#define I2C_SLAVE_ADDR                  0x29
#define I2C_TIMEOUT_MS                  1000

/* Mode Config */
// #define T_TOF_TEST_MODE              1

/* ========== TAG PAIRING (UI discovery scan) ========== */
#define PAIR_SCAN_TIMEOUT_MS            8000   /* per OP_SHOW discovery attempt */
#define PAIR_SCAN_MAX_ATTEMPTS          3      /* attempts before "no tags found" */
#define PAIR_SCAN_RETRY_DELAY_MS        500    /* pause between failed attempts */

#endif // TETHER_CONFIG_H
