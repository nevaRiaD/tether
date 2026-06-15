/*
 * SDIO master for the P4 host side of the P4 <-> C6 BLE link.
 *
 * Adapted from the ESP-IDF SDIO host example (Public Domain / CC0).
 * Differences from the upstream example:
 *   - SPI fallback path removed (SDMMC only).
 *   - Slave-board power-on GPIO logic removed (project board handles power).
 *   - Demo "job" loop removed; this file exposes a clean library API.
 *   - CONFIG_EXAMPLE_* Kconfig options replaced with tether_config.h defines.
 */

#include "sdio_master.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"

#include "tether_config.h"

static const char *TAG = "sdio_master";

#define TIMEOUT_MAX             UINT32_MAX
#define SLAVE_INTR_NOTIFY       0
#define SLAVE_REG_JOB           0
#define JOB_RESET               1

static esp_err_t print_sdio_cis_information(sdmmc_card_t *card)
{
    const size_t cis_buffer_size = 256;
    uint8_t cis_buffer[cis_buffer_size];
    size_t cis_data_len = 1024;
    esp_err_t ret = sdmmc_io_get_cis_data(card, cis_buffer, cis_buffer_size, &cis_data_len);

    if (ret == ESP_ERR_INVALID_SIZE) {
        uint8_t *temp_buf = malloc(cis_data_len);
        if (temp_buf == NULL) {
            ESP_LOGE(TAG, "CIS buffer alloc failed");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "CIS data longer than expected, temp buffer allocated");
        ret = sdmmc_io_get_cis_data(card, temp_buf, cis_data_len, &cis_data_len);
        if (ret == ESP_OK) {
            sdmmc_io_print_cis_info(temp_buf, cis_data_len, NULL);
        }
        free(temp_buf);
    } else if (ret == ESP_OK) {
        sdmmc_io_print_cis_info(cis_buffer, cis_data_len, NULL);
    } else {
        ESP_LOGE(TAG, "failed to get CIS data: 0x%x", ret);
    }
    return ret;
}

esp_err_t sdio_master_init(essl_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    sdmmc_host_t config = SDMMC_HOST_DEFAULT();

#if SDIO_MASTER_USE_4BIT
    ESP_LOGI(TAG, "Probing slave on SDIO 4-bit bus");
    config.flags = SDMMC_HOST_FLAG_4BIT;
#else
    ESP_LOGI(TAG, "Probing slave on SDIO 1-bit bus");
    config.flags = SDMMC_HOST_FLAG_1BIT;
#endif
    config.input_delay_phase = SDIO_MASTER_INPUT_DELAY;
#if SDIO_MASTER_HIGHSPEED
    config.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#else
    config.max_freq_khz = SDMMC_FREQ_DEFAULT;
#endif
    /* Required for unaligned CMD53 byte-mode DMA transfers. */
    config.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = SDIO_MASTER_USE_4BIT ? 4 : 1;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = SDIO_MASTER_PIN_CLK;
    slot_config.cmd = SDIO_MASTER_PIN_CMD;
    slot_config.d0  = SDIO_MASTER_PIN_D0;
    slot_config.d1  = SDIO_MASTER_PIN_D1;
    slot_config.d2  = SDIO_MASTER_PIN_D2;
    slot_config.d3  = SDIO_MASTER_PIN_D3;
#endif

    err = sdmmc_host_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_host_init failed: 0x%x", err);
        return err;
    }
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_host_init_slot failed: 0x%x", err);
        return err;
    }

    sdmmc_card_t *card = malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Slave may need a few attempts to come up after power-on. */
    int attempts = 0;
    while (sdmmc_card_init(&config, card) != ESP_OK) {
        if (++attempts >= 10) {
            ESP_LOGE(TAG, "slave init failed after %d attempts", attempts);
            free(card);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "slave init failed, retrying (%d)", attempts);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    sdmmc_card_print_info(stdout, card);

    /* The example pulls up all SDIO data lines explicitly. The board
       SHOULD have external pull-ups; this is a belt-and-suspenders. */
    gpio_pullup_en(SDIO_MASTER_PIN_CMD);  gpio_pulldown_dis(SDIO_MASTER_PIN_CMD);
    gpio_pullup_en(SDIO_MASTER_PIN_CLK);  gpio_pulldown_dis(SDIO_MASTER_PIN_CLK);
    gpio_pullup_en(SDIO_MASTER_PIN_D0);   gpio_pulldown_dis(SDIO_MASTER_PIN_D0);
    gpio_pullup_en(SDIO_MASTER_PIN_D1);   gpio_pulldown_dis(SDIO_MASTER_PIN_D1);
    gpio_pullup_en(SDIO_MASTER_PIN_D2);   gpio_pulldown_dis(SDIO_MASTER_PIN_D2);
    gpio_pullup_en(SDIO_MASTER_PIN_D3);   gpio_pulldown_dis(SDIO_MASTER_PIN_D3);

    essl_sdio_config_t ser_config = {
        .card = card,
        .recv_buffer_size = SDIO_MASTER_RECV_BUFFER_SZ,
    };
    err = essl_sdio_init_dev(out_handle, &ser_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "essl_sdio_init_dev failed: 0x%x", err);
        free(card);
        return err;
    }

    err = essl_init(*out_handle, TIMEOUT_MAX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "essl_init failed: 0x%x", err);
        return err;
    }

    /* Optional but useful at bringup; failure is non-fatal. */
    (void)print_sdio_cis_information(card);

    ESP_LOGI(TAG, "SDIO master ready");
    return ESP_OK;
}

esp_err_t sdio_master_reset(essl_handle_t handle)
{
    ESP_LOGI(TAG, "sending reset to slave");
    esp_err_t ret = essl_write_reg(handle, SLAVE_REG_JOB, JOB_RESET, NULL, TIMEOUT_MAX);
    if (ret != ESP_OK) return ret;
    ret = essl_send_slave_intr(handle, BIT(SLAVE_INTR_NOTIFY), TIMEOUT_MAX);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(500));
    ret = essl_wait_for_ready(handle, TIMEOUT_MAX);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "slave reset complete, link ready");
    }
    return ret;
}

static esp_err_t get_intr(essl_handle_t handle, uint32_t *out_raw, uint32_t *out_st,
                          TickType_t wait_ticks)
{
    esp_err_t ret = essl_wait_int(handle, wait_ticks);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = essl_get_intr(handle, out_raw, out_st, TIMEOUT_MAX);
    if (ret != ESP_OK) {
        return ret;
    }
    return essl_clear_intr(handle, *out_raw, TIMEOUT_MAX);
}

esp_err_t sdio_master_process_event(essl_handle_t handle,
                                    uint8_t *rcv_buffer,
                                    size_t buffer_size,
                                    size_t *out_size,
                                    uint32_t wait_ms)
{
    if (rcv_buffer == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_size = 0;

    /* UINT32_MAX is the sentinel for "block forever"; otherwise convert ms -> ticks. */
    TickType_t wait_ticks = (wait_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);

    uint32_t intr_raw, intr_st;
    esp_err_t ret = get_intr(handle, &intr_raw, &intr_st, wait_ticks);
    if (ret == ESP_ERR_TIMEOUT) {
        return ret;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get_intr failed: 0x%x", ret);
        return ret;
    }

    if (!(intr_raw & ESSL_SDIO_DEF_ESP32.new_packet_intr_mask)) {
        return ESP_OK;
    }

    /* New packet available — drain one. */
    const int packet_wait_ms = 50;
    size_t size_read = buffer_size;
    ret = essl_get_packet(handle, rcv_buffer, buffer_size, &size_read, packet_wait_ms);
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "packet interrupt fired but no data available");
        return ret;
    }
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
        ESP_LOGE(TAG, "essl_get_packet failed: 0x%x", ret);
        return ret;
    }

    *out_size = size_read;
    return ESP_OK;
}

esp_err_t sdio_master_send_packet(essl_handle_t handle,
                                  const uint8_t *data,
                                  size_t len,
                                  uint32_t wait_ms)
{
    return essl_send_packet(handle, data, len, wait_ms);
}
