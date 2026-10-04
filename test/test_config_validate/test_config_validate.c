#include <unity.h>

/* Single-TU compilation */
#include "../../main/config_validate.c"

void setUp(void) {}
void tearDown(void) {}

void test_accepts_valid_dates(void) {
    TEST_ASSERT_TRUE(config_is_iso_date("2026-08-20"));
    TEST_ASSERT_TRUE(config_is_iso_date("2026-01-01"));
    TEST_ASSERT_TRUE(config_is_iso_date("2026-12-31"));
    TEST_ASSERT_TRUE(config_is_iso_date("2028-02-29")); /* leap year */
}

void test_rejects_bad_shape(void) {
    TEST_ASSERT_FALSE(config_is_iso_date(NULL));
    TEST_ASSERT_FALSE(config_is_iso_date(""));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-8-20"));  /* not zero-padded */
    TEST_ASSERT_FALSE(config_is_iso_date("2026/08/20")); /* wrong separators */
    TEST_ASSERT_FALSE(config_is_iso_date("not-a-date"));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-08-2x"));
}

void test_rejects_impossible_calendar_dates(void) {
    TEST_ASSERT_FALSE(config_is_iso_date("2026-13-45"));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-00-10"));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-02-00"));
    TEST_ASSERT_FALSE(config_is_iso_date("2026-02-31")); /* Feb has no 31st */
    TEST_ASSERT_FALSE(config_is_iso_date("2026-04-31")); /* April has 30 days */
    TEST_ASSERT_FALSE(config_is_iso_date("2026-02-29")); /* not a leap year */
    TEST_ASSERT_FALSE(config_is_iso_date("2100-02-29")); /* century non-leap */
}

/* ---- https URL (OTA manifest endpoint) ---- */

void test_https_url_accepts_real_endpoints(void) {
    TEST_ASSERT_TRUE(config_is_https_url("https://example.com"));
    TEST_ASSERT_TRUE(config_is_https_url("https://raw.githubusercontent.com/u/r/main/ota/manifest.json"));
    TEST_ASSERT_TRUE(config_is_https_url("https://10.0.0.5:8443/ota.json?dev=magtag-a1b2c3"));
    /* Schemes are case-insensitive per RFC 3986; a URL that works in a
       browser must not be refused here. */
    TEST_ASSERT_TRUE(config_is_https_url("HTTPS://example.com"));
    TEST_ASSERT_TRUE(config_is_https_url("HtTpS://example.com"));
}

void test_https_url_rejects_other_schemes(void) {
    /* Plain http is the whole point of the check: an unauthenticated
       firmware image is an arbitrary-code-execution channel. */
    TEST_ASSERT_FALSE(config_is_https_url("http://example.com/ota.json"));
    TEST_ASSERT_FALSE(config_is_https_url("ftp://example.com/ota.json"));
    TEST_ASSERT_FALSE(config_is_https_url("example.com/ota.json"));
    TEST_ASSERT_FALSE(config_is_https_url("//example.com/ota.json"));
    TEST_ASSERT_FALSE(config_is_https_url(" https://example.com")); /* leading space */
}

void test_https_url_rejects_empty_and_hostless(void) {
    TEST_ASSERT_FALSE(config_is_https_url(NULL));
    TEST_ASSERT_FALSE(config_is_https_url(""));
    TEST_ASSERT_FALSE(config_is_https_url("https://")); /* scheme, no host */
    TEST_ASSERT_FALSE(config_is_https_url("https:/"));
    TEST_ASSERT_FALSE(config_is_https_url("https"));
}

void test_https_url_rejects_unsafe_characters(void) {
    /* Spaces and control chars have no place in a URL, and they are what
       would corrupt the cfg-state JSON or the HTTP request line. */
    TEST_ASSERT_FALSE(config_is_https_url("https://example.com/a b"));
    TEST_ASSERT_FALSE(config_is_https_url("https://example.com/a\nb"));
    TEST_ASSERT_FALSE(config_is_https_url("https://example.com/a\tb"));
    TEST_ASSERT_FALSE(config_is_https_url("https://example.com/\"x\""));
    TEST_ASSERT_FALSE(config_is_https_url("https://example.com/\\x"));
}

/* ---- MQTT broker URI (the setup form's scheme check) ---- */

void test_mqtt_uri_accepts_both_schemes(void) {
    TEST_ASSERT_TRUE(config_is_mqtt_uri("mqtt://broker.local"));
    TEST_ASSERT_TRUE(config_is_mqtt_uri("mqtts://broker.local:8883"));
    TEST_ASSERT_TRUE(config_is_mqtt_uri("mqtt://10.0.0.5:1883"));
    /* Case-insensitive, same as config_is_https_url. */
    TEST_ASSERT_TRUE(config_is_mqtt_uri("MQTT://broker.local"));
    TEST_ASSERT_TRUE(config_is_mqtt_uri("MqTtS://broker.local"));
}

void test_mqtt_uri_rejects_other_schemes(void) {
    TEST_ASSERT_FALSE(config_is_mqtt_uri("http://broker.local"));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("https://broker.local"));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("broker.local"));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("ftp://broker.local"));
    /* "mqtt" and "mqtts" do not falsely prefix-match each other. */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqttx://broker.local"));
}

void test_mqtt_uri_rejects_empty_null_and_hostless(void) {
    TEST_ASSERT_FALSE(config_is_mqtt_uri(NULL));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("")); /* "MQTT off" is mqtt_form.c's rule, not this one's */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://"));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtts://"));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt:/"));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt"));
}

void test_mqtt_uri_rejects_unsafe_characters(void) {
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://broker.local/a b"));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://broker.local/a\nb"));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://broker.local/\"x\""));
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://broker.local/\\x"));
}

/* ---- the strict authority grammar: host[:port][/], nothing else ---- */

void test_mqtt_uri_accepts_host_port_and_trailing_slash(void) {
    TEST_ASSERT_TRUE(config_is_mqtt_uri("mqtt://h"));
    TEST_ASSERT_TRUE(config_is_mqtt_uri("mqtt://h:1883"));
    TEST_ASSERT_TRUE(config_is_mqtt_uri("mqtts://broker.lan:8883/"));
    TEST_ASSERT_TRUE(config_is_mqtt_uri("mqtt://192.168.1.5"));
}

void test_mqtt_uri_rejects_malformed_authority(void) {
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://:1883"));   /* no host */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://@"));       /* no host */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt:///"));       /* no host */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://?x"));      /* no host, query */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://h:abc"));   /* non-numeric port */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://h:99999")); /* port out of range */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://h:0"));     /* port out of range */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://u:p@h"));   /* userinfo */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://h%20x"));   /* '%' */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://h\x7f"));   /* DEL */
    TEST_ASSERT_FALSE(config_is_mqtt_uri("mqtt://h\xff"));   /* byte >= 0x80 */
}

void test_mqtt_uri_check_returns_the_specific_reason(void) {
    TEST_ASSERT_EQUAL(CONFIG_MQTT_URI_OK, config_mqtt_uri_check("mqtt://h"));
    TEST_ASSERT_EQUAL(CONFIG_MQTT_URI_BAD_SCHEME, config_mqtt_uri_check("ftp://h"));
    TEST_ASSERT_EQUAL(CONFIG_MQTT_URI_NO_HOST, config_mqtt_uri_check("mqtt://"));
    TEST_ASSERT_EQUAL(CONFIG_MQTT_URI_BAD_PORT, config_mqtt_uri_check("mqtt://h:0"));
    TEST_ASSERT_EQUAL(CONFIG_MQTT_URI_BAD_CHAR, config_mqtt_uri_check("mqtt://h/path"));
}

/* ---- OTA URL field rule (the one both apply paths share) ---- */

void test_ota_url_allows_empty_as_the_off_switch(void) {
    /* Empty is how OTA is disabled — the only way to turn it off from HA,
       so it must stay valid. */
    TEST_ASSERT_TRUE(config_is_ota_url(""));
}

void test_ota_url_otherwise_matches_the_https_rule(void) {
    TEST_ASSERT_TRUE(config_is_ota_url("https://example.com/ota.json"));
    TEST_ASSERT_FALSE(config_is_ota_url("http://example.com/ota.json"));
    TEST_ASSERT_FALSE(config_is_ota_url("example.com"));
    TEST_ASSERT_FALSE(config_is_ota_url(NULL));
}

/* ---- JSON-safety rule shared by the set path and the bulk `ver` ---- */

void test_clean_str_accepts_ordinary_values(void) {
    TEST_ASSERT_TRUE(config_is_clean_str("")); /* empty disables a slot */
    TEST_ASSERT_TRUE(config_is_clean_str("20260811"));
    TEST_ASSERT_TRUE(config_is_clean_str("Kitchen MagTag"));
    TEST_ASSERT_TRUE(config_is_clean_str("EST5EDT,M3.2.0,M11.1.0"));
}

void test_clean_str_rejects_json_breaking_characters(void) {
    TEST_ASSERT_FALSE(config_is_clean_str(NULL));
    TEST_ASSERT_FALSE(config_is_clean_str("a\"b")); /* would end the string early */
    TEST_ASSERT_FALSE(config_is_clean_str("a\\b"));
    TEST_ASSERT_FALSE(config_is_clean_str("a\nb"));
    TEST_ASSERT_FALSE(config_is_clean_str("a\tb"));
}

/* ---- chore_free <= allocation (MINUTES), the rule both chore setters share ---- */

void test_chore_free_below_allocation_is_valid(void) {
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(30, 60));
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(1, 1440));
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(1439, 1440));
}

void test_chore_free_equal_to_allocation_is_the_off_switch(void) {
    /* Design 3.3: equal means nothing is withheld — the per-day-type off
       switch, expressed without an extra key. One character from the
       rejected case below, so both sides of the boundary are pinned. */
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(60, 60));
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(1, 1));
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(CFG_BOUND_ALLOC_HI, CFG_BOUND_ALLOC_HI));
}

void test_chore_free_one_over_allocation_is_invalid(void) {
    /* Withholding more than the day grants cannot mean anything, so the
       device refuses to guess rather than picking an interpretation. */
    TEST_ASSERT_FALSE(config_is_valid_chore_free_min(61, 60));
    TEST_ASSERT_FALSE(config_is_valid_chore_free_min(2, 1));
    TEST_ASSERT_FALSE(config_is_valid_chore_free_min(1440, 1439));
}

void test_chore_free_far_over_allocation_is_invalid(void) {
    TEST_ASSERT_FALSE(config_is_valid_chore_free_min(1440, 1));
    TEST_ASSERT_FALSE(config_is_valid_chore_free_min(CFG_BOUND_CHORE_FREE_HI, CFG_BOUND_ALLOC_LO));
    TEST_ASSERT_FALSE(config_is_valid_chore_free_min(600, 120));
}

void test_chore_free_zero_is_valid_against_any_allocation(void) {
    /* The default, and therefore the live value on every device in the
       field: it must validate against every allocation that exists. */
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(0, CFG_BOUND_ALLOC_LO));
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(0, CFG_BOUND_ALLOC_HI));
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(0, 0));
    /* uint32_t counter on purpose: a uint16_t one cannot terminate if
       CFG_BOUND_ALLOC_HI ever reaches 65535. */
    for (uint32_t alloc = CFG_BOUND_ALLOC_LO; alloc <= CFG_BOUND_ALLOC_HI; alloc++)
        TEST_ASSERT_TRUE(config_is_valid_chore_free_min(0, (uint16_t)alloc));
}

void test_chore_free_predicate_does_not_range_check(void) {
    /* Deliberate: this answers ONLY the cross-field question. The M2
       config-error gate has to be able to ask about a pair already
       sitting in NVS that never passed CFG_BOUND_CHORE_FREE_HI (older
       firmware, a bug in either setter), so an internally consistent pair
       must answer VALID however far out of range it sits. A "helpful"
       ceiling check added inside the predicate breaks exactly that case,
       and these assertions are what catch it. */
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(1500, 1500));
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(65535, 65535));
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(2000, 3000));
    /* Out of range still does not make an inconsistent pair valid. */
    TEST_ASSERT_FALSE(config_is_valid_chore_free_min(3000, 2000));
}

void test_chore_free_range_ends_validate(void) {
    /* Both ends of the advertised 0..1440 range are reachable: 0 against
       the smallest allocation, 1440 against the largest. */
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(CFG_BOUND_CHORE_FREE_LO, CFG_BOUND_ALLOC_LO));
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(CFG_BOUND_CHORE_FREE_HI, CFG_BOUND_ALLOC_HI));
}

void test_chore_free_bounds_keep_the_field_default_legal(void) {
    /* The asymmetry is load-bearing: an allocation of zero is not a
       thing, but a chore_free of zero is the default every device in the
       field is running, so copying CFG_BOUND_ALLOC_LO here would reject
       an incoming `chore_free_*: 0` and make 0 unselectable in HA. Pin
       the chore property itself — not its relation to the allocation LO,
       which is an unrelated constant this test has no business failing
       on. */
    TEST_ASSERT_EQUAL_UINT16(0, CFG_BOUND_CHORE_FREE_LO);
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(CFG_BOUND_CHORE_FREE_LO, CFG_BOUND_ALLOC_LO));
}

void test_chore_free_ceiling_keeps_the_off_switch_reachable(void) {
    /* The two ceilings must be EQUAL, not merely ordered. A chore_free
       ceiling BELOW the allocation ceiling compiles clean and breaks
       nothing visible, but it makes `chore_free == allocation` — the
       per-day-type off switch (design 3.3) — unreachable for every
       allocation above it, because HA would never advertise the matching
       number. That is the feature this module exists to protect. */
    TEST_ASSERT_EQUAL_UINT16(CFG_BOUND_ALLOC_HI, CFG_BOUND_CHORE_FREE_HI);
    TEST_ASSERT_TRUE(config_is_valid_chore_free_min(CFG_BOUND_ALLOC_HI, CFG_BOUND_ALLOC_HI));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_accepts_valid_dates);
    RUN_TEST(test_rejects_bad_shape);
    RUN_TEST(test_rejects_impossible_calendar_dates);
    RUN_TEST(test_https_url_accepts_real_endpoints);
    RUN_TEST(test_https_url_rejects_other_schemes);
    RUN_TEST(test_https_url_rejects_empty_and_hostless);
    RUN_TEST(test_https_url_rejects_unsafe_characters);
    RUN_TEST(test_mqtt_uri_accepts_both_schemes);
    RUN_TEST(test_mqtt_uri_rejects_other_schemes);
    RUN_TEST(test_mqtt_uri_rejects_empty_null_and_hostless);
    RUN_TEST(test_mqtt_uri_rejects_unsafe_characters);
    RUN_TEST(test_mqtt_uri_accepts_host_port_and_trailing_slash);
    RUN_TEST(test_mqtt_uri_rejects_malformed_authority);
    RUN_TEST(test_mqtt_uri_check_returns_the_specific_reason);
    RUN_TEST(test_ota_url_allows_empty_as_the_off_switch);
    RUN_TEST(test_ota_url_otherwise_matches_the_https_rule);
    RUN_TEST(test_clean_str_accepts_ordinary_values);
    RUN_TEST(test_clean_str_rejects_json_breaking_characters);
    RUN_TEST(test_chore_free_below_allocation_is_valid);
    RUN_TEST(test_chore_free_equal_to_allocation_is_the_off_switch);
    RUN_TEST(test_chore_free_one_over_allocation_is_invalid);
    RUN_TEST(test_chore_free_far_over_allocation_is_invalid);
    RUN_TEST(test_chore_free_zero_is_valid_against_any_allocation);
    RUN_TEST(test_chore_free_predicate_does_not_range_check);
    RUN_TEST(test_chore_free_range_ends_validate);
    RUN_TEST(test_chore_free_bounds_keep_the_field_default_legal);
    RUN_TEST(test_chore_free_ceiling_keeps_the_off_switch_reachable);
    return UNITY_END();
}
