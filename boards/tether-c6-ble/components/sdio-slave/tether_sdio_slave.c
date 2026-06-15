/*
 * Architecture:
 *
 * This file owns the C6 side of the SDIO transport to the P4 host. It (1) brings up the
 * SDIO slave peripheral with a fixed pool of DMA-capable RX buffers, (2) runs a single
 * task that drains inbound packets and hands assembled frames to tether_sdio_handle_frame()
 * in tether_sdio_cmd.c, and (3) exposes tether_sdio_send_frame() so the dispatcher and the
 * event drainer can push outbound TLV frames without touching the SDIO API directly.
 *
 * Packet vs frame:
 *   - The SDIO slave driver delivers one logical inbound packet across N receive buffers
 *     (each of size TETHER_MAX_FRAME_SIZE = BUFFER_SIZE). The task collects them via the
 *     ESP_ERR_NOT_FINISHED loop, concatenates into a contiguous scratch, recycles the
 *     buffers back to the pool, and dispatches.
 *   - For TX, every call to sdio_slave_send_queue is one packet on the host side. We
 *     keep each TLV frame within one SDIO buffer so framing stays 1:1 with the wire.
 *     Multi-fragment logical messages are signalled with TETHER_FLAG_MORE at the
 *     protocol layer (handled inside tether_sdio_cmd.c, not here).
 *
 * The legacy job-dispatch path (JOB_RESET / JOB_SEND_INT / JOB_WRITE_REG, triggered by
 * the host writing slave register 0) is retained as a low-level debug aid for host-side
 * bring-up; it has no relationship to the BLE command flow.
 *
 * Single-task ownership: only sdio_slave_task() ever calls sdio_slave_recv_*, and
 * tether_sdio_send_frame() is invoked synchronously from that same task (via the
 * dispatcher). That keeps buffer ownership and completion bookkeeping in one place.
 */

#include "tether_sdio_slave.h"
#include "tether_sdio_proto.h"
#include "tether_sdio_cmd.h"
#include "tether_sdio_events.h"
#include "tether_slave_cfg.h"

#include "driver/sdio_slave.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "soc/soc.h"
#include "sdkconfig.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define SDIO_SLAVE_QUEUE_SIZE 11
#define BUFFER_SIZE           TETHER_MAX_FRAME_SIZE
#define BUFFER_NUM            16

#define EV_STR(s) "================ "s" ================"

// skip interrupt regs.
#define SLAVE_ADDR(i)         ((i) >= 28 ? (i) + 4 : (i))

typedef enum {
    JOB_IDLE      = 0,
    JOB_RESET     = 1,
    JOB_SEND_INT  = 2,
    JOB_WRITE_REG = 4,
} sdio_job_t;

static const char TAG[] = "tether_sdio";
static volatile int s_job = JOB_IDLE;

static const char job_desc[][32] = {
    "JOB_IDLE",
    "JOB_RESET",
    "JOB_SEND_INT",
    "JOB_WRITE_REG",
};

DMA_ATTR static uint8_t s_buffer[BUFFER_NUM][BUFFER_SIZE];

/* Serializes tether_sdio_send_frame across callers (SDIO task for command responses, BLE/proximity
 * tasks for events). Without this, two tasks could overlap a send_queue + send_get_finished pair
 * and read each other's completion. */
static SemaphoreHandle_t s_send_mtx = NULL;


/* ========== Forward declarations ========== */

/* Stop the SDIO slave, restart it, then drain any in-flight TX descriptors so the next
 * dispatcher cycle starts clean.                                                       */
static esp_err_t slave_reset(void);

/* Pulse each of the 8 host-interrupt lines in sequence; abort early if a reset is requested.   */
static esp_err_t task_hostint(void);

/* Write a test pattern to the slave registers and log the readback as a hex dump.              */
static esp_err_t task_write_reg(void);

/* ISR callback for SDIO slave events; on slot 0, latch the next job from register 0.           */
static void event_cb(uint8_t pos);

/* Main task loop: assemble inbound packets into TLV frames, dispatch via the cmd handler,
 * and run the legacy register-driven jobs.                                                  */
static void sdio_slave_task(void *arg);


/* ========== Function definitions ========== */

/**
 * Stop the SDIO slave, restart it, then drain any in-flight TX descriptors so the next
 * dispatcher cycle starts clean.
 *
 * @return  ESP_OK on success, or the first failing esp_err_t from the slave driver.
 */
static esp_err_t slave_reset(void)
{
    esp_err_t ret;
    sdio_slave_stop();
    ret = sdio_slave_reset();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = sdio_slave_start();
    if (ret != ESP_OK) {
        return ret;
    }

    while (1) {
        void *unused = NULL;
        ret = sdio_slave_send_get_finished(&unused, 0);
        if (ret != ESP_OK) {
            break;
        }
    }
    return ESP_OK;
}

/**
 * Pulse each of the 8 host-interrupt lines in sequence with a 500 ms gap between them. Returns
 * early if a reset job is latched while pulsing.
 *
 * @return  ESP_OK always.
 */
static esp_err_t task_hostint(void)
{
    for (int i = 0; i < 8; i++) {
        ESP_LOGV(TAG, "send intr: %d", i);
        sdio_slave_send_host_int(i);
        if (s_job & JOB_RESET) {
            break;
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    return ESP_OK;
}

/**
 * Write a deterministic test pattern across the slave registers (skipping interrupt regs via
 * SLAVE_ADDR), then read them back and dump the result. Used for host-side validation.
 *
 * @return  ESP_OK always.
 */
static esp_err_t task_write_reg(void)
{
    uint8_t read = sdio_slave_read_reg(1);
    for (int i = 0; i < 60; i++) {
        sdio_slave_write_reg(SLAVE_ADDR(i), read + 3 * i);
    }
    uint8_t reg[60] = {0};
    for (int i = 0; i < 60; i++) {
        reg[i] = sdio_slave_read_reg(SLAVE_ADDR(i));
    }
    ESP_LOGI(TAG, "write regs:");
    ESP_LOG_BUFFER_HEXDUMP(TAG, reg, 60, ESP_LOG_INFO);
    return ESP_OK;
}

/**
 * SDIO slave event callback. Runs in ISR context — keep work minimal. On slot 0 events, latches
 * the next job code from slave register 0 and clears it so the main task can pick it up.
 *
 * @param pos  Index of the event slot that fired.
 */
static void event_cb(uint8_t pos)
{
    ESP_EARLY_LOGD(TAG, "event: %d", pos);
    switch (pos) {
    case 0:
        s_job = sdio_slave_read_reg(0);
        sdio_slave_write_reg(0, JOB_IDLE);
        break;
    }
}

/**
 * Main SDIO slave task loop. Each iteration: (1) drain any inbound packet by collecting all
 * its segments, assembling them into a contiguous TLV frame in a stack scratch, recycling
 * the RX buffers back to the pool, and handing the frame to tether_sdio_handle_frame();
 * (2) dispatch any low-level job bits set by event_cb() through slave_reset() /
 * task_hostint() / task_write_reg().
 *
 * @param arg  Unused; required by the FreeRTOS task signature.
 */
static void sdio_slave_task(void *arg)
{
    (void)arg;
    esp_err_t ret;

    for (;;) {
        const TickType_t non_blocking = 0, blocking = portMAX_DELAY;
        sdio_slave_buf_handle_t recv_queue[BUFFER_NUM];
        int packet_size = 0;

        sdio_slave_buf_handle_t handle;
        ret = sdio_slave_recv_packet(&handle, non_blocking);
        if (ret != ESP_ERR_TIMEOUT) {
            recv_queue[packet_size++] = handle;

            while (ret == ESP_ERR_NOT_FINISHED) {
                ret = sdio_slave_recv_packet(&handle, blocking);
                recv_queue[packet_size++] = handle;
            }
            ESP_ERROR_CHECK(ret);

            uint8_t frame[TETHER_MAX_FRAME_SIZE];
            size_t  frame_len = 0;
            for (int i = 0; i < packet_size; i++) {
                size_t   seg_len = 0;
                uint8_t *seg_ptr = sdio_slave_recv_get_buf(recv_queue[i], &seg_len);
                if (frame_len + seg_len <= sizeof frame) {
                    memcpy(frame + frame_len, seg_ptr, seg_len);
                    frame_len += seg_len;
                } else {
                    ESP_LOGW(TAG, "RX packet too large (%u + %u > %u); truncating",
                             (unsigned)frame_len, (unsigned)seg_len, (unsigned)sizeof frame);
                }
            }

            for (int i = 0; i < packet_size; i++) {
                ESP_ERROR_CHECK(sdio_slave_recv_load_buf(recv_queue[i]));
            }

            tether_sdio_handle_frame(frame, frame_len);
        }

        /* Drain any BLE-side events posted since the last loop iteration. */
        tether_sdio_drain_events();

        if (s_job != 0) {
            for (int i = 0; i < 8; i++) {
                if (s_job & BIT(i)) {
                    ESP_LOGI(TAG, EV_STR("%s"), job_desc[i + 1]);
                    s_job &= ~BIT(i);

                    switch (BIT(i)) {
                    case JOB_SEND_INT:
                        ret = task_hostint();
                        ESP_ERROR_CHECK(ret);
                        break;
                    case JOB_RESET:
                        ret = slave_reset();
                        ESP_ERROR_CHECK(ret);
                        break;
                    case JOB_WRITE_REG:
                        ret = task_write_reg();
                        ESP_ERROR_CHECK(ret);
                        break;
                    }
                }
            }
        }
        vTaskDelay(SDIO_SLAVE_TASK_DELAY);
    }
}

/**
 * Send one TLV frame to the host. Blocks the caller until the host has actually read the
 * buffer; that keeps frame ownership simple — only one outbound frame is ever in flight,
 * so a single shared scratch on the caller side is safe to reuse on return.
 *
 * @param buf  Pointer to a complete TLV frame (header + payload). Must be DMA-capable and
 *             32-bit aligned (typically DMA_ATTR static memory).
 * @param len  Total bytes to send. Must be in (0, 4092] per the sdio_slave driver.
 * @return     ESP_OK on success, or the first failing esp_err_t from the slave driver.
 */
esp_err_t tether_sdio_send_frame(const uint8_t *buf, size_t len)
{
    if (s_send_mtx != NULL) {
        xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    }
    esp_err_t ret = sdio_slave_send_queue((uint8_t *)buf, len, NULL, portMAX_DELAY);
    if (ret == ESP_OK) {
        void *finished = NULL;
        ret = sdio_slave_send_get_finished(&finished, portMAX_DELAY);
    }
    if (s_send_mtx != NULL) {
        xSemaphoreGive(s_send_mtx);
    }
    return ret;
}

/**
 * Configure and start the SDIO slave peripheral for the C6, register the RX buffer pool, enable
 * the host-interrupt mask, and spawn the receive/dispatch task. Call once from app_main; the
 * SDIO subsystem runs autonomously after this returns.
 */
void tether_sdio_init(void)
{
    sdio_slave_config_t config = {
        .sending_mode     = SDIO_SLAVE_SEND_PACKET,
        .send_queue_size  = SDIO_SLAVE_QUEUE_SIZE,
        .recv_buffer_size = BUFFER_SIZE,
        .event_cb         = event_cb,
        // External pullups required for reliable bus operation; do not rely on internal pullups.
        // .flags         = SDIO_SLAVE_FLAG_INTERNAL_PULLUP,
    };
#ifdef CONFIG_SDIO_DAT2_DISABLED
    config.flags |= SDIO_SLAVE_FLAG_DAT2_DISABLED;
#endif

    ESP_ERROR_CHECK(sdio_slave_initialize(&config));

    s_send_mtx = xSemaphoreCreateMutex();
    assert(s_send_mtx != NULL);

    /* Bring up the event queue before the task is spawned so any early BLE callback
     * already has a valid sink. */
    tether_sdio_events_init();

    sdio_slave_write_reg(0, JOB_IDLE);

    for (int i = 0; i < BUFFER_NUM; i++) {
        sdio_slave_buf_handle_t handle = sdio_slave_recv_register_buf(s_buffer[i]);
        assert(handle != NULL);
        ESP_ERROR_CHECK(sdio_slave_recv_load_buf(handle));
    }

    sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET |
                               SDIO_SLAVE_HOSTINT_BIT0 |
                               SDIO_SLAVE_HOSTINT_BIT1 |
                               SDIO_SLAVE_HOSTINT_BIT2 |
                               SDIO_SLAVE_HOSTINT_BIT3 |
                               SDIO_SLAVE_HOSTINT_BIT4 |
                               SDIO_SLAVE_HOSTINT_BIT5 |
                               SDIO_SLAVE_HOSTINT_BIT6 |
                               SDIO_SLAVE_HOSTINT_BIT7);

    ESP_ERROR_CHECK(sdio_slave_start());

    ESP_LOGI(TAG, EV_STR("slave ready"));

    BaseType_t ok = xTaskCreate(sdio_slave_task,
                                "tether_sdio",
                                SDIO_SLAVE_TASK_STACK,
                                NULL,
                                SDIO_SLAVE_TASK_PRIO,
                                NULL);
    assert(ok == pdPASS);
}