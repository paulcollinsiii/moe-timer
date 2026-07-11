#include "mqtt_topics.h"

#include <stdio.h>

int mqtt_topic(char *buf, size_t len, const char *device_id, const char *suffix) {
    return snprintf(buf, len, "magtag/%s/%s", device_id, suffix);
}

int mqtt_disc_topic(char *buf, size_t len, const char *component, const char *device_id, const char *object) {
    return snprintf(buf, len, "homeassistant/%s/%s_%s/config", component, device_id, object);
}
