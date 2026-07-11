/* Pure JSON builders for the HA MQTT integration — host-tested. */
#include "stats_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Append with snprintf semantics: pos carries the total needed length;
   writes land clamped inside the buffer, which stays NUL-terminated. */
static int jcat(char *buf, size_t len, int pos, const char *fmt, ...) {
    size_t off = ((size_t)pos < len) ? (size_t)pos : (len ? len - 1 : 0);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, len - off, fmt, ap);
    va_end(ap);
    return pos + n;
}

/* Escape into tmp for embedding in a JSON string: quotes and backslashes
   escaped, control bytes flattened to spaces. Truncates at tmplen. */
static const char *jesc(char *tmp, size_t tmplen, const char *s) {
    size_t o = 0;
    for (; s != NULL && *s != '\0' && o + 2 < tmplen; s++) {
        if (*s == '"' || *s == '\\')
            tmp[o++] = '\\';
        tmp[o++] = ((unsigned char)*s < 0x20) ? ' ' : *s;
    }
    tmp[o] = '\0';
    return tmp;
}

int stats_json_stat(char *buf, size_t len, const stats_snapshot_t *s) {
    /* Every string field is escaped (and NULL-flattened to "") — the pure
       boundary must never invoke UB on a bad/NULL field. */
    char state[24], name[64], day[24], fw[32], rst[24];
    int pos = 0;
    pos = jcat(buf, len, pos,
               "{\"batt_pct\":%d,\"batt_mv\":%d,\"light_mv\":%d,\"state\":\"%s\",\"active_timer\":\"%s\","
               "\"remaining_s\":%ld,\"allocation_s\":%lu,\"day_type\":\"%s\",\"completions\":[",
               s->batt_pct, s->batt_mv, s->light_mv, jesc(state, sizeof(state), s->state),
               jesc(name, sizeof(name), s->active_timer), (long)s->remaining_s, (unsigned long)s->allocation_s,
               jesc(day, sizeof(day), s->day_type));
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        pos = jcat(buf, len, pos, i ? ",%u" : "%u", (unsigned)s->completions[i]);
    }
    pos = jcat(buf, len, pos, "],\"charge_lock\":%s,\"fw\":\"%s\",\"reset\":\"%s\"}", s->charge_lock ? "true" : "false",
               jesc(fw, sizeof(fw), s->fw), jesc(rst, sizeof(rst), s->reset_reason));
    return pos;
}

int stats_json_summary(char *buf, size_t len, const char *date, int32_t screen_used_s,
                       const uint16_t completions[TIMER_EXTRA_SLOTS]) {
    int pos = jcat(buf, len, 0, "{\"date\":\"%s\",\"screen_used_s\":%ld,\"completions\":[", date, (long)screen_used_s);
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        pos = jcat(buf, len, pos, i ? ",%u" : "%u", (unsigned)completions[i]);
    }
    pos = jcat(buf, len, pos, "]}");
    return pos;
}

/* Discovery entity table. expire_after on stat-fed sensors is 2x the
   default idle sync interval + margin, so entities read available while
   the device sleeps but flag a genuinely dead device. The daily summary
   sensor never expires. Order matters only for [0] (battery) in tests. */
#define STAT_EXPIRE_SEC 7500

/* Fields: component, key, name, unit, dev_class, tpl, topic_suffix,
   expire_after, binary, ent_cat. ent_cat "diagnostic" tucks noisy
   read-onlys into HA's Diagnostic group; NULL = primary (top-level). */
#define DIAG "diagnostic"
static const ha_entity_t ENTITIES[] = {
    {"sensor", "battery", "Battery", "%", "battery", "{{ value_json.batt_pct }}", "stat", STAT_EXPIRE_SEC, false, NULL},
    {"sensor", "battery_mv", "Battery voltage", "mV", "voltage", "{{ value_json.batt_mv }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "light", "Ambient light", "mV", NULL, "{{ value_json.light_mv }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "state", "Timer state", NULL, NULL, "{{ value_json.state }}", "stat", STAT_EXPIRE_SEC, false, NULL},
    {"sensor", "active_timer", "Active timer", NULL, NULL, "{{ value_json.active_timer }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "remaining", "Time remaining", "min", "duration", "{{ (value_json.remaining_s / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, NULL},
    {"sensor", "allocation", "Today's limit", "min", "duration", "{{ (value_json.allocation_s / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "day_type", "Day type", NULL, NULL, "{{ value_json.day_type }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"binary_sensor", "charge_lock", "Charge lock", NULL, NULL, "{{ 'ON' if value_json.charge_lock else 'OFF' }}",
     "stat", STAT_EXPIRE_SEC, true, NULL},
    {"sensor", "screen_used", "Screen time used today", "min", "duration",
     "{{ (value_json.screen_used_s / 60) | round(0) }}", "summary", 0, false, DIAG},
    {"sensor", "completions_1", "Timer 1 runs", NULL, NULL, "{{ value_json.completions[0] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "completions_2", "Timer 2 runs", NULL, NULL, "{{ value_json.completions[1] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "completions_3", "Timer 3 runs", NULL, NULL, "{{ value_json.completions[2] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "completions_4", "Timer 4 runs", NULL, NULL, "{{ value_json.completions[3] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    /* Boot forensics: anything but DEEPSLEEP on a wake means the previous
       wake died (BROWNOUT/PANIC/...) — the USB CDC console loses that
       evidence, MQTT doesn't. Never expires. */
    {"sensor", "last_reset", "Last reset", NULL, NULL, "{{ value_json.reset }}", "stat", 0, false, DIAG},
};

const ha_entity_t *stats_json_entities(int *count) {
    *count = (int)(sizeof(ENTITIES) / sizeof(ENTITIES[0]));
    return ENTITIES;
}

int stats_json_discovery_topic(char *buf, size_t len, const char *dev_id, const ha_entity_t *ent) {
    return snprintf(buf, len, "homeassistant/%s/%s_%s/config", ent->component, dev_id, ent->key);
}

int stats_json_discovery_named(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                               const ha_entity_t *ent, const char *name_override) {
    char ename[64], dname[64];
    int pos = 0;
    pos = jcat(buf, len, pos, "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"magtag/%s/%s\",\"val_tpl\":\"%s\"",
               jesc(ename, sizeof(ename), name_override ? name_override : ent->name), dev_id, ent->key, dev_id,
               ent->topic_suffix, ent->tpl);
    if (ent->unit != NULL)
        pos = jcat(buf, len, pos, ",\"unit_of_meas\":\"%s\"", ent->unit);
    if (ent->dev_class != NULL)
        pos = jcat(buf, len, pos, ",\"dev_cla\":\"%s\"", ent->dev_class);
    if (ent->binary)
        pos = jcat(buf, len, pos, ",\"pl_on\":\"ON\",\"pl_off\":\"OFF\"");
    if (ent->ent_cat != NULL)
        pos = jcat(buf, len, pos, ",\"ent_cat\":\"%s\"", ent->ent_cat);
    if (ent->expire_after > 0)
        pos = jcat(buf, len, pos, ",\"expire_after\":%d", ent->expire_after);
    pos = jcat(buf, len, pos,
               ",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Adafruit\",\"mdl\":\"MagTag 2.9\","
               "\"sw\":\"%s\"}}",
               dev_id, jesc(dname, sizeof(dname), dev_name), fw);
    return pos;
}

int stats_json_discovery(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                         const ha_entity_t *ent) {
    return stats_json_discovery_named(buf, len, dev_id, dev_name, fw, ent, NULL);
}
