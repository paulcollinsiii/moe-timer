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

    static const char arr_pass[] = "{\"uri\":\"mqtt://broker.local\",\"pass\":[1,2]}";
    st = mqtt_form_parse_json(arr_pass, strlen(arr_pass), &out);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_BAD_JSON, st.err);
    TEST_ASSERT_EQUAL(MQTT_FORM_FIELD_PASS, st.field);
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

int main(void) {
    UNITY_BEGIN();
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
    RUN_TEST(test_urlencoded_uri_length_boundary);
    RUN_TEST(test_urlencoded_user_and_pass_length_boundary);
    RUN_TEST(test_urlencoded_body_at_cap_is_accepted_over_cap_is_rejected);
    RUN_TEST(test_json_accepts_full_submission);
    RUN_TEST(test_json_missing_uri_is_an_error);
    RUN_TEST(test_json_empty_body_is_bad_json);
    RUN_TEST(test_json_empty_uri_disables_mqtt_and_clears_user_pass);
    RUN_TEST(test_json_blank_or_absent_pass_keeps_existing);
    RUN_TEST(test_json_rejects_non_string_values);
    RUN_TEST(test_json_rejects_malformed_or_non_object_json);
    RUN_TEST(test_json_shares_the_uri_scheme_and_char_rule);
    RUN_TEST(test_json_uri_length_boundary);
    RUN_TEST(test_json_body_at_cap_is_accepted_over_cap_is_rejected);
    RUN_TEST(test_error_str_is_nonempty_for_every_error_and_empty_for_none);
    RUN_TEST(test_html_escape_escapes_all_five_characters);
    RUN_TEST(test_html_escape_passes_plain_text_through);
    RUN_TEST(test_html_escape_exact_fit_boundary);
    RUN_TEST(test_html_escape_fails_on_overflow_mid_expansion);
    RUN_TEST(test_html_escape_null_and_zero_length_safety);
    return UNITY_END();
}
