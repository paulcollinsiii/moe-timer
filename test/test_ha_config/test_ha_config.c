#include <stdio.h>
#include <string.h>
#include <unity.h>

/* Single-TU: the editable-config applier over mock NVS + real accessors
   and validators. No cJSON — values arrive as strings (from MQTT).

   timer.c is here because load_defs() falls back to timer_slot_def_raw()
   when the blob cannot be read (BUG-8): the real slot table, not a stub, so
   the fallback is exercised against the same table the device runs on.
   Tests that do not call timer_set_defs() see an empty table.

   timer_defs.c is here for timer_defs_compiled(), which is the OTHER seed:
   the write path reconstructs from menuconfig, never from the installed
   table (see load_defs()). This TU defines no CONFIG_MAGTAG_TIMER* symbols,
   so that compiled table is entirely empty — which is exactly what makes
   "the write seeded a table and stamped ONLY the edited slot" observable
   here. The rung-sensitive cases, where the compiled table is non-empty,
   live in test_timer_defs.

   setUp() stores an EMPTY-BUT-PRESENT timer-defs table. Most cases here are
   about validating and persisting a field, not about provenance, and an
   empty stored table gives them exactly the blob the old zeroed-fallback
   gave them. The cases that ARE about provenance clear it with
   mock_nvs_reset() or make it unreadable, and say so. */
// clang-format off
#include "mock_hal_nvs.c"
#include "mock_hal_time.c"
#include "../../main/timer.c"
#include "../../main/nvs_config.c"
#include "../../main/quiet_hours.c"
#include "../../main/bedtime.c"
#include "../../main/config_validate.c"
#include "../../main/tones.c"
#include "../../main/ha_config.c"
#include "../../main/timer_defs.c"
// clang-format on
/* Header only: the set/<key> transport slot the registry's advertised
   maximums have to fit through. */
#include "mqtt_rx.h"
/* Header only: STATS_JSON_PAYLOAD_MAX is the buffer mqtt_ha.c builds
   THESE payloads into as well, so the size bound belongs to the same
   number rather than to a copy of it. */
#include "stats_json.h"

static void seed_blob(void); /* defined with the Phase B tests below */

/* A stored table that defines no slots: "somebody wrote a table, and it is
   empty" — which is what every pre-BUG-8 case implicitly assumed, since
   boot used to write one unconditionally. Distinct from "no table at all",
   which the provenance cases below use. */
static void store_empty_table(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    nvs_config_set_timer_defs(&b);
}

void setUp(void) {
    mock_nvs_reset();
    timer_set_defs(NULL, 0); /* s_defs is a static: clear it like the NVS */
    store_empty_table();
}
void tearDown(void) {}

/* ---- ha_config_set: validate + persist per kind ---- */

void test_set_u16_valid_persists(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "45", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(45, v);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_set_u16_out_of_range_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "5000", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_weekday_min(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, v); /* unchanged */
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":false"));
}

void test_set_u16_non_numeric_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "lots", ack, sizeof(ack)));
}

void test_set_hhmm_valid_and_invalid(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("quiet_start", "2130", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_quiet_start(&v);
    TEST_ASSERT_EQUAL_UINT16(2130, v);
    /* minute 60 is not a real time */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("quiet_start", "2160", ack, sizeof(ack)));
}

void test_set_break_interval_zero_allowed(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("break_interval_min", "0", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_break_interval_min(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v);
}

void test_set_str_valid_and_too_long(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("name", "Kitchen", ack, sizeof(ack)));
    char s[64];
    nvs_config_get_dev_name(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("Kitchen", s);
    char toolong[64];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("name", toolong, ack, sizeof(ack)));
}

void test_set_unknown_key(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_UNKNOWN, ha_config_set("nonsense", "1", ack, sizeof(ack)));
}

/* ---- validation hardening (review H3/L6, parse_int, HHMM bounds) ---- */

void test_set_str_rejects_quote_and_backslash(void) {
    char ack[128];
    /* a name containing " or \ would corrupt discovery/state JSON */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("name", "bad\"name", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("name", "back\\slash", ack, sizeof(ack)));
    /* control characters likewise */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("name", "line\nbreak", ack, sizeof(ack)));
    /* a clean name with an apostrophe is fine */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("name", "Kids' Room", ack, sizeof(ack)));
}

void test_set_timer_name_rejects_quote(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_name", "a\"b", ack, sizeof(ack)));
}

void test_set_treload_rejects_non_onoff(void) {
    seed_blob();
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_reload", "on", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_reload", "true", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_reload", "", ack, sizeof(ack)));
    /* reload flag untouched by the rejected writes */
    nvs_timer_defs_blob_t b;
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_reload", "OFF", ack, sizeof(ack)));
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].reload);
}

void test_hhmm_boundaries(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("quiet_start", "2359", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("quiet_start", "2400", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("quiet_start", "0060", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("quiet_start", "-1", ack, sizeof(ack)));
}

void test_parse_int_trailing_junk_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "45x", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", " 45 ", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "", ack, sizeof(ack)));
}

/* ---- NVS write failure surfaces as a rejection (review M4) ---- */

void test_set_nvs_failure_rejected(void) {
    char ack[128];
    mock_nvs_fail_writes(1);
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "45", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nvs\""));
}

void test_set_timer_nvs_failure_rejected(void) {
    char ack[128];
    mock_nvs_fail_writes(1);
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_min", "25", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nvs\""));
}

/* ---- state JSON: escaping + worst-case size fits the firmware buffer ---- */

void test_state_json_escapes_specials(void) {
    /* config_apply (cJSON) can write a name with a quote even though the
       set path rejects one — the state builder must still escape it. */
    nvs_config_set_dev_name("a\"b\\c");
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"a\\\"b\\\\c\""));
}

/* Fill `dst` (a buffer of `cap`) with the longest value it can hold, made
   entirely of characters ha_config_json_escape DOUBLES. The escaped form
   is what the buffer actually has to hold, so a worst case built from
   plain ASCII understates it by half. */
static void fill_escapable(char *dst, size_t cap) {
    for (size_t i = 0; i + 1 < cap; i++)
        dst[i] = (i % 2) ? '\\' : '"';
    dst[cap - 1] = '\0';
}

void test_state_json_worst_case_fits_firmware_buffer(void) {
    char ack[128];
    /* EVERY u16-rendered field at 65535, which is the widest `%u` can
       print and NOT the bound the registry advertises. These are plain
       NVS keys: ha_config_set bounds an EDIT, but the state builder
       renders whatever the getter returns, and a key written by a
       firmware with different bounds still has to render — ha_config.c's
       CFG_BOOL and CFG_ENUM cases reason about exactly that case for
       their own kinds. Written through the accessors for that reason;
       routed through ha_config_set instead, each of these would cap at
       its advertised bound and this guard would sit 17 B under the truth.

       That includes the four chore pairs, which land at 65535/65535 —
       slice equal to allocation, the legitimate off switch. A pair the
       gate would REFUSE renders too: design row C11 is about a device
       that has one stored, and the cfg document has to carry it. Layer 1
       is not in the way here because these do not go through the set
       path, so the allocation-before-slice ordering the set path needs
       does not apply. */
    nvs_config_set_weekday_min(65535);
    nvs_config_set_weekend_min(65535);
    nvs_config_set_holiday_min(65535);
    nvs_config_set_summer_min(65535);
    nvs_config_set_chore_free_wd(65535);
    nvs_config_set_chore_free_we(65535);
    nvs_config_set_chore_free_hol(65535);
    nvs_config_set_chore_free_sum(65535);
    nvs_config_set_break_interval_min(65535);
    nvs_config_set_break_duration_min(65535);
    nvs_config_set_quiet_start(65535);
    nvs_config_set_quiet_end(65535);
    nvs_config_set_bedtime(65535);
    nvs_config_set_alert_volume(65535);
    /* Selects render the OPTION STRING, so all three go to the longest
       one — two of the defaults are shorter, which is part of why this
       guard used to read ~190 B under the truth. */
    ha_config_set("tone_expiry", "Marimba arpeggio", ack, sizeof(ack));
    ha_config_set("tone_break", "Marimba arpeggio", ack, sizeof(ack));
    ha_config_set("tone_bed", "Marimba arpeggio", ack, sizeof(ack));
    /* OFF, not ON: every switch in the registry renders one of the two
       literals and "OFF" is the longer by a byte. Nine switches — this
       one plus reload and break_eligible on all four slots — so the ON
       shape of this fixture read 9 B under the truth. */
    ha_config_set("ota_on_sync", "OFF", ack, sizeof(ack));

    /* Strings: maxed to their declared bound AND made of characters the
       escaper doubles. ha_config_set rejects quote/backslash, but the
       bulk-document path reaches these same fields with no cleanliness
       check (config_apply passes a NULL validator for name/tz, and
       apply_timers has none at all), so these values are reachable on a
       real device — see test_state_json_escapes_specials. Write them the
       way that path does, through the accessors. */
    char name[CFG_BOUND_NAME_MAX];
    fill_escapable(name, sizeof(name));
    nvs_config_set_dev_name(name);
    char tz[CFG_BOUND_TZ_MAX];
    fill_escapable(tz, sizeof(tz));
    nvs_config_set_tz(tz);

    nvs_timer_defs_blob_t defs;
    memset(&defs, 0, sizeof(defs));
    defs.version = TIMER_DEFS_BLOB_VERSION;
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        fill_escapable(defs.defs[i].name, sizeof(defs.defs[i].name));
        /* 65535, not the CFG_TMIN bound of 1440: same argument as the u16
           keys above — the state builder prints the stored blob field, and
           the blob is validated for size and version only.

           ONE AXIS IS DELIBERATELY NOT PUSHED, and it is the reason this
           number is a ceiling under a stated assumption rather than an
           absolute one: `min` is int32_t, so a blob holding a negative
           value would render up to 11 characters and cost another 28 B
           across the four slots. No in-tree writer can produce it
           (ha_config_set's CFG_TMIN takes 1..1440, config_apply's
           apply_timers bounds it too) and the blob version gate limits
           what a foreign firmware can hand us, so it is out of scope
           here. If a writer ever admits a wider or signed value, this
           guard has to move with it. */
        defs.defs[i].min = 65535;
        defs.defs[i].reload = 0; /* "OFF" renders a byte wider than "ON" */
        defs.defs[i].break_eligible = 0;
    }
    nvs_config_set_timer_defs(&defs);

    /* The OTA endpoint is the longest CFG_STR in the registry. It keeps a
       real https:// prefix because config_is_ota_url would reject
       anything else, and the set path is the only way it is written. */
    char url[CFG_BOUND_OTA_URL_MAX];
    memset(url, 'u', sizeof(url) - 1);
    memcpy(url, "https://", 8);
    url[sizeof(url) - 1] = '\0';
    ha_config_set("ota_url", url, ack, sizeof(ack));

    char buf[HA_CONFIG_STATE_MAX];
    int ret = ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_TRUE(ret < HA_CONFIG_STATE_MAX); /* not truncated */
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), ret);
    /* Headroom the firmware buffer actually has, so a future field
       addition trips here rather than silently knocking every editable
       control offline (a truncated doc is never published).

       THE REACHABLE MAXIMUM IS 1164 / 1536 B — headroom 372. It was 116
       against the old 1280 ceiling (and 146 by an earlier, wronger shape
       of this fixture); 1280 was raised because 116 B is less than one
       more string field. Every axis above is at
       its widest: 65535 on all fourteen u16/HHMM keys, 65535 on all four
       timer minutes, "OFF" on all nine switches, the longest option string
       on all three selects, and every string maxed AND filled with
       characters the escaper doubles. A fixture that renders the SHORTER
       option on an axis it controls does not understate the document
       harmlessly — it inflates the headroom a future field is measured
       against, which is the number this printf exists to publish. */
    printf("  worst-case cfg state: %d / %d bytes\n", ret, HA_CONFIG_STATE_MAX);
}

/* ---- ha_config_state_json ---- */

void test_state_json_reports_current_values(void) {
    ha_config_set("weekday_min", "45", (char[128]){0}, 128);
    ha_config_set("name", "Kitchen", (char[128]){0}, 128);
    char buf[512];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"weekday_min\":45"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Kitchen\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"quiet_start\":")); /* default present */
}

/* ---- discovery payloads ---- */

static const cfg_field_t *field_by_key(const char *key) {
    int n = 0;
    const cfg_field_t *f = ha_config_fields(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].key, key) == 0)
            return &f[i];
    return NULL;
}

void test_discovery_number_has_command_bounds_and_config_category(void) {
    const cfg_field_t *f = field_by_key("weekday_min");
    TEST_ASSERT_NOT_NULL(f);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/weekday_min\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_t\":\"magtag/magtag-a1b2c3/cfg\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.weekday_min"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"min\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"max\":1440"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"step\":1"));          /* round values must be valid in HA */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"box\""));    /* numeric entry, not a slider */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"optimistic\":true")); /* no snap-back on edit */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ent_cat\":\"config\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"retain\":true")); /* command retained for the sleeping device */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ids\":[\"magtag-a1b2c3\"]"));
}

void test_discovery_text_has_mode(void) {
    const cfg_field_t *f = field_by_key("name");
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen", "fw", f);
    TEST_ASSERT_EQUAL_STRING("text", f->component);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mode\":\"text\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/name\""));
}

/* Every text entity must advertise its length cap. HA's text platform
   defaults max to 255, so without this the UI accepts a name the device
   then rejects with "len" — and the rejection is invisible unless you are
   watching the ack topic. `hi` is the buffer size, so the longest string
   that fits is hi - 1. */
void test_discovery_text_advertises_max_length(void) {
    char buf[700];
    const struct {
        const char *key;
        const char *max;
    } cases[] = {
        {"name", "\"max\":31"},        /* CFG_BOUND_NAME_MAX 32 */
        {"tz", "\"max\":47"},          /* CFG_BOUND_TZ_MAX 48 */
        {"timer1_name", "\"max\":15"}, /* blob name field is 16 bytes */
        {"timer4_name", "\"max\":15"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const cfg_field_t *f = field_by_key(cases[i].key);
        TEST_ASSERT_NOT_NULL(f);
        TEST_ASSERT_EQUAL_STRING("text", f->component);
        ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, cases[i].max), cases[i].key);
    }
}

/* The advertised max and the device-side check must be the same number:
   an HA UI that accepts exactly max characters must never produce a
   value the device rejects. 15 fits, 16 does not. */
void test_timer_name_max_matches_the_reject_boundary(void) {
    const cfg_field_t *f = field_by_key("timer1_name");
    TEST_ASSERT_NOT_NULL(f);
    char ack[128], name[64];
    memset(name, 'x', sizeof(name));
    name[f->hi - 1] = '\0'; /* exactly the advertised max */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_name", name, ack, sizeof(ack)));
    name[f->hi - 1] = 'x';
    name[f->hi] = '\0'; /* one over */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_name", name, ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "len"));
}

void test_discovery_topic(void) {
    const cfg_field_t *f = field_by_key("weekday_min");
    char buf[128];
    ha_config_discovery_topic(buf, sizeof(buf), "magtag-a1b2c3", f);
    TEST_ASSERT_EQUAL_STRING("homeassistant/number/magtag-a1b2c3_weekday_min/config", buf);
}

/* ---- bedtime: field-specific HHMM validity (0 or 1800-2359) ---- */

void test_set_bedtime_accepts_evening_and_zero(void) {
    char ack[128];
    uint16_t v;
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("bedtime", "1800", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("bedtime", "2359", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("bedtime", "0", ack, sizeof(ack)));
    nvs_config_get_bedtime(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v); /* disabled sticks */
}

void test_set_bedtime_rejects_daytime(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("bedtime", "1759", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("bedtime", "900", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("bedtime", "2400", ack, sizeof(ack)));
    uint16_t v;
    nvs_config_get_bedtime(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_BEDTIME, v); /* unchanged */
    /* quiet fields keep their looser any-clock-time rule */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("quiet_start", "900", ack, sizeof(ack)));
}

/* ---- alert-tone selects (CFG_ENUM) ---- */

void test_set_tone_select_valid_option_persists(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("tone_expiry", "Gran Vals", ack, sizeof(ack)));
    uint16_t v = 0;
    nvs_config_get_tone_expiry(&v);
    TEST_ASSERT_EQUAL_UINT16(TONE_GRANVALS, v);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_set_tone_select_rejects_unknown_and_wrong_case(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("tone_break", "Kazoo", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("tone_break", "gran vals", ack, sizeof(ack)));
    uint16_t v = 0;
    nvs_config_get_tone_break(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_TONE_BREAK, v); /* unchanged */
    TEST_ASSERT_NOT_NULL(strstr(ack, "option"));
}

void test_state_json_emits_tone_option_strings(void) {
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf)); /* defaults */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tone_expiry\":\"Marimba arpeggio\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tone_break\":\"Gentle chime\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tone_bed\":\"Gran Vals\""));
}

void test_state_json_clamps_out_of_range_tone_index(void) {
    nvs_config_set_tone_expiry(999); /* e.g. stored by a future firmware */
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"tone_expiry\":\"Classic beep\""));
}

void test_discovery_select_lists_options(void) {
    const cfg_field_t *f = field_by_key("tone_bed");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("select", f->component);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf,
                                "\"options\":[\"Classic beep\",\"Ding-ding\",\"Gentle chime\","
                                "\"Marimba arpeggio\",\"Gran Vals\",\"Custom WAV\"]"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"optimistic\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/tone_bed\""));
}

/* ---- alert volume (CFG_U16, percent with boost range) ---- */

void test_set_alert_volume_valid_persists(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("alert_volume", "80", ack, sizeof(ack)));
    uint16_t v = 0;
    nvs_config_get_alert_volume(&v);
    TEST_ASSERT_EQUAL_UINT16(80, v);
    /* 0 = mute and the boost ceiling are both legal */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("alert_volume", "0", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("alert_volume", "200", ack, sizeof(ack)));
}

void test_set_alert_volume_out_of_range_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("alert_volume", "201", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("alert_volume", "-1", ack, sizeof(ack)));
    uint16_t v = 0;
    nvs_config_get_alert_volume(&v);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_ALERT_VOLUME, v); /* unchanged */
}

void test_alert_volume_discovery_and_state(void) {
    const cfg_field_t *f = field_by_key("alert_volume");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("number", f->component);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"min\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"max\":200"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"unit_of_meas\":\"%\""));
    char state[HA_CONFIG_STATE_MAX];
    ha_config_state_json(state, sizeof(state));
    char expect[48];
    snprintf(expect, sizeof(expect), "\"alert_volume\":%u", (unsigned)NVS_DEFAULT_ALERT_VOLUME);
    TEST_ASSERT_NOT_NULL(strstr(state, expect));
}

/* ---- Phase B: editable timer definitions (read-modify-write the blob) ---- */

static void seed_blob(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 15;
    b.defs[0].reload = 1;
    nvs_config_set_timer_defs(&b);
}

void test_set_timer_name_enables_slot(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer2_name", "Running", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_STRING("Running", b.defs[1].name);
}

void test_set_timer_name_empty_disables_slot(void) {
    seed_blob();
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_name", "", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_STRING("", b.defs[0].name); /* disabled */
}

void test_set_timer_name_too_long_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_name", "SixteenCharsPlus!", ack, sizeof(ack)));
}

/* Per-timer break-eligible switch: the HA-facing half of the flag. */
void test_set_timer_break_eligible(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_break", "ON", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_break", "OFF", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].break_eligible);
}

void test_set_timer_break_eligible_rejects_non_onoff(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_break", "ON", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_break", "true", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_break", "", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible); /* untouched */
}

/* Editing one per-slot field must not clobber its siblings: they all
   read-modify-write the same blob. */
void test_timer_fields_are_independent(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_break", "ON", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_reload", "ON", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_min", "25", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_name", "Violin", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_STRING("Violin", b.defs[0].name);
    TEST_ASSERT_EQUAL_INT32(25, b.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
}

void test_set_timer_min_and_reload(void) {
    seed_blob();
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_min", "25", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_reload", "OFF", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    nvs_config_get_timer_defs(&b);
    TEST_ASSERT_EQUAL_INT32(25, b.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].reload);
    TEST_ASSERT_EQUAL_STRING("Piano", b.defs[0].name); /* name preserved (read-modify-write) */
}

void test_set_timer_min_out_of_range_rejected(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_min", "9999", ack, sizeof(ack)));
}

void test_state_json_includes_timer_fields(void) {
    seed_blob();
    char buf[768];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_name\":\"Piano\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_min\":15"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_reload\":\"ON\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_break\":\"OFF\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer2_name\":\"\"")); /* empty slot */
}

void test_discovery_timer_reload_is_switch(void) {
    const cfg_field_t *f = field_by_key("timer1_reload");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("switch", f->component);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_on\":\"ON\""));
    /* Optimistic = assumed-state: HA renders it as two lightning-bolt
       buttons instead of a toggle. Tried both on-device; the user prefers
       that over the non-optimistic toggle's snap-back on every flip. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"optimistic\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/timer1_reload\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.timer1_reload"));
}

/* ---- OTA fields: generic CFG_BOOL switch + validated text ---- */

void test_set_ota_url_https_persists(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_url", "https://example.com/ota.json", ack, sizeof(ack)));
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("https://example.com/ota.json", s);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
}

void test_set_ota_url_rejects_non_https(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_url", "https://good.example/ota.json", ack, sizeof(ack)));
    /* http:// would make the update channel unauthenticated */
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_url", "http://evil.example/ota.json", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_url", "evil.example/ota.json", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":false"));
    /* the good value is still there — a rejected set writes nothing */
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("https://good.example/ota.json", s);
}

void test_set_ota_url_empty_disables_ota(void) {
    char ack[128];
    ha_config_set("ota_url", "https://example.com/ota.json", ack, sizeof(ack));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_url", "", ack, sizeof(ack)));
    char s[CFG_BOUND_OTA_URL_MAX];
    nvs_config_get_ota_url(s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("", s);
}

void test_set_ota_url_too_long_rejected(void) {
    char ack[128];
    char url[CFG_BOUND_OTA_URL_MAX + 8];
    memset(url, 'u', sizeof(url) - 1);
    memcpy(url, "https://", 8);
    url[sizeof(url) - 1] = '\0';
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_url", url, ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"len\""));
}

void test_set_ota_on_sync_switch_round_trip(void) {
    char ack[128];
    uint16_t v;
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_on_sync", "ON", ack, sizeof(ack)));
    nvs_config_get_ota_on_sync(&v);
    TEST_ASSERT_EQUAL_UINT16(1, v);
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("ota_on_sync", "OFF", ack, sizeof(ack)));
    nvs_config_get_ota_on_sync(&v);
    TEST_ASSERT_EQUAL_UINT16(0, v);
}

void test_set_ota_on_sync_rejects_non_onoff(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_on_sync", "1", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_on_sync", "on", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_on_sync", "", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"onoff\""));
}

void test_set_ota_on_sync_nvs_failure_rejected(void) {
    char ack[128];
    mock_nvs_fail_writes(1);
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("ota_on_sync", "ON", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nvs\""));
}

void test_state_json_includes_ota_fields(void) {
    char ack[128];
    ha_config_set("ota_url", "https://example.com/ota.json", ack, sizeof(ack));
    ha_config_set("ota_on_sync", "ON", ack, sizeof(ack));
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_url\":\"https://example.com/ota.json\""));
    /* CFG_BOOL renders as the discovery pl_on/pl_off strings, like the
       slot-bound switches — not as 0/1. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_on_sync\":\"ON\""));
    ha_config_set("ota_on_sync", "OFF", ack, sizeof(ack));
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_on_sync\":\"OFF\""));
}

/* A stored value from another firmware (or a corrupt one) must still
   render as a legal HA switch state. */
void test_state_json_ota_on_sync_nonzero_reads_as_on(void) {
    nvs_config_set_ota_on_sync(7);
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ota_on_sync\":\"ON\""));
}

void test_discovery_ota_on_sync_is_switch(void) {
    const cfg_field_t *f = field_by_key("ota_on_sync");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("switch", f->component);
    TEST_ASSERT_EQUAL(0, f->slot); /* generic, not slot-bound */
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_on\":\"ON\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_off\":\"OFF\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"optimistic\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/ota_on_sync\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "value_json.ota_on_sync"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ent_cat\":\"config\""));
}

void test_discovery_ota_url_is_text_with_max(void) {
    const cfg_field_t *f = field_by_key("ota_url");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_STRING("text", f->component);
    char buf[700];
    ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "K", "fw", f);
    char expect[32];
    snprintf(expect, sizeof(expect), "\"max\":%d", CFG_BOUND_OTA_URL_MAX - 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, expect));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"magtag/magtag-a1b2c3/set/ota_url\""));
}

/* ---- discovery freshness fingerprint ----
   mqtt_ha.c republishes discovery only when the schema version or this
   fingerprint changed. Discovery carries BOTH dev.name and dev.sw, so
   both have to be in it. */

void test_device_hash_is_deterministic(void) {
    TEST_ASSERT_EQUAL_UINT16(ha_config_device_hash("Kitchen", "1.5.0"), ha_config_device_hash("Kitchen", "1.5.0"));
}

/* THE regression: without fw in the fingerprint, an OTA update leaves the
   HA device card showing the version it was first discovered with,
   forever — a DISC_SCHEMA_VER bump masks it exactly once. */
void test_device_hash_changes_when_firmware_version_changes(void) {
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("Kitchen", "1.5.0"), ha_config_device_hash("Kitchen", "1.6.0"));
    /* a build-metadata-only difference still has to republish */
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("Kitchen", "1.5.0"), ha_config_device_hash("Kitchen", "1.5.0-dirty"));
}

void test_device_hash_changes_when_name_changes(void) {
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("Kitchen", "1.5.0"), ha_config_device_hash("Playroom", "1.5.0"));
}

/* The two strings are fingerprinted together, so the boundary between
   them must be marked or a rename could cancel a version bump out. */
void test_device_hash_does_not_confuse_the_field_boundary(void) {
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("ab", "c"), ha_config_device_hash("a", "bc"));
}

void test_device_hash_tolerates_null(void) {
    /* fw is a pointer off the stat snapshot; a NULL must not crash the
       window (ASan would catch a deref here). */
    ha_config_device_hash(NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(ha_config_device_hash("Kitchen", NULL), ha_config_device_hash("Kitchen", "1.5.0"));
}

/* ---- the discovery gate itself ----
   The compare/publish/stamp wiring lives in a static function in
   mqtt_ha.c, which has no host suite; this is the predicate it calls. */

void test_discovery_stale_on_a_fresh_device(void) {
    /* Blank NVS reads 0/0 — must republish, or a new device gets no
       entities at all. */
    TEST_ASSERT_TRUE(ha_config_discovery_stale(0, 0, 17, 0x1234));
}

void test_discovery_not_stale_when_both_match(void) {
    TEST_ASSERT_FALSE(ha_config_discovery_stale(17, 0x1234, 17, 0x1234));
}

void test_discovery_stale_on_schema_bump_alone(void) {
    TEST_ASSERT_TRUE(ha_config_discovery_stale(16, 0x1234, 17, 0x1234));
}

/* The leg this package added: no schema bump, only a new fingerprint
   (renamed device, new firmware version, renamed timer slot). */
void test_discovery_stale_on_fingerprint_change_alone(void) {
    TEST_ASSERT_TRUE(ha_config_discovery_stale(17, 0x1234, 17, 0x5678));
}

/* ---- slot names in the discovery fingerprint ----
   mqtt_ha publishes `<Name> remaining` / `<Name> limit` / `<Name> runs`
   per enabled slot, named from the HA-editable timerN_name. Neither the
   name nor the slot's existence is in DISC_SCHEMA_VER. */

void test_discovery_hash_changes_when_a_slot_is_renamed(void) {
    char ack[128];
    ha_config_set("timer1_name", "Piano", ack, sizeof(ack));
    uint16_t before = ha_config_discovery_hash("Kitchen", "1.5.0");
    ha_config_set("timer1_name", "Violin", ack, sizeof(ack));
    TEST_ASSERT_NOT_EQUAL(before, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

void test_discovery_hash_changes_when_a_slot_is_enabled_or_cleared(void) {
    char ack[128];
    uint16_t empty = ha_config_discovery_hash("Kitchen", "1.5.0");
    ha_config_set("timer2_name", "Reading", ack, sizeof(ack));
    uint16_t enabled = ha_config_discovery_hash("Kitchen", "1.5.0");
    TEST_ASSERT_NOT_EQUAL(empty, enabled);
    /* Clearing the name retires those entities — also a discovery change. */
    ha_config_set("timer2_name", "", ack, sizeof(ack));
    TEST_ASSERT_NOT_EQUAL(enabled, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

/* Slot names are folded with the same separator discipline as the dev
   block, so shifting a name between slots cannot cancel out. */
void test_discovery_hash_does_not_confuse_slot_boundaries(void) {
    char ack[128];
    ha_config_set("timer1_name", "ab", ack, sizeof(ack));
    ha_config_set("timer2_name", "", ack, sizeof(ack));
    uint16_t a = ha_config_discovery_hash("Kitchen", "1.5.0");
    ha_config_set("timer1_name", "a", ack, sizeof(ack));
    ha_config_set("timer2_name", "b", ack, sizeof(ack));
    TEST_ASSERT_NOT_EQUAL(a, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

void test_discovery_hash_still_tracks_the_device_block(void) {
    /* The dev-block legs must survive being folded together with slots. */
    TEST_ASSERT_NOT_EQUAL(ha_config_discovery_hash("Kitchen", "1.5.0"), ha_config_discovery_hash("Kitchen", "1.6.0"));
    TEST_ASSERT_NOT_EQUAL(ha_config_discovery_hash("Kitchen", "1.5.0"), ha_config_discovery_hash("Playroom", "1.5.0"));
}

/* ---- transport ----
   The set/<key> slot must be able to carry the longest value any field
   advertises to HA. It could not: ota_url advertised max 127 while the
   slot capped a payload at 79, so the endpoint was dropped in mqtt_rx —
   no ack, retained command never cleared, re-dropped every window, and
   OTA silently never turned on. Walk the registry, don't spot-check. */

void test_every_string_field_fits_the_set_transport(void) {
    int n = 0;
    const cfg_field_t *f = ha_config_fields(&n);
    for (int i = 0; i < n; i++) {
        if (f[i].kind != CFG_STR && f[i].kind != CFG_TNAME)
            continue;
        /* hi is the buffer size, so hi-1 chars must fit the value slot. */
        TEST_ASSERT_TRUE_MESSAGE((size_t)f[i].hi <= sizeof(((mqtt_set_kv_t *)0)->value), f[i].key);
    }
}

void test_every_field_key_fits_the_set_transport(void) {
    int n = 0;
    const cfg_field_t *f = ha_config_fields(&n);
    for (int i = 0; i < n; i++)
        TEST_ASSERT_TRUE_MESSAGE(strlen(f[i].key) < sizeof(((mqtt_set_kv_t *)0)->key), f[i].key);
}

/* ---- enablement is the name AND the duration ----
   timer.c gates a slot on name[0] != 0 AND duration > 0, and CFG_TNAME
   writes only the name. A name-only fold therefore missed the moment a
   slot actually became enabled. */

/* THE broken flow: two edits in two different device windows. */
void test_discovery_hash_changes_when_only_the_duration_enables_a_slot(void) {
    char ack[128];
    /* Window 1: name set, min still 0 -> slot is NOT yet enabled. */
    ha_config_set("timer4_name", "Yoga", ack, sizeof(ack));
    uint16_t named_but_disabled = ha_config_discovery_hash("Kitchen", "1.5.0");
    /* Window 2: min set -> slot flips to enabled and its three per-slot
       entities should now appear. The name did not change. */
    ha_config_set("timer4_min", "20", ack, sizeof(ack));
    TEST_ASSERT_NOT_EQUAL(named_but_disabled, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

/* The reviewer's measured collision: these two hashed identically. */
void test_discovery_hash_separates_zero_and_nonzero_duration(void) {
    char ack[128];
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 20;
    nvs_config_set_timer_defs(&b);
    uint16_t with_duration = ha_config_discovery_hash("Kitchen", "1.5.0");
    b.defs[0].min = 0;
    nvs_config_set_timer_defs(&b);
    TEST_ASSERT_NOT_EQUAL(with_duration, ha_config_discovery_hash("Kitchen", "1.5.0"));
    (void)ack;
}

/* Discovery depends on entity NAMES and existence, not on how long the
   timer runs — so a plain duration edit between two enabled values must
   NOT force a full discovery republish (a wasted radio burst on battery). */
void test_discovery_hash_ignores_a_duration_change_that_keeps_it_enabled(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 20;
    nvs_config_set_timer_defs(&b);
    uint16_t at20 = ha_config_discovery_hash("Kitchen", "1.5.0");
    b.defs[0].min = 30;
    nvs_config_set_timer_defs(&b);
    TEST_ASSERT_EQUAL_UINT16(at20, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

/* An unterminated name in the blob must not read into the next slot. */
void test_discovery_hash_tolerates_an_unterminated_slot_name(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    memset(b.defs[0].name, 'x', sizeof(b.defs[0].name)); /* no NUL */
    b.defs[0].min = 20;
    nvs_config_set_timer_defs(&b);
    ha_config_discovery_hash("Kitchen", "1.5.0"); /* ASan catches an overrun */
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf)); /* same field, via jesc */
}

/* ---- BUG-8: load_defs()'s fallback when the blob cannot be read ----

   timer_defs_install() no longer materializes the blob at boot, so
   "unreadable" is the ordinary state of a device whose NVS was erased —
   not an exotic error. load_defs() therefore falls back to a
   reconstruction rather than to zeros, and reports which one it handed
   back.

   The two failures are NOT the same event and the write path treats them
   differently, which is the split these cases pin:

     nothing stored (ESP_ERR_NVS_NOT_FOUND) — the write PROCEEDS. Refusing
       here was a dead end for an operator who drives the device only from
       the HA controls: no `timers` document ever arrives, so no table is
       ever created, so every edit NAKs forever with no diagnostic anywhere
       in HA (nothing publishes the ha_config_set ack — see mqtt_ha.c).
     stored but unparseable (anything else) — the write still NAKs. Bytes
       are physically there and a device is not allowed to write a table it
       did not read. Recovery is config_apply, which since BUG-5 rebuilds
       an unreadable table even on a `ver` match. */

/* The table timer_defs_install() would have left in place. File scope
   because timer.c keeps the pointers, not copies. */
static const timer_def_t INSTALLED[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false}, {"Piano", 15 * 60, true, true}, {"Meditation", 10 * 60, false, true},
    {"", 0, false, false},       {"", 0, false, false},
};

/* Same, plus slot 3 mid-way through the documented two-edit flow: named in
   one window, minutes still unset. timer_slot_def() hides this slot (it
   gates on name AND duration); timer_slot_def_raw(), which load_defs()
   uses, does not. */
static const timer_def_t INSTALLED_NAME_ONLY[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false},  {"Piano", 15 * 60, true, true}, {"Meditation", 10 * 60, false, true},
    {"Reading", 0, false, false}, {"", 0, false, false},
};

/* Store a blob the getter refuses. A stale layout version is the
   version-drift loss the entry lists as candidate 2, and it makes the read
   fail without adding a read-failure hook to a mock 39 other suites share. */
static void store_unreadable_blob(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = (uint8_t)(TIMER_DEFS_BLOB_VERSION - 1);
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Stale");
    nvs_config_set_timer_defs(&b);
    nvs_timer_defs_blob_t probe;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&probe));
}

/* The caller with teeth, and the reason load_defs() reports provenance.

   A read-modify-write on an unreadable blob first persisted ZEROES for
   every slot the edit did not touch (one transient failure while the
   operator nudged timer2_min wiped timer1 outright), and then — once the
   fallback became the installed table — persisted the BOOT SNAPSHOT for
   them instead. Both are the same mistake: writing a table that was not
   read. The device must refuse, and say so. */
void test_edit_with_unreadable_blob_is_refused(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    store_unreadable_blob();
    int writes_before = mock_nvs_write_count(NVS_KEY_TIMER_DEFS);
    char ack[128];
    ha_cfg_result_t r = ha_config_set("timer2_min", "25", ack, sizeof(ack));
    /* Nothing written: the unreadable blob is still whatever it was. This
       first, so a regression reports the write rather than the ack. */
    TEST_ASSERT_EQUAL_INT(writes_before, mock_nvs_write_count(NVS_KEY_TIMER_DEFS));
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, r);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":false"));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nodefs\""));
}

/* The controls-only hole, closed. All four slot-bound kinds take the same
   door, so all four have to work on a device that has never been handed a
   `timers` document — that operator has no other way in, and until this
   they NAK'd forever. */
void test_every_timer_field_is_accepted_without_a_stored_table(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    mock_nvs_reset(); /* no table at all: a device whose NVS was erased */
    const char *keys[4] = {"timer1_name", "timer1_min", "timer1_reload", "timer1_break"};
    const char *vals[4] = {"Cello", "25", "ON", "ON"};
    ha_cfg_result_t r[4];
    char acks[4][128];
    for (int i = 0; i < 4; i++)
        r[i] = ha_config_set(keys[i], vals[i], acks[i], sizeof(acks[i]));
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK, r[i], keys[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(acks[i], "\"ok\":true"), keys[i]);
    }
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_STRING("Cello", b.defs[0].name);
    TEST_ASSERT_EQUAL_INT32(25, b.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
}

/* ...and the write records WHO, per slot. Only the slot the edit named is
   stamped; the other three stay 0. Not a detail: stamping the whole table
   here is BUG-8 through a different door, since `defined` is exactly the
   "somebody authoritative chose this" signal the blob's mere existence used
   to be. That is the whole of what the assertions below pin.

   What they deliberately do NOT pin — and what an earlier version of this
   comment wrongly claimed they did — is that a 0 sends the slot back to the
   menuconfig rung on a later document. It does not, in general: apply_timers()
   tests `defined || name[0] != '\0'`, so a bystander slot carrying a
   menuconfig-seeded NAME reads as already defined and the stored value
   answers instead. That distinction is untestable in this TU, which compiles
   no CONFIG_MAGTAG_TIMER* symbols at all: the compile-time table is empty,
   nothing gets seeded, every bystander name is "", and the two tiers can
   never be told apart here. Do not add rung-sensitive coverage to this file
   — test_timer_defs/ is the only suite with a non-empty compile-time table,
   and test_the_rung_survives_a_table_a_control_created is where this
   behaviour is pinned and described honestly. */
void test_a_control_edit_stamps_only_the_slot_it_touched(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    mock_nvs_reset();
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer2_break", "ON", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[1].defined);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].defined);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[2].defined);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[3].defined);
    /* The stamp is what carries the edit, not the name: slot 2 is unnamed
       in this TU's (empty) compile-time table, so the pre-`defined` name
       test would have called this slot undefined and handed the operator's
       ON back to menuconfig on the next document. */
    TEST_ASSERT_EQUAL_STRING("", b.defs[1].name);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[1].break_eligible);
}

/* The seed is menuconfig, NOT the installed table. This TU compiles no
   CONFIG_MAGTAG_TIMER* symbols, so the compiled table is empty while
   INSTALLED names Piano/Meditation — if the write ever reconstructed from
   the running table it would persist those two names as though somebody
   had authored them, which is the BUG-8 masquerade with a boot snapshot
   in place of the Kconfig table. */
void test_the_created_table_is_seeded_from_menuconfig_not_the_running_table(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    mock_nvs_reset();
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer3_min", "20", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_STRING("", b.defs[0].name); /* not "Piano" */
    TEST_ASSERT_EQUAL_STRING("", b.defs[1].name); /* not "Meditation" */
    TEST_ASSERT_EQUAL_INT32(0, b.defs[0].min);
    TEST_ASSERT_EQUAL_INT32(20, b.defs[2].min);
}

/* The fallback is a BOOT snapshot, and the blob is rewritten mid-window
   without re-installing (net_apply's reconcile_defs runs after the window).
   So synthesizing from it does not merely invent bystanders, it REVERTS an
   edit the operator made minutes earlier — and used to ack that ok:true. */
void test_a_refused_edit_does_not_revert_an_earlier_one(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_name", "Cello", ack, sizeof(ack)));
    /* Now the blob stops being readable, content intact (version drift). */
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    b.version = (uint8_t)(TIMER_DEFS_BLOB_VERSION - 1);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&b));
    ha_cfg_result_t r = ha_config_set("timer2_min", "30", ack, sizeof(ack));
    /* Read past the version gate: the stored bytes must still say Cello, not
       the "Piano" the boot table would have written back over them. Asserted
       BEFORE the result so a regression reports the data loss rather than
       stopping at the ack. */
    nvs_timer_defs_blob_t raw;
    size_t len = sizeof(raw);
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_read_blob(NVS_KEY_TIMER_DEFS, &raw, &len));
    TEST_ASSERT_EQUAL_STRING("Cello", raw.defs[0].name);
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, r);
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nodefs\""));
}

/* The cfg state drives HA's text/number/switch controls. Empty names here
   are what the device would publish on the first window after an NVS erase
   if the fallback were still zeros. */
void test_state_json_falls_back_to_the_installed_table(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    store_unreadable_blob();
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_name\":\"Piano\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_min\":15"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer1_break\":\"ON\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer2_name\":\"Meditation\""));
}

/* The discovery hash is the caller that cannot wait for someone else to
   write the blob: mqtt_ha.c runs publish_states() BEFORE apply_incoming()
   applies the retained document. It also has to agree with the per-timer
   entities mqtt_ha.c builds from this same timer_slot_def(). */
void test_discovery_hash_falls_back_to_the_installed_table(void) {
    timer_set_defs(INSTALLED, TIMER_SLOT_COUNT);
    store_unreadable_blob();
    uint16_t from_installed = ha_config_discovery_hash("Kitchen", "1.5.0");

    /* The same two slots, but stored: the fallback must reach the same
       fingerprint, or the first window after an erase burns a discovery
       republish and the second burns another one changing it back. */
    mock_nvs_reset();
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 15;
    snprintf(b.defs[1].name, sizeof(b.defs[1].name), "Meditation");
    b.defs[1].min = 10;
    nvs_config_set_timer_defs(&b);
    TEST_ASSERT_EQUAL_UINT16(ha_config_discovery_hash("Kitchen", "1.5.0"), from_installed);

    /* And it is not the all-empty hash the zeroed fallback produced. */
    mock_nvs_reset();
    timer_set_defs(NULL, 0);
    TEST_ASSERT_NOT_EQUAL(from_installed, ha_config_discovery_hash("Kitchen", "1.5.0"));
}

/* A slot that is NAMED but has no minutes yet is the middle of the two-edit
   flow this file documents, and timer_slot_def() hides it. Published
   through the filtered accessor, the fallback blanked the operator's name
   in HA's text control — on a device whose only fault was an unreadable
   blob. */
void test_state_json_shows_a_name_only_slot(void) {
    timer_set_defs(INSTALLED_NAME_ONLY, TIMER_SLOT_COUNT);
    store_unreadable_blob();
    char buf[HA_CONFIG_STATE_MAX];
    ha_config_state_json(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer3_name\":\"Reading\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timer3_min\":0"));
}

/* Same slot, the other read-only caller. The fingerprint must not depend on
   whether the blob happened to be readable this window: it folds the name,
   so hiding a name-only slot moved the hash and cost a full retained
   discovery republish — then another one when the read next succeeded,
   which is precisely the waste this fallback exists to prevent. */
void test_discovery_hash_is_stable_for_a_name_only_slot(void) {
    timer_set_defs(INSTALLED_NAME_ONLY, TIMER_SLOT_COUNT);
    store_unreadable_blob();
    uint16_t from_installed = ha_config_discovery_hash("Kitchen", "1.5.0");

    mock_nvs_reset();
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 15;
    snprintf(b.defs[1].name, sizeof(b.defs[1].name), "Meditation");
    b.defs[1].min = 10;
    snprintf(b.defs[2].name, sizeof(b.defs[2].name), "Reading"); /* min still 0 */
    nvs_config_set_timer_defs(&b);
    TEST_ASSERT_EQUAL_UINT16(ha_config_discovery_hash("Kitchen", "1.5.0"), from_installed);
}

/* Same rule as the entity table in stats_json.c: every discovery payload
   carries def_ent_id, and its value is "<component>.<uniq_id>". These are
   the editable controls — the switches an over-broad HA automation reaches
   for — so they are the half of the surface that actually got swept.

   The component prefix is load-bearing, not decoration: HA reads the field
   as a full entity_id and keeps only what follows the FIRST dot, so a
   dotless value registers an EMPTY object id. The walk below therefore
   splits the value the way HA does rather than only looking for a
   substring. (obj_id, the field this replaces, was removed from HA's MQTT
   discovery in 2026.4.0; def_ent_id has existed since 2025.10.)

   This is also where the size ceiling for the config payloads is proven,
   on the same buffer and the same -128 B margin as the entity table's
   test, because the two want the same walk and def_ent_id must not depend
   on the device name at all — which 63 W's state more loudly than
   "Kitchen MagTag" does. */
static void assert_config_discovery(const char *dev_name, const char *fw, const char *what) {
    char buf[STATS_JSON_PAYLOAD_MAX];
    char want[128];
    int count = 0, worst = 0;
    const char *worst_key = "";
    const cfg_field_t *fields = ha_config_fields(&count);
    TEST_ASSERT_TRUE(count > 0);
    for (int i = 0; i < count; i++) {
        const int n = ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", dev_name, fw, &fields[i]);
        snprintf(want, sizeof(want), "\"uniq_id\":\"magtag-a1b2c3_%s\"", fields[i].key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want), fields[i].key);
        snprintf(want, sizeof(want), "\"def_ent_id\":\"%s.magtag-a1b2c3_%s\"", fields[i].component, fields[i].key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want), fields[i].key);
        /* HA's own split: `_, _, object_id = value.partition(".")`. */
        const char *v = strstr(buf, "\"def_ent_id\":\"");
        TEST_ASSERT_NOT_NULL_MESSAGE(v, fields[i].key);
        v += strlen("\"def_ent_id\":\"");
        const char *dot = strchr(v, '.');
        const char *end = strchr(v, '"');
        TEST_ASSERT_NOT_NULL_MESSAGE(dot, fields[i].key); /* dotless -> empty object id */
        TEST_ASSERT_NOT_NULL_MESSAGE(end, fields[i].key);
        TEST_ASSERT_TRUE_MESSAGE(dot < end, fields[i].key);
        snprintf(want, sizeof(want), "magtag-a1b2c3_%s", fields[i].key);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)strlen(want), (int)(end - dot - 1), fields[i].key);
        /* The payload's component must be the one the topic routes on. */
        char topic[128];
        ha_config_discovery_topic(topic, sizeof(topic), "magtag-a1b2c3", &fields[i]);
        snprintf(want, sizeof(want), "homeassistant/%s/", fields[i].component);
        TEST_ASSERT_EQUAL_STRING_LEN_MESSAGE(want, topic, strlen(want), fields[i].key);
        TEST_ASSERT_TRUE_MESSAGE(n < (int)sizeof(buf), fields[i].key);
        if (n > worst) {
            worst = n;
            worst_key = fields[i].key;
        }
    }
    /* Not an equality assert — a headroom report that fails only when the
       margin is gone, so a field can be added without editing a magic
       number. The message carries the measurement and the widest field so
       a failure says WHICH control blew the budget. */
    char msg[192];
    snprintf(msg, sizeof(msg), "%s: worst %d B of %d, widest field '%s'", what, worst, (int)sizeof(buf), worst_key);
    TEST_ASSERT_TRUE_MESSAGE(worst < (int)sizeof(buf) - 128, msg);
}

void test_every_config_discovery_payload_carries_def_ent_id_and_fits(void) {
    /* 63 W's, not the 31 the HA text entity advertises as its max: the
       transport ceiling is mqtt_ha.c's `char dev_name[64]`, and a name
       written to NVS by an older firmware or by a path that does not go
       through CFG_BOUND_NAME_MAX arrives through that buffer. Same figure
       as the entity table's test, so one worst case covers both.

       fw is 31 chars because that is what the device passes:
       esp_app_get_description()->version is char[32], filled here from
       `git describe`. The 2-char "fw" this test used to pass understated
       every payload by 29 bytes. */
    char longname[64];
    memset(longname, 'W', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    char fw[32];
    memset(fw, 'W', sizeof(fw) - 1);
    fw[sizeof(fw) - 1] = '\0';
    assert_config_discovery(longname, fw, "config discovery headroom below 128 B (plain 63-char name)");

    /* The escape-expansion case, and the real ceiling. Each quote escapes
       to two bytes, and ha_config.c escapes the device name into char[128]
       where stats_json.c uses char[64] — so all 63 quotes land here as 126
       bytes, while the entity table fills its smaller buffer and stops at
       62 bytes whatever the name is. The quotes therefore cost the entity
       payloads nothing and cost these ones 63 B, which is what makes the
       config registry, not the entity table, the widest thing mqtt_ha.c
       publishes. */
    char quoted[64];
    memset(quoted, '"', sizeof(quoted) - 1);
    quoted[sizeof(quoted) - 1] = '\0';
    assert_config_discovery(quoted, fw, "config discovery headroom below 128 B (63 quotes, escaped to 126 B)");
}

/* ---- the chore gate's cross-field rule (design 5.3, layers 1 and 2) ----

   The pairing is written out BY HAND here rather than read from
   ha_config.c's own table: this TU #includes ha_config.c, so borrowing its
   table would make a mispairing (summer clamped against the weekend)
   agree with itself and pass. An independent list is the only thing that
   can disagree. */
typedef struct {
    const char *free_key;
    const char *alloc_key;
    esp_err_t (*get_free)(uint16_t *);
    esp_err_t (*get_alloc)(uint16_t *);
} pair_ref_t;

static const pair_ref_t PAIR_REFS[] = {
    {"chore_free_wd", "weekday_min", nvs_config_get_chore_free_wd, nvs_config_get_weekday_min},
    {"chore_free_we", "weekend_min", nvs_config_get_chore_free_we, nvs_config_get_weekend_min},
    {"chore_free_hol", "holiday_min", nvs_config_get_chore_free_hol, nvs_config_get_holiday_min},
    {"chore_free_sum", "summer_min", nvs_config_get_chore_free_sum, nvs_config_get_summer_min},
};
#define PAIR_REF_COUNT (sizeof(PAIR_REFS) / sizeof(PAIR_REFS[0]))

void test_chore_free_fields_accept_the_full_range(void) {
    char ack[128];
    for (size_t i = 0; i < PAIR_REF_COUNT; i++) {
        const pair_ref_t *p = &PAIR_REFS[i];
        /* 1440 is only a legal slice of a 1440-minute day, so open the
           allocation first: the cross-field rule is not the range rule,
           and this case is about the range. */
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK, ha_config_set(p->alloc_key, "1440", ack, sizeof(ack)), p->alloc_key);
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK, ha_config_set(p->free_key, "1440", ack, sizeof(ack)), p->free_key);
        uint16_t v = 1;
        p->get_free(&v);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(1440, v, p->free_key);
        /* 0 = fully gated, the default and the one value every deployed
           device is running. It must stay settable. */
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK, ha_config_set(p->free_key, "0", ack, sizeof(ack)), p->free_key);
        p->get_free(&v);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, v, p->free_key);
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_REJECTED, ha_config_set(p->free_key, "1441", ack, sizeof(ack)), p->free_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"err\":\"range\""), p->free_key);
    }
}

/* Four DISTINCT allocations, one per pair, and the distinctness is the
   point rather than tidiness: the layer-1 cases below read an allocation
   through the pairing, so if all four sat at one shared value a setter
   consulting the WRONG partner would read that same value and every
   assertion would pass anyway. A mutation run proved this is not
   theoretical — `chore_free_sum` mispaired against `weekend_min` SURVIVED
   the shared-60 version of the test below, because both allocations were
   60. Spread them out and one of the two arithmetic cases always flips. */
static const struct {
    const char *text;
    uint16_t value;
} SPREAD_ALLOCS[] = {{"100", 100}, {"200", 200}, {"300", 300}, {"400", 400}};

/* LAYER 1. The free slice is the side that gets refused, because a slice
   bigger than the day it comes out of cannot mean anything. Both
   arithmetic cases are here on purpose: a mispairing that reads a SMALLER
   partner refuses the legal `alloc - 1`, and one that reads a LARGER
   partner accepts the illegal `alloc + 1`, so between them no mispairing
   in either direction survives. */
void test_chore_free_above_its_allocation_is_refused_and_nvs_untouched(void) {
    char ack[128];
    /* Every allocation first: each pair has to be judged in a tree where
       the other three allocations are numbers its own is not. */
    for (size_t i = 0; i < PAIR_REF_COUNT; i++)
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK,
                                  ha_config_set(PAIR_REFS[i].alloc_key, SPREAD_ALLOCS[i].text, ack, sizeof(ack)),
                                  PAIR_REFS[i].alloc_key);
    for (size_t i = 0; i < PAIR_REF_COUNT; i++) {
        const pair_ref_t *p = &PAIR_REFS[i];
        char want[64], text[8];
        uint16_t free_min = 0, alloc_min = 0;
        /* One under its own allocation: accepted and stored. */
        snprintf(text, sizeof(text), "%u", (unsigned)(SPREAD_ALLOCS[i].value - 1));
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK, ha_config_set(p->free_key, text, ack, sizeof(ack)), p->free_key);
        p->get_free(&free_min);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(SPREAD_ALLOCS[i].value - 1, free_min, p->free_key);
        /* One over: refused. */
        snprintf(text, sizeof(text), "%u", (unsigned)(SPREAD_ALLOCS[i].value + 1));
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_REJECTED, ha_config_set(p->free_key, text, ack, sizeof(ack)), p->free_key);
        /* Named in the ack: on this path the ack's own `key` IS the field
           name, and the refusal carries a reason of its own so "too big a
           slice" is not confused with "out of range". */
        snprintf(want, sizeof(want), "\"key\":\"%s\"", p->free_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, want), p->free_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"ok\":false"), p->free_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, "\"err\":\"pair\""), p->free_key);
        /* A refusal stores NOTHING — neither half moves. */
        p->get_free(&free_min);
        p->get_alloc(&alloc_min);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(SPREAD_ALLOCS[i].value - 1, free_min, p->free_key);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(SPREAD_ALLOCS[i].value, alloc_min, p->alloc_key);
    }
}

/* chore_free == allocation is the per-day-type OFF SWITCH (design 3.3) and
   needs no extra key, so it is VALID. Pinned right beside the `>` case
   above so the boundary is held from both sides: a setter hand-written
   with `<` instead of `<=` would refuse the off switch. */
void test_chore_free_equal_to_its_allocation_is_the_off_switch(void) {
    char ack[128];
    /* Spread again, for the reason SPREAD_ALLOCS records: the exact-match
       case is the one a mispaired setter is most likely to get right by
       accident, since a shared allocation makes every partner the right
       partner. */
    for (size_t i = 0; i < PAIR_REF_COUNT; i++)
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK,
                                  ha_config_set(PAIR_REFS[i].alloc_key, SPREAD_ALLOCS[i].text, ack, sizeof(ack)),
                                  PAIR_REFS[i].alloc_key);
    for (size_t i = 0; i < PAIR_REF_COUNT; i++) {
        const pair_ref_t *p = &PAIR_REFS[i];
        char text[8];
        uint16_t v = 0;
        snprintf(text, sizeof(text), "%u", (unsigned)SPREAD_ALLOCS[i].value);
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK, ha_config_set(p->free_key, text, ack, sizeof(ack)), p->free_key);
        p->get_free(&v);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(SPREAD_ALLOCS[i].value, v, p->free_key);
        snprintf(text, sizeof(text), "%u", (unsigned)(SPREAD_ALLOCS[i].value + 1));
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_REJECTED, ha_config_set(p->free_key, text, ack, sizeof(ack)), p->free_key);
    }
}

/* LAYER 2, and the case that matters most. The allocation setter CLAMPS
   its paired slice down instead of refusing: a parent lowering screen time
   must not be blocked by a chore setting they are not thinking about.
   This is also the test that fails if config_is_valid_chore_free_min's
   arguments are swapped at the allocation site — (30, 120) reads VALID,
   no clamp fires, and nothing downstream would ever say so, because
   schedule_get_chore_free_sec() clamps and the stored invalid pair reads
   back identically to the legitimate off switch. */
void test_lowering_an_allocation_clamps_the_paired_chore_free(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "120", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("chore_free_wd", "120", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "30", ack, sizeof(ack)));
    uint16_t alloc_min = 0, free_min = 0;
    nvs_config_get_weekday_min(&alloc_min);
    nvs_config_get_chore_free_wd(&free_min);
    TEST_ASSERT_EQUAL_UINT16(30, alloc_min);
    TEST_ASSERT_EQUAL_UINT16(30, free_min); /* clamped to the new allocation */
    /* A clamp is not silent: the ack reports which field moved and to
       what, on top of the success it is reporting. */
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"clamped\":\"chore_free_wd\""));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"clamped_to\":30"));
}

/* The same boundary from the allocation side: landing exactly ON the
   stored slice is the off switch, not a violation, so nothing is written
   and the ack stays plain. */
void test_an_allocation_equal_to_its_chore_free_does_not_clamp(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "120", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("chore_free_wd", "60", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "60", ack, sizeof(ack)));
    uint16_t free_min = 0;
    nvs_config_get_chore_free_wd(&free_min);
    TEST_ASSERT_EQUAL_UINT16(60, free_min);
    TEST_ASSERT_NULL(strstr(ack, "clamped"));
}

void test_raising_an_allocation_leaves_the_chore_free_alone(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "60", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("chore_free_wd", "30", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "1440", ack, sizeof(ack)));
    uint16_t free_min = 0;
    nvs_config_get_chore_free_wd(&free_min);
    TEST_ASSERT_EQUAL_UINT16(30, free_min);
    TEST_ASSERT_NULL(strstr(ack, "clamped"));
}

/* One lowering pass over all four pairs: `slice[]` is each pair's
   pre-clamp free slice, `lower_to[]` what its allocation is then lowered
   to. Both are per-pair and DISTINCT, for the reason SPREAD_ALLOCS above
   records and this test learned the hard way in its own right.

   THE FLAT VERSION OF THIS TEST WAS BLIND, and not in a subtle place: it
   set all four slices to the same 600 before lowering each allocation to
   a value unique to it. The lower-to values being distinct was not enough,
   because the CLAMP DECISION reads the slice, not the allocation — with
   every slice at 600, a setter consulting the wrong partner read 600 too,
   reached the same decision, and clamped. A mutation run over all twelve
   single-pointer mispairings of the pairing table this file used to have
   found two surviving the entire suite, one of which stored
   `holiday_min: 100` beside `chore_free_hol: 600` under ok:true — the
   exact state this layer exists to prevent, with every test green.

   WHAT MAKES A SWAP VISIBLE. Each lower_to[i] sits BETWEEN its own pair's
   slice and the next slice below it, so the clamp DECISION differs between
   the right partner (fires) and every wrong partner holding a smaller
   slice (does not fire, the ack carries no "clamped", the assertion
   fails). A wrong partner holding a LARGER slice still fires, so the
   decision alone cannot separate it — and cannot be made to, for the
   lowest-sliced pair no lower_to exists that is below its own slice and
   above every other. Two things close that half:
     - the full-vector check after EVERY lowering, which catches a clamp
       that landed on the wrong slice even when the decision agreed;
     - the caller running this pass TWICE with the spread reversed, so a
       partner that was larger in one pass is smaller in the other. The
       union of the two passes flips the decision for all twelve
       mispairings; neither pass alone flips more than six. */
static void clamp_pass(const uint16_t *slice, const uint16_t *lower_to, const char *pass) {
    char ack[128], text[16], msg[96];
    uint16_t cur[PAIR_REF_COUNT];
    /* Open every allocation first. The spread below is a RANGE question,
       not a pairing question, and a 400-minute slice needs a day that
       holds it; raising an allocation never clamps. */
    for (size_t i = 0; i < PAIR_REF_COUNT; i++) {
        snprintf(msg, sizeof(msg), "%s: %s", pass, PAIR_REFS[i].alloc_key);
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK, ha_config_set(PAIR_REFS[i].alloc_key, "1440", ack, sizeof(ack)), msg);
    }
    for (size_t i = 0; i < PAIR_REF_COUNT; i++) {
        snprintf(text, sizeof(text), "%u", (unsigned)slice[i]);
        snprintf(msg, sizeof(msg), "%s: %s", pass, PAIR_REFS[i].free_key);
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK, ha_config_set(PAIR_REFS[i].free_key, text, ack, sizeof(ack)), msg);
        cur[i] = slice[i];
    }
    for (size_t i = 0; i < PAIR_REF_COUNT; i++) {
        char want[64];
        snprintf(text, sizeof(text), "%u", (unsigned)lower_to[i]);
        snprintf(msg, sizeof(msg), "%s: %s", pass, PAIR_REFS[i].alloc_key);
        TEST_ASSERT_EQUAL_MESSAGE(HA_CFG_OK, ha_config_set(PAIR_REFS[i].alloc_key, text, ack, sizeof(ack)), msg);
        /* The clamp fired, it named THIS pair's slice, and it landed on
           the new allocation. */
        snprintf(want, sizeof(want), "\"clamped\":\"%s\",\"clamped_to\":%u", PAIR_REFS[i].free_key,
                 (unsigned)lower_to[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ack, want), msg);
        cur[i] = lower_to[i];
        /* And NOTHING ELSE MOVED — all four slices, after every single
           lowering. Checking only at the end of the loop would let a clamp
           land on a pair that is lowered later anyway and be overwritten
           before anyone looked. */
        for (size_t j = 0; j < PAIR_REF_COUNT; j++) {
            uint16_t v = 0xFFFF;
            PAIR_REFS[j].get_free(&v);
            snprintf(msg, sizeof(msg), "%s: %s after %s", pass, PAIR_REFS[j].free_key, PAIR_REFS[i].alloc_key);
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(cur[j], v, msg);
        }
    }
}

void test_each_allocation_clamps_only_its_own_partner(void) {
    /* Ascending, then descending. Each lower_to is its own pair's slice
       minus 50, which puts it above the next slice down in both shapes. */
    static const uint16_t ASC_SLICE[] = {100, 200, 300, 400};
    static const uint16_t ASC_LOWER[] = {50, 150, 250, 350};
    static const uint16_t DESC_SLICE[] = {400, 300, 200, 100};
    static const uint16_t DESC_LOWER[] = {350, 250, 150, 50};
    clamp_pass(ASC_SLICE, ASC_LOWER, "ascending slices");
    clamp_pass(DESC_SLICE, DESC_LOWER, "descending slices");
}

/* THE PAIRING, ASSERTED DIRECTLY, not only through arithmetic. Since the
   pairing became one alloc_key string per chore_free_* row, a mispairing
   is a wrong string — and a wrong string leaves the allocation it stole
   the slice from with no slice at all, so the behavioural cases above see
   it as a clamp that never fired. This test says it in one line instead,
   and adds the two things arithmetic cannot see: a string that resolves to
   NOTHING (a rename on either side, which would make both layers silent
   no-ops on a build that still compiles) and a pairing graph that is not
   four disjoint pairs. The expected pairing comes from PAIR_REFS, the
   hand-written list, which is the only thing in this TU that can disagree
   with ha_config.c's registry. */
void test_every_chore_free_row_names_its_own_allocation(void) {
    int n = 0;
    const cfg_field_t *fields = ha_config_fields(&n);
    int paired = 0;
    for (int i = 0; i < n; i++) {
        if (fields[i].alloc_key == NULL)
            continue;
        paired++;
        const cfg_field_t *alloc = field_by_key(fields[i].alloc_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(alloc, fields[i].alloc_key);       /* resolves */
        TEST_ASSERT_EQUAL_MESSAGE(CFG_U16, alloc->kind, fields[i].key); /* to a number */
        /* An allocation must not itself be somebody's slice, or the graph
           is a chain and one write can cascade. */
        TEST_ASSERT_NULL_MESSAGE(alloc->alloc_key, fields[i].key);
    }
    TEST_ASSERT_EQUAL_INT(PAIR_REF_COUNT, paired); /* exactly four pairs, no more */
    for (size_t i = 0; i < PAIR_REF_COUNT; i++) {
        const cfg_field_t *f = field_by_key(PAIR_REFS[i].free_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(f, PAIR_REFS[i].free_key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(PAIR_REFS[i].alloc_key, f->alloc_key, PAIR_REFS[i].free_key);
    }
}

/* The clamp write goes FIRST, before the allocation's own write, so a
   failing NVS leaves BOTH halves as they were. The other order would
   commit the new allocation and then fail to clamp — persisting exactly
   the invalid pair this layer exists to prevent.

   THE OUTCOME DOES NOT PROVE THE ORDER, and this test used to assert only
   the outcome. mock_nvs_fail_writes(1) refuses whichever write goes
   first, so "both halves unchanged after one injected failure" holds under
   EITHER order — a mutation that moved the allocation write ahead of the
   clamp survived that version of this test. The per-key ATTEMPT counts are
   what pin it (mock_nvs_write_count counts attempts, injected failures
   included): the clamp was attempted and refused, and the allocation was
   never attempted at all. */
void test_a_failed_clamp_write_leaves_both_halves_alone(void) {
    char ack[128];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "120", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("chore_free_wd", "120", ack, sizeof(ack)));
    const int alloc_writes = mock_nvs_write_count(NVS_KEY_WEEKDAY_MIN);
    const int free_writes = mock_nvs_write_count(NVS_KEY_CHORE_FREE_WD);
    mock_nvs_fail_writes(1);
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("weekday_min", "30", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nvs\""));
    TEST_ASSERT_EQUAL_INT(free_writes + 1, mock_nvs_write_count(NVS_KEY_CHORE_FREE_WD));
    TEST_ASSERT_EQUAL_INT(alloc_writes, mock_nvs_write_count(NVS_KEY_WEEKDAY_MIN));
    uint16_t alloc_min = 0, free_min = 0;
    nvs_config_get_weekday_min(&alloc_min);
    nvs_config_get_chore_free_wd(&free_min);
    TEST_ASSERT_EQUAL_UINT16(120, alloc_min);
    TEST_ASSERT_EQUAL_UINT16(120, free_min);
}

void test_chore_free_discovery_advertises_the_gated_bounds(void) {
    char ack[128];
    for (size_t i = 0; i < PAIR_REF_COUNT; i++) {
        const cfg_field_t *f = field_by_key(PAIR_REFS[i].free_key);
        char buf[700], want[128];
        TEST_ASSERT_NOT_NULL_MESSAGE(f, PAIR_REFS[i].free_key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("number", f->component, PAIR_REFS[i].free_key);
        ha_config_discovery(buf, sizeof(buf), "magtag-a1b2c3", "Kitchen MagTag", "fw", f);
        /* min 0, not the allocations' 1: 0 is the default every device is
           running, and an advertised min of 1 makes it unselectable. */
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"min\":0"), PAIR_REFS[i].free_key);
        /* max EQUAL to the allocation ceiling, or the off switch is
           unreachable for every allocation above it. */
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"max\":1440"), PAIR_REFS[i].free_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"step\":1"), PAIR_REFS[i].free_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"unit_of_meas\":\"min\""), PAIR_REFS[i].free_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "\"ent_cat\":\"config\""), PAIR_REFS[i].free_key);
        snprintf(want, sizeof(want), "\"cmd_t\":\"magtag/magtag-a1b2c3/set/%s\"", PAIR_REFS[i].free_key);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, want), PAIR_REFS[i].free_key);
    }
    /* And the cfg state document carries all four, or HA renders every
       one of these controls from a missing value. */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("weekday_min", "120", ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("chore_free_wd", "45", ack, sizeof(ack)));
    char state[HA_CONFIG_STATE_MAX];
    ha_config_state_json(state, sizeof(state));
    TEST_ASSERT_NOT_NULL(strstr(state, "\"chore_free_wd\":45"));
    TEST_ASSERT_NOT_NULL(strstr(state, "\"chore_free_we\":0"));
    TEST_ASSERT_NOT_NULL(strstr(state, "\"chore_free_hol\":0"));
    TEST_ASSERT_NOT_NULL(strstr(state, "\"chore_free_sum\":0"));
}

/* HA does not re-read a retained discovery config it has already seen,
   and ha_config_discovery_stale() is what decides whether mqtt_ha.c sends
   one again. The schema version is one of its two inputs: it ORs the
   version against ha_config_discovery_hash(), which folds the firmware
   version string, so any release that changes `fw` republishes all three
   discovery documents whether or not anyone bumped. The bump is still
   required — a same-version reflash moves neither input, and it is the
   only explicit signal — but "without a bump HA never learns" is too
   strong, and the four keys are the thing to pin here.

   They arrived in v21; >= rather than == so a later bump for an unrelated
   entity does not have to edit this line. The joint COUNT+VERSION pin for
   this registry is the test below, and the one for the stat entity table
   is in test_stats_json. */
void test_chore_free_entities_need_the_discovery_schema_bump(void) {
    for (size_t i = 0; i < PAIR_REF_COUNT; i++)
        TEST_ASSERT_NOT_NULL_MESSAGE(field_by_key(PAIR_REFS[i].free_key), PAIR_REFS[i].free_key);
    TEST_ASSERT_TRUE(STATS_JSON_DISC_SCHEMA_VER >= 21);
}

/* THE CONFIG REGISTRY'S BUMP, pinned to the registry it describes — the
   joint pin test_stats_json makes for ENTITIES, which the config registry
   did not have. Without it, the >= assertion above is a one-time pin for
   four specific keys: field #38 could ship with the version left alone and
   pass every test in the tree, and on every device that has already
   published discovery at this firmware version HA would never be told the
   new control exists. Nothing appears, nothing errors.

   The two numbers are asserted TOGETHER, and that is the whole mechanism:
   neither can be edited without landing in this test, where the rule is
   written down. Adding an editable field fails the count; correcting the
   count puts the version on the next line under the author's eyes. It is a
   forcing function, not an implication — a determined editor can change
   both numbers and bump nothing — so: A NEW EDITABLE FIELD MUST BUMP
   STATS_JSON_DISC_SCHEMA_VER.

   HA_CONFIG_SET_SLOTS is the other number the count feeds (ha_config.c
   static-asserts count + 2 <= 48, so the headroom is 9 fields). Do NOT
   raise it to make room: it also sizes mqtt_ha.c's set transport, which
   lives on the net_win task's window heap. */
void test_config_registry_count_moves_with_the_discovery_schema(void) {
    int n = 0;
    (void)ha_config_fields(&n);
    TEST_ASSERT_EQUAL_INT(37, n);
    /* 22 with the registry count unchanged: v22 added stat ENTITIES rows
       (M3-T1's chore entities and config warning), not editable fields —
       the mirror of v21, which moved this registry and left ENTITIES
       alone. test_stats_json's joint pin records that side. */
    TEST_ASSERT_EQUAL_INT(22, STATS_JSON_DISC_SCHEMA_VER);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_set_timer_name_enables_slot);
    RUN_TEST(test_set_timer_name_empty_disables_slot);
    RUN_TEST(test_set_timer_name_too_long_rejected);
    RUN_TEST(test_set_timer_break_eligible);
    RUN_TEST(test_set_timer_break_eligible_rejects_non_onoff);
    RUN_TEST(test_timer_fields_are_independent);
    RUN_TEST(test_set_timer_min_and_reload);
    RUN_TEST(test_set_timer_min_out_of_range_rejected);
    RUN_TEST(test_state_json_includes_timer_fields);
    RUN_TEST(test_discovery_timer_reload_is_switch);
    RUN_TEST(test_set_u16_valid_persists);
    RUN_TEST(test_set_u16_out_of_range_rejected);
    RUN_TEST(test_set_u16_non_numeric_rejected);
    RUN_TEST(test_set_hhmm_valid_and_invalid);
    RUN_TEST(test_set_break_interval_zero_allowed);
    RUN_TEST(test_set_str_valid_and_too_long);
    RUN_TEST(test_set_unknown_key);
    RUN_TEST(test_set_str_rejects_quote_and_backslash);
    RUN_TEST(test_set_timer_name_rejects_quote);
    RUN_TEST(test_set_treload_rejects_non_onoff);
    RUN_TEST(test_hhmm_boundaries);
    RUN_TEST(test_parse_int_trailing_junk_rejected);
    RUN_TEST(test_set_nvs_failure_rejected);
    RUN_TEST(test_set_timer_nvs_failure_rejected);
    RUN_TEST(test_state_json_escapes_specials);
    RUN_TEST(test_state_json_worst_case_fits_firmware_buffer);
    RUN_TEST(test_state_json_reports_current_values);
    RUN_TEST(test_discovery_number_has_command_bounds_and_config_category);
    RUN_TEST(test_every_config_discovery_payload_carries_def_ent_id_and_fits);
    RUN_TEST(test_discovery_text_has_mode);
    RUN_TEST(test_discovery_text_advertises_max_length);
    RUN_TEST(test_timer_name_max_matches_the_reject_boundary);
    RUN_TEST(test_discovery_topic);
    RUN_TEST(test_set_bedtime_accepts_evening_and_zero);
    RUN_TEST(test_set_bedtime_rejects_daytime);
    RUN_TEST(test_set_tone_select_valid_option_persists);
    RUN_TEST(test_set_tone_select_rejects_unknown_and_wrong_case);
    RUN_TEST(test_state_json_emits_tone_option_strings);
    RUN_TEST(test_state_json_clamps_out_of_range_tone_index);
    RUN_TEST(test_discovery_select_lists_options);
    RUN_TEST(test_set_alert_volume_valid_persists);
    RUN_TEST(test_set_alert_volume_out_of_range_rejected);
    RUN_TEST(test_alert_volume_discovery_and_state);
    RUN_TEST(test_set_ota_url_https_persists);
    RUN_TEST(test_set_ota_url_rejects_non_https);
    RUN_TEST(test_set_ota_url_empty_disables_ota);
    RUN_TEST(test_set_ota_url_too_long_rejected);
    RUN_TEST(test_set_ota_on_sync_switch_round_trip);
    RUN_TEST(test_set_ota_on_sync_rejects_non_onoff);
    RUN_TEST(test_set_ota_on_sync_nvs_failure_rejected);
    RUN_TEST(test_state_json_includes_ota_fields);
    RUN_TEST(test_state_json_ota_on_sync_nonzero_reads_as_on);
    RUN_TEST(test_discovery_ota_on_sync_is_switch);
    RUN_TEST(test_discovery_ota_url_is_text_with_max);
    RUN_TEST(test_device_hash_is_deterministic);
    RUN_TEST(test_device_hash_changes_when_firmware_version_changes);
    RUN_TEST(test_device_hash_changes_when_name_changes);
    RUN_TEST(test_device_hash_does_not_confuse_the_field_boundary);
    RUN_TEST(test_device_hash_tolerates_null);
    RUN_TEST(test_discovery_stale_on_a_fresh_device);
    RUN_TEST(test_discovery_not_stale_when_both_match);
    RUN_TEST(test_discovery_stale_on_schema_bump_alone);
    RUN_TEST(test_discovery_stale_on_fingerprint_change_alone);
    RUN_TEST(test_discovery_hash_changes_when_a_slot_is_renamed);
    RUN_TEST(test_discovery_hash_changes_when_a_slot_is_enabled_or_cleared);
    RUN_TEST(test_discovery_hash_does_not_confuse_slot_boundaries);
    RUN_TEST(test_discovery_hash_still_tracks_the_device_block);
    RUN_TEST(test_every_string_field_fits_the_set_transport);
    RUN_TEST(test_every_field_key_fits_the_set_transport);
    RUN_TEST(test_discovery_hash_changes_when_only_the_duration_enables_a_slot);
    RUN_TEST(test_discovery_hash_separates_zero_and_nonzero_duration);
    RUN_TEST(test_discovery_hash_ignores_a_duration_change_that_keeps_it_enabled);
    RUN_TEST(test_discovery_hash_tolerates_an_unterminated_slot_name);
    RUN_TEST(test_edit_with_unreadable_blob_is_refused);
    RUN_TEST(test_every_timer_field_is_accepted_without_a_stored_table);
    RUN_TEST(test_a_control_edit_stamps_only_the_slot_it_touched);
    RUN_TEST(test_the_created_table_is_seeded_from_menuconfig_not_the_running_table);
    RUN_TEST(test_a_refused_edit_does_not_revert_an_earlier_one);
    RUN_TEST(test_state_json_falls_back_to_the_installed_table);
    RUN_TEST(test_discovery_hash_falls_back_to_the_installed_table);
    RUN_TEST(test_state_json_shows_a_name_only_slot);
    RUN_TEST(test_discovery_hash_is_stable_for_a_name_only_slot);
    /* the chore gate's cross-field rule (design 5.3, layers 1 and 2) */
    RUN_TEST(test_chore_free_fields_accept_the_full_range);
    RUN_TEST(test_chore_free_above_its_allocation_is_refused_and_nvs_untouched);
    RUN_TEST(test_chore_free_equal_to_its_allocation_is_the_off_switch);
    RUN_TEST(test_lowering_an_allocation_clamps_the_paired_chore_free);
    RUN_TEST(test_an_allocation_equal_to_its_chore_free_does_not_clamp);
    RUN_TEST(test_raising_an_allocation_leaves_the_chore_free_alone);
    RUN_TEST(test_each_allocation_clamps_only_its_own_partner);
    RUN_TEST(test_every_chore_free_row_names_its_own_allocation);
    RUN_TEST(test_a_failed_clamp_write_leaves_both_halves_alone);
    RUN_TEST(test_chore_free_discovery_advertises_the_gated_bounds);
    RUN_TEST(test_chore_free_entities_need_the_discovery_schema_bump);
    RUN_TEST(test_config_registry_count_moves_with_the_discovery_schema);
    return UNITY_END();
}
