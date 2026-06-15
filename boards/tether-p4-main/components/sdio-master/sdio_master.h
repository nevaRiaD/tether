#ifndef SDIO_MASTER_H
#define SDIO_MASTER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_serial_slave_link/essl_sdio.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the SDIO bus, probe the slave card, and bring up the ESSL
 * (Espressif Serial Slave Link) handle used to talk to the C6.
 *
 * On success, *out_handle is populated and ready for reset/process_event/
 * send_packet calls. The caller owns the handle for the lifetime of the link.
 */
esp_err_t sdio_master_init(essl_handle_t *out_handle);

/*
 * Reset the slave's counters / state. Typically called once after init
 * to put the slave into a known state before normal traffic begins.
 */
esp_err_t sdio_master_reset(essl_handle_t handle);

/*
 * Wait up to `wait_ms` for a slave-side event (interrupt + any pending packet)
 * and copy the packet payload into rcv_buffer. Pass UINT32_MAX to block
 * indefinitely until an interrupt arrives; pass 0 to poll. Returns:
 *   ESP_OK            - event handled (rcv_buffer populated, *out_size set)
 *   ESP_ERR_TIMEOUT   - no event arrived within wait_ms
 *   other             - bus / protocol error
 *
 * Intended to be called from a dedicated task in a loop with a long wait, so
 * the task blocks instead of spinning on the SDIO bus.
 */
esp_err_t sdio_master_process_event(essl_handle_t handle,
                                    uint8_t *rcv_buffer,
                                    size_t buffer_size,
                                    size_t *out_size,
                                    uint32_t wait_ms);

/*
 * Send a single packet to the slave's receive FIFO. Length need not be
 * 4-byte aligned but unaligned lengths cost an extra transaction.
 */
esp_err_t sdio_master_send_packet(essl_handle_t handle,
                                  const uint8_t *data,
                                  size_t len,
                                  uint32_t wait_ms);

#ifdef __cplusplus
}
#endif

#endif /* SDIO_MASTER_H */
