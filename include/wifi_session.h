#pragma once
#include "esp_compat.h"

/* WiFi station session for the periodic network window: begin connects
   (credentials from NVS, bounded wait), end tears the driver down again.
   Extracted from ntp.c so SNTP and the MQTT/HA session share one radio
   window. WiFi is off outside begin/end — the deep-sleep power budget
   depends on it. */

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_session_begin(void);
void wifi_session_end(void);

#ifdef __cplusplus
}
#endif
