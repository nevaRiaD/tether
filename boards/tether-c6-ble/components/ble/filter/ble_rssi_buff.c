#include "ble_rssi_buff.h"
#include "ble_pair.h"

#include "esp_central.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "ble_accel.h"
#include <math.h>
#include <string.h>
#include "tether_slave_cfg.h"

static const char *TAG = "RSSI";

/* ── 2-state Kalman motion detection ─────────────────────────────────────
 * Active when T_BLE_TEST_RSSI_ROLLING is defined in tether_slave_cfg.h.
 *
 * State vector: x = [level (dBm), velocity (dBm/s)]
 * Process model: constant velocity — F = [[1, dt], [0, 1]]
 * Measurement: RSSI level only — H = [1, 0]
 *
 * Velocity is a first-class filter state, not a post-hoc derivative.
 * Motion is declared when drift (level vs slow baseline) or velocity
 * magnitude feeds a leaky bucket past the trigger threshold.
 * -------------------------------------------------------------------- */
#ifdef T_BLE_TEST_RSSI_ROLLING

/* Called every BLE_POLL_INTERVAL_MS per peer.
 * Runs a full predict+update step of the 2-state Kalman filter. */
static void rssi_kf_update(struct peer *peer, int8_t rssi_raw, uint32_t now_ms)
{
    if (!peer->rssi_kf_initialized) {
        peer->rssi_kf_level          = (float)rssi_raw;
        peer->rssi_kf_velocity       = 0.0f;
        peer->rssi_kf_level_baseline = (float)rssi_raw;
        peer->rssi_kf_P[0]          = 1.0f;
        peer->rssi_kf_P[1]          = 0.0f;
        peer->rssi_kf_P[2]          = 0.0f;
        peer->rssi_kf_P[3]          = 10.0f;
        peer->rssi_kf_last_ms       = now_ms;
        peer->rssi_motion_score      = RSSI_MOTION_SCORE_MAX;
        peer->rssi_motion_moving     = true;
        peer->rssi_kf_initialized    = true;
        return;
    }

    float dt = (float)(now_ms - peer->rssi_kf_last_ms) * 0.001f;
    if (dt <= 0.0f) return;
    if (dt > 1.0f) dt = 1.0f; /* cap large gaps to prevent covariance blow-up */
    peer->rssi_kf_last_ms = now_ms;

    /* ── Predict: F = [[1,dt],[0,1]] ──────────────────────────────── */
    float lp = peer->rssi_kf_level + dt * peer->rssi_kf_velocity;
    float vp = peer->rssi_kf_velocity;

    float p00 = peer->rssi_kf_P[0];
    float p01 = peer->rssi_kf_P[1];
    float p10 = peer->rssi_kf_P[2];
    float p11 = peer->rssi_kf_P[3];

    /* P_pred = F*P*F^T + Q */
    float pp00 = p00 + dt * (p10 + p01) + dt * dt * p11 + RSSI_MOTION_KF_Q_LEVEL;
    float pp01 = p01 + dt * p11;
    float pp10 = p10 + dt * p11;
    float pp11 = p11 + RSSI_MOTION_KF_Q_VEL;

    /* ── Update: H = [1, 0] ───────────────────────────────────────── */
    float innov = (float)rssi_raw - lp;
    float S     = pp00 + RSSI_MOTION_KF_R;
    float k0    = pp00 / S;  /* Kalman gain for level */
    float k1    = pp10 / S;  /* Kalman gain for velocity */

    peer->rssi_kf_level    = lp + k0 * innov;
    peer->rssi_kf_velocity = vp + k1 * innov;

    /* P = (I - K*H) * P_pred */
    peer->rssi_kf_P[0] = (1.0f - k0) * pp00;
    peer->rssi_kf_P[1] = (1.0f - k0) * pp01;
    peer->rssi_kf_P[2] = pp10 - k1 * pp00;
    peer->rssi_kf_P[3] = pp11 - k1 * pp01;
}

#endif /* T_BLE_TEST_RSSI_ROLLING */

/* ========== RSSI BUFFER FUNCTIONS ========== */

/*
 * @brief Check if RSSI buffer is full (reserves 1 slot)
*/
bool peer_rssi_buff_full(const struct peer *peer)
{
	return ((peer->rssi_head + 1) % PEER_RSSI_BUFF_SIZE) == peer->rssi_tail;
}

/*
 * @brief Check if RSSI buffer is empty
*/
bool peer_rssi_buff_empty(const struct peer *peer)
{
	return peer->rssi_head == peer->rssi_tail;
}

int peer_rssi_buff_push(struct peer *peer, int8_t rssi)
{
	peer->rssi_buff[peer->rssi_head] = rssi;
	peer->rssi_head = (peer->rssi_head + 1) % PEER_RSSI_BUFF_SIZE;
	if (peer->rssi_head == peer->rssi_tail) {
		peer->rssi_tail = (peer->rssi_tail + 1) % PEER_RSSI_BUFF_SIZE;
	}
	return 0;
}

int peer_rssi_buff_pop(struct peer *peer, int8_t *rssi)
{
	if (peer_rssi_buff_empty(peer)) {
		return 1;
	}
	*rssi = peer->rssi_buff[peer->rssi_tail];
    peer->rssi_tail = (peer->rssi_tail + 1) % PEER_RSSI_BUFF_SIZE;
    return 0;
}

/*
 * @brief Compute variance based on RSSI circular buffer
*/
int peer_rssi_comp_var(const struct peer *peer, uint16_t *var_out)
{
    uint8_t entries_count = (peer->rssi_head - peer->rssi_tail + PEER_RSSI_BUFF_SIZE) % PEER_RSSI_BUFF_SIZE;
    if (entries_count < MIN_RSSI_ENTRIES) return 1;

    int sum = 0;
    for (uint8_t i = 0; i < entries_count; i++) {
        sum += peer->rssi_buff[(peer->rssi_tail + i) % PEER_RSSI_BUFF_SIZE];
    }
    int mean = sum / entries_count;

    int32_t sq_sum = 0;
    for (uint8_t i = 0; i < entries_count; i++) {
        int d = peer->rssi_buff[(peer->rssi_tail + i) % PEER_RSSI_BUFF_SIZE] - mean;
        sq_sum += d * d;
    }
    *var_out = (uint16_t)(sq_sum / entries_count);

    return 0;
}

/*
 * Called once per BLE_POLL_INTERVAL_MS per connected peer.
 * Decides whether the peer is "close and non-stationary" and, if so,
 * adds its MAC to the outgoing scan_report.
 */
int ble_rssi_move_close(const struct peer *peer, void *arg)
{
	scan_report_t *scan_report = (scan_report_t *)arg;
	int8_t rssi = 0;

	if (peer->device_type != DEVICE_TYPE_RSSI) {
		return 0; /* skip this peer */
	}

	if (ble_gap_conn_rssi(peer->conn_handle, &rssi) != 0) {
		return 0; /* skip this peer, keep traversing */
	}

	/* Hysteretic range gate: enter CLOSE at -57 dBm, exit at -67 dBm.
	 * Prevents tags oscillating at the boundary from generating spurious detections. */
	struct peer *mp = (struct peer *)peer;
	if (rssi >= RSSI_CLOSE_ENTER_THRESHOLD)      mp->rssi_close = true;
	else if (rssi < RSSI_CLOSE_EXIT_THRESHOLD)   mp->rssi_close = false;
	bool is_close = mp->rssi_close;
	int  is_moving = 0;

#ifdef T_BLE_TEST_RSSI_ROLLING
	/* ── 2-state Kalman + level drift + velocity + leaky bucket ──── */
	uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
	rssi_kf_update(mp, rssi, now_ms);

	mp->rssi_kf_level_baseline += RSSI_MOTION_DRIFT_ALPHA
	    * (mp->rssi_kf_level - mp->rssi_kf_level_baseline);

	float drift = mp->rssi_kf_level - mp->rssi_kf_level_baseline;

	if (drift >= RSSI_MOTION_DRIFT_THRESHOLD)
		mp->rssi_motion_score += 1.0f;
	else
		mp->rssi_motion_score -= RSSI_MOTION_SCORE_DECAY;
	if (mp->rssi_motion_score > RSSI_MOTION_SCORE_MAX) mp->rssi_motion_score = RSSI_MOTION_SCORE_MAX;
	if (mp->rssi_motion_score < 0.0f) mp->rssi_motion_score = 0.0f;

	if (mp->rssi_motion_score >= RSSI_MOTION_SCORE_TRIGGER) mp->rssi_motion_moving = true;
	if (mp->rssi_motion_score <= 0.0f) mp->rssi_motion_moving = false;

	is_moving = mp->rssi_motion_moving ? 1 : 0;
#else
	/* ── Fallback: simple variance over the circular RSSI buffer ─── */
	uint16_t rssi_var = 0;
	if (peer_rssi_comp_var(peer, &rssi_var) == 0) {
#ifndef T_BLE_NO_VAR
		non_stationary = (rssi_var > RSSI_VAR_THRESHOLD);
#else
		non_stationary = 1;
#endif
	}
	/* push *after* computing so variance reflects prior history */
	peer_rssi_buff_push((struct peer *)peer, rssi);
#endif

	if (is_close && is_moving && (scan_report->count < MAX_CONNECTED_DEVICES)) {
		struct ble_gap_conn_desc desc;
		if (ble_gap_conn_find(peer->conn_handle, &desc) != 0) {
			ESP_LOGI(TAG, "Cannot find connection handle");
			return 0;
		}
		uint8_t i = scan_report->count;
		memcpy(scan_report->entries[i], desc.peer_id_addr.val, 6);
		scan_report->count++;
	}

#ifdef T_BLE_TEST_MOVE_CLOSE
	/* Rate-limit debug output to ~2 Hz per peer (every 25th poll at 50 Hz).
	 * Logging with float formatting at full rate starves the SDIO slave task. */
	mp->rssi_log_skip = (mp->rssi_log_skip + 1) % 25;
	if (mp->rssi_log_skip == 0) {
		struct ble_gap_conn_desc desc;
		if (ble_gap_conn_find(peer->conn_handle, &desc) == 0) {
#ifdef T_BLE_TEST_RSSI_ROLLING
			ESP_LOGI(TAG, "MOVE:%d CLOSE:%d [%02X:%02X:%02X:%02X:%02X:%02X] RSSI:%d DRIFT:%.1f SCR:%.1f",
					 is_moving, is_close,
					 desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3],
					 desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
					 rssi,
					 (double)(peer->rssi_kf_level - peer->rssi_kf_level_baseline),
					 (double)peer->rssi_motion_score);
#endif
		}
	}
#endif
	return 0;
}