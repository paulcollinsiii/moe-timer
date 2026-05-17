#pragma once
#ifndef NATIVE
#include "esp_err.h"
#else
typedef int esp_err_t;
#endif

/* Performs a full WiFi+SNTP sync cycle.
   On ESP_OK, time(NULL) reflects corrected UTC.
   Does NOT read or write expiry_wall_time. */
esp_err_t ntp_sync(void);
