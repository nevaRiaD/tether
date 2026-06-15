#ifndef TETHER_SLAVE_CFG_H
#define TETHER_SLAVE_CFG_H

#include "tether_sdio_proto.h"

/* ========== FREERTOS TASK PRIORITY ========== */
/*
 * @brief   Higher values mean higher priorities, latency critical tasks require higher values
 * 
 * Priority (1-24)	Typical use
 * 1–4	Background / housekeeping (logging, slow polling)
 * 5	Default for most app tasks (ESP_TASK_MAIN_PRIO)
 * 10–15	Time-sensitive sensor / control loops
 * 18+	Wi-Fi / Bluetooth / IDF system tasks — don't crowd these
*/
#define BLE_DISPLAY_RSSI_TASK_PRIO      5
#define SDIO_SLAVE_TASK_PRIO            10  // transport from C6; higher than consumers

/* TASK STACK MEMORY (B)
 * @brief   memory allocated for each stack size
 *          by default should be 4096 = 4KB
*/
#define BLE_DISPLAY_RSSI_TASK_STACK     8192   /* Kalman needs extra stack for floats */
#define BLE_RSSI_TRACE_CMD_TASK_PRIO    5
#define BLE_RSSI_TRACE_CMD_TASK_STACK   3072
#define SDIO_SLAVE_TASK_STACK           4096

/* TASK DELAY INTERVAL (MS) */
#define SDIO_SLAVE_TASK_DELAY           1

/* ========== BLUETOOTH LOW ENERGY ========== */

/* BLE */
#define BLE_POLL_INTERVAL_MS			20
#define MAX_CONNECTED_DEVICES			TETHER_SCAN_MAX
#define MAX_ADVERTISED_DEVICES			255
#define BLE_SUPERVISION_TIMEOUT_FACTOR 	30
#define MAX_ADVERTISEMENT_INTERVAL		10000	// 10 seconds

/* RSSI Buffer */
#define MAX_RSSI_BUFF_SIZE				8
#define MIN_RSSI_ENTRIES				4
#define RSSI_VAR_THRESHOLD				10	// stddev ~4 dBm; tune empirically (stationary << this, moving >> this)
#define RSSI_THRESHOLD					-55

/* BLE Accelerometer Tags */
#define RSSI_THRESHOLD_ACCEL            -95

/* ── 2-state Kalman rolling motion detection ────────────────────────────────
 * Used when T_BLE_TEST_RSSI_ROLLING is defined (below).
 *
 * State vector: [rssi_level (dBm), rssi_velocity (dBm/s)]
 * Process model: constant-velocity (F = [[1,dt],[0,1]])
 * Measurement: RSSI level only (H = [1, 0])
 *
 * Motion is detected via drift (level vs slow baseline) and velocity magnitude.
 * A leaky bucket integrates evidence; hysteresis prevents chatter.
 */
#define RSSI_TRACE_COUNTDOWN_MS                         3000
#define RSSI_TRACE_TEST_MS                              4000
#define RSSI_TRACE_MAX_SAMPLES \
    (((RSSI_TRACE_TEST_MS + BLE_POLL_INTERVAL_MS - 1) / BLE_POLL_INTERVAL_MS) + 1)

#define RSSI_MOTION_KF_Q_LEVEL      0.5f   /* process noise added to level variance per sample */
#define RSSI_MOTION_KF_Q_VEL        0.5f   /* process noise added to velocity variance per sample */
#define RSSI_MOTION_KF_R            20.0f  /* measurement noise variance — lower = faster level tracking */
#define RSSI_MOTION_DRIFT_ALPHA     0.002f /* EMA smoothing factor for baseline (~10s time constant at 50Hz) */
#define RSSI_MOTION_DRIFT_THRESHOLD 5.5f   /* dBm drift above baseline to feed the leaky bucket */
#define RSSI_MOTION_SCORE_DECAY     0.05f  /* bucket drain per sample when drift < threshold */
#define RSSI_MOTION_SCORE_TRIGGER   3.0f   /* bucket level to declare moving */
#define RSSI_MOTION_SCORE_MAX       15.0f  /* bucket cap — limits MOVE persistence to ~6s after last spike */

/* Range gate hysteresis: require RSSI to cross a higher threshold to enter CLOSE state,
 * and a lower threshold to exit. Prevents boundary oscillation (multipath near -61 dBm)
 * from generating spurious CLOSE:1/MOVE:1 combinations. */
#define RSSI_CLOSE_ENTER_THRESHOLD  -57    /* dBm — must exceed this to become CLOSE (~2 m) */
#define RSSI_CLOSE_EXIT_THRESHOLD   -67    /* dBm — must drop below this to leave CLOSE (~3.5 m) */

/* Mode Config */
#define T_BLE_LEGACY_MODE 			    1
#define T_BLE_NO_VAR					1
// #define BLE_AUTOCONNECT_ENABLED			1  // comment out to disable auto-connect to BLE_PEER_NAME advertisers
// #define BLE_PAIRING_MODE				1  // disabled — no SMP key exchange needed
#define T_BLE_TEST_MOVE_CLOSE		    1
#define T_BLE_TEST_RSSI_ROLLING			1   // enable Kalman rolling motion detection
// #define T_BLE_TEST_RSSI_TRACE		    1   // enable manual RSSI trace capture (dev only)
//#define T_BLE_LOG_ACCEL_MOVE_CLOSE      1
//#define T_BLE_LOG_DEVICESCAN            1   // enable logs for scan reports

#endif // TETHER_SLAVE_CFG_H