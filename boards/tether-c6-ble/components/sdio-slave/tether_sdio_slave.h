#ifndef TETHER_SDIO_SLAVE_H
#define TETHER_SDIO_SLAVE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure and start the SDIO slave, then spawn the receive/dispatch task.
 * Call once from app_main; the SDIO subsystem runs autonomously after this returns.
 */
void tether_sdio_init(void);

/* Send one TLV frame to the host (blocking until the host reads the buffer).
 * `buf` must point to DMA-capable, 32-bit aligned memory. Used by the command
 * dispatcher for responses and by the event drainer for asynchronous notifications.
 *
 * @return ESP_OK on success, or the underlying sdio_slave_send_* error code.
 */
esp_err_t tether_sdio_send_frame(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // TETHER_SDIO_SLAVE_H
