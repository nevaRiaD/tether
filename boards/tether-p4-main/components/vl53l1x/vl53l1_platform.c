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

#include "vl53l1_platform.h"
#include "bsp/esp-bsp.h"

i2c_master_dev_handle_t dev_handle;
i2c_master_bus_handle_t bus_handle;

static SemaphoreHandle_t tof_mutex;
static uint16_t tof_distance_mm = UINT16_MAX;

static const char *TAG = "I2C_VL53L1X";

int8_t VL53L1_WriteMulti(uint16_t dev, uint16_t index, uint8_t *pdata, uint32_t count)
{
    uint8_t buf[count + 2];
    buf[0] = (index >> 8) & 0xFF;
    buf[1] = index & 0xFF;
    memcpy(&buf[2], pdata, count);
    return i2c_master_transmit(dev_handle, buf, count + 2, pdMS_TO_TICKS(100));
}

int8_t VL53L1_ReadMulti(uint16_t dev, uint16_t index, uint8_t *pdata, uint32_t count)
{
    uint8_t reg[2] = {(index >> 8) & 0xFF, index & 0xFF};
    return i2c_master_transmit_receive(dev_handle, reg, 2, pdata, count, pdMS_TO_TICKS(100));
}

int8_t VL53L1_WrByte(uint16_t dev, uint16_t index, uint8_t data)
{
    return VL53L1_WriteMulti(dev, index, &data, 1);
}

int8_t VL53L1_WrWord(uint16_t dev, uint16_t index, uint16_t data)
{
    uint8_t buf[2] = {(data >> 8) & 0xFF, data & 0xFF};
    return VL53L1_WriteMulti(dev, index, buf, 2);
}

int8_t VL53L1_WrDWord(uint16_t dev, uint16_t index, uint32_t data)
{
    uint8_t buf[4] = {
        (data >> 24) & 0xFF,
        (data >> 16) & 0xFF,
        (data >> 8) & 0xFF,
        data & 0xFF,
    };
    return VL53L1_WriteMulti(dev, index, buf, 4);
}

int8_t VL53L1_RdByte(uint16_t dev, uint16_t index, uint8_t *data)
{
    return VL53L1_ReadMulti(dev, index, data, 1);
}

int8_t VL53L1_RdWord(uint16_t dev, uint16_t index, uint16_t *data)
{
    uint8_t buf[2];
    int8_t status = VL53L1_ReadMulti(dev, index, buf, 2);
    *data = ((uint16_t)buf[0] << 8) | buf[1];
    return status;
}

int8_t VL53L1_RdDWord(uint16_t dev, uint16_t index, uint32_t *data)
{
    uint8_t buf[4];
    int8_t status = VL53L1_ReadMulti(dev, index, buf, 4);
    *data = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
            ((uint32_t)buf[2] << 8) | buf[3];
    return status;
}

int8_t VL53L1_WaitMs(uint16_t dev, int32_t wait_ms)
{
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    return 0;
}

void VL531LX_Setup(void)
{
    tof_mutex = xSemaphoreCreateMutex();
    configASSERT(tof_mutex);

    bus_handle = bsp_i2c_get_handle();

    esp_err_t ret = i2c_master_probe(bus_handle, I2C_SLAVE_ADDR, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VL53L1X not found on I2C bus!");
        return;
    }
    ESP_LOGI(TAG, "VL53L1X found!");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_SLAVE_ADDR,
        .scl_speed_hz = 400000,
    };

    i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);

    uint8_t sensorState = 0;
    int boot_attempts = 0;
    while (sensorState == 0 && boot_attempts < 100) {
        VL53L1X_BootState(I2C_SLAVE_ADDR, &sensorState);
        boot_attempts++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (sensorState == 0) {
        ESP_LOGE(TAG, "VL53L1X failed to boot!");
        return;
    }

    ESP_LOGI(TAG, "VL53L1X booted successfully!");

    if (VL53L1X_SensorInit(I2C_SLAVE_ADDR) != 0) {
        ESP_LOGE(TAG, "VL53L1X_SensorInit failed!");
        return;
    }
    if (VL53L1X_StartRanging(I2C_SLAVE_ADDR) != 0) {
        ESP_LOGE(TAG, "VL53L1X_StartRanging failed!");
        return;
    }

    xTaskCreate(vl53l1x_detect_task, "vl53l1x_detect", VL53L1X_DETECT_TASK_STACK, NULL,
                VL53L1X_DETECT_TASK_PRIO, NULL);
}

static bool tof_success_check(VL53L1X_ERROR status);

void vl53l1x_detect_task(void *param)
{
    ESP_LOGI(TAG, "VL53L1X (ToF Sensor) Detect Task Started");
    uint8_t ready_status;
    uint8_t range_status;
    uint16_t sensor_distance;

    while (1) {
        VL53L1X_CheckForDataReady(I2C_SLAVE_ADDR, &ready_status);
        if (!ready_status) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (!tof_success_check(VL53L1X_GetRangeStatus(I2C_SLAVE_ADDR, &range_status))) {
            vTaskDelay(10);
            continue;
        }
        if (!tof_success_check(VL53L1X_GetDistance(I2C_SLAVE_ADDR, &sensor_distance))) {
            vTaskDelay(10);
            continue;
        };
        if (!tof_success_check(VL53L1X_ClearInterrupt(I2C_SLAVE_ADDR))) {
            vTaskDelay(10);
            continue;
        };

        if (range_status != 0) {
            xSemaphoreTake(tof_mutex, portMAX_DELAY);
            tof_distance_mm = UINT16_MAX;
            xSemaphoreGive(tof_mutex);
            vTaskDelay(pdMS_TO_TICKS(VL53L1X_DETECT_TASK_DELAY));
            continue;
        }

#ifdef T_TOF_TEST_MODE
        if (sensor_distance < TOF_THRESHOLD_MM) {
            ESP_LOGI(TAG, "Object detected: %u mm", sensor_distance);
        } else {
            ESP_LOGI(TAG, "No object detected: %u mm", sensor_distance);
        }
#else
        xSemaphoreTake(tof_mutex, portMAX_DELAY);
        tof_distance_mm = sensor_distance;
        xSemaphoreGive(tof_mutex);
#endif
        vTaskDelay(pdMS_TO_TICKS(VL53L1X_DETECT_TASK_DELAY));
    }
}

bool get_tof_distance(uint16_t *distance_mm)
{
    if (tof_mutex == NULL) {
        *distance_mm = UINT16_MAX;
        return false;
    }
    if (xSemaphoreTake(tof_mutex, portMAX_DELAY) == pdTRUE) {
        *distance_mm = tof_distance_mm;
        xSemaphoreGive(tof_mutex);
        return true;
    }
    return false;
}

static bool tof_success_check(VL53L1X_ERROR status){
    if (status != VL53L1X_ERROR_NONE) {
        i2c_master_bus_reset(bus_handle);
        return false;
    }
    return true;
}
