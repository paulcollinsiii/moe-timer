#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Incoming-message routing + chunk reassembly for the MQTT window — pure
   (no ESP deps), host-tested. The event handler feeds every DATA event in;
   this routes it to the config document, the command document, or the
   editable-config set buffer, reassembling chunked payloads by absolute
   offset. Routing is by topic CONTENT first, not length: magtag/<id>/set/tz
   is the same length as magtag/<id>/config, so the set/+ check must win. */

typedef struct {
    char key[24];
    char value[80];
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
