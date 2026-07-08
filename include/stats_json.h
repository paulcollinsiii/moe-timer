#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "timer.h" /* TIMER_EXTRA_SLOTS */

/* Pure JSON payload builders for the Home Assistant MQTT integration —
   no ESP dependencies; host-tested (test_stats_json). All builders use
   snprintf semantics: the return value is the full needed length, the
   buffer is always NUL-terminated, callers check ret < len. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int batt_pct;
    int batt_mv;
    int light_mv;
    const char *state;        /* "IDLE"/"RUNNING"/... */
    const char *active_timer; /* "Screen" or the extra timer's name */
    int32_t remaining_s;
    uint32_t allocation_s;
    const char *day_type;
    uint16_t completions[TIMER_EXTRA_SLOTS]; /* extra slots 1..N */
    bool charge_lock;
    const char *fw;
} stats_snapshot_t;

int stats_json_stat(char *buf, size_t len, const stats_snapshot_t *s);
int stats_json_summary(char *buf, size_t len, const char *date, int32_t screen_used_s,
                       const uint16_t completions[TIMER_EXTRA_SLOTS]);

/* One HA MQTT-discovery entity. The static table (stats_json_entities)
   fully describes the fixed entities; the per-slot completion sensors use
   stats_json_discovery_named to carry the runtime timer name. */
typedef struct {
    const char *component;    /* "sensor" / "binary_sensor" */
    const char *key;          /* unique_id suffix, e.g. "battery" */
    const char *name;         /* default display name */
    const char *unit;         /* NULL = omit */
    const char *dev_class;    /* NULL = omit */
    const char *tpl;          /* value_template */
    const char *topic_suffix; /* "stat" or "summary" */
    int expire_after;         /* seconds; 0 = omit (value persists) */
    bool binary;              /* adds pl_on/pl_off */
} ha_entity_t;

const ha_entity_t *stats_json_entities(int *count);
int stats_json_discovery_topic(char *buf, size_t len, const char *dev_id, const ha_entity_t *ent);
int stats_json_discovery(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                         const ha_entity_t *ent);
/* Same, with a runtime display-name override (per-slot completion sensors). */
int stats_json_discovery_named(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                               const ha_entity_t *ent, const char *name_override);

#ifdef __cplusplus
}
#endif
