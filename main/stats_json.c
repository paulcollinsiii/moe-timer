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

int stats_json_stat(char *buf, size_t len, const stats_snapshot_t *s, const ota_stat_t *ota, const diag_stat_t *diag) {
    /* Every string field is escaped (and NULL-flattened to "") — the pure
       boundary must never invoke UB on a bad/NULL field. The two OTA
       buffers are DOUBLE the stored width because jesc escapes, and a
       target string of nothing but backslashes doubles in length. */
    char state[24], name[64], day[24], fw[32], rst[24];
    char ores[CFG_BOUND_OTA_RESULT_MAX * 2], otgt[CFG_BOUND_OTA_TARGET_MAX * 2];
    /* The phase label is built by panic_diag.c from a fixed literal map,
       so it can contain no quote and no backslash. Escaped anyway, and
       double-width like the OTA pair: this function's contract is that
       NO string reaching it can break the JSON, and a field that is safe
       only because of what some other module currently does is a field
       that stops being safe the day that module changes. */
    char pph[DIAG_PHASE_MAX * 2];
    int pos = 0;
    pos = jcat(buf, len, pos,
               "{\"batt_pct\":%d,\"batt_mv\":%d,\"light_mv\":%d,\"state\":\"%s\",\"active_timer\":\"%s\","
               "\"remaining_s\":[",
               s->batt_pct, s->batt_mv, s->light_mv, jesc(state, sizeof(state), s->state),
               jesc(name, sizeof(name), s->active_timer));
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        pos = jcat(buf, len, pos, i ? ",%ld" : "%ld", (long)s->remaining_s[i]);
    }
    pos = jcat(buf, len, pos, "],\"allocation_s\":[");
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        pos = jcat(buf, len, pos, i ? ",%lu" : "%lu", (unsigned long)s->allocation_s[i]);
    }
    pos = jcat(buf, len, pos, "],\"day_type\":\"%s\",\"completions\":[", jesc(day, sizeof(day), s->day_type));
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        pos = jcat(buf, len, pos, i ? ",%u" : "%u", (unsigned)s->completions[i]);
    }
    pos = jcat(buf, len, pos, "],\"charge_lock\":%s,\"break_s\":%ld,\"accum_s\":%ld,\"fw\":\"%s\",\"reset\":\"%s\"",
               s->charge_lock ? "true" : "false", (long)s->break_remaining_s, (long)s->accum_s,
               jesc(fw, sizeof(fw), s->fw), jesc(rst, sizeof(rst), s->reset_reason));
    /* The OTA leg. Read from NVS at publish time and handed in — never a
       snapshot field; stats_json.h says why. */
    pos = jcat(buf, len, pos, ",\"ota_result\":\"%s\",\"ota_target\":\"%s\",\"ota_fails\":%u,\"ota_dl_ms\":%lu",
               jesc(ores, sizeof(ores), (ota != NULL) ? ota->result : NULL),
               jesc(otgt, sizeof(otgt), (ota != NULL) ? ota->target : NULL), (unsigned)((ota != NULL) ? ota->fails : 0),
               (unsigned long)((ota != NULL) ? ota->dl_ms : 0));
    /* The diagnostics leg — the last panic, then the live health
       readings. Same NULL tolerance and the same read-at-publish-time
       reasoning as the OTA leg above; stats_json.h says why neither can
       ride in the snapshot. */
    pos = jcat(buf, len, pos, ",\"panics\":%lu,\"pphase\":\"%s\",\"pup_s\":%lu,\"pheap\":%lu",
               (unsigned long)((diag != NULL) ? diag->panics : 0),
               jesc(pph, sizeof(pph), (diag != NULL) ? diag->panic_phase : NULL),
               (unsigned long)((diag != NULL) ? diag->panic_uptime_s : 0),
               (unsigned long)((diag != NULL) ? diag->panic_heap : 0));
    pos = jcat(buf, len, pos, ",\"pstk_main\":%u,\"pstk_net\":%u",
               (unsigned)((diag != NULL) ? diag->panic_stack_main : 0),
               (unsigned)((diag != NULL) ? diag->panic_stack_net : 0));
    pos = jcat(buf, len, pos, ",\"heap\":%lu,\"heap_min\":%lu,\"stk_main\":%u,\"stk_net\":%u,\"nvs_free\":%u}",
               (unsigned long)((diag != NULL) ? diag->heap_free : 0),
               (unsigned long)((diag != NULL) ? diag->heap_min : 0), (unsigned)((diag != NULL) ? diag->stack_main : 0),
               (unsigned)((diag != NULL) ? diag->stack_net : 0), (unsigned)((diag != NULL) ? diag->nvs_free : 0));
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
    /* Per-slot remaining/limit ([0] = Screen; extra slots below, runtime-
       named like completions_N) so each timer keeps its own HA history.
       "Used" is derivable: limit - remaining. */
    {"sensor", "screen_remaining", "Screen time remaining", "min", "duration",
     "{{ (value_json.remaining_s[0] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, NULL},
    {"sensor", "screen_limit", "Screen time limit", "min", "duration",
     "{{ (value_json.allocation_s[0] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "day_type", "Day type", NULL, NULL, "{{ value_json.day_type }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"binary_sensor", "charge_lock", "Charge lock", NULL, NULL, "{{ 'ON' if value_json.charge_lock else 'OFF' }}",
     "stat", STAT_EXPIRE_SEC, true, NULL},
    /* Screen Break: a break runs behind whatever timer is selected, so
       the "state" sensor reports BREAK only when Screen happens to be
       selected — these two are the honest signal. Keys deliberately do
       NOT start with remaining_/limit_/completions_, which mqtt_ha.c
       matches by prefix to attach a runtime slot name. */
    {"binary_sensor", "screen_break", "Screen break", NULL, NULL, "{{ 'ON' if value_json.break_s > 0 else 'OFF' }}",
     "stat", STAT_EXPIRE_SEC, true, NULL},
    {"sensor", "break_remaining", "Screen break remaining", "min", "duration",
     "{{ (value_json.break_s / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    /* The exposure balance driving the break: rises while a non-eligible
       timer runs, falls while a break-eligible one does. Read against the
       configured interval it explains every break that did or did not
       fire. Key clear of the remaining_/limit_/completions_ prefixes for
       the same reason as the two above. */
    {"sensor", "screen_exposure", "Screen exposure", "min", "duration", "{{ (value_json.accum_s / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "completions_1", "Timer 1 runs", NULL, NULL, "{{ value_json.completions[0] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "completions_2", "Timer 2 runs", NULL, NULL, "{{ value_json.completions[1] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "completions_3", "Timer 3 runs", NULL, NULL, "{{ value_json.completions[2] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "completions_4", "Timer 4 runs", NULL, NULL, "{{ value_json.completions[3] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "remaining_1", "Timer 1 remaining", "min", "duration",
     "{{ (value_json.remaining_s[1] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "remaining_2", "Timer 2 remaining", "min", "duration",
     "{{ (value_json.remaining_s[2] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "remaining_3", "Timer 3 remaining", "min", "duration",
     "{{ (value_json.remaining_s[3] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "remaining_4", "Timer 4 remaining", "min", "duration",
     "{{ (value_json.remaining_s[4] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "limit_1", "Timer 1 limit", "min", "duration", "{{ (value_json.allocation_s[1] / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "limit_2", "Timer 2 limit", "min", "duration", "{{ (value_json.allocation_s[2] / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "limit_3", "Timer 3 limit", "min", "duration", "{{ (value_json.allocation_s[3] / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "limit_4", "Timer 4 limit", "min", "duration", "{{ (value_json.allocation_s[4] / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG},
    /* Boot forensics: anything but DEEPSLEEP on a wake means the previous
       wake died (BROWNOUT/PANIC/...) — the USB CDC console loses that
       evidence, MQTT doesn't. Never expires. */
    {"sensor", "last_reset", "Last reset", NULL, NULL, "{{ value_json.reset }}", "stat", 0, false, DIAG},
    /* ---- OTA. All four carry expire_after 0, and that is a decision,
       not a copy of the neighbour above.

       STAT_EXPIRE_SEC exists so a device that has stopped checking in
       reads "unavailable" instead of showing stale live telemetry, and
       for a battery reading that is exactly right. These four are not
       telemetry: they are the record of the last update attempt, and
       they change only when an attempt happens — which on this device is
       at most once a day and usually never. An expiry would blank them
       on precisely the device this task exists to make legible: one that
       tried to update, rolled back, and is now failing to check in. The
       evidence has to outlive the silence, so they follow last_reset's
       precedent rather than the sensors around it. (They are still
       republished every window, so a live device keeps them fresh either
       way; expire_after only decides what happens when it stops.)

       ota_result is the one PRIMARY entity of the four. "Did my update
       work?" is the operator's question and this is the answer to it —
       the whole point of the entry in docs/planning/ota.plan.md is that
       a rollback was invisible. The other three are the supporting
       detail consulted after that answer, so they sit in the Diagnostic
       group. Keys are clear of the remaining_/limit_/completions_
       prefixes mqtt_ha.c matches on to attach a runtime slot name. */
    {"sensor", "ota_result", "Update result", NULL, NULL, "{{ value_json.ota_result }}", "stat", 0, false, NULL},
    {"sensor", "ota_target", "Update target", NULL, NULL, "{{ value_json.ota_target }}", "stat", 0, false, DIAG},
    {"sensor", "ota_fails", "Update failures", NULL, NULL, "{{ value_json.ota_fails }}", "stat", 0, false, DIAG},
    /* Milliseconds, unconverted: the number is read against the download
       deadline (CONFIG_MAGTAG_OTA_MAX_SEC), and a link trending toward it
       shows up as a rising figure long before it becomes a timeout. */
    {"sensor", "ota_dl_ms", "Update download time", "ms", "duration", "{{ value_json.ota_dl_ms }}", "stat", 0, false,
     DIAG},
    /* ---- panic forensics. See include/panic_diag.h for the whole
       argument; what matters HERE is expire_after and the primary/
       diagnostic split.

       The four panic entities carry expire_after 0, for last_reset's
       reason rather than the telemetry sensors' reason: they are the
       RECORD of an event, not a live reading, and they change only when
       a panic happens. An expiry would blank them on exactly the device
       this exists for — one that panicked and then went quiet.

       panic_count is the one PRIMARY entity of the group. "Is it still
       crashing, and how often?" is the operator's question; the phase,
       the uptime and the heap are the detail consulted after that
       answer, so they sit in Diagnostic.

       panic_count is a TOTAL_INCREASING measurement in HA terms, which
       is deliberately NOT declared as a state_class here: this table has
       no state_class column and adding one for a single row would be a
       schema change for cosmetics. HA still graphs the raw value, and
       the useful reading is the difference between two points, which
       works either way.

       Keys are clear of the remaining_/limit_/completions_ prefixes
       mqtt_ha.c matches on to attach a runtime slot name. */
    {"sensor", "panic_count", "Panic count", NULL, NULL, "{{ value_json.panics }}", "stat", 0, false, NULL},
    /* Blank means no breadcrumb is on file; "NONE" means a panic landed
       outside every marked phase. panic_diag.c keeps those distinct on
       purpose. */
    {"sensor", "panic_phase", "Panic phase", NULL, NULL, "{{ value_json.pphase }}", "stat", 0, false, DIAG},
    {"sensor", "panic_uptime", "Panic uptime", "s", "duration", "{{ value_json.pup_s }}", "stat", 0, false, DIAG},
    {"sensor", "panic_heap", "Panic free heap", "B", NULL, "{{ value_json.pheap }}", "stat", 0, false, DIAG},
    {"sensor", "panic_stack_main", "Panic stack free (main)", "B", NULL, "{{ value_json.pstk_main }}", "stat", 0, false,
     DIAG},
    {"sensor", "panic_stack_net", "Panic stack free (net)", "B", NULL, "{{ value_json.pstk_net }}", "stat", 0, false,
     DIAG},
    /* ---- live health. These five ARE telemetry, so they take
       STAT_EXPIRE_SEC like the battery: a device that has stopped
       checking in should read unavailable rather than show a heap figure
       from yesterday.

       Bytes, unconverted, and no dev_class: HA's data_size class exists
       but brings unit conversion with it, and these numbers are read
       against fixed budgets (the 10 KB net_win stack, the 16 KB ota_dl
       stack) where a helpfully rescaled "9.8 kB" is harder to compare,
       not easier. */
    {"sensor", "heap_free", "Free heap", "B", NULL, "{{ value_json.heap }}", "stat", STAT_EXPIRE_SEC, false, DIAG},
    {"sensor", "heap_min", "Free heap low water", "B", NULL, "{{ value_json.heap_min }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "stack_main", "Main task stack free", "B", NULL, "{{ value_json.stk_main }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    {"sensor", "stack_net", "Network task stack free", "B", NULL, "{{ value_json.stk_net }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG},
    /* Free ENTRIES, not bytes, and the only NVS figure published: total
       is a constant of a partition table frozen for OTA'd devices and
       used is total - free, so either would be the same fact twice.
       Answers the headroom question behind main.c's silent
       nvs_flash_erase() on ESP_ERR_NVS_NO_FREE_PAGES. */
    {"sensor", "nvs_free", "NVS free entries", NULL, NULL, "{{ value_json.nvs_free }}", "stat", STAT_EXPIRE_SEC, false,
     DIAG},
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
    /* obj_id carries the SAME string as uniq_id, on purpose. Without it
       HA builds the entity_id from the device name and the entity name,
       so renaming a device silently re-slugs every entity under it —
       which is how a house automation that excluded "magtag" stopped
       matching devices renamed to "Testing Timer" and swept their
       switches off. dev_id is MAC-derived (device_id.h) and outlives any
       rename, so entity_ids built from it are stable by construction.
       See DISC_SCHEMA_VER's note: this only takes effect at an entity's
       FIRST registration. */
    pos = jcat(buf, len, pos,
               "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"obj_id\":\"%s_%s\","
               "\"stat_t\":\"magtag/%s/%s\",\"val_tpl\":\"%s\"",
               jesc(ename, sizeof(ename), name_override ? name_override : ent->name), dev_id, ent->key, dev_id,
               ent->key, dev_id, ent->topic_suffix, ent->tpl);
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
