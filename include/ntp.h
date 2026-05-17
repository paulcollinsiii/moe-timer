#pragma once
/* esp_err_t: from esp_err.h in magtag build. */

/* Performs a full WiFi+SNTP sync cycle.
   On ESP_OK, time(NULL) reflects corrected UTC.
   Does NOT read or write expiry_wall_time. */
esp_err_t ntp_sync(void);
