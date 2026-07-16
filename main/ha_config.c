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
#include "tones.h"

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
/* Extra-timer slot fields — read-modify-write the timer_defs blob by slot. */
#define TIMER_NAME(n) \
    { .key = "timer" #n "_name", .component = "text", .name = "Timer " #n " name", .kind = CFG_TNAME, .slot = n }
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
/* Alert-tone selects: option string in HA, stored as its u16 index. */
#define TONE_SELECT(k, nm, set, get)                                                                   \
    {                                                                                                  \
        .key = k, .component = "select", .name = nm, .kind = CFG_ENUM, .set_u16 = set, .get_u16 = get, \
        .options = tones_names, .n_options = TONE_COUNT                                                \
    }

/* The registry hardcodes extra-timer slots 1..4; if TIMER_EXTRA_SLOTS ever
   shrinks, defs[slot-1] in the state builder would read out of bounds. */
_Static_assert(TIMER_EXTRA_SLOTS >= 4, "ha_config registry assumes >= 4 extra-timer slots");

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
    TIMER_NAME(2),
    TIMER_MIN(2),
    TIMER_RELOAD(2),
    TIMER_NAME(3),
    TIMER_MIN(3),
    TIMER_RELOAD(3),
    TIMER_NAME(4),
    TIMER_MIN(4),
    TIMER_RELOAD(4),
    TONE_SELECT("tone_expiry", "Expiry tone", nvs_config_set_tone_expiry, nvs_config_get_tone_expiry),
    TONE_SELECT("tone_break", "Break tone", nvs_config_set_tone_break, nvs_config_get_tone_break),
    TONE_SELECT("tone_bed", "Bed time tone", nvs_config_set_tone_bed, nvs_config_get_tone_bed),
};

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

/* Reject values that would corrupt the discovery/state JSON (unescaped
   quote/backslash) or the MQTT/HA layer (control chars). Empty is allowed
   here (an empty timer name disables the slot); NULL is not clean. */
static bool str_is_clean(const char *s) {
    if (s == NULL)
        return false;
    for (; *s != '\0'; s++)
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20)
            return false;
    return true;
}

static ha_cfg_result_t reject(char *ack, size_t len, const char *key, const char *err) {
    snprintf(ack, len, "{\"key\":\"%s\",\"ok\":false,\"err\":\"%s\"}", key, err);
    return HA_CFG_REJECTED;
}

/* Load the timer-defs blob, or a fresh zeroed one (all slots disabled) if
   none exists yet — so the first HA edit materializes a valid blob. */
static void load_defs(nvs_timer_defs_blob_t *b) {
    if (nvs_config_get_timer_defs(b) != ESP_OK) {
        memset(b, 0, sizeof(*b));
        b->version = TIMER_DEFS_BLOB_VERSION;
    }
}

ha_cfg_result_t ha_config_set(const char *key, const char *value, char *ack, size_t ack_len) {
    const cfg_field_t *f = find_field(key);
    if (f == NULL) {
        snprintf(ack, ack_len, "{\"key\":\"%s\",\"ok\":false,\"err\":\"unknown\"}", key ? key : "");
        return HA_CFG_UNKNOWN;
    }
    /* Extra-timer slots index the blob by (slot-1); guard against a
       registry that outgrew TIMER_EXTRA_SLOTS. */
    if ((f->kind == CFG_TNAME || f->kind == CFG_TMIN || f->kind == CFG_TRELOAD) &&
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
            if (!str_is_clean(value))
                return reject(ack, ack_len, key, "char");
            if (f->set_str(value) != ESP_OK)
                return reject(ack, ack_len, key, "nvs");
            break;
        }
        case CFG_TNAME: {
            nvs_timer_defs_blob_t b;
            if (value == NULL || strlen(value) >= sizeof(b.defs[0].name))
                return reject(ack, ack_len, key, "len");
            if (!str_is_clean(value))
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
        case CFG_TRELOAD: {
            if (value == NULL || (strcmp(value, "ON") != 0 && strcmp(value, "OFF") != 0))
                return reject(ack, ack_len, key, "onoff");
            nvs_timer_defs_blob_t b;
            load_defs(&b);
            b.defs[f->slot - 1].reload = (strcmp(value, "ON") == 0) ? 1 : 0;
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
        char esc[128]; /* holds a fully-escaped tz (<=47) or device name */
        switch (f->kind) {
            case CFG_STR: {
                char raw[64];
                raw[0] = '\0';
                f->get_str(raw, sizeof(raw));
                pos = jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, jesc(esc, sizeof(esc), raw));
                break;
            }
            case CFG_TNAME:
                pos = jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, jesc(esc, sizeof(esc), defs.defs[f->slot - 1].name));
                break;
            case CFG_TMIN:
                pos = jcat(buf, len, pos, "\"%s\":%ld", f->key, (long)defs.defs[f->slot - 1].min);
                break;
            case CFG_TRELOAD:
                pos = jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, defs.defs[f->slot - 1].reload ? "ON" : "OFF");
                break;
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
        pos = jcat(buf, len, pos, ",\"mode\":\"text\"");
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
