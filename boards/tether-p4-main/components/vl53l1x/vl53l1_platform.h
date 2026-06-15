/**
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/**
 * @file  vl53l1_platform.h
 * @brief Those platform functions are platform dependent and have to be implemented by the user
 */

#ifndef _VL53L1_PLATFORM_H_
#define _VL53L1_PLATFORM_H_

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "vl53l1_types.h"
#include "VL53L1X_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "tether_config.h"
#include "utils.h"
#include "VL53L1X_calibration.h"
#include "vl53l1_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct {
    uint32_t dummy;
} VL53L1_Dev_t;

typedef VL53L1_Dev_t *VL53L1_DEV;

int8_t VL53L1_WriteMulti(uint16_t dev, uint16_t index, uint8_t *pdata, uint32_t count);
int8_t VL53L1_ReadMulti(uint16_t dev, uint16_t index, uint8_t *pdata, uint32_t count);
int8_t VL53L1_WrByte(uint16_t dev, uint16_t index, uint8_t data);
int8_t VL53L1_WrWord(uint16_t dev, uint16_t index, uint16_t data);
int8_t VL53L1_WrDWord(uint16_t dev, uint16_t index, uint32_t data);
int8_t VL53L1_RdByte(uint16_t dev, uint16_t index, uint8_t *pdata);
int8_t VL53L1_RdWord(uint16_t dev, uint16_t index, uint16_t *pdata);
int8_t VL53L1_RdDWord(uint16_t dev, uint16_t index, uint32_t *pdata);
int8_t VL53L1_WaitMs(uint16_t dev, int32_t wait_ms);

void VL531LX_Setup(void);
void vl53l1x_detect_task(void *param);
bool get_tof_distance(uint16_t *distance_mm);

#ifdef __cplusplus
}
#endif

#endif
