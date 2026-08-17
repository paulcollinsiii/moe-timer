/* Redirect acceptance for the OTA transport.
 *
 * This suite exists because the rule it covers is the only decision in
 * main/ota.c's job that does not need a radio, and it is the one that
 * matters most: config_is_https_url() closes the plaintext-firmware hole
 * at the config boundary, and esp_http_client_set_redirection() reopens
 * it one hop later with no scheme check of its own. Left inside ota.c the
 * rule would be reachable only from a hardware smoke test, which is to
 * say it would ship unasserted.
 *
 * Every case below is written against a specific way of getting it wrong,
 * and each one was mutation-checked: the defect is introduced, the suite
 * must fail, the defect is reverted. A case that survives its own
 * mutation is not evidence of anything.
 */
#include <unity.h>

/* Single-TU compilation. config_validate.c comes first because ota_url.c
   composes with it — deliberately, so that the https rule has exactly one
   implementation and this suite is exercising the real one rather than a
   restatement of it. */
#include "../../main/config_validate.c"
#include "../../main/ota_url.c"

void setUp(void) {}
void tearDown(void) {}

/* ---- the case the module exists for ---- */

void test_refuses_a_downgrade_to_plain_http(void) {
    /* The attack and the misconfiguration are the same bytes: an
       endpoint that validated as https answers 302 with an http target.
       Following it fetches firmware in the clear. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("http://evil.example/fw.bin", 0, 3));
    /* Case-insensitively, because the scheme is. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("HTTP://evil.example/fw.bin", 0, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("HtTp://evil.example/fw.bin", 0, 3));
}

void test_refuses_every_other_scheme_too(void) {
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("ftp://host/fw.bin", 0, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("file:///dev/zero", 0, 3));
    /* Nearly-https shapes that a prefix comparison written by hand would
       wave through. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("https:/host/fw.bin", 0, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("https://", 0, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check(" https://host/fw.bin", 0, 3));
}

void test_refuses_relative_targets(void) {
    /* Documented, deliberate, and the reason is in ota_url.h: following a
       relative target would in fact be safe (the scheme is inherited), but
       accepting it means a second implementation of the character rule,
       and a security rule with two implementations drifts. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("/firmware/1.6.0.bin", 0, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("1.6.0.bin", 0, 3));
    /* Protocol-relative: same host rules, no scheme of its own. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check("//other.example/fw.bin", 0, 3));
}

void test_follows_a_real_https_target(void) {
    TEST_ASSERT_EQUAL(OTA_REDIRECT_FOLLOW, ota_url_redirect_check("https://host/fw.bin", 0, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_FOLLOW, ota_url_redirect_check("HTTPS://host/fw.bin", 0, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_FOLLOW, ota_url_redirect_check("https://10.0.0.5:8443/ota/magtag-1.6.0.bin", 2, 3));
}

/* ---- the bound that keeps the loop finite ---- */

void test_spends_a_budget_and_then_stops(void) {
    /* esp_https_ota's own connect loop has NO redirect cap: it loops
       while the status says "redirect" and never consults
       max_redirection_count. A host answering 302 with a Location
       pointing at itself would spin until the awake failsafe fired. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_FOLLOW, ota_url_redirect_check("https://host/a", 2, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_MANY, ota_url_redirect_check("https://host/a", 3, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_MANY, ota_url_redirect_check("https://host/a", 9, 3));
}

void test_a_zero_budget_follows_nothing(void) {
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_MANY, ota_url_redirect_check("https://host/a", 0, 0));
}

void test_a_corrupt_hop_count_fails_closed(void) {
    /* "I do not know how many hops I have taken" reads as "too many". */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_MANY, ota_url_redirect_check("https://host/a", -1, 3));
}

/* ---- length, which is a refusal and not a truncation ---- */

void test_refuses_a_target_that_would_not_fit(void) {
    char url[OTA_URL_MAX + 64];
    size_t prefix = strlen("https://host/");
    memset(url, 'a', sizeof(url));
    memcpy(url, "https://host/", prefix);

    /* One under the buffer: fits, with room for the NUL. */
    url[OTA_URL_MAX - 1] = '\0';
    TEST_ASSERT_EQUAL(OTA_REDIRECT_FOLLOW, ota_url_redirect_check(url, 0, 3));

    /* Exactly the buffer width, and beyond: refused rather than copied in
       truncated form and followed as some URL nobody named. */
    url[OTA_URL_MAX - 1] = 'a';
    url[OTA_URL_MAX] = '\0';
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_LONG, ota_url_redirect_check(url, 0, 3));

    url[OTA_URL_MAX] = 'a';
    url[sizeof(url) - 1] = '\0';
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_LONG, ota_url_redirect_check(url, 0, 3));
}

/* The order of the OTHER two, which the suite claimed to pin and did
   not: swapping the length and budget checks (while leaving the scheme
   check first) survived all eleven cases. No security consequence —
   both mutants still refuse — but "seven mutations, no survivors"
   overstated what was actually asserted, and an unasserted ordering is
   an ordering that will drift.
   TOO_LONG wins because it names the TARGET, which the operator can go
   and look at; TOO_MANY names only a limit this firmware chose. */
void test_length_beats_the_budget(void) {
    char url[OTA_URL_MAX + 64];
    size_t prefix = strlen("https://host/");
    memset(url, 'a', sizeof(url));
    memcpy(url, "https://host/", prefix);
    url[sizeof(url) - 1] = '\0';

    /* Over-long AND out of hops: the length is the answer. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_LONG, ota_url_redirect_check(url, 3, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_LONG, ota_url_redirect_check(url, 9, 3));
    /* Over-long against a budget that follows nothing at all. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_LONG, ota_url_redirect_check(url, 0, 0));
    /* And a corrupt hop count still loses to it, so the precedence is
       about the checks rather than about the sign of the counter. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_TOO_LONG, ota_url_redirect_check(url, -1, 3));
}

void test_scheme_beats_length_and_budget(void) {
    /* Precedence is a decision, not an accident: "your host redirected
       firmware to plain http" is the refusal that names something the
       operator must go and fix, and it is equally true at hop 9 with a
       500-character target. The other two name limits this firmware
       chose. */
    char url[OTA_URL_MAX + 64];
    memset(url, 'a', sizeof(url));
    memcpy(url, "http://evil.example/", strlen("http://evil.example/"));
    url[sizeof(url) - 1] = '\0';
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NOT_HTTPS, ota_url_redirect_check(url, 9, 3));
}

/* ---- a 3xx that names nowhere ---- */

void test_refuses_a_redirect_with_no_target(void) {
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NO_TARGET, ota_url_redirect_check(NULL, 0, 3));
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NO_TARGET, ota_url_redirect_check("", 0, 3));
    /* And a NULL is answered even when everything else is also wrong,
       because the caller may hand over the header value it never got. */
    TEST_ASSERT_EQUAL(OTA_REDIRECT_NO_TARGET, ota_url_redirect_check(NULL, 99, 0));
}

/* ---- the strings that reach the log ---- */

void test_every_outcome_has_a_distinct_code(void) {
    TEST_ASSERT_EQUAL_STRING("", ota_url_redirect_str(OTA_REDIRECT_FOLLOW));
    TEST_ASSERT_EQUAL_STRING("no_target", ota_url_redirect_str(OTA_REDIRECT_NO_TARGET));
    TEST_ASSERT_EQUAL_STRING("not_https", ota_url_redirect_str(OTA_REDIRECT_NOT_HTTPS));
    TEST_ASSERT_EQUAL_STRING("too_long", ota_url_redirect_str(OTA_REDIRECT_TOO_LONG));
    TEST_ASSERT_EQUAL_STRING("too_many", ota_url_redirect_str(OTA_REDIRECT_TOO_MANY));
    /* Out of range is "" rather than a read past the table. */
    TEST_ASSERT_EQUAL_STRING("", ota_url_redirect_str((ota_redirect_t)99));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_refuses_a_downgrade_to_plain_http);
    RUN_TEST(test_refuses_every_other_scheme_too);
    RUN_TEST(test_refuses_relative_targets);
    RUN_TEST(test_follows_a_real_https_target);
    RUN_TEST(test_spends_a_budget_and_then_stops);
    RUN_TEST(test_a_zero_budget_follows_nothing);
    RUN_TEST(test_a_corrupt_hop_count_fails_closed);
    RUN_TEST(test_refuses_a_target_that_would_not_fit);
    RUN_TEST(test_length_beats_the_budget);
    RUN_TEST(test_scheme_beats_length_and_budget);
    RUN_TEST(test_refuses_a_redirect_with_no_target);
    RUN_TEST(test_every_outcome_has_a_distinct_code);
    return UNITY_END();
}
