#ifndef DEVICE_SCAN_H
#define DEVICE_SCAN_H

#include <stdint.h>
#include "vl53l1_platform.h"
#include "esp_log.h"
#include "tether_config.h"
#include <stdbool.h>

// Include function for both BLE and ToF

typedef struct __attribute__((packed)) {
	uint8_t mac[6];
} device_entry_t;

typedef struct __attribute__((packed)) {
	uint8_t			count;
	uint8_t			entries[MAX_DEVICES][6];
} scan_report_t;

// scan_report_t Tether_Scan(void);
bool User_Detected(void);

#endif // DEVICE_SCAN_H
