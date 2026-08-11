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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_accepts_valid_dates);
    RUN_TEST(test_rejects_bad_shape);
    RUN_TEST(test_rejects_impossible_calendar_dates);
    RUN_TEST(test_https_url_accepts_real_endpoints);
    RUN_TEST(test_https_url_rejects_other_schemes);
    RUN_TEST(test_https_url_rejects_empty_and_hostless);
    RUN_TEST(test_https_url_rejects_unsafe_characters);
    RUN_TEST(test_ota_url_allows_empty_as_the_off_switch);
    RUN_TEST(test_ota_url_otherwise_matches_the_https_rule);
    return UNITY_END();
}
