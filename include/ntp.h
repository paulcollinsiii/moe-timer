#pragma once
#include "esp_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Performs a full WiFi+SNTP sync cycle.
   On ESP_OK, time(NULL) reflects corrected UTC.
   Does NOT read or write expiry_wall_time. */
esp_err_t ntp_sync(void);

#ifdef __cplusplus
}
#endif
