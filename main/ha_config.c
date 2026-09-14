/* Editable HA config entities. Over nvs_config + the shared validators, and
   — since BUG-8 — over the timer module's INSTALLED slot table, which the
   timer-defs readers fall back to when the NVS blob cannot be read. Not
   pure: the state JSON and the discovery fingerprint depend on what
   timer_defs_install() put in RAM this boot. Host-tested. */
#include "ha_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bedtime.h"
#include "config_validate.h"
#include "nvs_config.h"
#include "quiet_hours.h"
#include "timer.h" /* timer_slot_def_raw: this boot's table; timer_defs_compiled: menuconfig */
#include "tones.h"

#ifndef NATIVE
#include "esp_log.h"
#include "nvs.h" /* ESP_ERR_NVS_NOT_FOUND — esp_compat.h only defines it on NATIVE */
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

/* File-scoped, and deliberately not plain `TAG`: timer_defs.c has its own,
   and since BUG-8 the two files are close enough that a single test TU
   compiles both (test_timer_defs). Two `static const char *TAG` at file
   scope in one translation unit is a redefinition. */
static const char *TAG_HA_CONFIG = "ha_config";

/* snprintf-append with truncation tracking; buffer stays NUL-terminated. */
static int jcat(char *buf, size_t len, int pos, const char *fmt, ...) {
    size_t off = ((size_t)pos < len) ? (size_t)pos : (len ? len - 1 : 0);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, len - off, fmt, ap);
    va_end(ap);
    return pos + n;
}

/* Escape a string for a JSON value (quotes/backslashes; NULL -> ""). */
const char *ha_config_json_escape(char *tmp, size_t tmplen, const char *s) {
    size_t o = 0;
    for (; s != NULL && *s != '\0' && o + 2 < tmplen; s++) {
        if (*s == '"' || *s == '\\')
            tmp[o++] = '\\';
        tmp[o++] = ((unsigned char)*s < 0x20) ? ' ' : *s;
    }
    tmp[o] = '\0';
    return tmp;
}
#define jesc ha_config_json_escape

uint16_t ha_config_device_hash(const char *dev_name, const char *fw) {
    /* djb2 over both fields, with a separator between them so a rename
       cannot cancel out a version change ("ab"+"c" != "a"+"bc"). The
       separator is a NUL, which neither field can contain. */
    uint16_t h = 5381;
    const char *parts[2] = {dev_name, fw};
    for (int i = 0; i < 2; i++) {
        for (const char *p = parts[i]; p != NULL && *p != '\0'; p++)
            h = (uint16_t)(h * 33u + (unsigned char)*p);
        h = (uint16_t)(h * 33u); /* field separator */
    }
    return h;
}

/* ---- field registry (Phase A: scalar settings) ---- */

/* Designated initializers so adding/reordering fields can't silently
   misalign the function pointers. */
#define NUM_U16(k, nm, un, lo_, hi_, st, set, get)                                                                  \
    {                                                                                                               \
        .key = k, .component = "number", .name = nm, .unit = un, .kind = CFG_U16, .lo = lo_, .hi = hi_, .step = st, \
        .set_u16 = set, .get_u16 = get                                                                              \
    }
/* A chore-gate free slice: a bounded u16 that also NAMES the allocation it
   is carved out of. Bounds are not parameters — both come from the shared
   CFG_BOUND_CHORE_FREE_*, so discovery advertises exactly what
   ha_config_set accepts, and all four rows are identical but for the key,
   the label and the partner. */
#define NUM_CHORE_FREE(k, nm, alloc, set, get)                                                                      \
    {                                                                                                               \
        .key = k, .component = "number", .name = nm, .unit = "min", .kind = CFG_U16, .lo = CFG_BOUND_CHORE_FREE_LO, \
        .hi = CFG_BOUND_CHORE_FREE_HI, .step = 1, .set_u16 = set, .get_u16 = get, .alloc_key = alloc                \
    }
#define NUM_HHMM(k, nm, set, get)                                                                                      \
    {                                                                                                                  \
        .key = k, .component = "number", .name = nm, .kind = CFG_HHMM, .lo = 0, .hi = 2359, .step = 1, .set_u16 = set, \
        .get_u16 = get                                                                                                 \
    }
/* HHMM with a field-specific validity rule (advertised bounds stay
   0-2359; the device is the gatekeeper and the cfg republish corrects
   an optimistic HA edit that was rejected). */
#define NUM_HHMM_V(k, nm, set, get, val)                                                                               \
    {                                                                                                                  \
        .key = k, .component = "number", .name = nm, .kind = CFG_HHMM, .lo = 0, .hi = 2359, .step = 1, .set_u16 = set, \
        .get_u16 = get, .validate = val                                                                                \
    }
#define TEXT(k, nm, maxlen, set, get) \
    { .key = k, .component = "text", .name = nm, .kind = CFG_STR, .hi = maxlen, .set_str = set, .get_str = get }
/* Text with a field-specific content rule on top of length + cleanliness;
   the same predicate config_apply.c uses, so the per-entity and bulk
   paths accept exactly the same values. */
#define TEXT_V(k, nm, maxlen, set, get, val)                                                                      \
    {                                                                                                             \
        .key = k, .component = "text", .name = nm, .kind = CFG_STR, .hi = maxlen, .set_str = set, .get_str = get, \
        .validate_str = val                                                                                       \
    }
/* Standalone on/off setting: a switch backed by a u16 0/1 accessor pair
   (no timer slot). See CFG_BOOL in ha_config.h. */
#define BOOL_SWITCH(k, nm, set, get) \
    { .key = k, .component = "switch", .name = nm, .kind = CFG_BOOL, .set_u16 = set, .get_u16 = get }
/* Extra-timer slot fields — read-modify-write the timer_defs blob by slot. */
/* Derived from the blob field, not written out: `hi` is what discovery
   advertises to HA *and* what ha_config_set rejects on, so the two can
   never drift apart. */
#define CFG_TIMER_NAME_CAP ((int)sizeof(((nvs_timer_defs_blob_t *)0)->defs[0].name))
#define TIMER_NAME(n)                                                                                   \
    {                                                                                                   \
        .key = "timer" #n "_name", .component = "text", .name = "Timer " #n " name", .kind = CFG_TNAME, \
        .hi = CFG_TIMER_NAME_CAP, .slot = n                                                             \
    }
#define TIMER_MIN(n)                                                                                       \
    {                                                                                                      \
        .key = "timer" #n "_min", .component = "number", .name = "Timer " #n " minutes", .unit = "min",    \
        .kind = CFG_TMIN, .lo = CFG_BOUND_TIMER_MIN_LO, .hi = CFG_BOUND_TIMER_MIN_HI, .step = 1, .slot = n \
    }
#define TIMER_RELOAD(n)                                                                                             \
    {                                                                                                               \
        .key = "timer" #n "_reload", .component = "switch", .name = "Timer " #n " reloadable", .kind = CFG_TRELOAD, \
        .slot = n                                                                                                   \
    }
/* "Break eligible": may be started during a Screen Break, and drains the
   exposure balance instead of feeding it. Key stays clear of the
   remaining_/limit_/completions_ prefixes mqtt_ha.c matches on. */
#define TIMER_BREAK(n)                                                                                                \
    {                                                                                                                 \
        .key = "timer" #n "_break", .component = "switch", .name = "Timer " #n " break eligible", .kind = CFG_TBREAK, \
        .slot = n                                                                                                     \
    }
/* Alert-tone selects: option string in HA, stored as its u16 index. */
#define TONE_SELECT(k, nm, set, get)                                                                   \
    {                                                                                                  \
        .key = k, .component = "select", .name = nm, .kind = CFG_ENUM, .set_u16 = set, .get_u16 = get, \
        .options = tones_names, .n_options = TONE_COUNT                                                \
    }

/* The registry hardcodes extra-timer slots 1..4; if TIMER_EXTRA_SLOTS ever
   shrinks, defs[slot-1] in the state builder would read out of bounds. */
_Static_assert(TIMER_EXTRA_SLOTS >= 4, "ha_config registry assumes >= 4 extra-timer slots");
/* ha_config_state_json's render frame is raw[CFG_STR_MAX] plus
   esc[2 * CFG_STR_MAX], and it runs on the net_win task (10 KB stack,
   net_window.c) with the MQTT client and TLS beneath it. CFG_STR_MAX
   therefore controls a stack frame, not just a buffer size, so the frame
   gets a stated ceiling of its own: raising CFG_STR_MAX to 512 would take
   this from 384 B to 1.5 KB, and nothing else in the build would notice. */
#define STATE_JSON_SCRATCH (3 * CFG_STR_MAX)
_Static_assert(STATE_JSON_SCRATCH <= 1024, "state_json scratch must stay a small fraction of the net_win stack");

/* The per-bound "<= CFG_STR_MAX" asserts live in config_validate.h, next
   to the bounds themselves, so a new TEXT() field is checked wherever it
   is declared rather than against a conjunction maintained by hand here
   (which a fifth string field would have passed while overrunning raw[]).
   Slot count is asserted against the window's set buffer below. */

static const cfg_field_t FIELDS[] = {
    /* step=1: HA validates entries against min+k*step, so a step of 5 with
       a min of 1 rejects round values (30, 45, 60). Keep it 1. */
    NUM_U16("weekday_min", "Weekday allocation", "min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, 1,
            nvs_config_set_weekday_min, nvs_config_get_weekday_min),
    NUM_U16("weekend_min", "Weekend allocation", "min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, 1,
            nvs_config_set_weekend_min, nvs_config_get_weekend_min),
    NUM_U16("holiday_min", "Holiday allocation", "min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, 1,
            nvs_config_set_holiday_min, nvs_config_get_holiday_min),
    NUM_U16("summer_min", "Summer allocation", "min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, 1,
            nvs_config_set_summer_min, nvs_config_get_summer_min),
    /* The chore gate's free slice, one per day type, each NAMING the
       allocation directly above it that it is carved out of — that third
       argument is the whole pairing, read from both ends by the two
       lookups below. LO is 0, not the allocations' 1 (config_validate.h
       says why: 0 is the default every deployed device is running, and an
       advertised min of 1 would make it unselectable in HA), and HI must
       EQUAL the allocation HI or `chore_free == allocation` — the
       per-day-type off switch — becomes unreachable for the tallest
       allocations. */
    NUM_CHORE_FREE("chore_free_wd", "Weekday chore-free", "weekday_min", nvs_config_set_chore_free_wd,
                   nvs_config_get_chore_free_wd),
    NUM_CHORE_FREE("chore_free_we", "Weekend chore-free", "weekend_min", nvs_config_set_chore_free_we,
                   nvs_config_get_chore_free_we),
    NUM_CHORE_FREE("chore_free_hol", "Holiday chore-free", "holiday_min", nvs_config_set_chore_free_hol,
                   nvs_config_get_chore_free_hol),
    NUM_CHORE_FREE("chore_free_sum", "Summer chore-free", "summer_min", nvs_config_set_chore_free_sum,
                   nvs_config_get_chore_free_sum),
    NUM_HHMM("quiet_start", "Quiet hours start (HHMM)", nvs_config_set_quiet_start, nvs_config_get_quiet_start),
    NUM_HHMM("quiet_end", "Quiet hours end (HHMM)", nvs_config_set_quiet_end, nvs_config_get_quiet_end),
    NUM_HHMM_V("bedtime", "Bed time (HHMM, 0=off)", nvs_config_set_bedtime, nvs_config_get_bedtime, bedtime_hhmm_valid),
    NUM_U16("break_interval_min", "Break interval", "min", CFG_BOUND_BREAK_INT_LO, CFG_BOUND_BREAK_INT_HI, 1,
            nvs_config_set_break_interval_min, nvs_config_get_break_interval_min),
    NUM_U16("break_duration_min", "Break duration", "min", CFG_BOUND_BREAK_DUR_LO, CFG_BOUND_BREAK_DUR_HI, 1,
            nvs_config_set_break_duration_min, nvs_config_get_break_duration_min),
    TEXT("name", "Device name", CFG_BOUND_NAME_MAX, nvs_config_set_dev_name, nvs_config_get_dev_name),
    TEXT("tz", "Timezone", CFG_BOUND_TZ_MAX, nvs_config_set_tz, nvs_config_get_tz),
    TIMER_NAME(1),
    TIMER_MIN(1),
    TIMER_RELOAD(1), /* extra-timer slots 1..4 */
    TIMER_BREAK(1),
    TIMER_NAME(2),
    TIMER_MIN(2),
    TIMER_RELOAD(2),
    TIMER_BREAK(2),
    TIMER_NAME(3),
    TIMER_MIN(3),
    TIMER_RELOAD(3),
    TIMER_BREAK(3),
    TIMER_NAME(4),
    TIMER_MIN(4),
    TIMER_RELOAD(4),
    TIMER_BREAK(4),
    TONE_SELECT("tone_expiry", "Expiry tone", nvs_config_set_tone_expiry, nvs_config_get_tone_expiry),
    TONE_SELECT("tone_break", "Break tone", nvs_config_set_tone_break, nvs_config_get_tone_break),
    TONE_SELECT("tone_bed", "Bed time tone", nvs_config_set_tone_bed, nvs_config_get_tone_bed),
    /* >100% applies clipping gain in the renderer — louder, harsher. */
    NUM_U16("alert_volume", "Alert volume", "%", 0, TONES_VOLUME_MAX, 1, nvs_config_set_alert_volume,
            nvs_config_get_alert_volume),
    /* OTA. Empty URL = updates disabled, which is why config_is_ota_url
       accepts "" — it is the only off switch HA has for the endpoint.
       Both fields are also parsed from the bulk config document
       (config_apply.c) and documented in docs/home_assistant.md; all
       three are required or an applied document silently clears them. */
    TEXT_V("ota_url", "OTA manifest URL", CFG_BOUND_OTA_URL_MAX, nvs_config_set_ota_url, nvs_config_get_ota_url,
           config_is_ota_url),
    BOOL_SWITCH("ota_on_sync", "OTA check on sync", nvs_config_set_ota_on_sync, nvs_config_get_ota_on_sync),
};

/* The window's set buffer has to hold every field at once (all of them can
   be sitting retained on their set/<key> topics), plus mqtt_ha's two action
   entities, plus room for a live edit arriving on top of a retained copy.
   Asserted here because this is the only place the real field count is
   known at compile time. */
_Static_assert((int)(sizeof(FIELDS) / sizeof(FIELDS[0])) + 2 <= HA_CONFIG_SET_SLOTS,
               "HA_CONFIG_SET_SLOTS must cover every registry field plus the action entities");

const cfg_field_t *ha_config_fields(int *count) {
    *count = (int)(sizeof(FIELDS) / sizeof(FIELDS[0]));
    return FIELDS;
}

static const cfg_field_t *find_field(const char *key) {
    if (key == NULL)
        return NULL;
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++)
        if (strcmp(FIELDS[i].key, key) == 0)
            return &FIELDS[i];
    return NULL;
}

/* ---- the chore gate's cross-field rule (design 5.3, layers 1 and 2) ----

   NO PAIRING TABLE. The pairing is ONE string per pair — cfg_field_t's
   alloc_key, set on the four chore_free_* rows and nowhere else — and both
   lookups below derive from it through the registry.

   There used to be a second table here, a row per pair holding
   {free_key, alloc_key, set_free, get_free, get_alloc}, and every one of
   those five members was bit-identical to something already in FIELDS. It
   was therefore five chances per pair to name the wrong partner, silent in
   a build that still compiles: a review ran all twelve single-pointer
   mispairings and found two of them surviving the entire host suite, one
   of which stored `holiday_min: 100` next to `chore_free_hol: 600` under
   ok:true. Derivation collapses that surface from twelve pointers to four
   strings, and a wrong string cannot desynchronise the two layers — they
   read the SAME string from opposite ends, so they always agree about who
   is paired with whom.

   Why the pairing has to be declared at all: the two halves of a pair are
   different registry rows and the relation is not in the keys
   (`chore_free_wd` and `weekday_min` share no stem). And a mistake is
   silent at runtime as well as at compile time — summer clamped against
   the weekend allocation stores a plausible number and nothing downstream
   complains, because schedule_get_chore_free_sec() clamps and an invalid
   stored pair reads back identically to the legitimate off switch.

   TWO LOOKUPS, one per direction, and neither substitutes for the other:
   the two ends of a pair do OPPOSITE things with a false from the shared
   predicate (the slice is refused; the allocation clamps its slice), so
   the setter has to know which end it is standing on. That is what
   alloc_key encodes — a row that HAS one is a slice, a row NAMED by one is
   an allocation. See the DO NOT UNIFY note in config_validate.h: the bulk
   applier is a third consumer with a third behaviour, and unifying the
   three would remove design rows C11 and C12's subject matter.
   config_apply.c keeps its own pairing table for that path and is
   deliberately left alone — with this table gone it is the only other
   copy in the FIRMWARE. test_ha_config's PAIR_REFS is a third writing of
   the pairing on purpose: an independent list is the only thing that can
   disagree with this one, so it is the check, not a duplicate.

   A pair's two keys must stay real registry rows: a rename on either side
   makes a lookup miss and BOTH layers quietly become no-ops on a build
   that still compiles. strcmp rules out a static assert, so the guard is
   in test_ha_config — every non-NULL alloc_key must resolve to a real
   CFG_U16 row, over a pairing written out BY HAND there, since a table
   borrowed from here would agree with its own mispairing. */
static const cfg_field_t *chore_alloc_of(const cfg_field_t *slice) {
    return (slice->alloc_key != NULL) ? find_field(slice->alloc_key) : NULL;
}

/* The same string from the other end: the slice carved out of THIS
   allocation, or NULL if no row names it. */
static const cfg_field_t *chore_slice_of(const cfg_field_t *alloc) {
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++)
        if (FIELDS[i].alloc_key != NULL && strcmp(FIELDS[i].alloc_key, alloc->key) == 0)
            return &FIELDS[i];
    return NULL;
}

/* Parse a whole-string integer; false if empty or trailing junk. */
static bool parse_int(const char *s, long *out) {
    if (s == NULL || *s == '\0')
        return false;
    char *end;
    *out = strtol(s, &end, 10);
    return *end == '\0';
}

/* Switch payloads are the pl_on/pl_off strings discovery advertises, so
   the match is exact and case-sensitive — shared by every switch kind
   (CFG_BOOL and the two slot-bound ones) to keep them identical. */
static bool parse_onoff(const char *s, uint8_t *out) {
    if (s == NULL)
        return false;
    if (strcmp(s, "ON") == 0) {
        *out = 1;
        return true;
    }
    if (strcmp(s, "OFF") == 0) {
        *out = 0;
        return true;
    }
    return false;
}

/* The "would this value corrupt the discovery/state JSON (unescaped
   quote/backslash) or the MQTT/HA layer (control chars)" rule lives in
   config_validate.c: the bulk-document path needs the same one for its
   `ver` string, and two copies would drift. Called by its real name
   below rather than through a local alias. */

static ha_cfg_result_t reject(char *ack, size_t len, const char *key, const char *err) {
    snprintf(ack, len, "{\"key\":\"%s\",\"ok\":false,\"err\":\"%s\"}", key, err);
    return HA_CFG_REJECTED;
}

/* See the ESP_LOGW in load_defs(): one warning per boot, not one per call. */
static bool s_defs_fallback_warned;

/* Load the timer-defs blob, falling back to a reconstruction when it cannot
   be read. RETURNS ESP_OK ONLY WHEN THE BLOB WAS READ FROM FLASH — any
   other value means "this is a reconstruction, not stored state", and it
   says WHICH failure, because the write path treats two of them
   differently.

   That return value is the whole point. The fallback used to be a ZEROED
   blob, and the read-modify-write cases in ha_config_set persist whatever
   this returns, so one unreadable read while the operator edited a single
   field wiped every OTHER slot's name, minutes and flags. Replacing the
   zeros with the installed table (BUG-8) fixed the wipe and replaced it
   with a subtler failure of the same shape: the RMW then persisted a table
   nobody had chosen — the compile-time defaults for every slot the edit did
   not touch, and, worse, a BOOT-TIME SNAPSHOT that silently reverts an edit
   made earlier in this same window (the blob is written here but the RAM
   table is only re-installed after the window, in net_apply's
   reconcile_defs). A device is not allowed to write a table it did not
   read.

   THE NAK IS NOW NARROWER, and the narrowing is the point of the `defined`
   bit. It used to fire on any read failure, including ESP_ERR_NVS_NOT_FOUND
   — "no table has ever been written" — which on a device driven ONLY by the
   HA per-timer controls is the permanent state. That operator never
   publishes a `timers` document, so nothing ever creates the table, so
   every control edit NAKs forever, and (because nothing publishes the ack,
   see mqtt_ha.c) with no diagnostic in HA at all. The self-healing story
   above is true only for an operator who has a retained document.

   So the two failures are now told apart, which is also why they get
   separate log lines:

     ESP_ERR_NVS_NOT_FOUND  nothing is there. The edit PROCEEDS: it writes
                            the compile-time table with the operator's field
                            applied, and stamps `defined` on that one slot.
                            Nothing is overwritten, because nothing existed.
     anything else          bytes ARE there and could not be parsed
                            (version drift, a bad read). Still NAKs. A
                            device is not allowed to write a table it did
                            not read, and here the recovery is config_apply,
                            which since BUG-5 rebuilds an unreadable table
                            even when the document's `ver` matches.

   The two READ-ONLY callers are the reason the fallback exists at all and
   they still use it. Since BUG-8, timer_defs_install() no longer
   materializes the blob at boot, so "unreadable" is the ordinary state of a
   device whose NVS was erased; zeros there would publish blank names into
   HA's text controls and produce a discovery fingerprint that disagrees
   with the per-timer entities mqtt_ha.c builds from the same installed
   table — a full retained-discovery republish on the window the read fails
   and another undoing it on the window it succeeds.

   Hence `seed`, and the two answers are NOT interchangeable:

   DEFS_SEED_INSTALLED (readers) — timer_slot_def_raw(), the table this boot
   is running. It has to be that table, or the fingerprint disagrees with
   the entities mqtt_ha.c builds from it and the republish above happens.
   timer_slot_def_raw() rather than timer_slot_def() because the filtered
   accessor hides a slot that has a NAME but no duration yet, which is
   exactly what the documented two-edit flow produces (set timer4_name in
   one window, timer4_min in a later one). Hiding it made the fallback
   publish an empty name over the operator's and fingerprint differently
   from the readable blob for identical device state. The enabled/disabled
   distinction still lives in the `min` field, which is where the blob keeps
   it too.

   DEFS_SEED_COMPILED (the write path) — timer_defs_compiled(), menuconfig.
   Correct by construction: what gets PERSISTED must be a value with a
   provenance, and the installed table has none of its own (it is either a
   copy of the blob or a copy of menuconfig, and which one is a fact about
   an earlier moment in this boot, not about the operator). */
typedef enum {
    DEFS_SEED_INSTALLED, /* the table this boot is running — read-only callers */
    DEFS_SEED_COMPILED,  /* menuconfig — the only seed safe to persist */
} defs_seed_t;

static esp_err_t load_defs(nvs_timer_defs_blob_t *b, defs_seed_t seed) {
    esp_err_t err = nvs_config_get_timer_defs(b);
    if (err == ESP_OK) {
        s_defs_fallback_warned = false; /* a later loss warns about it again */
        return ESP_OK;
    }
    memset(b, 0, sizeof(*b));
    b->version = TIMER_DEFS_BLOB_VERSION;
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        const timer_def_t *d = (seed == DEFS_SEED_COMPILED) ? timer_defs_compiled(i + 1) : timer_slot_def_raw(i + 1);
        if (d == NULL || d->name == NULL)
            continue;
        snprintf(b->defs[i].name, sizeof(b->defs[i].name), "%s", d->name);
        b->defs[i].min = d->duration_sec / 60;
        b->defs[i].reload = d->reloadable ? 1 : 0;
        b->defs[i].break_eligible = d->break_eligible ? 1 : 0;
        /* `defined` stays 0: nothing here was chosen by anybody. */
    }
    /* Once per boot: both read-only callers run every network window, so an
       unconditional warn is several log lines per window on a device that
       legitimately has no blob. Statics do not survive deep sleep, so "per
       boot" is "per wake".

       Two messages, not one. "Never written" is the expected state of a
       fresh or erased device and costs nothing; "written but unreadable" is
       a data-loss event that also disables every per-timer control until a
       document rebuilds the table. Sharing one line made the second
       invisible inside the noise of the first. */
    if (!s_defs_fallback_warned) {
        if (err == ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(TAG_HA_CONFIG,
                     "no timer-defs blob stored; using the fallback table (a control edit will create one)");
        else
            ESP_LOGW(TAG_HA_CONFIG,
                     "timer-defs blob present but UNREADABLE (%s): reads use the installed table, control edits will "
                     "NAK until a config document rebuilds it",
                     esp_err_to_name(err));
        s_defs_fallback_warned = true;
    }
    return err;
}

/* The write path's form. True = `b` may be persisted, either because it
   came out of flash or because there was nothing in flash to lose. */
static bool load_defs_for_write(nvs_timer_defs_blob_t *b) {
    esp_err_t err = load_defs(b, DEFS_SEED_COMPILED);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

/* Record that an authority set this slot. Only the slot the edit names:
   every other slot keeps `defined` as it was (0 in a freshly seeded table).

   That bit alone does NOT hand those slots back to the menuconfig rung, and
   an earlier version of this comment claimed it did. apply_timers()'
   `existed` test is `defined || name[0] != '\0'` (config_apply.c) — an OR,
   not the bit alone — and the seed loop above fills a non-empty name for
   every slot menuconfig NAMES. So on the common path (a control edit on a
   device with no blob, whose sdkconfig names its timers) the untouched
   named slots read as `existed`, and tier 2 — the value just stored —
   answers for them, not tier 3. Only a slot menuconfig leaves unnamed goes
   to the rung. Harmless at the instant of the write, since the seed came
   from timer_defs_compiled() and the two tiers hold the same value; the
   consequence is that the first control edit provisions the WHOLE table,
   and the bystander slots stop tracking a later menuconfig change. Stated
   at length in config_apply.c's `existed` comment; do not re-derive it.

   The semantics this creates, stated because they are a real trade:
   stamping a slot means THE OPERATOR ADOPTED IT. That slot's other fields
   freeze at whatever they were at that moment — for a slot seeded from
   menuconfig, at those menuconfig values — and stop following menuconfig
   across firmware updates, exactly as a document-provisioned slot does.
   Intended. Per-slot granularity was chosen over per-field. */
static void mark_defined(nvs_timer_defs_blob_t *b, int slot) {
    b->defs[slot - 1].defined = 1;
}

uint16_t ha_config_discovery_hash(const char *dev_name, const char *fw) {
    /* Start from the dev block, then fold every extra-timer slot name.
       Those names are the second mutable input to discovery: mqtt_ha's
       per-timer stat entities (`<Name> remaining` / `<Name> limit` /
       `<Name> runs`) take their published names from timerN_name, and
       which of them exist at all depends on whether the slot is enabled.
       Neither is covered by DISC_SCHEMA_VER, so without this a rename in
       HA left the old entity names in place until someone bumped the
       schema — the same defect class as the stale `sw` field.

       The name alone is NOT enough, because enablement is not the name.
       timer.c gates a slot on name[0] != 0 AND duration > 0, while
       ha_config_set's CFG_TNAME case writes only the name — so the normal
       two-edit flow (set timer4_name in one window, timer4_min in a
       later one) flipped the slot to enabled without moving a
       name-only fold, and the three per-slot entities never appeared.
       Each slot therefore contributes its name AND an enabled bit.

       The bit rather than the raw duration, deliberately: discovery
       depends on whether the entities exist and what they are called,
       not on how long the timer runs. Folding the minutes would force a
       full discovery republish — a wasted radio burst on a battery
       device — every time someone nudged 20 minutes to 30. */
    uint16_t h = ha_config_device_hash(dev_name, fw);
    nvs_timer_defs_blob_t defs;
    (void)load_defs(&defs, DEFS_SEED_INSTALLED); /* read-only: the reconstruction is the right answer here */
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        const char *name = defs.defs[i].name;
        /* Bounded: the blob comes from flash and nvs_config_get_timer_defs
           validates only size and version, so an unterminated name would
           otherwise read on into the next slot. */
        size_t n = strnlen(name, sizeof(defs.defs[i].name));
        for (size_t j = 0; j < n; j++)
            h = (uint16_t)(h * 33u + (unsigned char)name[j]);
        h = (uint16_t)(h * 33u); /* name/enabled separator */
        /* Mirrors timer.c's enablement predicate (name AND duration). */
        h = (uint16_t)(h * 33u + ((n > 0 && defs.defs[i].min > 0) ? 1u : 0u));
        h = (uint16_t)(h * 33u); /* slot separator, as in device_hash */
    }
    return h;
}

bool ha_config_discovery_stale(uint16_t stored_ver, uint16_t stored_hash, uint16_t schema_ver, uint16_t dev_hash) {
    return (stored_ver != schema_ver) || (stored_hash != dev_hash);
}

ha_cfg_result_t ha_config_set(const char *key, const char *value, char *ack, size_t ack_len) {
    const cfg_field_t *f = find_field(key);
    if (f == NULL) {
        snprintf(ack, ack_len, "{\"key\":\"%s\",\"ok\":false,\"err\":\"unknown\"}", key ? key : "");
        return HA_CFG_UNKNOWN;
    }
    /* Extra-timer slots index the blob by (slot-1); guard against a
       registry that outgrew TIMER_EXTRA_SLOTS. */
    if ((f->kind == CFG_TNAME || f->kind == CFG_TMIN || f->kind == CFG_TRELOAD || f->kind == CFG_TBREAK) &&
        (f->slot < 1 || f->slot > TIMER_EXTRA_SLOTS))
        return reject(ack, ack_len, key, "slot");
    switch (f->kind) {
        case CFG_U16: {
            long v;
            if (!parse_int(value, &v))
                return reject(ack, ack_len, key, "nan");
            if (v < f->lo || v > f->hi)
                return reject(ack, ack_len, key, "range");
            /* LAYER 1 — this field IS a chore-free slice (it names an
               allocation): refuse a slice bigger than the day it comes out
               of. A distinct err from "range" because the value is
               perfectly in range and the operator needs to know it was the
               OTHER field that made it impossible. Nothing is written, so
               neither half moves.

               A refusal LEAVES THE SET RETAINED. mqtt_ha.c clears
               set/<key> only on HA_CFG_OK, so a retained
               `set/chore_free_wd: 120` refused here is re-delivered and
               re-refused every window, indefinitely, until the operator
               raises the allocation or clears the topic. That is the
               intended trade — a rejected value stays visible rather than
               vanishing — but it is a standing per-window cost on a
               battery device; M3 owns whether a repeatedly-refused
               retained set should be cleared. */
            const cfg_field_t *alloc = chore_alloc_of(f);
            if (alloc != NULL) {
                uint16_t alloc_min = 0;
                /* Return code deliberately unchecked, as in
                   config_apply.c's check_chore_free_pairs(): every
                   nvs_config getter fills *out on every path
                   (get_u16_with_default, nvs_config.c), so an unreadable
                   key reads as its COMPILE-TIME DEFAULT rather than as
                   garbage.

                   THAT IS THE MECHANISM, NOT A FAIL-SAFE ARGUMENT, and an
                   earlier version of this comment claimed a read fault
                   "fails towards refusing". It does not. The candidate is
                   judged against NVS_DEFAULT_*_MIN instead of the stored
                   allocation, so the direction depends on which side of
                   the default the stored allocation sits: ABOVE it a legal
                   slice is falsely refused, BELOW it an illegal slice is
                   ACCEPTED and the invalid pair is persisted under
                   ok:true. Below is ordinary, not exotic —
                   CFG_BOUND_ALLOC_LO is 1, and `weekday_min: 30` against
                   the 60-minute default is design 5.3's own worked
                   example. Layer 2 reads the same way for the same reason
                   (a faulted slice reads 0, so no clamp fires), and the
                   backstop for both is the same: design row C11's
                   config-error gate (M2), which re-reads the stored pair
                   later and is not fooled by a fault that happened here.
                   Until C11 lands the consequence is bounded rather than
                   absent — schedule_get_chore_free_sec() clamps, so a
                   stored invalid pair behaves as the per-day-type off
                   switch; the operator's setting is wrong, the device is
                   not. */
                alloc->get_u16(&alloc_min);
                /* Argument order copied verbatim from config_validate.h:
                   at THIS site the candidate is the FIRST argument. Both
                   parameters are uint16_t, so a swap compiles silently
                   and inverts the answer for every unequal pair. */
                if (!config_is_valid_chore_free_min((uint16_t)v, alloc_min))
                    return reject(ack, ack_len, key, "pair");
            }
            /* LAYER 2 — this field is an ALLOCATION: clamp its paired
               slice down instead of refusing. A parent lowering screen
               time must not be blocked by a chore setting they are not
               thinking about (design 5.3), and the clamp target falls out
               of the boundary: the largest valid slice of an allocation is
               the allocation itself.

               THE CLAMP WRITE GOES FIRST, before the allocation's own.
               The other order commits the new allocation and can then
               fail to clamp, persisting exactly the invalid pair this
               layer exists to prevent; this order's failure mode is a
               slice lowered under an allocation that stayed put, which is
               still valid. */
            const cfg_field_t *slice = chore_slice_of(f);
            bool clamped = false;
            if (slice != NULL) {
                uint16_t free_min = 0;
                slice->get_u16(&free_min); /* unchecked, same reasoning as above */
                /* And at THIS site the candidate is the SECOND argument.
                   A swap here is the dangerous one: lowering to 30 with a
                   slice of 60 would evaluate (30, 60) -> VALID, no clamp
                   would fire, and NVS would keep the invalid pair. */
                if (!config_is_valid_chore_free_min(free_min, (uint16_t)v)) {
                    /* The clamp target is hand-written here as the new
                       allocation, which config_validate.h lists as an
                       acknowledged gap ("the CLAMP TARGET is prose above,
                       not code"). Still hand-written, deliberately: this
                       is the only CLAMPING consumer in the tree — the bulk
                       applier neither clamps nor refuses, it names the
                       field in its ack and leaves the broken pair standing
                       (design rows C11/C12) — so a shared helper would be
                       an identity function with a single caller. Revisit
                       if a second clamping consumer appears.

                       KNOWN ASYMMETRY, flagged for M3. This write is
                       committed before the allocation's own, so the
                       sequence "clamp lands, allocation write fails" is
                       reachable: the ack then reads
                       {"key":"holiday_min","ok":false,"err":"nvs"} while
                       chore_free_hol has moved permanently — 600 ("gate
                       off") to 100 ("gated for 500 of 600 minutes") — and
                       nothing names it. The operator is told the edit
                       failed, and it did; a field they were not editing
                       moved anyway. The PAIR stays valid, which is why
                       this order is still right: the other order persists
                       the invalid pair this layer exists to prevent, which
                       is strictly worse. Do not restructure to avoid it.
                       M3 decides whether a reject ack should carry the
                       clamp annotation too.

                       Not covered by a host test, and not for want of
                       trying: mock_nvs_fail_writes(N) refuses the NEXT N
                       writes, so "the first write succeeds and the second
                       fails" cannot be expressed without per-key injection
                       the mock does not have. */
                    if (slice->set_u16((uint16_t)v) != ESP_OK)
                        return reject(ack, ack_len, key, "nvs");
                    clamped = true;
                }
            }
            if (f->set_u16((uint16_t)v) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
            /* A clamp is not silent, but MIND WHERE THE SIGNAL GOES. This
               ack is never published: mqtt_ha.c:466 hands it to
               ESP_LOGI("set %s: %s") and nothing else, and the retained
               config_ack topic carries only config_apply's and cmd_apply's
               acks. The annotation is therefore a SERIAL-ONLY record, and
               design 5.3's "says so in the ack" is satisfied on serial
               only. The operator's real MQTT signal is the cfg state
               republish at the tail of this same apply_sets() call, which
               carries the clamped value — HA's number control simply
               moves.

               ok:true is load-bearing, not cosmetic: mqtt_ha.c clears the
               retained set/<key> only on HA_CFG_OK and config discovery
               emits retain:true, so reporting a clamp as REJECTED would
               leave the edit retained and re-applied every window forever.

               This is also the longest ack the function writes, and it
               fits with room to spare: 76 B plus the NUL at
               holiday_min/chore_free_hol (the widest key pair) with a
               four-digit clamped_to, which is the ceiling because `v` has
               already passed the CFG_BOUND_ALLOC_HI range check above.
               mqtt_ha.c's buffer is 256 B.

               A SAME-WAKE COLLISION IS REAL AND UNTESTED (recorded for
               M2/M3 — do not fix the ordering here): apply_incoming() runs
               config_apply and publishes the retained config_ack FIRST
               (mqtt_ha.c:710-713), then apply_sets (line 741). A retained
               bulk document carrying chore_free_wd:120 plus a retained
               set/weekday_min:30 therefore ends the window with a VALID
               pair — this clamp fixed it — behind a retained config_ack
               that still says ok:false,errors:["chore_free_wd"], and the
               operator's 120 silently overwritten by 30. */
            if (clamped) {
                snprintf(ack, ack_len, "{\"key\":\"%s\",\"ok\":true,\"clamped\":\"%s\",\"clamped_to\":%u}", key,
                         slice->key, (unsigned)v);
                return HA_CFG_OK;
            }
            break;
        }
        case CFG_HHMM: {
            long v;
            if (!parse_int(value, &v))
                return reject(ack, ack_len, key, "nan");
            if (f->validate != NULL ? !f->validate((int)v) : !quiet_hhmm_valid((int)v))
                return reject(ack, ack_len, key, "time");
            if (f->set_u16((uint16_t)v) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
            break;
        }
        case CFG_STR: {
            if (value == NULL || strlen(value) >= (size_t)f->hi)
                return reject(ack, ack_len, key, "len");
            if (!config_is_clean_str(value))
                return reject(ack, ack_len, key, "char");
            if (f->validate_str != NULL && !f->validate_str(value))
                return reject(ack, ack_len, key, "value");
            if (f->set_str(value) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
            break;
        }
        case CFG_BOOL: {
            uint8_t on;
            if (!parse_onoff(value, &on))
                return reject(ack, ack_len, key, "onoff");
            if (f->set_u16(on) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
            break;
        }
        case CFG_TNAME: {
            nvs_timer_defs_blob_t b;
            if (value == NULL || strlen(value) >= (size_t)f->hi)
                return reject(ack, ack_len, key, "len");
            if (!config_is_clean_str(value))
                return reject(ack, ack_len, key, "char");
            if (!load_defs_for_write(&b))
                return reject(ack, ack_len, key, "nodefs");
            snprintf(b.defs[f->slot - 1].name, sizeof(b.defs[0].name), "%s", value);
            mark_defined(&b, f->slot);
            if (nvs_config_set_timer_defs(&b) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
            break;
        }
        case CFG_TMIN: {
            long v;
            if (!parse_int(value, &v))
                return reject(ack, ack_len, key, "nan");
            if (v < 1 || v > 1440)
                return reject(ack, ack_len, key, "range");
            nvs_timer_defs_blob_t b;
            if (!load_defs_for_write(&b))
                return reject(ack, ack_len, key, "nodefs");
            b.defs[f->slot - 1].min = (int32_t)v;
            mark_defined(&b, f->slot);
            if (nvs_config_set_timer_defs(&b) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
            break;
        }
        case CFG_TRELOAD:
        case CFG_TBREAK: {
            uint8_t on;
            if (!parse_onoff(value, &on))
                return reject(ack, ack_len, key, "onoff");
            nvs_timer_defs_blob_t b;
            if (!load_defs_for_write(&b))
                return reject(ack, ack_len, key, "nodefs");
            if (f->kind == CFG_TRELOAD)
                b.defs[f->slot - 1].reload = on;
            else
                b.defs[f->slot - 1].break_eligible = on;
            mark_defined(&b, f->slot);
            if (nvs_config_set_timer_defs(&b) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
            break;
        }
        case CFG_ENUM: {
            int idx = -1;
            for (int i = 0; value != NULL && i < f->n_options; i++) {
                if (strcmp(value, f->options[i]) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0)
                return reject(ack, ack_len, key, "option");
            if (f->set_u16((uint16_t)idx) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
            break;
        }
        default:
            return reject(ack, ack_len, key, "unsupported");
    }
    snprintf(ack, ack_len, "{\"key\":\"%s\",\"ok\":true}", key);
    return HA_CFG_OK;
}

int ha_config_state_json(char *buf, size_t len) {
    nvs_timer_defs_blob_t defs;
    (void)load_defs(&defs, DEFS_SEED_INSTALLED); /* read-only: publish what the device is running */
    int pos = jcat(buf, len, 0, "{");
    int n = (int)(sizeof(FIELDS) / sizeof(FIELDS[0]));
    for (int i = 0; i < n; i++) {
        const cfg_field_t *f = &FIELDS[i];
        if (i)
            pos = jcat(buf, len, pos, ",");
        /* Worst case is a maxed OTA URL with every character escaped. */
        char esc[2 * CFG_STR_MAX];
        switch (f->kind) {
            case CFG_STR: {
                /* Must fit the longest CFG_STR value in the registry, or a
                   stored value is silently truncated on republish. */
                char raw[CFG_STR_MAX];
                raw[0] = '\0';
                f->get_str(raw, sizeof(raw));
                pos = jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, jesc(esc, sizeof(esc), raw));
                break;
            }
            case CFG_TNAME: {
                /* Copy through a bounded buffer first: the blob is flash
                   data validated only for size and version, so an
                   unterminated name would let jesc run into the next
                   slot. */
                char tname[sizeof(defs.defs[0].name) + 1];
                snprintf(tname, sizeof(tname), "%.*s", (int)sizeof(defs.defs[0].name), defs.defs[f->slot - 1].name);
                pos = jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, jesc(esc, sizeof(esc), tname));
                break;
            }
            case CFG_TMIN:
                pos = jcat(buf, len, pos, "\"%s\":%ld", f->key, (long)defs.defs[f->slot - 1].min);
                break;
            case CFG_TRELOAD:
                pos = jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, defs.defs[f->slot - 1].reload ? "ON" : "OFF");
                break;
            case CFG_TBREAK:
                pos =
                    jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, defs.defs[f->slot - 1].break_eligible ? "ON" : "OFF");
                break;
            case CFG_BOOL: {
                /* Any non-zero stored value reads as ON: HA's switch state
                   must be one of pl_on/pl_off, and a value written by a
                   different firmware still has to render. */
                uint16_t v = 0;
                f->get_u16(&v);
                pos = jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, v ? "ON" : "OFF");
                break;
            }
            case CFG_ENUM: {
                /* HA select state must be one of the options — clamp a
                   stored index from a different firmware to option 0. */
                uint16_t v = 0;
                f->get_u16(&v);
                if (v >= (uint16_t)f->n_options)
                    v = 0;
                pos = jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, f->options[v]);
                break;
            }
            default: { /* CFG_U16 / CFG_HHMM */
                uint16_t v = 0;
                f->get_u16(&v);
                pos = jcat(buf, len, pos, "\"%s\":%u", f->key, (unsigned)v);
                break;
            }
        }
    }
    return jcat(buf, len, pos, "}");
}

int ha_config_discovery_topic(char *buf, size_t len, const char *dev_id, const cfg_field_t *f) {
    return snprintf(buf, len, "homeassistant/%s/%s_%s/config", f->component, dev_id, f->key);
}

int ha_config_discovery(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                        const cfg_field_t *f) {
    char dname[128]; /* escaped device name (<=63 raw) */
    int pos = 0;
    /* def_ent_id: "<component>.<uniq_id>", same reason as the entity table
       in stats_json.c — entity_ids must not move when the device is
       renamed. The "<component>." half is required, not decoration: HA
       reads the value as a full entity_id and keeps only what follows the
       first dot, so a dotless value registers an EMPTY object id.
       f->component is what ha_config_discovery_topic() writes into the
       topic, so payload and topic agree by construction. (This replaces
       obj_id, which HA removed from MQTT discovery in 2026.4.0;
       def_ent_id dates from 2025.10 and is silently ignored before it.) */
    pos = jcat(buf, len, pos,
               "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"def_ent_id\":\"%s.%s_%s\","
               "\"stat_t\":\"magtag/%s/cfg\","
               "\"val_tpl\":\"{{ value_json.%s }}\",\"cmd_t\":\"magtag/%s/set/%s\",\"retain\":true",
               f->name, dev_id, f->key, f->component, dev_id, f->key, dev_id, f->key, dev_id, f->key);
    /* optimistic: the device is asleep, so the cfg state topic lags an edit
       by a whole window. Without this, HA re-renders the control from the
       stale retained state the instant you change it — the switch snaps back
       and a revert to the old number sends nothing. Optimistic shows the
       commanded value immediately; the device's cfg republish then confirms
       (or corrects) it. Cost on switches: HA renders assumed-state switches
       as two lightning-bolt buttons instead of a toggle — the user chose
       that over the snap-back (tried both on-device). */
    if (strcmp(f->component, "number") == 0) {
        /* mode:box -> numeric entry field, not a slider (sliders are painful
           for wide ranges like 1..1440). */
        pos = jcat(buf, len, pos, ",\"min\":%d,\"max\":%d,\"step\":%d,\"mode\":\"box\",\"optimistic\":true", f->lo,
                   f->hi, f->step);
        if (f->unit != NULL)
            pos = jcat(buf, len, pos, ",\"unit_of_meas\":\"%s\"", f->unit);
    } else if (strcmp(f->component, "text") == 0) {
        /* HA's text platform defaults max to 255. Without an explicit max
           the UI accepts a value the device must then reject with "len",
           and nothing surfaces that unless you watch the ack topic — the
           control simply snaps back at the next cfg republish. Worse for
           the bulk config document, where one over-long timer name aborts
           the whole timers array. `hi` is the buffer size, so the longest
           string that fits is hi - 1. */
        pos = jcat(buf, len, pos, ",\"mode\":\"text\",\"max\":%d", f->hi - 1);
    } else if (strcmp(f->component, "switch") == 0) {
        pos = jcat(buf, len, pos, ",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"optimistic\":true");
    } else if (strcmp(f->component, "select") == 0) {
        pos = jcat(buf, len, pos, ",\"options\":[");
        for (int i = 0; i < f->n_options; i++)
            pos = jcat(buf, len, pos, "%s\"%s\"", i ? "," : "", f->options[i]);
        pos = jcat(buf, len, pos, "],\"optimistic\":true");
    }
    pos = jcat(buf, len, pos, ",\"ent_cat\":\"config\"");
    pos = jcat(buf, len, pos,
               ",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Adafruit\",\"mdl\":\"MagTag 2.9\","
               "\"sw\":\"%s\"}}",
               dev_id, jesc(dname, sizeof(dname), dev_name), fw);
    return pos;
}
