/* Editable HA config entities — pure over nvs_config + validators. */
#include "ha_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_validate.h"
#include "nvs_config.h"
#include "quiet_hours.h"

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
#define TEXT(k, nm, maxlen, set, get) \
    { .key = k, .component = "text", .name = nm, .kind = CFG_STR, .hi = maxlen, .set_str = set, .get_str = get }

static const cfg_field_t FIELDS[] = {
    NUM_U16("weekday_min", "Weekday allocation", "min", 1, 1440, 5, nvs_config_set_weekday_min,
            nvs_config_get_weekday_min),
    NUM_U16("weekend_min", "Weekend allocation", "min", 1, 1440, 5, nvs_config_set_weekend_min,
            nvs_config_get_weekend_min),
    NUM_U16("holiday_min", "Holiday allocation", "min", 1, 1440, 5, nvs_config_set_holiday_min,
            nvs_config_get_holiday_min),
    NUM_U16("summer_min", "Summer allocation", "min", 1, 1440, 5, nvs_config_set_summer_min, nvs_config_get_summer_min),
    NUM_HHMM("quiet_start", "Quiet hours start (HHMM)", nvs_config_set_quiet_start, nvs_config_get_quiet_start),
    NUM_HHMM("quiet_end", "Quiet hours end (HHMM)", nvs_config_set_quiet_end, nvs_config_get_quiet_end),
    NUM_U16("break_interval_min", "Break interval", "min", 0, 480, 5, nvs_config_set_break_interval_min,
            nvs_config_get_break_interval_min),
    NUM_U16("break_duration_min", "Break duration", "min", 1, 120, 1, nvs_config_set_break_duration_min,
            nvs_config_get_break_duration_min),
    TEXT("name", "Device name", 32, nvs_config_set_dev_name, nvs_config_get_dev_name),
    TEXT("tz", "Timezone", 48, nvs_config_set_tz, nvs_config_get_tz),
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

static ha_cfg_result_t reject(char *ack, size_t len, const char *key, const char *err) {
    snprintf(ack, len, "{\"key\":\"%s\",\"ok\":false,\"err\":\"%s\"}", key, err);
    return HA_CFG_REJECTED;
}

ha_cfg_result_t ha_config_set(const char *key, const char *value, char *ack, size_t ack_len) {
    const cfg_field_t *f = find_field(key);
    if (f == NULL) {
        snprintf(ack, ack_len, "{\"key\":\"%s\",\"ok\":false,\"err\":\"unknown\"}", key ? key : "");
        return HA_CFG_UNKNOWN;
    }
    switch (f->kind) {
        case CFG_U16: {
            long v;
            if (!parse_int(value, &v))
                return reject(ack, ack_len, key, "nan");
            if (v < f->lo || v > f->hi)
                return reject(ack, ack_len, key, "range");
            f->set_u16((uint16_t)v);
            break;
        }
        case CFG_HHMM: {
            long v;
            if (!parse_int(value, &v))
                return reject(ack, ack_len, key, "nan");
            if (!quiet_hhmm_valid((int)v))
                return reject(ack, ack_len, key, "time");
            f->set_u16((uint16_t)v);
            break;
        }
        case CFG_STR: {
            if (value == NULL || strlen(value) >= (size_t)f->hi)
                return reject(ack, ack_len, key, "len");
            f->set_str(value);
            break;
        }
        default:
            return reject(ack, ack_len, key, "unsupported");
    }
    snprintf(ack, ack_len, "{\"key\":\"%s\",\"ok\":true}", key);
    return HA_CFG_OK;
}

int ha_config_state_json(char *buf, size_t len) {
    int pos = jcat(buf, len, 0, "{");
    int n = (int)(sizeof(FIELDS) / sizeof(FIELDS[0]));
    for (int i = 0; i < n; i++) {
        const cfg_field_t *f = &FIELDS[i];
        if (i)
            pos = jcat(buf, len, pos, ",");
        if (f->kind == CFG_STR) {
            char raw[64], esc[80];
            raw[0] = '\0';
            f->get_str(raw, sizeof(raw));
            pos = jcat(buf, len, pos, "\"%s\":\"%s\"", f->key, jesc(esc, sizeof(esc), raw));
        } else {
            uint16_t v = 0;
            f->get_u16(&v);
            pos = jcat(buf, len, pos, "\"%s\":%u", f->key, (unsigned)v);
        }
    }
    return jcat(buf, len, pos, "}");
}

int ha_config_discovery_topic(char *buf, size_t len, const char *dev_id, const cfg_field_t *f) {
    return snprintf(buf, len, "homeassistant/%s/%s_%s/config", f->component, dev_id, f->key);
}

int ha_config_discovery(char *buf, size_t len, const char *dev_id, const char *dev_name, const char *fw,
                        const cfg_field_t *f) {
    char dname[64];
    int pos = 0;
    pos = jcat(buf, len, pos,
               "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"magtag/%s/cfg\","
               "\"val_tpl\":\"{{ value_json.%s }}\",\"cmd_t\":\"magtag/%s/set/%s\",\"retain\":true",
               f->name, dev_id, f->key, dev_id, f->key, dev_id, f->key);
    if (strcmp(f->component, "number") == 0) {
        pos = jcat(buf, len, pos, ",\"min\":%d,\"max\":%d,\"step\":%d", f->lo, f->hi, f->step);
        if (f->unit != NULL)
            pos = jcat(buf, len, pos, ",\"unit_of_meas\":\"%s\"", f->unit);
    } else if (strcmp(f->component, "text") == 0) {
        pos = jcat(buf, len, pos, ",\"mode\":\"text\"");
    } else if (strcmp(f->component, "switch") == 0) {
        pos = jcat(buf, len, pos, ",\"pl_on\":\"ON\",\"pl_off\":\"OFF\"");
    }
    pos = jcat(buf, len, pos, ",\"ent_cat\":\"config\"");
    pos = jcat(buf, len, pos,
               ",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Adafruit\",\"mdl\":\"MagTag 2.9\","
               "\"sw\":\"%s\"}}",
               dev_id, jesc(dname, sizeof(dname), dev_name), fw);
    return pos;
}
