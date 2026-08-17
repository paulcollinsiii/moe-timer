/* Editable HA config entities — pure over nvs_config + validators. */
#include "ha_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bedtime.h"
#include "config_validate.h"
#include "nvs_config.h"
#include "quiet_hours.h"
#include "timer.h" /* timer_slot_def: the table this boot is running */
#include "tones.h"

#ifndef NATIVE
#include "esp_log.h"
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

static const char *TAG = "ha_config";

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

/* Load the timer-defs blob, falling back to the table this boot is
   actually running when it cannot be read.

   The fallback used to be a ZEROED blob, and the read-modify-write cases
   in ha_config_set persist whatever this returns — so one transient read
   failure while the operator edited a single field wiped every OTHER
   slot's name, minutes and flags. Zeros are also the wrong answer for the
   two read-only callers below: since BUG-8, timer_defs_install() no
   longer materializes the blob at boot, so "unreadable" is the ordinary
   state of a device whose NVS was erased, and empties here would publish
   blank names in the cfg state and a discovery hash that disagrees with
   the per-timer entities mqtt_ha.c builds from this very same
   timer_slot_def(). Falling back to the installed table gives all three
   callers what the device is running, whether that came from NVS or from
   the compile-time defaults. A slot the timer layer reports as disabled
   stays zeroed, which is what a disabled slot means in the blob too. */
static void load_defs(nvs_timer_defs_blob_t *b) {
    if (nvs_config_get_timer_defs(b) == ESP_OK)
        return;
    memset(b, 0, sizeof(*b));
    b->version = TIMER_DEFS_BLOB_VERSION;
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        const timer_def_t *d = timer_slot_def(i + 1);
        if (d == NULL)
            continue;
        snprintf(b->defs[i].name, sizeof(b->defs[i].name), "%s", d->name);
        b->defs[i].min = d->duration_sec / 60;
        b->defs[i].reload = d->reloadable ? 1 : 0;
        b->defs[i].break_eligible = d->break_eligible ? 1 : 0;
    }
    ESP_LOGW(TAG, "timer-defs blob unreadable; using the installed table");
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
    load_defs(&defs);
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
            if (f->set_u16((uint16_t)v) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
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
            load_defs(&b);
            snprintf(b.defs[f->slot - 1].name, sizeof(b.defs[0].name), "%s", value);
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
            load_defs(&b);
            b.defs[f->slot - 1].min = (int32_t)v;
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
            load_defs(&b);
            if (f->kind == CFG_TRELOAD)
                b.defs[f->slot - 1].reload = on;
            else
                b.defs[f->slot - 1].break_eligible = on;
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
    load_defs(&defs);
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
    pos = jcat(buf, len, pos,
               "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"magtag/%s/cfg\","
               "\"val_tpl\":\"{{ value_json.%s }}\",\"cmd_t\":\"magtag/%s/set/%s\",\"retain\":true",
               f->name, dev_id, f->key, dev_id, f->key, dev_id, f->key);
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
