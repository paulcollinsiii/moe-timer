#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chores.h"          /* CHORE_NAME_MAX / CHORE_NAME_BUF — the chore_N display names */
#include "config_validate.h" /* CFG_BOUND_OTA_* — the stored OTA widths */
#include "timer.h"           /* TIMER_EXTRA_SLOTS */

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
    /* Per-slot ([0] = Screen, [N] = extra timer N): HA tracks each timer's
       own history — active-timer scalars would mix timers into one series.
       Disabled slots report 0/0. */
    int32_t remaining_s[TIMER_SLOT_COUNT];
    uint32_t allocation_s[TIMER_SLOT_COUNT];
    const char *day_type;
    uint16_t completions[TIMER_EXTRA_SLOTS]; /* extra slots 1..N */
    bool charge_lock;
    const char *fw;
    int32_t screen_bonus_applied_s; /* slot 0 bonus_applied — the act publish
                                       must not read live timer state (the
                                       MQTT window runs on the network task) */
    /* Seconds left on a Screen Break, 0 = none. Its own field because a
       break runs on slot 0 behind whatever timer is selected, so "state"
       says BREAK only when Screen happens to be the selected one. */
    int32_t break_remaining_s;
    /* Screen-exposure balance: how much eye exposure has accrued toward
       the next break. Against the known break interval this answers "why
       didn't my break fire?" directly, and it is the only visibility the
       balance gets — the panel deliberately does not show it. Always
       >= 0 (app_state feeds it timer_run_accum's clamped read). */
    int32_t accum_s;
    const char *reset_reason; /* this boot's esp_reset_reason, e.g.
                                 "DEEPSLEEP"; anything else on a wake
                                 means the previous wake died — boot
                                 forensics over MQTT, because the USB
                                 CDC console drops output around
                                 sleep/reset transitions */
    /* ---- the chore checklist, read-only (design 1.4). The device is the
       sole authority on acks; HA only displays them. With no chores
       configured all three are 0 — the feature is inert (row C1). */
    uint8_t chores_left; /* configured chores not yet acked today */
    uint8_t chores_done; /* configured chores acked today */
    uint8_t chore_acked; /* bit i = chore i acked; the builder reports only
                            bits below chores_left + chores_done */
    /* M2-D6: bit (1u << d) = day type d's STORED chore_free exceeds its
       allocation (schedule_chore_free_broken_mask). 0 = every pair valid.

       It rides the snapshot rather than a publish-time argument like
       ota_stat_t, and the difference is real rather than convenient. Those
       fields change on the NETWORK task between the snapshot and the
       publish (the OTA check runs in between). This one changes only when
       a config document or a set/ lands, and mqtt_ha applies those AFTER
       this window's stat publish (apply_incoming follows publish_states),
       so a publish-time read would see exactly what the snapshot saw. It
       is recomputed from NVS on every wake that publishes — schedule.c's
       cache is wake-scoped RAM — so a fix that lands in one window clears
       the warning in the next, and nothing short of a fix clears it. The
       read has to stay on the main task anyway: the schedule cache it
       goes through is unsynchronised. */
    uint8_t chore_free_bad;
} stats_snapshot_t;

/* The OTA leg of the stat payload, and the reason it is a SEPARATE
   argument rather than four more stats_snapshot_t fields.

   stats_snapshot_t is filled by stats_collect() on the main task and
   posted to the network task BY VALUE (wake_flow_post_stats_snapshot);
   net_window_task has already copied it off the queue before it calls
   ota_flow_check(). An OTA field carried in the snapshot is therefore
   frozen BEFORE this wake's check runs, and could only ever publish the
   PREVIOUS wake's result — net_window_task's ordering, ota_flow_check()
   and then mqtt_ha_window() two statements later, buys nothing for it.
   (The ":103-105" that stood here points at that file's own comment
   block, not at the ordering.) Keeping these four out of the snapshot makes that
   mistake unrepresentable rather than merely discouraged. They are read
   from NVS at publish time by ota_flow_stat(); see docs/planning/
   ota.plan.md task 12, "A correction to this entry's own third bullet".

   dl_ms belongs here for a second reason on top of that one: the
   download runs in a window that closes before MQTT opens, and its
   success path ends in esp_restart(), so the value has to come off flash
   whatever else happens. It reports the LAST download, one wake later. */
typedef struct {
    char result[CFG_BOUND_OTA_RESULT_MAX]; /* ota_policy reason code, "" = none */
    char target[CFG_BOUND_OTA_TARGET_MAX]; /* version the retry budget counts against */
    uint16_t fails;                        /* consecutive failures for that target */
    uint32_t dl_ms;                        /* last download's wall time */
} ota_stat_t;

/* Width of the published panic-phase label: main + '+' + net + NUL.
   The longest the phase table can now produce is "BOOT_LOCK+OTA_CHECK"
   (19 + NUL), which is this number EXACTLY — the field is full. It was
   "RENDER+OTA_CHECK" (16 + NUL) until PANIC_PHASE_BOOT was subdivided,
   and the three characters that went is the whole budget a main-slot
   phase name has left: nine, because the widest net-slot label is
   "OTA_CHECK" (9) and 20 - 1 - 9 - 1 = 9.

   Two things hold that. panic_diag.c's _Static_assert now checks every
   row of the real phase table against that nine-character budget rather
   than comparing two hardcoded literals (the old form could not see a
   longer name being added at all), and
   test_every_phase_pair_fits_the_published_field
   walks the whole PANIC_PHASE__COUNT^2 cross-product through the actual
   label builder. A longer phase name fails both. Shorten the name or
   grow this number — and growing it costs a byte in every stat payload,
   which mqtt_ha.c's publish path has a measured budget for. */
#define DIAG_PHASE_MAX 20

/* The panic/health leg of the stat payload. A SEPARATE argument for
   exactly the reason ota_stat_t is one, and read at the same moment by
   the same code path (mqtt_ha.c's publish_states):

     - the panic fields come off NVS, which is the only place they can
       come from — the breadcrumb is latched from RTC memory at BOOT, on
       the main task, long before the snapshot for this window exists;

     - the LIVE fields (heap, stacks, NVS headroom) must be sampled on
       the NETWORK task at publish time. stack_net is a high-water mark
       and only means anything read from the task that owns that stack,
       and a heap figure taken back on the main task would predate wifi,
       TLS and the MQTT client — i.e. it would miss every allocation this
       feature exists to watch.

   Carrying either group in stats_snapshot_t would therefore publish the
   wrong wake's numbers, the same trap ota_stat_t was split out to make
   unrepresentable. */
typedef struct {
    /* ---- the last panic (sticky; survives until the next one) ---- */
    uint32_t panics;                  /* monotonic count; 0 = none ever */
    char panic_phase[DIAG_PHASE_MAX]; /* "RENDER+OTA_CHECK"; "" = no breadcrumb on file */
    uint32_t panic_uptime_s;          /* how far into that wake it died */
    uint32_t panic_heap;              /* free heap at the last mark before it died */
    uint16_t panic_stack_main;        /* main-side stack floor at that mark, bytes */
    uint16_t panic_stack_net;         /* network/OTA-side stack floor, bytes */
    /* ---- live, this window ---- */
    uint32_t heap_free;
    uint32_t heap_min; /* minimum ever free THIS BOOT (esp_get_minimum_free_heap_size) */
    uint16_t stack_main;
    uint16_t stack_net;
    uint16_t nvs_free; /* nvs_get_stats() free entries */
} diag_stat_t;

/* ota and diag may each be NULL, which publishes ""/0 — the same
   NULL-tolerance every string field here already has. */
int stats_json_stat(char *buf, size_t len, const stats_snapshot_t *s, const ota_stat_t *ota, const diag_stat_t *diag);
/* Inputs to the act payload — the Screen-adjust confirmation HA renders.
   The number entity draws its value from this topic and `optimistic` does
   not protect the box: HA's MQTT number subscribes to state_topic either
   way, so whatever is published here overwrites what the user typed.
   Which matters because the adjustment is applied AFTER the window that
   receives it (mqtt_ha buffers, net_apply_finish reconciles), so the
   snapshot's applied figure is a window stale on exactly the sync that
   acts on a fresh set. Precedence: a day about to reset reports 0, then a
   target that arrived this window, then the confirmed applied value. */
typedef struct {
    int32_t applied_s;   /* bonus_applied, from the pre-window snapshot */
    int32_t target_s;    /* target that arrived this window (if pending) */
    bool target_pending; /* a set/screen_bonus landed this window */
    bool day_cleared;    /* rollover: the day resets when this window closes */
} act_state_t;

int stats_json_act(char *buf, size_t len, const act_state_t *a);

/* The finished day, retained on magtag/<id>/summary:
   {"date":..,"screen_used_s":..,"completions":[..],"chores_done":N,"chores":M}.
   Fields only ever APPEND: the summary-topic rows' templates read them by
   name. chores_done is the day's acked chores among the `chores` that
   were configured when the rollover captured it (0 and 0 with no list);
   `chores` is carried so a reader can tell "none of 3 done" from "no
   list", and has no entity of its own.

   chores = STATS_JSON_CHORES_UNKNOWN (any negative) when the chore list
   could not be read at the rollover (chore_store_names_known() false):
   both fields are then OMITTED, chores_done ignored. The day's count is
   unknown, and day_chores' template renders "None" for a missing field,
   so HA records the day as unknown instead of a permanent 0. */
#define STATS_JSON_CHORES_UNKNOWN (-1)
int stats_json_summary(char *buf, size_t len, const char *date, int32_t screen_used_s,
                       const uint16_t completions[TIMER_EXTRA_SLOTS], unsigned chores_done, int chores);

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
    const char *topic_suffix; /* stat_t is magtag/<id>/<this>: "stat" or "summary" */
    int expire_after;         /* seconds; 0 = omit (value persists) */
    bool binary;              /* adds pl_on/pl_off */
    const char *ent_cat;      /* "diagnostic" / NULL = primary (top-level in HA) */
    /* "measurement" etc. / NULL = omit. HA keeps long-term statistics
       ONLY for a sensor that declares one — and leaves every such sensor
       out of the logbook, so a text or event-like sensor whose changes
       are the point must stay NULL. Added in v22 for the chore counts;
       v23 set it on the battery and the summary-topic rows. */
    const char *state_class;
    /* last_reset_value_template (lrst_val_tpl) / NULL = omit. HA accepts
       it ONLY with state_class "total" and rejects the whole discovery
       config otherwise. Added in v23 for the summary-topic rows: each
       summary is one finished day, so a new date is a new cycle. */
    const char *last_reset_tpl;
} ha_entity_t;

/* Home Assistant re-reads a discovery config only when something in it
   changes; a NEW entity does not appear at all until the retained
   discovery documents are republished, and mqtt_ha.c republishes them
   only when this number moves (ha_config_discovery_stale). So it lives
   HERE, next to the table it versions, rather than in mqtt_ha.c where
   adding a row to ENTITIES left it out of sight — and test_stats_json
   pins it against the row count, so adding an entity without bumping it
   fails a host test rather than a hardware smoke test.

   v18: + the four OTA entities (result/target/fails/dl_ms).
   v19: + the eleven panic/health diagnostics (panic count and
        breadcrumb, live heap and task stack floors, NVS headroom).
   v20: + def_ent_id on every discovery payload, so entity_ids derive
        from the MAC-based device id instead of the user-editable device
        name. Value is "<component>.<uniq_id>" — HA treats the field as a
        full entity_id and keeps only what follows the FIRST dot, so the
        component prefix is mandatory and a dotless value would register
        an EMPTY object id.

   FLOOR: HA >= 2025.10, when default_entity_id was added. Older HA drops
   the unknown key (the MQTT platform schemas are extra=REMOVE_EXTRA) and
   entity_ids simply stay as they are today, so the field costs nothing
   and breaks nothing below the floor.

   v20 first shipped carrying obj_id, which HA had already removed from
   MQTT discovery in 2026.4.0 and which every current HA therefore threw
   away unread. That v20 never reached a device, so the correction reuses
   the number instead of spending another retained-discovery burst.

   NOTE on v20, because it is the one bump that does NOT finish the job:
   def_ent_id seeds an entity_id only at the entity's FIRST registration.
   Republishing discovery over an already-registered entity updates
   everything else about it and leaves the entity_id alone — HA keys the
   registry on uniq_id and will not re-slug behind the user's back. So on
   a device HA already knows, this bump changes nothing visible until the
   MQTT device is deleted in HA once and allowed to re-register. Order
   matters: let this firmware publish the new retained discovery FIRST,
   then delete, or HA re-adds from the old retained payload and re-slugs
   from the name again.

   ON MAC COLLISIONS: device_id() is the last three MAC bytes, so two
   colliding devices already collided on uniq_id and this changes nothing
   about that. It changes the SYMPTOM: HA's async_generate_entity_id
   appends _2 to a taken entity_id, so the second device lands on
   ..._2 ids that read as a cosmetic naming quirk rather than as the MAC
   clash they are.

   v21: + the four chore_free_* number entities (the chore gate's free
        slice, one per day type). Those are ha_config.c REGISTRY rows, not
        rows in the ENTITIES table below, so the ENTITIES count does not
        move for this bump — but the republish gate is the same one
        (mqtt_ha.c compares this number for every discovery document it
        publishes, config entities included). The joint count+version pin
        for the config registry is in test_ha_config, beside the one for
        this table in test_stats_json.

        WHY BUMP, stated without the overstatement an earlier draft of
        this note carried: it is NOT true that a device which has
        published discovery once would "never republish" without it.
        ha_config_discovery_stale() ORs this version against
        ha_config_discovery_hash(dev_name, fw, ...), and that hash folds the
        firmware version string — so any release whose `fw` differs
        republishes all three discovery documents, bump or no bump
        (ha_config_discovery_gate(), called from mqtt_ha.c's
        publish_states()). What the bump covers is the case the hash
        cannot see: a SAME-VERSION reflash or an in-place image, where
        neither input moves and HA is never told the four controls exist.
        It is also the only explicit, reviewable signal that discovery
        changed. Costs the usual retained-discovery burst; the def_ent_id
        note above still applies, and these four register for the first
        time here so they get their MAC-derived entity_ids straight
        away.

   v22: + the chore checklist's read-only entities (design 1.4):
        chores_left, chores_done, and one chore_N binary_sensor per
        possible chore (CHORE_MAX rows; mqtt_ha.c names each from the list
        and retires the ones past the configured count, exactly as it does
        for a disabled timer slot). + config_warning, M2-D6's durable
        report of a broken chore_free pair on ANY day type. And the
        table's state_class column (ha_entity_t), set on chores_left and
        chores_done only, so HA keeps long-term statistics for them. One
        bump for all of it, as the plan's M3-T1 requires.

   v23: the dashboard's graph data (M4-T1). + the summary-topic rows, the
        first whose topic column is "summary", not "stat" (the column
        already fed stat_t, so no code moved): screen_used_day, the
        finished day's Screen minutes, and day_runs_1..4, the finished
        day's runs of each extra timer (named and retired with their slot,
        like completions_N; see stats_json_slot_of), and day_chores, the
        finished day's acked chores (M4-T5, added before v23 shipped, so
        the number was reused: the summary gained "chores_done" and
        "chores", appended after "completions", captured at the rollover
        before anything resets the acks). All six carry
        state_class "total" and the new last_reset_tpl column, which reads
        the summary's own date, so each summary starts a new cycle and the
        statistics `change` for a period is exactly what the summaries in
        it reported. A retained redelivery or an HA restart repeats the
        same value under the same last_reset and adds nothing; a day with
        no summary adds nothing. No expire_after: the summary arrives once
        a day. + state_class "measurement" on battery.
        completions_1..4 keep NO state_class. The live counters were the
        first draft's source for the runs graph, and they lose every run
        finished after the day's last window: the rollover zeroes them
        before HA sees the count. The summary carries those runs.
        chores_left/chores_done keep their v22 "measurement" but are not
        the chores graph: a daily `max` of chores_done counts the value
        carried across midnight, so a day with nothing done could show
        the previous day's full count. day_chores replaces it.
        WHY BUMP: a changed state_class is a changed discovery payload, and
        a new row is a new entity; neither reaches HA on a same-version
        reflash without it (the v21 note's reasoning). Costs one more
        retained discovery message per republish burst than v22 for each
        of the six new rows.
        THE LOGBOOK: HA keeps no logbook entries for a sensor with a
        state_class OR a unit. The battery (%) and screen_used_day (min)
        would be left out by their unit alone, so this bump costs the
        logbook only the day_runs_N and day_chores lines — and the owner
        chose the graphs over those. completions_N, the chore_N flags and
        every other row still log.
        THE FIRST SUMMARY: HA's statistics take the first value they ever
        see for a sensor as its zero point, not as a change. On the first
        registration after the OTA, that is the broker's existing retained
        summary (yesterday's, from older firmware), which is thus recorded
        once as a state and counted in no graph; the next day's summary is
        the first to count. On a device whose summary was never published,
        the six read "unknown" until one is, and that one is the zero
        point instead. day_chores has one more case: a summary without
        "chores_done" (retained by firmware older than M4-T5, or sent
        with the field omitted because the chore list could not be read)
        makes its template render "None", which HA's MQTT sensor takes as
        "no value": the state is set to "unknown", which the statistics
        skip. On a sensor that never had a value it stays "unknown" until
        the first summary carrying the field, which is then its zero
        point; on one that had a value (a later rollback, or a failed
        read) that day is recorded as unknown, not as 0 and not as the
        previous day's count repeated, and the next summary carrying the
        field counts normally. An empty render would have been worse: HA
        ignores an empty numeric state but still takes the new
        last_reset, so the old value would count again as a new day.
        THE DAY SHIFT: a day's summary is published at the first window
        after midnight, and HA stamps a state with its arrival time, so the
        per-day graphs file each day's figures under the NEXT day. The
        shift loses no run: a run finished after the day's last window is
        still in that summary. (A day whose summary is never published is
        lost, which is a different matter: the pending summary is plain
        RAM, see mqtt_ha.c's s_summary.) */
#define STATS_JSON_DISC_SCHEMA_VER 23

/* Buffer the stat/summary/discovery payloads are built into (mqtt_ha.c).
   Named here because stats_json_stat is what can outgrow it, and a stat
   payload that does is silently NOT PUBLISHED — publish_states drops any
   build whose needed length reaches the buffer size. Pinned against the
   worst case by test_stats_json.

   768 -> 1024 with the diagnostics leg: the measured worst case was
   already 705 of 768, i.e. 63 bytes of headroom, and eleven more fields
   do not fit in that. This buffer is inside window_mem_t, which is
   HEAP-allocated at window start and freed at teardown (mqtt_ha.c), so
   the 256 bytes are borrowed for the length of an MQTT window rather
   than parked in .bss — and the struct's own _Static_assert against its
   per-window heap budget (beside window_mem_t in mqtt_ha.c, which also
   carries the arithmetic) is what keeps that growth deliberate.

   1024 -> 1280 with the chore leg (v22): the measured worst case was 938
   of 1024, 85 bytes usable (the gate is needed < size), and the chore
   counts, the per-chore ack array and the config warning naming all four
   day types need about a hundred. Same heap, same lifetime, same assert
   as the step above; mqtt_ha.c carries the window_mem_t arithmetic.
   test_stat_payload_worst_case_fits_the_publish_buffer reports the
   current headroom rather than pinning a number here that would go
   stale. */
#define STATS_JSON_PAYLOAD_MAX 1280

const ha_entity_t *stats_json_entities(int *count);
int stats_json_discovery_topic(char *buf, size_t len, const char *dev_id, const ha_entity_t *ent);
int stats_json_discovery(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                         const ha_entity_t *ent);
/* Same, with a runtime display-name override (per-slot completion sensors). */
int stats_json_discovery_named(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                               const ha_entity_t *ent, const char *name_override);
/* Which chore a per-chore entity reports: 0..CHORE_MAX-1 for the
   chore_N binary sensors, -1 for every other row. mqtt_ha.c uses it to
   name the entity from the chore list and to retire the rows past the
   configured count. An exact match on the whole key, not a prefix test,
   so chores_left / chores_done can never be mistaken for a chore. */
int stats_json_chore_index(const ha_entity_t *ent);

/* Which extra timer slot a per-slot row reports: 1..TIMER_EXTRA_SLOTS for
   completions_N, day_runs_N, remaining_N and limit_N, 0 for every other
   row. On a match *suffix is the word mqtt_ha.c appends to the slot's
   timer name ("Violin runs", "Violin runs per day"), and mqtt_ha.c
   retires the row when the slot is disabled. The prefix must be followed
   by exactly one digit in range, so no other key can be read as a slot. */
int stats_json_slot_of(const ha_entity_t *ent, const char **suffix);

/* What mqtt_ha.c's discovery pass does with one ENTITIES row, as far as
   the chore list is concerned. The decision lives here, pure, so the host
   suite can pin it; the publishing stays in mqtt_ha.c. */
typedef enum {
    STATS_CHORE_DISC_NOT_CHORE = 0, /* not a chore_N row: the caller's usual path */
    STATS_CHORE_DISC_PUBLISH,       /* configured chore: publish, named name_out */
    STATS_CHORE_DISC_RETIRE,        /* past the configured count: empty retained config */
    STATS_CHORE_DISC_SKIP,          /* list UNKNOWN: touch nothing, and do not stamp the pass */
} stats_chore_disc_t;

/* "<chore name> done". The buffer a chore_N display name needs: the
   longest name the config path accepts (CHORE_NAME_MAX bytes, enforced in
   config_apply.c's apply_chores) plus the suffix and its NUL. A caller
   declares its buffer from this and asserts it at compile time. */
#define STATS_JSON_CHORE_DONE_SUFFIX " done"
#define STATS_JSON_CHORE_ENTITY_NAME_BUF (CHORE_NAME_MAX + sizeof(STATS_JSON_CHORE_DONE_SUFFIX))

/* `names`/`n` are chore_store_load_names()'s outputs, with n < 0 meaning
   the load was not authoritative (chore_store_names_known() false): the
   list is unknown, so every chore row is SKIPPED — neither renamed nor
   retired, because retiring on a failed read would delete the owner's
   entities over a transient error. Otherwise an index at or past n
   RETIRES, and one below it PUBLISHES named "<name> done" — or under the
   table's default name when the stored row is empty (the blob can hold
   "3 chores, the middle one empty"; the config path cannot write it), so
   an entity is never called just " done". `name_out` is written only for
   PUBLISH, and must hold STATS_JSON_CHORE_ENTITY_NAME_BUF bytes. */
stats_chore_disc_t stats_json_chore_discovery(const ha_entity_t *ent, const char names[][CHORE_NAME_BUF], int n,
                                              char *name_out, size_t name_len);

#ifdef __cplusplus
}
#endif
