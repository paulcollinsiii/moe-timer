#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
} stats_snapshot_t;

/* The OTA leg of the stat payload, and the reason it is a SEPARATE
   argument rather than four more stats_snapshot_t fields.

   stats_snapshot_t is filled by stats_collect() on the main task and
   posted to the network task BY VALUE (wake_flow_post_stats_snapshot);
   net_window_task has already copied it off the queue before it calls
   ota_flow_check(). An OTA field carried in the snapshot is therefore
   frozen BEFORE this wake's check runs, and could only ever publish the
   PREVIOUS wake's result — the ordering at net_window.c:103-105 buys
   nothing for it. Keeping these four out of the snapshot makes that
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
    const char *ent_cat;      /* "diagnostic" / NULL = primary (top-level in HA) */
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
   clash they are. */
#define STATS_JSON_DISC_SCHEMA_VER 20

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
   12 KB budget is what keeps that growth deliberate. */
#define STATS_JSON_PAYLOAD_MAX 1024

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
