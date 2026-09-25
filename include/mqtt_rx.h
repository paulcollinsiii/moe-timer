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
   offset.

   ROUTING IS BY TOPIC CONTENT, FULL STOP. It used to be by topic LENGTH
   for the two document topics (topic_len == config_topic_len), with only
   the set/+ check doing a content comparison — and the comment that used
   to sit here, "routing is by topic CONTENT first, not length", described
   the set/+ check and nothing else. Any same-length topic that reached
   this function was therefore treated as our config document:
   magtag/<other-device>/config on a shared broker is the obvious one, and
   the tail of a chunked magtag/<id>/set/tz is the near miss — the set/
   branch claims chunk ZERO only, and set/tz is exactly as long as config.

   (That second one is not reachable on the device as configured: esp-mqtt
   leaves `topic` NULL on every fragment after the first unless
   CONFIG_MQTT_TOPIC_PRESENT_ALL_DATA_EVENTS is set, and it is not. It is
   pinned by a test anyway — the whole hazard is that the routing must not
   depend on which of two unrelated Kconfig settings is in force.)

   While the failure was a silent drop this was untidy. Now that an
   over-capacity document publishes a RETAINED config_ack saying "your
   config document was too long", a length-only match could put a false,
   durable, operator-visible accusation on the broker about a document
   nobody sent. So the caller hands over the topic STRINGS and the match
   is a real comparison.

   Reassembly by absolute offset is kept, but it is not what the device
   relies on: mqtt_ha.c sizes the client receive buffer so that any
   document within the ceiling arrives in a single event, precisely
   because a fragment with no topic cannot be routed here. See the
   buffer.size note at the client config. */

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
    /* Retained config document (chunk-reassembled). `config_topic` is the
       exact subscribed topic, NUL-terminated, and must outlive the window
       — mqtt_ha.c keeps it in the same window allocation as config_buf.
       NULL disables the route rather than matching everything. */
    char *config_buf;
    int config_cap;
    const char *config_topic; /* "magtag/<id>/config" */
    volatile bool config_done;
    /* Refusal state, readable from the window flow. The RESULT code alone
       is not enough: it is consumed on the mqtt client task inside the
       event handler, while the code that has to publish the ack and stop
       waiting runs on the network task. Same idiom as config_done. */
    volatile bool config_too_long;
    volatile int config_too_long_len; /* the refused total_len, for the ack */
    /* Retained command document (chunk-reassembled). */
    char *cmd_buf;
    int cmd_cap;
    const char *cmd_topic; /* "magtag/<id>/cmd" */
    volatile bool cmd_done;
    volatile bool cmd_too_long;
    volatile int cmd_too_long_len;
    /* Editable-config sets (single-chunk key/value pairs). */
    mqtt_set_kv_t *sets;
    int sets_cap;
    volatile int set_count;
    int set_prefix_len; /* strlen("magtag/<id>/set/") */
} mqtt_rx_t;

/* Appended, never reordered — and NOT because anything depends on the
   numbers. Nothing stores or transmits them, and the one switch over this
   enum (mqtt_ha.c's event handler) labels its cases by name, so a renumber
   would be invisible to it. The rule is about the results themselves:
   MQTT_RX_IGNORED used to carry "my topic, but the document is too big"
   alongside "not my topic at all", which is exactly how an over-size
   retained config came to be re-dropped on every reconnect in silence.
   Every distinct outcome gets its own value, and that switch carries no
   `default:`, so adding one here fails the build until someone decides
   what it means. */
typedef enum {
    MQTT_RX_OK = 0,          /* routed (or final chunk landed) */
    MQTT_RX_IGNORED,         /* not one of ours / empty retained clear */
    MQTT_RX_SETS_FULL,       /* set buffer full: edit dropped (caller logs) */
    MQTT_RX_SET_TOO_LONG,    /* set key/value over capacity: dropped */
    MQTT_RX_CONFIG_TOO_LONG, /* OUR config topic, document over config_cap */
    MQTT_RX_CMD_TOO_LONG,    /* OUR cmd topic, document over cmd_cap */
    MQTT_RX_BAD_CHUNK,       /* offset/data_len outside total_len (framing) */
} mqtt_rx_result_t;

mqtt_rx_result_t mqtt_rx_on_data(mqtt_rx_t *rx, const char *topic, int topic_len, const char *data, int data_len,
                                 int total_len, int offset);

#ifdef __cplusplus
}
#endif
