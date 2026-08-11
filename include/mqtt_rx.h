#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "config_validate.h" /* CFG_STR_MAX — sizes the set value slot */

#ifdef __cplusplus
extern "C" {
#endif

/* Incoming-message routing + chunk reassembly for the MQTT window — pure
   (no ESP deps), host-tested. The event handler feeds every DATA event in;
   this routes it to the config document, the command document, or the
   editable-config set buffer, reassembling chunked payloads by absolute
   offset. Routing is by topic CONTENT first, not length: magtag/<id>/set/tz
   is the same length as magtag/<id>/config, so the set/+ check must win. */

/* The value slot is sized from the shared string-field ceiling, not a
   local literal: this transport has to be able to deliver the longest
   value any editable field advertises to HA. When the two were
   independent numbers the OTA manifest URL advertised 127 and was
   rejected here at 80 — silently, every window. */
typedef struct {
    char key[24];
    char value[CFG_STR_MAX];
} mqtt_set_kv_t;

typedef struct {
    /* Retained config document (chunk-reassembled). */
    char *config_buf;
    int config_cap;
    int config_topic_len; /* strlen("magtag/<id>/config") */
    volatile bool config_done;
    /* Retained command document (chunk-reassembled). */
    char *cmd_buf;
    int cmd_cap;
    int cmd_topic_len;
    volatile bool cmd_done;
    /* Editable-config sets (single-chunk key/value pairs). */
    mqtt_set_kv_t *sets;
    int sets_cap;
    volatile int set_count;
    int set_prefix_len; /* strlen("magtag/<id>/set/") */
} mqtt_rx_t;

typedef enum {
    MQTT_RX_OK = 0,       /* routed (or final chunk landed) */
    MQTT_RX_IGNORED,      /* not one of ours / empty retained clear */
    MQTT_RX_SETS_FULL,    /* set buffer full: edit dropped (caller logs) */
    MQTT_RX_SET_TOO_LONG, /* set key/value over capacity: dropped */
} mqtt_rx_result_t;

mqtt_rx_result_t mqtt_rx_on_data(mqtt_rx_t *rx, const char *topic, int topic_len, const char *data, int data_len,
                                 int total_len, int offset);

#ifdef __cplusplus
}
#endif
