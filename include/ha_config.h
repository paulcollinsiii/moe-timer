#pragma once
#include <stddef.h>
#include <stdint.h>

#include "chores.h" /* CHORE_MAX / CHORE_NAME_BUF: the discovery fingerprint's chore rows */
#include "esp_compat.h"

/* Editable Home Assistant config entities. A single field registry drives
   three things — MQTT-discovery payloads (number/text/switch), the current-
   value state JSON (magtag/<id>/cfg), and per-field command apply
   (magtag/<id>/set/<key>). Over nvs_config + the shared validators, and —
   since BUG-8 — over the timer module's INSTALLED slot table: when the
   timer-defs blob cannot be read, the state JSON and the discovery
   fingerprint fall back to whatever timer_defs_install() put in RAM this
   boot, so this is not a pure function of NVS. The per-field timer writes
   deliberately do NOT use that fallback: when a table was stored but could
   not be parsed they NAK (err "nodefs"), because a table nobody read must
   not be overwritten. When NOTHING was ever stored they now create one from
   the compile-time table and stamp `defined` on the slot the edit names —
   an operator who drives the device only from the HA controls, and never
   publishes a `timers` document, otherwise NAKs forever. Host-tested. No
   cJSON — values arrive from MQTT as strings. */

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
    CFG_TBREAK,  /* extra-timer break_eligible (switch) */
    CFG_ENUM,    /* option string stored as u16 index (select) */
    /* Generic on/off setting (switch), stored as a u16 0/1 through a plain
       nvs_config accessor pair. CFG_TRELOAD/CFG_TBREAK are switches too,
       but they are slot-bound: each read-modify-writes one field of the
       timer_defs blob. CFG_BOOL is the slot-free form — same ON/OFF wire
       payloads and the same discovery block, but set_u16/get_u16 instead
       of the blob. Use it for any standalone boolean setting. */
    CFG_BOOL,
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
    /* CFG_STR: extra field-specific rule applied after the length and
       clean-character checks (e.g. the OTA URL's https-or-empty rule).
       Shared with config_apply.c's bulk-document path so the two cannot
       disagree about what a valid value is. NULL = length + cleanliness
       are the whole rule. */
    bool (*validate_str)(const char *);
    /* The chore gate's cross-field partner (design 5.3), set on the four
       chore_free_* rows ONLY: the key of the ALLOCATION that slice is
       carved out of. This one string IS the pairing table — a row that
       HAS an alloc_key is a free slice, a row NAMED by one is an
       allocation, and both of ha_config.c's pair lookups derive from it.
       See the layer-1/layer-2 note there for why the relation has to be
       declared and why it is a key rather than a set of accessors. */
    const char *alloc_key;
} cfg_field_t;

typedef enum { HA_CFG_OK = 0, HA_CFG_REJECTED, HA_CFG_UNKNOWN } ha_cfg_result_t;

/* Buffer size the firmware must give ha_config_state_json: the whole field
   registry serialized, plus headroom. Callers must check the return value
   against this before publishing — a truncated document is never
   published, which knocks every editable control offline, so the headroom
   is deliberate and test_state_json_worst_case_fits_firmware_buffer prints
   the live figure.

   The worst case is 1164 B, MEASURED by that test with every axis at its
   widest: 65535 on all fourteen u16/HHMM keys, 65535 on all four timer
   minutes, "OFF" on all nine switches, the longest option string on all
   three selects, and every string field maxed AND filled with characters
   the escaper doubles. (An earlier version of this comment said "~860 B",
   which was a hand estimate from before the fixture maxed every axis.)

   1280 left 116 B — under one more string field — so this is 1536, for
   372 B of headroom. It is part of the per-window heap struct in
   mqtt_ha.c, which static-asserts the total; growing it again is a
   deliberate edit there as well as here. */
#define HA_CONFIG_STATE_MAX 1536

/* Set-buffer slots the MQTT window must provide (mqtt_ha.c's SET_MAX).
   Every registry field can arrive as a retained set/<key> in a single
   window, plus mqtt_ha's own action entities (screen bonus, locate), plus
   margin for a field edited live during the window — that edit lands on
   top of its retained copy, and neither may be dropped. ha_config.c
   static-asserts the registry against this, so adding a field trips the
   build instead of silently dropping edits at the far end; the previous
   hand-maintained count had already gone stale twice. */
#define HA_CONFIG_SET_SLOTS 48

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

/* 16-bit fingerprint of the discovery `dev` block's mutable fields — the
   device name and the firmware version, the two things ha_config_discovery
   embeds that can change without a DISC_SCHEMA_VER bump. mqtt_ha.c stores
   it and republishes discovery when it differs, so a rename OR an OTA
   version change refreshes the HA device card. Leaving `fw` out was a live
   defect: a schema bump masks a stale version exactly once, and every
   later update leaves the card showing the old version permanently.
   NULL-safe (a NULL argument folds as an empty string). */
uint16_t ha_config_device_hash(const char *dev_name, const char *fw);

/* The chore list as ONE discovery window sees it: the rows and the count
   travel together, so the fingerprint and the discovery pass cannot be
   handed different counts. `n` is stats_json_chore_discovery()'s
   convention exactly — the configured count, 0 for none (a rejected blob
   included: the device runs on 0), or -1 when the read FAILED and the
   list is unknown. `names` always holds CHORE_MAX NUL-terminated rows;
   only the first min(n, CHORE_MAX) mean anything. */
typedef struct {
    char names[CHORE_MAX][CHORE_NAME_BUF];
    int n;
} ha_disc_chores_t;

/* The full discovery fingerprint mqtt_ha stores: the dev block (above)
   PLUS every extra-timer slot name, because those drive the published
   names of the per-timer stat entities and which of them exist, PLUS the
   chore list, because it drives the names of the chore_1..CHORE_MAX
   binary sensors and which of them exist. Reads the timer-defs blob, so
   it is host-tested over the mock NVS rather than pure. This is the value
   to compare and to store — ha_config_device_hash alone would leave a
   rename invisible until the next schema bump.

   The chore list is an ARGUMENT, not read here, and that asymmetry with
   the timer slots is deliberate: discovery must fingerprint the list it
   actually publishes, and the only way to guarantee that is one read per
   window handed to both — ha_config_discovery_gate() below does exactly
   that. NULL means "no chores" (n = 0). An unknown list (n < 0) folds a
   marker no readable list can produce at that position, so it cannot
   match a real list's fingerprint there; the final 16-bit value can
   still collide with a stored one by chance, and what is relied on to
   keep a failed read from being certified is the withheld stamp
   (ha_config_discovery_gate()'s `stamp`), not the marker. */
uint16_t ha_config_discovery_hash(const char *dev_name, const char *fw, const ha_disc_chores_t *chores);

/* The one read of the chore list that a discovery window makes: fills
   `out` (every row always written — all-empty on any failure) with the
   list and its `n`: the configured count when chore_store_load_names()
   answered authoritatively — the list, "never configured" (0) or a
   rejected blob (0, because the whole device runs on 0) — and -1 when the
   read itself failed and the list is unknown. Does not log: the loader
   already reports the failed read, and the gate reports what it costs. */
void ha_config_discovery_chores(ha_disc_chores_t *out);

/* The discovery-freshness predicate: discovery is republished when the
   stored schema version or the stored fingerprint disagrees with the
   current pair. Kept as a predicate because the wiring around it
   (compare, publish, then stamp only after a successful drain) is what
   actually went wrong before; ha_config_discovery_gate() is that wiring. */
bool ha_config_discovery_stale(uint16_t stored_ver, uint16_t stored_hash, uint16_t schema_ver, uint16_t dev_hash);

/* One window's discovery decision, everything mqtt_ha.c acts on. */
typedef struct {
    uint16_t hash; /* the fingerprint of what this window publishes: the value to stamp */
    bool stale;    /* run the discovery passes this window */
    bool stamp;    /* after a successful drain, write the schema version and `hash` */
} ha_disc_verdict_t;

/* The discovery gate, lifted out of mqtt_ha.c so every decision the
   stamp's safety rests on is host-tested: reads the chore list ONCE into
   `chores_out` (ha_config_discovery_chores()), fingerprints exactly that
   (ha_config_discovery_hash()), and judges it against the stored pair
   (ha_config_discovery_stale()). mqtt_ha.c hands `chores_out` to the
   discovery pass unchanged, so the pass publishes the list the hash
   certifies.

   `stamp` is `stale` AND the chore list known. On a failed read the pass
   skips every chore row (n = -1: leaves the owner's entities as they
   are), so a stamp would certify rows that were never published; it is
   withheld instead, the stored pair stays stale, and the next window
   runs the whole pass again. The stored pair is the caller's to read and
   write (NVS_KEY_DISC_VER / NVS_KEY_DISC_NAME), beside the drain the
   write waits on. */
ha_disc_verdict_t ha_config_discovery_gate(const char *dev_name, const char *fw, uint16_t stored_ver,
                                           uint16_t stored_hash, uint16_t schema_ver, ha_disc_chores_t *chores_out);

#ifdef __cplusplus
}
#endif
