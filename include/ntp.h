#pragma once
#include "esp_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SNTP clock sync; requires an open wifi_session window.
   On ESP_OK, time(NULL) reflects corrected UTC.
   Does NOT read or write expiry_wall_time. */
esp_err_t ntp_sync_in_session(void);

#ifdef __cplusplus
}
#endif
