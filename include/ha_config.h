#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_compat.h"

/* Editable Home Assistant config entities. A single field registry drives
   three things — MQTT-discovery payloads (number/text/switch), the current-
   value state JSON (magtag/<id>/cfg), and per-field command apply
   (magtag/<id>/set/<key>). Pure over nvs_config + the shared validators;
   host-tested. No cJSON — values arrive from MQTT as strings. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFG_U16,     /* bounded integer (number)  */
    CFG_HHMM,    /* time-of-day HHMM (number) */
    CFG_STR,     /* string (text)             */
    CFG_TNAME,   /* extra-timer name (text)   */
    CFG_TMIN,    /* extra-timer minutes (number) */
    CFG_TRELOAD, /* extra-timer reloadable (switch) */
    CFG_ENUM,    /* option string stored as u16 index (select) */
} cfg_kind_t;

typedef struct {
    const char *key;       /* "weekday_min", "timer1_name", ... */
    const char *component; /* "number" / "text" / "switch" / "select" */
    const char *name;      /* HA display name */
    const char *unit;      /* number unit; NULL to omit */
    cfg_kind_t kind;
    int lo, hi, step; /* number bounds/step; CFG_STR uses hi as max length */
    int slot;         /* CFG_T*: extra-timer slot (1..N) */
    esp_err_t (*set_u16)(uint16_t);
    esp_err_t (*get_u16)(uint16_t *);
    esp_err_t (*set_str)(const char *);
    esp_err_t (*get_str)(char *, size_t);
    const char *const *options; /* CFG_ENUM: option strings */
    int n_options;
    bool (*validate)(int); /* CFG_HHMM: overrides quiet_hhmm_valid */
} cfg_field_t;

typedef enum { HA_CFG_OK = 0, HA_CFG_REJECTED, HA_CFG_UNKNOWN } ha_cfg_result_t;

/* Buffer size the firmware must give ha_config_state_json: the whole field
   registry serialized (worst case ~750 B with maxed strings and the tone
   option strings) plus headroom. Callers must check the return value
   against this before publishing. */
#define HA_CONFIG_STATE_MAX 1024

const cfg_field_t *ha_config_fields(int *count);
/* Validate `value` (a string from MQTT) for `key` and persist via the
   field's nvs setter; writes an ack ({"key":...,"ok":bool[,"err":...]}). */
ha_cfg_result_t ha_config_set(const char *key, const char *value, char *ack, size_t ack_len);
/* Current values of every field as one JSON object (the cfg state topic). */
int ha_config_state_json(char *buf, size_t len);
/* Discovery topic + payload for one editable entity. */
int ha_config_discovery_topic(char *buf, size_t len, const char *dev_id, const cfg_field_t *f);
int ha_config_discovery(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                        const cfg_field_t *f);
/* Escape `src` into `dst` for embedding as a JSON string value (quote and
   backslash escaped, control chars -> space); returns `dst`. Shared so
   callers that hand-build discovery JSON (mqtt_ha action entities) escape
   the user-editable device name the same way. */
const char *ha_config_json_escape(char *dst, size_t dstlen, const char *src);

#ifdef __cplusplus
}
#endif
