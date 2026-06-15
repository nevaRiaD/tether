#ifndef TETHER_SDIO_CMD_H
#define TETHER_SDIO_CMD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a TLV frame received from the host and dispatch to the appropriate handler.
 * Each command produces one response frame (or a multi-fragment response when needed),
 * pushed through tether_sdio_send_frame(). Called from the SDIO slave task. */
void tether_sdio_handle_frame(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TETHER_SDIO_CMD_H */
