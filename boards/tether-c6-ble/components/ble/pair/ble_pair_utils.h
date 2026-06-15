#ifndef BLE_PAIR_UTILS_H
#define BLE_PAIR_UTILS_H

#define ADV_HANDLE_NONE        0xFFFF
#define ADV_SCAN_WINDOW_MS     5000
#define ADV_SINGLE_SHOT_SCAN_MS      1500
#define ADV_NAME_MAX           32  /* 31 chars + NUL */
#define ADV_DATA_MAX           31

#include <stdint.h>
#include "ble_rssi_buff.h"
#include "freertos/FreeRTOS.h"

/* Snapshot of devices seen during a discovery scan. Sized for MAX_ADVERTISED_DEVICES,    */
/* which can be much larger than the connected-peer cap used by scan_report_t.            */
typedef struct {
    uint8_t  count;
    uint8_t  entries[MAX_ADVERTISED_DEVICES][6];
    uint16_t handles[MAX_ADVERTISED_DEVICES];
} adv_snapshot_t;

typedef struct{
    SemaphoreHandle_t mtx;
    adv_snapshot_t    snapshot;
    uint8_t           types[MAX_ADVERTISED_DEVICES];
    char              names[MAX_ADVERTISED_DEVICES][ADV_NAME_MAX];
    uint8_t           data[MAX_ADVERTISED_DEVICES][ADV_DATA_MAX];
    uint8_t           data_len[MAX_ADVERTISED_DEVICES];
    int8_t            rssi[MAX_ADVERTISED_DEVICES];
    TickType_t        last_seen[MAX_ADVERTISED_DEVICES];
} s_adv_t;

/*
* Copies advertisement name from one buffer to another
*/
void copy_adv_name(char *dest, uint8_t dest_size, const uint8_t *src, uint8_t src_len);

/*
* Copies advertisement data from one buffer to another 
*/
void copy_adv_data(uint8_t *dest, uint8_t dest_size, uint8_t *data_len, const uint8_t *source, uint8_t source_len);

/*
* Determines priority of an advertisement based on its name for sorting purposes.
*/
uint8_t adv_priority(uint8_t *device_name);

/*
* Swaps all advertisement data for two slots in the snapshot for sorting purposes.
*/
void swap_adv_slot(s_adv_t *adv, uint8_t a, uint8_t b);

void ble_pair_ensure_mtx(SemaphoreHandle_t *mtx);

void ble_pair_clear_adv(s_adv_t *s_adv);

const char *find_peer_name_by_addr(const uint8_t *addr, s_adv_t *s_adv);

void sort_snapshot_by_priority(s_adv_t *s_adv);

#endif // BLE_PAIR_UTILS_H
