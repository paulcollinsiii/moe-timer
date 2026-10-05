#include <stdio.h>
#include <string.h>
#include <unity.h>

/* Single-TU compilation. config_validate.c comes first because
   mqtt_form.c composes with it for config_is_mqtt_uri — the scheme rule
   under test is the real one, not a restatement (same reason
   test_ota_url carries config_validate.c). */
#include "../../main/config_validate.c"
#include "../../main/mqtt_form.c"
#include "cJSON.h"

void setUp(void) {}
void tearDown(void) {}

/* Builds "mqtt://" + (total_len - 7) filler host bytes, NUL-terminated.
   Used to pin the exact length boundaries the plan calls out (127
   accepted / 128 rejected for uri, and the matching pair for user/pass
   at their own limits). total_len must be >= 7. */
static void build_mqtt_uri_of_len(char *buf, size_t total_len) {
    memcpy(buf, "mqtt://", 7);
    memset(buf + 7, 'a', total_len - 7);
    buf[total_len] = '\0';
}

/* ---- urlencoded: the submission shape ---- */

void test_urlencoded_accepts_full_submission(void) {
    static const char body[] = "uri=mqtt://broker.local&user=bob&pass=secret";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.local", out.uri);
    TEST_ASSERT_EQUAL_STRING("bob", out.user);
    TEST_ASSERT_EQUAL_STRING("secret", out.pass);
    TEST_ASSERT_FALSE(out.keep_pass);
}

void test_urlencoded_percent_decodes_and_plus_decodes(void) {
    /* %3A%2F%2F = "://", "+" in user = space, %40 in pass = '@'. */
    static const char body[] = "uri=mqtt%3A%2F%2Fbroker.local&user=a+b&pass=p%40ss";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.local", out.uri);
    TEST_ASSERT_EQUAL_STRING("a b", out.user);
    TEST_ASSERT_EQUAL_STRING("p@ss", out.pass);
}

void test_urlencoded_field_order_does_not_matter(void) {
    static const char body[] = "pass=secret&user=bob&uri=mqtt://broker.local";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.local", out.uri);
}

/* ---- empty/missing uri: "MQTT off" vs. a malformed submission ---- */

void test_urlencoded_empty_uri_disables_mqtt_and_clears_user_pass(void) {
    static const char body[] = "uri=&user=bob&pass=secret";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("", out.uri);
    TEST_ASSERT_EQUAL_STRING("", out.user);
    TEST_ASSERT_EQUAL_STRING("", out.pass);
    TEST_ASSERT_FALSE(out.keep_pass);
}

void test_urlencoded_missing_uri_field_is_an_error(void) {
    static const char body[] = "user=bob&pass=secret"; /* no "uri" key at all */
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_MISSING, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

void test_urlencoded_empty_body_is_missing_uri(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded("", 0, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_MISSING, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

/* ---- blank/absent pass: "keep the current one" ---- */

void test_urlencoded_blank_pass_keeps_existing(void) {
    static const char body[] = "uri=mqtt://broker.local&user=bob&pass=";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_TRUE(out.keep_pass);
    TEST_ASSERT_EQUAL_STRING("", out.pass);
}

void test_urlencoded_absent_pass_keeps_existing(void) {
    static const char body[] = "uri=mqtt://broker.local&user=bob";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_TRUE(out.keep_pass);
}

void test_urlencoded_literal_pass_is_not_kept(void) {
    static const char body[] = "uri=mqtt://broker.local&pass=newsecret";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_FALSE(out.keep_pass);
    TEST_ASSERT_EQUAL_STRING("newsecret", out.pass);
}

/* ---- uri scheme/character rules, and telling the two apart ---- */

void test_urlencoded_rejects_bad_scheme(void) {
    static const char body[] = "uri=ftp://broker.local";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_SCHEME, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

void test_urlencoded_rejects_scheme_with_no_host(void) {
    static const char body[] = "uri=mqtt://";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_SCHEME, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

void test_urlencoded_rejects_bad_char_in_uri_after_valid_scheme(void) {
    /* %20 decodes to a literal space; the right scheme is present, so
       this must be reported as BAD_CHAR, not BAD_SCHEME. */
    static const char body[] = "uri=mqtt://broker.local/a%20b";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_CHAR, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

/* ---- user/pass: control characters only, nothing else ---- */

void test_urlencoded_rejects_control_char_in_user(void) {
    static const char body[] = "uri=mqtt://broker.local&user=a%09b"; /* tab */
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_CHAR, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);
}

void test_urlencoded_rejects_control_char_in_pass(void) {
    static const char body[] = "uri=mqtt://broker.local&pass=a%0Ab"; /* newline */
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_CHAR, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_PASS, st.field);
}

void test_urlencoded_user_may_be_empty(void) {
    /* Anonymous broker: an empty user is not an error. */
    static const char body[] = "uri=mqtt://broker.local&user=&pass=secret";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("", out.user);
}

void test_urlencoded_user_and_pass_allow_quotes_and_backslash(void) {
    /* Spec: user/pass forbid only control characters, unlike the uri
       rule, which also forbids quote and backslash. A broker password
       containing either must not be refused. */
    static const char body[] = "uri=mqtt://broker.local&user=a%22b&pass=c%5Cd";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("a\"b", out.user);
    TEST_ASSERT_EQUAL_STRING("c\\d", out.pass);
}

/* ---- percent-decoding edge cases ---- */

void test_urlencoded_percent_edge_cases_are_malformed(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    static const char bare_percent[] = "uri=mqtt://broker.local&user=100%";
    st = mqtt_form_parse_urlencoded(bare_percent, strlen(bare_percent), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_MALFORMED_ENCODING, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    static const char one_hex_digit[] = "uri=mqtt://broker.local&user=10%4";
    st = mqtt_form_parse_urlencoded(one_hex_digit, strlen(one_hex_digit), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_MALFORMED_ENCODING, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    static const char bad_hex_digits[] = "uri=mqtt://broker.local&user=10%zz";
    st = mqtt_form_parse_urlencoded(bad_hex_digits, strlen(bad_hex_digits), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_MALFORMED_ENCODING, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    static const char decoded_nul[] = "uri=mqtt://broker.local&user=10%00";
    st = mqtt_form_parse_urlencoded(decoded_nul, strlen(decoded_nul), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_MALFORMED_ENCODING, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);
}

/* ---- unknown fields and duplicates ---- */

void test_urlencoded_ignores_unknown_fields_even_if_malformed(void) {
    /* The unknown field's own value is never decoded or checked. */
    static const char body[] = "uri=mqtt://broker.local&foo=bad%zz%encoding&user=bob";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("bob", out.user);
}

void test_urlencoded_rejects_duplicate_known_field(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    static const char dup_uri[] = "uri=mqtt://a.local&uri=mqtt://b.local";
    st = mqtt_form_parse_urlencoded(dup_uri, strlen(dup_uri), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);

    static const char dup_user[] = "uri=mqtt://a.local&user=a&user=b";
    st = mqtt_form_parse_urlencoded(dup_user, strlen(dup_user), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);
}

void test_urlencoded_pass_key_with_no_equals_means_keep(void) {
    /* "pass" present with no "=" at all decodes to the same empty value
       as "pass=" -- both mean "keep the current password". */
    static const char body[] = "uri=mqtt://h&pass";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_TRUE(out.keep_pass);
    TEST_ASSERT_EQUAL_STRING("", out.pass);
}

/* ---- raw embedded NUL: a decoded-NUL-shaped hazard without a %00 ---- */

void test_urlencoded_rejects_raw_nul_byte(void) {
    /* A literal 0x00 byte in the body, not a "%00" escape -- decode_value
       never reaches it on this field, so the guard has to run before any
       field is decoded, over the whole body. */
    char body[24];
    size_t n = 0;
    memcpy(body + n, "uri=mqtt://h", 12);
    n += 12;
    body[n++] = '\0';
    memcpy(body + n, "junk", 4);
    n += 4;

    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_MALFORMED_ENCODING, st.err);
}

/* ---- empty uri: syntax errors in other fields still fail ---- */

void test_urlencoded_empty_uri_still_checks_other_fields_syntax(void) {
    /* The empty-uri shortcut in finalize() skips the CONTENT rule
       (control characters) it would otherwise apply to user/pass -- it
       does not skip the SYNTAX rules the decode step enforces on every
       submission regardless of uri. */
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    char too_long[16 + MQTT_FORM_USER_MAX];
    memcpy(too_long, "uri=&user=", 10);
    memset(too_long + 10, 'a', MQTT_FORM_USER_MAX);
    st = mqtt_form_parse_urlencoded(too_long, 10 + MQTT_FORM_USER_MAX, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    static const char malformed[] = "uri=&user=%zz";
    st = mqtt_form_parse_urlencoded(malformed, strlen(malformed), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_MALFORMED_ENCODING, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    static const char dup[] = "uri=&user=a&user=b";
    st = mqtt_form_parse_urlencoded(dup, strlen(dup), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    /* %01 decodes to a control character: this is the one check the
       empty-uri shortcut DOES skip, so this one succeeds. */
    static const char control_char[] = "uri=&user=%01";
    st = mqtt_form_parse_urlencoded(control_char, strlen(control_char), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
}

/* ---- the strict uri classifier (config_mqtt_uri_check), end to end ---- */

void test_urlencoded_rejects_malformed_mqtt_uri_variants(void) {
    static const char *const bad[] = {
        "uri=mqtt://:1883", "uri=mqtt://@",       "uri=mqtt:///",    "uri=mqtt://?x",
        "uri=mqtt://h:abc", "uri=mqtt://h:99999", "uri=mqtt://h:0",  "uri=mqtt://u:p@h",
        "uri=mqtt://h%25x", "uri=mqtt://h%7F",    "uri=mqtt://h%FF",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        mqtt_form_result_t out;
        mqtt_form_status_t st = mqtt_form_parse_urlencoded(bad[i], strlen(bad[i]), &out);
        TEST_ASSERT_TRUE(st.err != MQTT_FORM_ERR_NONE);
        TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
    }
}

void test_urlencoded_accepts_strict_mqtt_uri_variants(void) {
    static const char *const good[] = {
        "uri=mqtt://h",
        "uri=mqtt://h:1883",
        "uri=mqtts://broker.lan:8883/",
        "uri=mqtt://192.168.1.5",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        mqtt_form_result_t out;
        mqtt_form_status_t st = mqtt_form_parse_urlencoded(good[i], strlen(good[i]), &out);
        TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    }
}

void test_urlencoded_rejects_bad_port_as_bad_char(void) {
    /* Pinned separately from the variants list above: a bad port is a
       character-level problem with what follows the scheme, so it must
       report BAD_CHAR (pointing at the uri field's content), not
       BAD_SCHEME (which would read as "you typed the wrong scheme"). */
    static const char body[] = "uri=mqtt://h:99999";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_CHAR, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

/* ---- the failure contract: *out is zeroed, including a password ---- */

void test_failed_parse_zeroes_output_struct(void) {
    static const char body[] = "pass=secret&uri=ftp://x";
    mqtt_form_result_t out;
    memset(&out, 0xAA, sizeof(out)); /* poison, so a missed field shows up */
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_SCHEME, st.err);
    TEST_ASSERT_EQUAL_STRING("", out.uri);
    TEST_ASSERT_EQUAL_STRING("", out.user);
    TEST_ASSERT_EQUAL_STRING("", out.pass);
    TEST_ASSERT_FALSE(out.keep_pass);
}

/* ---- exact length boundaries ---- */

void test_urlencoded_uri_length_boundary(void) {
    char uri127[128], uri128[129];
    build_mqtt_uri_of_len(uri127, 127);
    build_mqtt_uri_of_len(uri128, 128);

    char body[160];
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    int n = snprintf(body, sizeof(body), "uri=%s", uri127);
    st = mqtt_form_parse_urlencoded(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL(127u, strlen(out.uri));

    n = snprintf(body, sizeof(body), "uri=%s", uri128);
    st = mqtt_form_parse_urlencoded(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

void test_urlencoded_user_and_pass_length_boundary(void) {
    char user63[64], user64[65];
    memset(user63, 'b', 63);
    user63[63] = '\0';
    memset(user64, 'b', 64);
    user64[64] = '\0';

    char body[128];
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    int n = snprintf(body, sizeof(body), "uri=mqtt://h&user=%s", user63);
    st = mqtt_form_parse_urlencoded(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL(63u, strlen(out.user));

    n = snprintf(body, sizeof(body), "uri=mqtt://h&user=%s", user64);
    st = mqtt_form_parse_urlencoded(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    /* pass shares USER's buffer size (64); pin it independently rather
       than assume the two fields can never drift apart. */
    n = snprintf(body, sizeof(body), "uri=mqtt://h&pass=%s", user63);
    st = mqtt_form_parse_urlencoded(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);

    n = snprintf(body, sizeof(body), "uri=mqtt://h&pass=%s", user64);
    st = mqtt_form_parse_urlencoded(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_PASS, st.field);
}

/* ---- the request body cap, independent of field content ---- */

void test_urlencoded_body_at_cap_is_accepted_over_cap_is_rejected(void) {
    static const char prefix[] = "uri=mqtt://broker.local&filler=";
    char body[MQTT_FORM_BODY_MAX + 2];
    size_t prefix_len = strlen(prefix);
    memcpy(body, prefix, prefix_len);
    memset(body + prefix_len, 'a', sizeof(body) - prefix_len);

    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, MQTT_FORM_BODY_MAX, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err); /* exactly at the cap: fine */

    st = mqtt_form_parse_urlencoded(body, MQTT_FORM_BODY_MAX + 1, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BODY_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_NONE, st.field);
}

void test_urlencoded_worst_case_percent_encoded_body_is_accepted(void) {
    /* The body MQTT_FORM_BODY_MAX's header comment derives: uri, user and
       pass each at their own MAX, every byte of every one of them
       percent-encoded. Pins the arithmetic (775 for today's sizes) and
       that the cap set from it actually accepts the body it was raised
       for. */
    char body[MQTT_FORM_BODY_MAX];
    size_t n = 0;

    memcpy(body + n, "uri=", 4);
    n += 4;
    {
        char decoded[MQTT_FORM_URI_MAX];
        memcpy(decoded, "mqtt://", 7);
        memset(decoded + 7, 'h', MQTT_FORM_URI_MAX - 1 - 7);
        for (size_t i = 0; i < MQTT_FORM_URI_MAX - 1; i++)
            n += (size_t)sprintf(body + n, "%%%02X", (unsigned char)decoded[i]);
    }

    memcpy(body + n, "&user=", 6);
    n += 6;
    for (size_t i = 0; i < MQTT_FORM_USER_MAX - 1; i++)
        n += (size_t)sprintf(body + n, "%%61");

    memcpy(body + n, "&pass=", 6);
    n += 6;
    for (size_t i = 0; i < MQTT_FORM_PASS_MAX - 1; i++)
        n += (size_t)sprintf(body + n, "%%62");

    TEST_ASSERT_EQUAL(775u, n);
    TEST_ASSERT_TRUE(n <= MQTT_FORM_BODY_MAX);

    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_urlencoded(body, n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL(127u, strlen(out.uri));
    TEST_ASSERT_EQUAL(63u, strlen(out.user));
    TEST_ASSERT_EQUAL(63u, strlen(out.pass));
}

/* ---- JSON body: the same three keys, the same validated result ---- */

void test_json_accepts_full_submission(void) {
    static const char body[] = "{\"uri\":\"mqtt://broker.local\",\"user\":\"bob\",\"pass\":\"secret\"}";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_json(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.local", out.uri);
    TEST_ASSERT_EQUAL_STRING("bob", out.user);
    TEST_ASSERT_EQUAL_STRING("secret", out.pass);
    TEST_ASSERT_FALSE(out.keep_pass);
}

void test_json_missing_uri_is_an_error(void) {
    static const char body[] = "{\"user\":\"bob\"}";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_json(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_MISSING, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

void test_json_empty_body_is_bad_json(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_json("", 0, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_NONE, st.field);
}

void test_json_empty_uri_disables_mqtt_and_clears_user_pass(void) {
    static const char body[] = "{\"uri\":\"\",\"user\":\"bob\",\"pass\":\"secret\"}";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_json(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("", out.uri);
    TEST_ASSERT_EQUAL_STRING("", out.user);
    TEST_ASSERT_EQUAL_STRING("", out.pass);
    TEST_ASSERT_FALSE(out.keep_pass);
}

void test_json_blank_or_absent_pass_keeps_existing(void) {
    static const char blank[] = "{\"uri\":\"mqtt://broker.local\",\"pass\":\"\"}";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_json(blank, strlen(blank), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_TRUE(out.keep_pass);

    static const char absent[] = "{\"uri\":\"mqtt://broker.local\"}";
    st = mqtt_form_parse_json(absent, strlen(absent), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_TRUE(out.keep_pass);
}

void test_json_rejects_non_string_values(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    static const char num_uri[] = "{\"uri\":123}";
    st = mqtt_form_parse_json(num_uri, strlen(num_uri), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);

    static const char bool_user[] = "{\"uri\":\"mqtt://broker.local\",\"user\":true}";
    st = mqtt_form_parse_json(bool_user, strlen(bool_user), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    /* A number value for pass, same shape as num_uri/bool_user above:
       not nested, so this one does reach the per-field type check and
       names the field. */
    static const char num_pass[] = "{\"uri\":\"mqtt://broker.local\",\"pass\":5}";
    st = mqtt_form_parse_json(num_pass, strlen(num_pass), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_PASS, st.field);

    /* An array value, unlike the three above, IS nesting: the body-level
       pre-scan (this format's one-flat-object-of-strings rule) rejects
       it before cJSON ever runs, so this is a body-level failure
       (FIELD_NONE), not a per-field one. */
    static const char arr_pass[] = "{\"uri\":\"mqtt://broker.local\",\"pass\":[1,2]}";
    st = mqtt_form_parse_json(arr_pass, strlen(arr_pass), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_NONE, st.field);
}

void test_json_rejects_malformed_or_non_object_json(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    static const char truncated[] = "{\"uri\":";
    st = mqtt_form_parse_json(truncated, strlen(truncated), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);

    static const char not_object[] = "[1,2,3]";
    st = mqtt_form_parse_json(not_object, strlen(not_object), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
}

/* ---- the pre-scan that runs before cJSON ever sees the body ---- */

void test_json_rejects_deeply_nested_body(void) {
    /* Just the opening brackets are enough: depth crosses 1 on the
       second one, long before cJSON would ever be called. */
    char body[300];
    memset(body, '[', sizeof(body));
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_json(body, sizeof(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
}

void test_json_accepts_brackets_inside_string_value(void) {
    /* The nesting pre-scan must not mistake a bracket inside a string
       for structural nesting -- this stays a flat, one-level object. */
    static const char body[] = "{\"uri\":\"mqtt://h\",\"user\":\"[{]}\"}";
    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_json(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("[{]}", out.user);
}

void test_json_rejects_duplicate_known_fields(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    static const char dup_uri[] = "{\"uri\":\"mqtt://a\",\"uri\":\"mqtt://b\"}";
    st = mqtt_form_parse_json(dup_uri, strlen(dup_uri), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);

    /* The second occurrence's type must not matter -- this is a
       duplicate-key rejection, not a type check on whichever one
       cJSON_GetObjectItemCaseSensitive would have returned. */
    static const char dup_uri_bad_type[] = "{\"uri\":\"mqtt://a\",\"uri\":5}";
    st = mqtt_form_parse_json(dup_uri_bad_type, strlen(dup_uri_bad_type), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);

    static const char dup_user[] = "{\"uri\":\"mqtt://a\",\"user\":\"x\",\"user\":\"y\"}";
    st = mqtt_form_parse_json(dup_user, strlen(dup_user), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    static const char dup_pass[] = "{\"uri\":\"mqtt://a\",\"pass\":\"x\",\"pass\":\"y\"}";
    st = mqtt_form_parse_json(dup_pass, strlen(dup_pass), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_PASS, st.field);
}

void test_json_rejects_raw_nul_byte(void) {
    static const char prefix[] = "{\"uri\":\"mqtt://h";
    static const char suffix[] = "\"}";
    char body[40];
    size_t n = 0;
    memcpy(body + n, prefix, sizeof(prefix) - 1);
    n += sizeof(prefix) - 1;
    body[n++] = '\0';
    memcpy(body + n, suffix, sizeof(suffix) - 1);
    n += sizeof(suffix) - 1;

    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_json(body, n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
}

void test_json_rejects_u0000_escape(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    static const char in_uri[] = "{\"uri\":\"mqtt://h\\u0000junk\"}";
    st = mqtt_form_parse_json(in_uri, strlen(in_uri), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);

    static const char in_pass[] = "{\"uri\":\"mqtt://h\",\"pass\":\"ab\\u0000cd\"}";
    st = mqtt_form_parse_json(in_pass, strlen(in_pass), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);

    /* Case-insensitive on the 'u'. */
    static const char upper[] = "{\"uri\":\"mqtt://h\\U0000junk\"}";
    st = mqtt_form_parse_json(upper, strlen(upper), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
}

void test_json_rejects_trailing_bytes_after_the_object(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    static const char garbage[] = "{\"uri\":\"mqtt://h\"}garbage";
    st = mqtt_form_parse_json(garbage, strlen(garbage), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);

    static const char second_obj[] = "{\"uri\":\"mqtt://h\"}{\"uri\":5}";
    st = mqtt_form_parse_json(second_obj, strlen(second_obj), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);

    /* Trailing whitespace, unlike trailing garbage, is fine. */
    static const char ws[] = "{\"uri\":\"mqtt://h\"}  \n";
    st = mqtt_form_parse_json(ws, strlen(ws), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
}

void test_json_failed_parse_zeroes_output_struct(void) {
    static const char body[] = "{\"pass\":\"secret\",\"uri\":\"ftp://x\"}";
    mqtt_form_result_t out;
    memset(&out, 0xAA, sizeof(out));
    mqtt_form_status_t st = mqtt_form_parse_json(body, strlen(body), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_SCHEME, st.err);
    TEST_ASSERT_EQUAL_STRING("", out.pass);
}

void test_json_shares_the_uri_scheme_and_char_rule(void) {
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    static const char bad_scheme[] = "{\"uri\":\"ftp://broker.local\"}";
    st = mqtt_form_parse_json(bad_scheme, strlen(bad_scheme), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_SCHEME, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);

    /* No percent-encoding in JSON: the space is literal in the string. */
    static const char bad_char[] = "{\"uri\":\"mqtt://broker.local/a b\"}";
    st = mqtt_form_parse_json(bad_char, strlen(bad_char), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_CHAR, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

void test_json_uri_length_boundary(void) {
    char uri127[128], uri128[129];
    build_mqtt_uri_of_len(uri127, 127);
    build_mqtt_uri_of_len(uri128, 128);

    char body[160];
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    int n = snprintf(body, sizeof(body), "{\"uri\":\"%s\"}", uri127);
    st = mqtt_form_parse_json(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL(127u, strlen(out.uri));

    n = snprintf(body, sizeof(body), "{\"uri\":\"%s\"}", uri128);
    st = mqtt_form_parse_json(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

void test_json_user_and_pass_length_boundary(void) {
    char user63[64], user64[65];
    memset(user63, 'b', 63);
    user63[63] = '\0';
    memset(user64, 'b', 64);
    user64[64] = '\0';

    char body[256];
    mqtt_form_result_t out;
    mqtt_form_status_t st;

    int n = snprintf(body, sizeof(body), "{\"uri\":\"mqtt://h\",\"user\":\"%s\"}", user63);
    st = mqtt_form_parse_json(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL(63u, strlen(out.user));

    n = snprintf(body, sizeof(body), "{\"uri\":\"mqtt://h\",\"user\":\"%s\"}", user64);
    st = mqtt_form_parse_json(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_USER, st.field);

    n = snprintf(body, sizeof(body), "{\"uri\":\"mqtt://h\",\"pass\":\"%s\"}", user63);
    st = mqtt_form_parse_json(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);

    n = snprintf(body, sizeof(body), "{\"uri\":\"mqtt://h\",\"pass\":\"%s\"}", user64);
    st = mqtt_form_parse_json(body, (size_t)n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_PASS, st.field);
}

void test_json_body_at_cap_is_accepted_over_cap_is_rejected(void) {
    static const char prefix[] = "{\"uri\":\"mqtt://broker.local\",\"filler\":\"";
    static const char suffix[] = "\"}";
    size_t prefix_len = strlen(prefix), suffix_len = strlen(suffix);
    size_t pad = MQTT_FORM_BODY_MAX - prefix_len - suffix_len;

    /* body[0, MQTT_FORM_BODY_MAX) is exactly one valid, complete JSON
       object; the byte at MQTT_FORM_BODY_MAX is harmless filler that the
       over-cap call below must never reach (the length check runs
       before cJSON sees a single byte). */
    char body[MQTT_FORM_BODY_MAX + 2];
    memset(body, 'a', sizeof(body));
    memcpy(body, prefix, prefix_len);
    memset(body + prefix_len, 'a', pad);
    memcpy(body + prefix_len + pad, suffix, suffix_len);

    mqtt_form_result_t out;
    mqtt_form_status_t st = mqtt_form_parse_json(body, MQTT_FORM_BODY_MAX, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);

    st = mqtt_form_parse_json(body, MQTT_FORM_BODY_MAX + 1, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BODY_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_NONE, st.field);
}

/* ---- error text ---- */

void test_error_str_is_nonempty_for_every_error_and_empty_for_none(void) {
    TEST_ASSERT_EQUAL_STRING("", mqtt_form_error_str(MQTT_FORM_ERR_NONE));
    static const mqtt_form_err_t errs[] = {
        MQTT_FORM_ERR_MISSING,         MQTT_FORM_ERR_TOO_LONG,           MQTT_FORM_ERR_BAD_SCHEME,
        MQTT_FORM_ERR_BAD_CHAR,        MQTT_FORM_ERR_MALFORMED_ENCODING, MQTT_FORM_ERR_BODY_TOO_LONG,
        MQTT_FORM_ERR_DUPLICATE_FIELD, MQTT_FORM_ERR_BAD_JSON,
    };
    for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++)
        TEST_ASSERT_TRUE(strlen(mqtt_form_error_str(errs[i])) > 0);
}

/* ---- HTML escaping (task 4's form page prefill) ---- */

void test_html_escape_escapes_all_five_characters(void) {
    char out[64];
    TEST_ASSERT_TRUE(mqtt_form_html_escape("a&b<c>d\"e'f", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a&amp;b&lt;c&gt;d&quot;e&#39;f", out);
}

void test_html_escape_passes_plain_text_through(void) {
    char out[32];
    TEST_ASSERT_TRUE(mqtt_form_html_escape("mqtt://broker.local", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.local", out);
}

void test_html_escape_exact_fit_boundary(void) {
    /* "ab" + NUL = 3 bytes: fits exactly in a 3-byte buffer. */
    char tiny[3];
    TEST_ASSERT_TRUE(mqtt_form_html_escape("ab", tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("ab", tiny);
    /* One byte short must fail and clear the output. */
    char too_small[2];
    TEST_ASSERT_FALSE(mqtt_form_html_escape("ab", too_small, sizeof(too_small)));
    TEST_ASSERT_EQUAL_STRING("", too_small);
}

void test_html_escape_exact_fit_with_entity(void) {
    /* "&" -> "&amp;" + NUL = 6 bytes: fits exactly in a 6-byte buffer. */
    char out6[6];
    TEST_ASSERT_TRUE(mqtt_form_html_escape("&", out6, sizeof(out6)));
    TEST_ASSERT_EQUAL_STRING("&amp;", out6);

    /* One byte short must fail and clear the output. */
    char out5[5];
    TEST_ASSERT_FALSE(mqtt_form_html_escape("&", out5, sizeof(out5)));
    TEST_ASSERT_EQUAL_STRING("", out5);
}

void test_html_escape_fails_on_overflow_mid_expansion(void) {
    /* "&" alone expands to "&amp;" (5 bytes); a 5-byte buffer has room
       for only "&amp;" with no NUL, so even the first character fails. */
    char small[5];
    TEST_ASSERT_FALSE(mqtt_form_html_escape("&&&", small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
}

void test_html_escape_null_and_zero_length_safety(void) {
    char out[8] = "garbage";
    TEST_ASSERT_FALSE(mqtt_form_html_escape(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);

    TEST_ASSERT_FALSE(mqtt_form_html_escape("x", NULL, 5));
    TEST_ASSERT_FALSE(mqtt_form_html_escape("x", out, 0));
}

/* ---- the setup page's combined form: WiFi + MQTT, each group optional ---- */

static mqtt_form_status_t parse_setup(const char *body, mqtt_form_setup_t *out) {
    return mqtt_form_parse_setup(body, strlen(body), out);
}

void test_setup_accepts_wifi_and_mqtt_together(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st =
        parse_setup("ssid=HomeNet&wpass=hunter22x&uri=mqtt://broker.local&user=bob&pass=secret", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_TRUE(out.has_wifi);
    TEST_ASSERT_EQUAL_STRING("HomeNet", out.ssid);
    TEST_ASSERT_EQUAL_STRING("hunter22x", out.wifi_pass);
    TEST_ASSERT_TRUE(out.has_mqtt);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.local", out.mqtt.uri);
    TEST_ASSERT_EQUAL_STRING("bob", out.mqtt.user);
    TEST_ASSERT_EQUAL_STRING("secret", out.mqtt.pass);
    TEST_ASSERT_FALSE(out.mqtt.keep_pass);
}

void test_setup_blank_mqtt_group_leaves_the_broker_alone(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st = parse_setup("ssid=HomeNet&wpass=hunter22x&uri=&user=&pass=", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_TRUE(out.has_wifi);
    TEST_ASSERT_FALSE(out.has_mqtt);
    TEST_ASSERT_EQUAL_STRING("", out.mqtt.uri);

    /* The MQTT fields absent altogether is the same thing. */
    st = parse_setup("ssid=HomeNet&wpass=hunter22x", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_FALSE(out.has_mqtt);
}

void test_setup_blank_wifi_group_means_an_mqtt_only_update(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st = parse_setup("ssid=&wpass=&uri=mqtt://broker.local&user=&pass=", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_FALSE(out.has_wifi);
    TEST_ASSERT_TRUE(out.has_mqtt);
    TEST_ASSERT_TRUE(out.mqtt.keep_pass); /* a blank password never overwrites the stored one */
}

void test_setup_a_password_without_its_ssid_is_ignored_not_stored(void) {
    /* A password manager fills the password box on its own; with no
       network name there is nothing to join and nothing to keep. */
    mqtt_form_setup_t out;
    mqtt_form_status_t st = parse_setup("ssid=&wpass=autofilled&uri=mqtt://b.local", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_FALSE(out.has_wifi);
    TEST_ASSERT_EQUAL_STRING("", out.wifi_pass);
}

void test_setup_a_user_or_pass_without_a_uri_is_ignored(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st = parse_setup("ssid=HomeNet&wpass=&uri=&user=bob&pass=secret", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_FALSE(out.has_mqtt);
    TEST_ASSERT_EQUAL_STRING("", out.mqtt.user);
    TEST_ASSERT_EQUAL_STRING("", out.mqtt.pass);
}

void test_setup_an_empty_submission_parses_as_nothing_to_save(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st = mqtt_form_parse_setup("", 0, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_FALSE(out.has_wifi);
    TEST_ASSERT_FALSE(out.has_mqtt);
}

void test_setup_a_blank_wifi_password_is_an_open_network(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st = parse_setup("ssid=CafeOpen&wpass=", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_TRUE(out.has_wifi);
    TEST_ASSERT_EQUAL_STRING("", out.wifi_pass);
}

void test_setup_decodes_percent_and_plus_in_the_wifi_fields(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st = parse_setup("ssid=My+Net%21&wpass=p%26ss+word%3B", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL_STRING("My Net!", out.ssid);
    TEST_ASSERT_EQUAL_STRING("p&ss word;", out.wifi_pass);
}

void test_setup_ssid_length_boundary_is_32_bytes(void) {
    char body[96];
    mqtt_form_setup_t out;
    memcpy(body, "ssid=", 5);
    memset(body + 5, 'n', 32);
    body[37] = '\0';
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, parse_setup(body, &out).err);
    TEST_ASSERT_EQUAL(32u, strlen(out.ssid));

    body[37] = 'n';
    body[38] = '\0';
    mqtt_form_status_t st = parse_setup(body, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_WIFI_SSID, st.field);
}

void test_setup_wifi_password_length_boundaries(void) {
    char body[128];
    mqtt_form_setup_t out;
    memcpy(body, "ssid=Net&wpass=", 15);

    /* 1..7 characters can never be a WPA2 passphrase */
    for (size_t len = 1; len <= 7; len++) {
        memset(body + 15, 'p', len);
        body[15 + len] = '\0';
        mqtt_form_status_t st = parse_setup(body, &out);
        TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_SHORT, st.err);
        TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_WIFI_PASS, st.field);
    }
    /* 8 and 63 are passphrases of any printable character */
    size_t ok_lens[] = {8, 63};
    for (size_t i = 0; i < 2; i++) {
        memset(body + 15, 'p', ok_lens[i]);
        body[15 + ok_lens[i]] = '\0';
        TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, parse_setup(body, &out).err);
        TEST_ASSERT_EQUAL(ok_lens[i], strlen(out.wifi_pass));
    }
    memset(body + 15, 'p', 65);
    body[15 + 65] = '\0';
    mqtt_form_status_t st = parse_setup(body, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_WIFI_PASS, st.field);
}

/* Exactly 64 characters is a raw PSK, which the driver only accepts as hex;
   anything else of that length is neither a passphrase (max 63) nor a key. */
void test_setup_64_character_password_must_be_hex(void) {
    char body[128];
    mqtt_form_setup_t out;
    memcpy(body, "ssid=Net&wpass=", 15);

    memset(body + 15, 'a', 64);
    body[15 + 64] = '\0';
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, parse_setup(body, &out).err);
    TEST_ASSERT_EQUAL(64, strlen(out.wifi_pass));

    memcpy(body + 15, "0123456789abcdefABCDEF0123456789abcdefABCDEF0123456789abcdefABCD", 64);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, parse_setup(body, &out).err);

    memset(body + 15, 'p', 64);
    mqtt_form_status_t st = parse_setup(body, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_CHAR, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_WIFI_PASS, st.field);
    TEST_ASSERT_EQUAL(0, out.wifi_pass[0]); /* zeroed on failure */

    memset(body + 15, 'a', 64);
    body[15 + 63] = 'g'; /* one non-hex digit at the end */
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_CHAR, parse_setup(body, &out).err);
}

/* A browser sends UTF-8 once the page declares it. The parser must hand
   those bytes through untouched and count the SSID limit in bytes, which is
   what the 802.11 SSID field is. */
void test_setup_ssid_utf8_passes_through_and_counts_bytes(void) {
    mqtt_form_setup_t out;
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, parse_setup("ssid=Caf%C3%A9&wpass=pass%C3%A9word", &out).err);
    TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9", out.ssid);
    TEST_ASSERT_EQUAL_STRING("pass\xC3\xA9word", out.wifi_pass);

    /* 16 two-byte characters = 32 bytes: fits. 17 = 34 bytes: too long. */
    char body[256] = "ssid=";
    size_t n = 5;
    for (int i = 0; i < 16; i++) {
        memcpy(body + n, "%C3%A9", 6);
        n += 6;
    }
    body[n] = '\0';
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, parse_setup(body, &out).err);
    TEST_ASSERT_EQUAL(32, strlen(out.ssid));
    memcpy(body + n, "%C3%A9", 7);
    mqtt_form_status_t st = parse_setup(body, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_TOO_LONG, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_WIFI_SSID, st.field);
}

void test_setup_rejects_control_characters_in_the_wifi_fields(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st = parse_setup("ssid=Net%0Aname&wpass=", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_CHAR, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_WIFI_SSID, st.field);

    st = parse_setup("ssid=Net&wpass=pass%09word1", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_CHAR, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_WIFI_PASS, st.field);
}

void test_setup_runs_the_mqtt_validators_when_a_uri_is_present(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st = parse_setup("ssid=Net&wpass=hunter22x&uri=http://nope", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_SCHEME, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

void test_setup_a_failure_zeroes_every_field_including_the_wifi_password(void) {
    mqtt_form_setup_t out;
    memset(&out, 0xAA, sizeof(out));
    mqtt_form_status_t st = parse_setup("ssid=Net&wpass=hunter22x&uri=http://nope&pass=secret", &out);
    TEST_ASSERT_NOT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    mqtt_form_setup_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &out, sizeof(out));
}

void test_setup_rejects_a_duplicated_field_of_either_group(void) {
    mqtt_form_setup_t out;
    mqtt_form_status_t st = parse_setup("ssid=a&ssid=b", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_WIFI_SSID, st.field);

    st = parse_setup("ssid=a&wpass=x&wpass=y", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_WIFI_PASS, st.field);

    st = parse_setup("uri=mqtt://a&uri=mqtt://b", &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_DUPLICATE_FIELD, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_URI, st.field);
}

void test_setup_body_cap_applies_and_the_worst_case_encoded_body_fits(void) {
    static char body[MQTT_FORM_BODY_MAX + 2];
    mqtt_form_setup_t out;
    memset(body, 'a', sizeof(body));
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BODY_TOO_LONG, mqtt_form_parse_setup(body, MQTT_FORM_BODY_MAX + 1, &out).err);

    /* every field at its own MAX with every byte percent-encoded */
    size_t n = 0;
    n += (size_t)sprintf(body + n, "ssid=");
    for (int i = 0; i < 32; i++)
        n += (size_t)sprintf(body + n, "%%61");
    n += (size_t)sprintf(body + n, "&wpass=");
    for (int i = 0; i < 64; i++)
        n += (size_t)sprintf(body + n, "%%62");
    n += (size_t)sprintf(body + n, "&uri=");
    {
        char decoded[MQTT_FORM_URI_MAX];
        memcpy(decoded, "mqtt://", 7);
        memset(decoded + 7, 'h', MQTT_FORM_URI_MAX - 1 - 7);
        for (size_t i = 0; i < MQTT_FORM_URI_MAX - 1; i++)
            n += (size_t)sprintf(body + n, "%%%02X", (unsigned char)decoded[i]);
    }
    n += (size_t)sprintf(body + n, "&user=");
    for (int i = 0; i < MQTT_FORM_USER_MAX - 1; i++)
        n += (size_t)sprintf(body + n, "%%63");
    n += (size_t)sprintf(body + n, "&pass=");
    for (int i = 0; i < MQTT_FORM_PASS_MAX - 1; i++)
        n += (size_t)sprintf(body + n, "%%64");

    TEST_ASSERT_TRUE(n <= MQTT_FORM_BODY_MAX);
    mqtt_form_status_t st = mqtt_form_parse_setup(body, n, &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    TEST_ASSERT_EQUAL(32u, strlen(out.ssid));
    TEST_ASSERT_EQUAL(64u, strlen(out.wifi_pass));
    TEST_ASSERT_EQUAL(127u, strlen(out.mqtt.uri));
}

void test_setup_error_text_covers_the_new_error_and_fields_are_distinct(void) {
    TEST_ASSERT_EQUAL_STRING("value is too short", mqtt_form_error_str(MQTT_FORM_ERR_TOO_SHORT));
    TEST_ASSERT_NOT_EQUAL(MQTT_FORM_FIELD_WIFI_SSID, MQTT_FORM_FIELD_WIFI_PASS);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_setup_accepts_wifi_and_mqtt_together);
    RUN_TEST(test_setup_blank_mqtt_group_leaves_the_broker_alone);
    RUN_TEST(test_setup_blank_wifi_group_means_an_mqtt_only_update);
    RUN_TEST(test_setup_a_password_without_its_ssid_is_ignored_not_stored);
    RUN_TEST(test_setup_a_user_or_pass_without_a_uri_is_ignored);
    RUN_TEST(test_setup_an_empty_submission_parses_as_nothing_to_save);
    RUN_TEST(test_setup_a_blank_wifi_password_is_an_open_network);
    RUN_TEST(test_setup_decodes_percent_and_plus_in_the_wifi_fields);
    RUN_TEST(test_setup_ssid_length_boundary_is_32_bytes);
    RUN_TEST(test_setup_wifi_password_length_boundaries);
    RUN_TEST(test_setup_64_character_password_must_be_hex);
    RUN_TEST(test_setup_ssid_utf8_passes_through_and_counts_bytes);
    RUN_TEST(test_setup_rejects_control_characters_in_the_wifi_fields);
    RUN_TEST(test_setup_runs_the_mqtt_validators_when_a_uri_is_present);
    RUN_TEST(test_setup_a_failure_zeroes_every_field_including_the_wifi_password);
    RUN_TEST(test_setup_rejects_a_duplicated_field_of_either_group);
    RUN_TEST(test_setup_body_cap_applies_and_the_worst_case_encoded_body_fits);
    RUN_TEST(test_setup_error_text_covers_the_new_error_and_fields_are_distinct);
    RUN_TEST(test_urlencoded_accepts_full_submission);
    RUN_TEST(test_urlencoded_percent_decodes_and_plus_decodes);
    RUN_TEST(test_urlencoded_field_order_does_not_matter);
    RUN_TEST(test_urlencoded_empty_uri_disables_mqtt_and_clears_user_pass);
    RUN_TEST(test_urlencoded_missing_uri_field_is_an_error);
    RUN_TEST(test_urlencoded_empty_body_is_missing_uri);
    RUN_TEST(test_urlencoded_blank_pass_keeps_existing);
    RUN_TEST(test_urlencoded_absent_pass_keeps_existing);
    RUN_TEST(test_urlencoded_literal_pass_is_not_kept);
    RUN_TEST(test_urlencoded_rejects_bad_scheme);
    RUN_TEST(test_urlencoded_rejects_scheme_with_no_host);
    RUN_TEST(test_urlencoded_rejects_bad_char_in_uri_after_valid_scheme);
    RUN_TEST(test_urlencoded_rejects_control_char_in_user);
    RUN_TEST(test_urlencoded_rejects_control_char_in_pass);
    RUN_TEST(test_urlencoded_user_may_be_empty);
    RUN_TEST(test_urlencoded_user_and_pass_allow_quotes_and_backslash);
    RUN_TEST(test_urlencoded_percent_edge_cases_are_malformed);
    RUN_TEST(test_urlencoded_ignores_unknown_fields_even_if_malformed);
    RUN_TEST(test_urlencoded_rejects_duplicate_known_field);
    RUN_TEST(test_urlencoded_pass_key_with_no_equals_means_keep);
    RUN_TEST(test_urlencoded_rejects_raw_nul_byte);
    RUN_TEST(test_urlencoded_empty_uri_still_checks_other_fields_syntax);
    RUN_TEST(test_urlencoded_rejects_malformed_mqtt_uri_variants);
    RUN_TEST(test_urlencoded_accepts_strict_mqtt_uri_variants);
    RUN_TEST(test_urlencoded_rejects_bad_port_as_bad_char);
    RUN_TEST(test_failed_parse_zeroes_output_struct);
    RUN_TEST(test_urlencoded_uri_length_boundary);
    RUN_TEST(test_urlencoded_user_and_pass_length_boundary);
    RUN_TEST(test_urlencoded_body_at_cap_is_accepted_over_cap_is_rejected);
    RUN_TEST(test_urlencoded_worst_case_percent_encoded_body_is_accepted);
    RUN_TEST(test_json_accepts_full_submission);
    RUN_TEST(test_json_missing_uri_is_an_error);
    RUN_TEST(test_json_empty_body_is_bad_json);
    RUN_TEST(test_json_empty_uri_disables_mqtt_and_clears_user_pass);
    RUN_TEST(test_json_blank_or_absent_pass_keeps_existing);
    RUN_TEST(test_json_rejects_non_string_values);
    RUN_TEST(test_json_rejects_malformed_or_non_object_json);
    RUN_TEST(test_json_rejects_deeply_nested_body);
    RUN_TEST(test_json_accepts_brackets_inside_string_value);
    RUN_TEST(test_json_rejects_duplicate_known_fields);
    RUN_TEST(test_json_rejects_raw_nul_byte);
    RUN_TEST(test_json_rejects_u0000_escape);
    RUN_TEST(test_json_rejects_trailing_bytes_after_the_object);
    RUN_TEST(test_json_failed_parse_zeroes_output_struct);
    RUN_TEST(test_json_shares_the_uri_scheme_and_char_rule);
    RUN_TEST(test_json_uri_length_boundary);
    RUN_TEST(test_json_user_and_pass_length_boundary);
    RUN_TEST(test_json_body_at_cap_is_accepted_over_cap_is_rejected);
    RUN_TEST(test_error_str_is_nonempty_for_every_error_and_empty_for_none);
    RUN_TEST(test_html_escape_escapes_all_five_characters);
    RUN_TEST(test_html_escape_passes_plain_text_through);
    RUN_TEST(test_html_escape_exact_fit_boundary);
    RUN_TEST(test_html_escape_exact_fit_with_entity);
    RUN_TEST(test_html_escape_fails_on_overflow_mid_expansion);
    RUN_TEST(test_html_escape_null_and_zero_length_safety);
    return UNITY_END();
}
