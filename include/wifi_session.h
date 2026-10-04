#pragma once
#include "esp_compat.h"

#ifndef NATIVE
#include "esp_netif.h"
#endif

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

#ifndef NATIVE
/* The boot-global default STA netif, created on first call and returned
   on every call after. esp_netif_create_default_wifi_sta() asserts on a
   duplicate "WIFI_STA_DEF" if_key (esp_netif_lwip.c), so exactly one
   place in the image may call it per boot — this is that place.
   setup_session_idf.c's own session also runs WIFI_MODE_APSTA over a STA
   netif (network_provisioning's scheme_softap sets the mode but creates
   no netif of its own), and a wake that runs a normal network window and
   then enters setup previously hit that assert because the two files
   each kept a separate static. Both now call this instead of creating
   their own. */
esp_netif_t *wifi_session_sta_netif(void);
#endif

#ifdef __cplusplus
}
#endif
