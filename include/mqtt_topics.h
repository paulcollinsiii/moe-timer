#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical MQTT topic shapes — pure, host-tested. Every device topic is
   "magtag/<id>/<suffix>"; discovery configs are
   "homeassistant/<component>/<id>_<object>/config". One builder each so
   the shapes cannot drift between the fifteen-odd call sites. Both return
   the would-be strlen (snprintf semantics). */
int mqtt_topic(char *buf, size_t len, const char *device_id, const char *suffix);
int mqtt_disc_topic(char *buf, size_t len, const char *component, const char *device_id, const char *object);

#ifdef __cplusplus
}
#endif
