#pragma once

#include "esp_serial_slave_link/essl_sdio.h"

void sdio_event_start(essl_handle_t handle);

/* Pause/resume the background detect-poll + auto-reconnect loop. Use while the
 * pairing screen is open so the C6's single BLE radio is free for discovery
 * scans (a connected tag stops advertising and can't be found otherwise). */
void sdio_event_pause_polling(void);
void sdio_event_resume_polling(void);
