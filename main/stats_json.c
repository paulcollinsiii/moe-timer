/* Pure JSON builders for the HA MQTT integration — host-tested. */
#include "stats_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "chores.h"   /* CHORE_MAX */
#include "schedule.h" /* SCHEDULE_DAY_TYPES, schedule_day_type_name() — header only */

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

/* The config warning's state: "OK", or every broken day type by name in
   day_type_t order, ", "-separated. Never "": HA's MQTT sensor IGNORES an
   empty state and keeps the previous one, so a warning that cleared to ""
   would read as still broken, and the clear — the one event the owner
   watches the activity log for — would never be logged. Bits at or above
   SCHEDULE_DAY_TYPES are ignored rather than trusted. */
#define CFG_WARN_OK "OK"
static const char *cfg_warn_text(char *tmp, size_t tmplen, uint8_t bad) {
    size_t o = 0;
    tmp[0] = '\0';
    for (unsigned d = 0; d < SCHEDULE_DAY_TYPES; d++) {
        if ((bad & (1u << d)) == 0)
            continue;
        int n = snprintf(tmp + o, tmplen - o, "%s%s", o ? ", " : "", schedule_day_type_name((day_type_t)d));
        if (n < 0 || (size_t)n >= tmplen - o)
            break; /* cannot happen at the size below; never write past */
        o += (size_t)n;
    }
    return (o == 0) ? CFG_WARN_OK : tmp;
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
    /* The chore leg (design 1.4). chore_ack always carries CHORE_MAX
       entries, as completions always carries every extra slot, so each
       chore_N sensor's template has an index to read whatever the list
       length; a position past the configured count reports 0 even if the
       caller left its bit set, because a stale bit must never light a
       sensor for a chore that is not there. */
    const unsigned chore_n = (unsigned)s->chores_left + (unsigned)s->chores_done;
    pos = jcat(buf, len, pos, ",\"chores_left\":%u,\"chores_done\":%u,\"chore_ack\":[", (unsigned)s->chores_left,
               (unsigned)s->chores_done);
    for (unsigned i = 0; i < CHORE_MAX; i++) {
        const unsigned on = (i < chore_n && (s->chore_acked & (1u << i)) != 0) ? 1u : 0u;
        pos = jcat(buf, len, pos, i ? ",%u" : "%u", on);
    }
    /* Widest reading: all four day types, "Weekday, Weekend, Holiday,
       Summer" — 33 characters. The names are literals from schedule.h and
       cannot hold a quote; escaped anyway, and double-width, for the
       reason the panic phase is (see the top of this function). */
    char warn[48], warn_esc[96];
    pos = jcat(buf, len, pos, "],\"cfg_warn\":\"%s\"",
               jesc(warn_esc, sizeof(warn_esc), cfg_warn_text(warn, sizeof(warn), s->chore_free_bad)));
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

int stats_json_act(char *buf, size_t len, const act_state_t *a) {
    int32_t sec = a->applied_s;
    if (a->day_cleared) {
        sec = 0; /* outranks a stale retained target from yesterday */
    } else if (a->target_pending) {
        sec = a->target_s; /* applied at the join, moments from now */
    }
    /* locate is momentary: the switch always reports back off. */
    return jcat(buf, len, 0, "{\"screen_bonus\":%ld,\"locate\":\"OFF\"}", (long)(sec / 60));
}

int stats_json_summary(char *buf, size_t len, const char *date, int32_t screen_used_s,
                       const uint16_t completions[TIMER_EXTRA_SLOTS], unsigned chores_done, int chores) {
    int pos = jcat(buf, len, 0, "{\"date\":\"%s\",\"screen_used_s\":%ld,\"completions\":[", date, (long)screen_used_s);
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        pos = jcat(buf, len, pos, i ? ",%u" : "%u", (unsigned)completions[i]);
    }
    pos = jcat(buf, len, pos, "]");
    if (chores >= 0) { /* a list that could not be read: both fields omitted */
        pos = jcat(buf, len, pos, ",\"chores_done\":%u,\"chores\":%d", chores_done, chores);
    }
    pos = jcat(buf, len, pos, "}");
    return pos;
}

/* Discovery entity table. expire_after on stat-fed sensors is 2x the
   default idle sync interval + margin, so entities read available while
   the device sleeps but flag a genuinely dead device. The daily summary
   sensors never expire. Order matters only for [0] (battery) in tests. */
#define STAT_EXPIRE_SEC 7500

/* Fields: component, key, name, unit, dev_class, tpl, topic_suffix,
   expire_after, binary, ent_cat, state_class, last_reset_tpl. ent_cat
   "diagnostic" tucks noisy read-onlys into HA's Diagnostic group; NULL =
   primary (top-level). state_class NULL = omit; ha_entity_t says what it
   costs. last_reset_tpl NULL = omit; only the summary-topic rows set it. */
#define DIAG "diagnostic"

/* The summary-topic rows (v23): state_class "total", and a last_reset
   read from the summary's own date, which stats_json_summary writes as
   the device's local "YYYY-MM-DD". Each summary is one finished day, so a
   new date is a new cycle, and HA's statistics `change` for a period is
   exactly what the summaries in it reported. A retained redelivery or an
   HA restart carries the same date and value, and adds nothing.

   HA parses last_reset as a datetime, so the date alone will not do. It
   is a cycle marker, compared only for "did it change": any time that is
   distinct per date and rises with it works. The fixed +00:00 makes it
   parse the same way on every HA, whatever its time zone; the local
   midnight it is not is visible only in the entity's attributes. */
#define SUMMARY_STATE_CLASS "total"
#define SUMMARY_LAST_RESET "{{ value_json.date ~ 'T00:00:00+00:00' }}"

static const ha_entity_t ENTITIES[] = {
    /* state_class "measurement" (v23): "battery over weeks" is a
       statistics graph, and HA keeps long-term statistics only for a
       sensor that declares one. The logbook loses nothing by it: HA
       already left the battery out for its unit (%). */
    {"sensor", "battery", "Battery", "%", "battery", "{{ value_json.batt_pct }}", "stat", STAT_EXPIRE_SEC, false, NULL,
     "measurement", NULL},
    {"sensor", "battery_mv", "Battery voltage", "mV", "voltage", "{{ value_json.batt_mv }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG, NULL, NULL},
    {"sensor", "light", "Ambient light", "mV", NULL, "{{ value_json.light_mv }}", "stat", STAT_EXPIRE_SEC, false, DIAG,
     NULL, NULL},
    {"sensor", "state", "Timer state", NULL, NULL, "{{ value_json.state }}", "stat", STAT_EXPIRE_SEC, false, NULL, NULL,
     NULL},
    {"sensor", "active_timer", "Active timer", NULL, NULL, "{{ value_json.active_timer }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG, NULL, NULL},
    /* Per-slot remaining/limit ([0] = Screen; extra slots below, runtime-
       named like completions_N) so each timer keeps its own HA history.
       "Used" is derivable: limit - remaining. */
    {"sensor", "screen_remaining", "Screen time remaining", "min", "duration",
     "{{ (value_json.remaining_s[0] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, NULL, NULL, NULL},
    {"sensor", "screen_limit", "Screen time limit", "min", "duration",
     "{{ (value_json.allocation_s[0] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    /* The finished day's Screen minutes (v23), the "screen minutes per
       day" statistics graph, and a summary-topic row (see
       SUMMARY_LAST_RESET): mqtt_ha.c publishes the retained summary once,
       at the first window after the rollover, so expire_after is 0 — an
       expiry would blank the value for the 23 hours in which nothing new
       is due.

       DAY SHIFT: the summary lands after midnight, so HA files the value
       under the day AFTER the one it describes. Nothing here can move it —
       HA stamps a state with its arrival time — so the dashboard says so.

       The key is NOT "screen_used": mqtt_ha.c's RETIRED[] publishes an
       empty discovery for that one on every pass, which would delete this
       entity as fast as it is created. And it is clear of every prefix
       stats_json_slot_of() matches. */
    {"sensor", "screen_used_day", "Screen time per day", "min", "duration",
     "{{ (value_json.screen_used_s / 60) | round(0) }}", "summary", 0, false, DIAG, SUMMARY_STATE_CLASS,
     SUMMARY_LAST_RESET},
    {"sensor", "day_type", "Day type", NULL, NULL, "{{ value_json.day_type }}", "stat", STAT_EXPIRE_SEC, false, DIAG,
     NULL, NULL},
    {"binary_sensor", "charge_lock", "Charge lock", NULL, NULL, "{{ 'ON' if value_json.charge_lock else 'OFF' }}",
     "stat", STAT_EXPIRE_SEC, true, NULL, NULL, NULL},
    /* Screen Break: a break runs behind whatever timer is selected, so
       the "state" sensor reports BREAK only when Screen happens to be
       selected — these two are the honest signal. Keys deliberately do
       NOT start with any per-slot prefix (stats_json_slot_of), which
       mqtt_ha.c matches to attach a runtime slot name. */
    {"binary_sensor", "screen_break", "Screen break", NULL, NULL, "{{ 'ON' if value_json.break_s > 0 else 'OFF' }}",
     "stat", STAT_EXPIRE_SEC, true, NULL, NULL, NULL},
    {"sensor", "break_remaining", "Screen break remaining", "min", "duration",
     "{{ (value_json.break_s / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    /* The exposure balance driving the break: rises while a non-eligible
       timer runs, falls while a break-eligible one does. Read against the
       configured interval it explains every break that did or did not
       fire. Key clear of the per-slot prefixes for the same reason as the
       two above. */
    {"sensor", "screen_exposure", "Screen exposure", "min", "duration", "{{ (value_json.accum_s / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    /* The live run counts: NO state_class, so each run keeps its logbook
       line. They are not the runs graph's source: the rollover zeroes
       them, and a run finished after the day's last window never reaches
       HA through them. day_runs_N below reads the summary, which has it. */
    {"sensor", "completions_1", "Timer 1 runs", NULL, NULL, "{{ value_json.completions[0] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG, NULL, NULL},
    {"sensor", "completions_2", "Timer 2 runs", NULL, NULL, "{{ value_json.completions[1] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG, NULL, NULL},
    {"sensor", "completions_3", "Timer 3 runs", NULL, NULL, "{{ value_json.completions[2] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG, NULL, NULL},
    {"sensor", "completions_4", "Timer 4 runs", NULL, NULL, "{{ value_json.completions[3] }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG, NULL, NULL},
    /* The finished day's runs of each extra timer (v23), the "how often
       was Violin finished" graph: summary-topic rows (see
       SUMMARY_LAST_RESET), so every run of the day is counted, the last
       one before midnight included, under the day after (screen_used_day's
       day shift). Named "<timer> runs per day" and retired with a disabled
       slot, like completions_N (stats_json_slot_of). No unit, like the
       live counts: a number of runs has none, and a unit would buy
       nothing — state_class already takes these out of the logbook.
       DIAG, as screen_used_day. The key avoids every prefix and RETIRED[]
       key mqtt_ha.c treats specially; "completions_" in particular must
       not match it, or the slot name would be "Violin runs" twice. */
    {"sensor", "day_runs_1", "Timer 1 runs per day", NULL, NULL, "{{ value_json.completions[0] }}", "summary", 0, false,
     DIAG, SUMMARY_STATE_CLASS, SUMMARY_LAST_RESET},
    {"sensor", "day_runs_2", "Timer 2 runs per day", NULL, NULL, "{{ value_json.completions[1] }}", "summary", 0, false,
     DIAG, SUMMARY_STATE_CLASS, SUMMARY_LAST_RESET},
    {"sensor", "day_runs_3", "Timer 3 runs per day", NULL, NULL, "{{ value_json.completions[2] }}", "summary", 0, false,
     DIAG, SUMMARY_STATE_CLASS, SUMMARY_LAST_RESET},
    {"sensor", "day_runs_4", "Timer 4 runs per day", NULL, NULL, "{{ value_json.completions[3] }}", "summary", 0, false,
     DIAG, SUMMARY_STATE_CLASS, SUMMARY_LAST_RESET},
    {"sensor", "remaining_1", "Timer 1 remaining", "min", "duration",
     "{{ (value_json.remaining_s[1] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    {"sensor", "remaining_2", "Timer 2 remaining", "min", "duration",
     "{{ (value_json.remaining_s[2] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    {"sensor", "remaining_3", "Timer 3 remaining", "min", "duration",
     "{{ (value_json.remaining_s[3] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    {"sensor", "remaining_4", "Timer 4 remaining", "min", "duration",
     "{{ (value_json.remaining_s[4] / 60) | round(0) }}", "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    {"sensor", "limit_1", "Timer 1 limit", "min", "duration", "{{ (value_json.allocation_s[1] / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    {"sensor", "limit_2", "Timer 2 limit", "min", "duration", "{{ (value_json.allocation_s[2] / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    {"sensor", "limit_3", "Timer 3 limit", "min", "duration", "{{ (value_json.allocation_s[3] / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    {"sensor", "limit_4", "Timer 4 limit", "min", "duration", "{{ (value_json.allocation_s[4] / 60) | round(0) }}",
     "stat", STAT_EXPIRE_SEC, false, DIAG, NULL, NULL},
    /* Boot forensics: anything but DEEPSLEEP on a wake means the previous
       wake died (BROWNOUT/PANIC/...) — the USB CDC console loses that
       evidence, MQTT doesn't. Never expires. */
    {"sensor", "last_reset", "Last reset", NULL, NULL, "{{ value_json.reset }}", "stat", 0, false, DIAG, NULL, NULL},
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
       group. Keys are clear of the per-slot prefixes
       (stats_json_slot_of) mqtt_ha.c matches on to attach a runtime slot
       name. */
    {"sensor", "ota_result", "Update result", NULL, NULL, "{{ value_json.ota_result }}", "stat", 0, false, NULL, NULL,
     NULL},
    {"sensor", "ota_target", "Update target", NULL, NULL, "{{ value_json.ota_target }}", "stat", 0, false, DIAG, NULL,
     NULL},
    {"sensor", "ota_fails", "Update failures", NULL, NULL, "{{ value_json.ota_fails }}", "stat", 0, false, DIAG, NULL,
     NULL},
    /* Milliseconds, unconverted: the number is read against the download
       deadline (CONFIG_MAGTAG_OTA_MAX_SEC), and a link trending toward it
       shows up as a rising figure long before it becomes a timeout. */
    {"sensor", "ota_dl_ms", "Update download time", "ms", "duration", "{{ value_json.ota_dl_ms }}", "stat", 0, false,
     DIAG, NULL, NULL},
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
       is deliberately NOT declared as a state_class here. The column
       exists (v22 added it for the chore counts), but this row was left
       as it shipped: HA still graphs the raw value, and the useful
       reading is the difference between two points, which works either
       way.

       Keys are clear of the per-slot prefixes (stats_json_slot_of)
       mqtt_ha.c matches on to attach a runtime slot name. */
    {"sensor", "panic_count", "Panic count", NULL, NULL, "{{ value_json.panics }}", "stat", 0, false, NULL, NULL, NULL},
    /* Blank means no breadcrumb is on file; "NONE" means a panic landed
       outside every marked phase. panic_diag.c keeps those distinct on
       purpose. */
    {"sensor", "panic_phase", "Panic phase", NULL, NULL, "{{ value_json.pphase }}", "stat", 0, false, DIAG, NULL, NULL},
    {"sensor", "panic_uptime", "Panic uptime", "s", "duration", "{{ value_json.pup_s }}", "stat", 0, false, DIAG, NULL,
     NULL},
    {"sensor", "panic_heap", "Panic free heap", "B", NULL, "{{ value_json.pheap }}", "stat", 0, false, DIAG, NULL,
     NULL},
    {"sensor", "panic_stack_main", "Panic stack free (main)", "B", NULL, "{{ value_json.pstk_main }}", "stat", 0, false,
     DIAG, NULL, NULL},
    {"sensor", "panic_stack_net", "Panic stack free (net)", "B", NULL, "{{ value_json.pstk_net }}", "stat", 0, false,
     DIAG, NULL, NULL},
    /* ---- live health. These five ARE telemetry, so they take
       STAT_EXPIRE_SEC like the battery: a device that has stopped
       checking in should read unavailable rather than show a heap figure
       from yesterday.

       Bytes, unconverted, and no dev_class: HA's data_size class exists
       but brings unit conversion with it, and these numbers are read
       against fixed budgets (the 10 KB net_win stack, the 16 KB ota_dl
       stack) where a helpfully rescaled "9.8 kB" is harder to compare,
       not easier. */
    {"sensor", "heap_free", "Free heap", "B", NULL, "{{ value_json.heap }}", "stat", STAT_EXPIRE_SEC, false, DIAG, NULL,
     NULL},
    {"sensor", "heap_min", "Free heap low water", "B", NULL, "{{ value_json.heap_min }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG, NULL, NULL},
    {"sensor", "stack_main", "Main task stack free", "B", NULL, "{{ value_json.stk_main }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG, NULL, NULL},
    {"sensor", "stack_net", "Network task stack free", "B", NULL, "{{ value_json.stk_net }}", "stat", STAT_EXPIRE_SEC,
     false, DIAG, NULL, NULL},
    /* Free ENTRIES, not bytes, and the only NVS figure published: total
       is a constant of a partition table frozen for OTA'd devices and
       used is total - free, so either would be the same fact twice.
       Answers the headroom question behind main.c's silent
       nvs_flash_erase() on ESP_ERR_NVS_NO_FREE_PAGES. */
    {"sensor", "nvs_free", "NVS free entries", NULL, NULL, "{{ value_json.nvs_free }}", "stat", STAT_EXPIRE_SEC, false,
     DIAG, NULL, NULL},
    /* ---- the chore checklist (design 1.4). Read-only: the device is the
       sole authority on acks, and HA displays them. All live state, so
       STAT_EXPIRE_SEC like the battery.

       chores_left is PRIMARY and chores_done DIAGNOSTIC, as the design
       lists them: "how much is still to do" is the parent's question.

       state_class "measurement" on both (v22): HA keeps LONG-TERM
       STATISTICS only for a sensor that declares one. The price is the
       logbook — HA leaves any sensor with a state_class (or a unit) out
       of it — and it is affordable here because the chore_N binary
       sensors below carry the per-chore audit trail ("Homework done" on,
       off) on their own. Still no unit: a count of chores has none.
       They are NOT the "chores done per day" graph's source: a daily
       `max` of chores_done includes the value carried across midnight, so
       a day with nothing done can show the day before's full count.
       day_chores, below, reads the summary instead.

       The full list of rows that declare a state_class (test_stats_json
       pins it): battery, screen_used_day, day_runs_1..4, day_chores and
       these two. Every other row stays NULL. */
    {"sensor", "chores_left", "Chores left", NULL, NULL, "{{ value_json.chores_left }}", "stat", STAT_EXPIRE_SEC, false,
     NULL, "measurement", NULL},
    {"sensor", "chores_done", "Chores done", NULL, NULL, "{{ value_json.chores_done }}", "stat", STAT_EXPIRE_SEC, false,
     DIAG, "measurement", NULL},
    /* The finished day's acked chores (v23, M4-T5), the "chores done per
       day" statistics graph: a summary-topic row like day_runs_N (see
       SUMMARY_LAST_RESET), with the same day shift. The rollover captures
       it before anything resets the acks (wake_flow.c
       queue_rollover_summary). No unit, DIAG, no expire, as day_runs_N.
       The key is clear of every stats_json_slot_of() prefix ("day_runs_"
       included), of the exact chore_N match, and of RETIRED[].

       The template renders "None" when the summary has no chores_done
       (older firmware, or a list that could not be read): HA's MQTT
       sensor sets "unknown" for that, which the statistics skip. A bare
       {{ value_json.chores_done }} would render "" there, and HA ignores
       an empty numeric state while still taking the new last_reset, so
       the previous day's count would be counted again. `is defined` also
       keeps HA from logging an undefined-variable warning per summary. */
    {"sensor", "day_chores", "Chores done per day", NULL, NULL,
     "{{ value_json.chores_done if value_json.chores_done is defined else 'None' }}", "summary", 0, false, DIAG,
     SUMMARY_STATE_CLASS, SUMMARY_LAST_RESET},
    /* One per possible chore, named at discovery time from the list
       ("<name> done") by mqtt_ha.c through stats_json_chore_index(); a row
       past the configured count is RETIRED there, exactly as a disabled
       timer slot's rows are. PRIMARY: "Homework done, 16:04" is the
       per-chore history the design calls the parent's audit trail, and it
       belongs on the device card, not under Diagnostic. The keys are
       chore_1..chore_N and are matched EXACTLY — chores_left shares the
       first five characters and must never be read as a chore. */
    {"binary_sensor", "chore_1", "Chore 1 done", NULL, NULL, "{{ 'ON' if value_json.chore_ack[0] else 'OFF' }}", "stat",
     STAT_EXPIRE_SEC, true, NULL, NULL, NULL},
    {"binary_sensor", "chore_2", "Chore 2 done", NULL, NULL, "{{ 'ON' if value_json.chore_ack[1] else 'OFF' }}", "stat",
     STAT_EXPIRE_SEC, true, NULL, NULL, NULL},
    {"binary_sensor", "chore_3", "Chore 3 done", NULL, NULL, "{{ 'ON' if value_json.chore_ack[2] else 'OFF' }}", "stat",
     STAT_EXPIRE_SEC, true, NULL, NULL, NULL},
    /* M2-D6: a broken chore_free pair on ANY day type, by name ("OK" when
       every pair is valid). It exists because nothing else reports a
       broken pair durably: the config_ack names it once and the next
       document overwrites it, and the config-error lock reads only
       today's day type — a broken summer pair written in December is
       otherwise invisible until it locks the device in June.

       A TEXT SENSOR, not a problem binary_sensor, because the day type is
       the actionable part and a binary_sensor could only carry it as an
       attribute, which this table has no column for. A text state puts
       the name in the activity-log line itself ("Config warning changed
       to Summer"), for the appearing AND the clearing, which is the
       surface the owner chose. So no unit and NO state_class, for the
       logbook reason above: either one would drop it from the logbook.

       expire_after 0: this reports a CONFIGURATION that stays broken until
       someone fixes it, not a live reading, so it follows last_reset's
       precedent. An expiry would blank the warning on a device that has
       gone quiet (a flat battery, a dead wifi), and the broken pair is
       still sitting in its NVS waiting for it to come back. The key is
       clear of every prefix mqtt_ha.c matches. */
    {"sensor", "config_warning", "Config warning", NULL, NULL, "{{ value_json.cfg_warn }}", "stat", 0, false, DIAG,
     NULL, NULL},
};

const ha_entity_t *stats_json_entities(int *count) {
    *count = (int)(sizeof(ENTITIES) / sizeof(ENTITIES[0]));
    return ENTITIES;
}

/* One digit per chore key, and one ENTITIES row per possible chore. */
_Static_assert(CHORE_MAX >= 1 && CHORE_MAX <= 9, "chore_N keys are single-digit");
int stats_json_chore_index(const ha_entity_t *ent) {
    const char *k = ent->key;
    if (strncmp(k, "chore_", 6) != 0)
        return -1;
    if (k[6] < '1' || k[6] > '0' + CHORE_MAX || k[7] != '\0')
        return -1;
    return k[6] - '1';
}

/* One digit per slot key. */
_Static_assert(TIMER_EXTRA_SLOTS >= 1 && TIMER_EXTRA_SLOTS <= 9, "per-slot keys are single-digit");
int stats_json_slot_of(const ha_entity_t *ent, const char **suffix) {
    static const struct {
        const char *prefix, *suffix;
    } SLOT_ROWS[] = {
        {"completions_", "runs"},
        {"day_runs_", "runs per day"},
        {"remaining_", "remaining"},
        {"limit_", "limit"},
    };
    for (size_t i = 0; i < sizeof(SLOT_ROWS) / sizeof(SLOT_ROWS[0]); i++) {
        const size_t n = strlen(SLOT_ROWS[i].prefix);
        if (strncmp(ent->key, SLOT_ROWS[i].prefix, n) != 0)
            continue;
        const char *d = ent->key + n;
        if (d[0] < '1' || d[0] > '0' + TIMER_EXTRA_SLOTS || d[1] != '\0')
            return 0;
        *suffix = SLOT_ROWS[i].suffix;
        return d[0] - '0';
    }
    return 0;
}

stats_chore_disc_t stats_json_chore_discovery(const ha_entity_t *ent, const char names[][CHORE_NAME_BUF], int n,
                                              char *name_out, size_t name_len) {
    const int chore = stats_json_chore_index(ent);
    if (chore < 0)
        return STATS_CHORE_DISC_NOT_CHORE;
    if (n < 0)
        return STATS_CHORE_DISC_SKIP;
    /* Retire, don't skip: skipping would leave the previous chore's
       retained config in HA, forever unavailable — the disabled-timer-slot
       reasoning in mqtt_ha.c's publish_discovery(). */
    if (chore >= n)
        return STATS_CHORE_DISC_RETIRE;
    /* %.*s: the loader terminates every row, but the bound costs nothing
       and keeps a hand-built row from reading past its 21 bytes. */
    const char *nm = names[chore];
    if (nm[0] == '\0')
        snprintf(name_out, name_len, "%s", ent->name);
    else
        snprintf(name_out, name_len, "%.*s" STATS_JSON_CHORE_DONE_SUFFIX, CHORE_NAME_MAX, nm);
    return STATS_CHORE_DISC_PUBLISH;
}

int stats_json_discovery_topic(char *buf, size_t len, const char *dev_id, const ha_entity_t *ent) {
    return snprintf(buf, len, "homeassistant/%s/%s_%s/config", ent->component, dev_id, ent->key);
}

int stats_json_discovery_named(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                               const ha_entity_t *ent, const char *name_override) {
    char ename[64], dname[64];
    int pos = 0;
    /* def_ent_id carries "<component>.<uniq_id>", on purpose. Without it
       HA builds the entity_id from the device name and the entity name,
       so renaming a device silently re-slugs every entity under it —
       which is how a house automation that excluded "magtag" stopped
       matching devices renamed to "Testing Timer" and swept their
       switches off. dev_id is MAC-derived (device_id.h) and outlives any
       rename, so entity_ids built from it are stable by construction.

       The "<component>." prefix is mandatory, not cosmetic. HA reads
       default_entity_id as a FULL entity_id and keeps only what follows
       the FIRST dot; a value with no dot partitions to an EMPTY object
       id, which is worse than sending nothing at all. ent->component is
       the same string stats_json_discovery_topic() puts in the topic, so
       payload and topic name one platform by construction — they have to
       agree, or HA registers the entity under one and names it for
       another.

       Why def_ent_id and not obj_id: HA removed obj_id from MQTT
       discovery in 2026.4.0. def_ent_id has existed since 2025.10, and
       HA older than that drops the unknown key (the platform schemas are
       extra=REMOVE_EXTRA) and leaves entity_ids exactly as they are — so
       this fails safe rather than failing loudly. See DISC_SCHEMA_VER's
       note: it only takes effect at an entity's FIRST registration. */
    pos = jcat(buf, len, pos,
               "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"def_ent_id\":\"%s.%s_%s\","
               "\"stat_t\":\"magtag/%s/%s\",\"val_tpl\":\"%s\"",
               jesc(ename, sizeof(ename), name_override ? name_override : ent->name), dev_id, ent->key, ent->component,
               dev_id, ent->key, dev_id, ent->topic_suffix, ent->tpl);
    if (ent->unit != NULL)
        pos = jcat(buf, len, pos, ",\"unit_of_meas\":\"%s\"", ent->unit);
    if (ent->dev_class != NULL)
        pos = jcat(buf, len, pos, ",\"dev_cla\":\"%s\"", ent->dev_class);
    if (ent->state_class != NULL)
        pos = jcat(buf, len, pos, ",\"stat_cla\":\"%s\"", ent->state_class);
    if (ent->last_reset_tpl != NULL)
        pos = jcat(buf, len, pos, ",\"lrst_val_tpl\":\"%s\"", ent->last_reset_tpl);
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
