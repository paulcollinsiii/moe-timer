#pragma once
#include <stddef.h>

/* Device identity for MQTT topics and HA discovery. The topic id derives
   from the WiFi MAC (magtag-xxxxxx) — stable across reflashes with no
   provisioning; the friendly display name is HA-configurable (NVS
   "dev_name", phase 2) and defaults to the id. */

#ifdef __cplusplus
extern "C" {
#endif

const char *device_id(void);
void device_name(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
