#include <stdlib.h>
#include <string.h>
#include <unity.h>

/* Single-TU: the vendored cJSON, the shared validators and the pure policy
   module. Nothing else is linked in — that is the point of layer 1. If this
   suite ever needs a mock, ota_policy.c has stopped being pure.
   config_validate.c is here because the manifest's binary URL is held to
   the same https rule as the HA-set endpoint, from one definition. */
// clang-format off
#include "cJSON.h"
#include "../../main/config_validate.c"
#include "../../main/ota_policy.c"
// clang-format on

void setUp(void) {}
void tearDown(void) {}

#define DEV "magtag-a1b2c3"
#define OTHER_DEV "magtag-ffffff"
#define RUNNING "1.5.0"

/* ---- helpers ----

   Every manifest goes through an exactly-sized heap block with NO trailing
   NUL. ASAN then traps any read past the length the caller declared, which
   is what gives the truncated/empty-body cases teeth: a parser that trusts
   NUL termination instead of the length passes the naive version of these
   tests and fails this one. */
static void decide_bytes(const char *json, size_t len, const char *dev, const char *running, ota_decision_t *out) {
    char *buf = malloc(len > 0 ? len : 1);
    TEST_ASSERT_NOT_NULL(buf);
    if (len > 0)
        memcpy(buf, json, len);
    ota_decide_in_t in = {
        .manifest = buf,
        .manifest_len = len,
        .device_id = dev,
        .running_version = running,
        .counted_target = "",
        .fails = 0,
        .max_fails = 3,
    };
    ota_policy_decide(&in, out);
    free(buf);
}

static void decide(const char *json, const char *dev, const char *running, ota_decision_t *out) {
    decide_bytes(json, strlen(json), dev, running, out);
}

/* A one-block schema-1 manifest with only a default entry. */
#define MANIFEST_DEFAULT_ONLY                            \
    "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\"," \
    "\"url\":\"https://ota.example.com/magtag-1.6.0.bin\"}}]"

/* ================= block selection (the array) ================= */

void test_single_schema1_block_is_selected(void) {
    ota_decision_t d;
    decide(MANIFEST_DEFAULT_ONLY, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL(1, d.schema);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
    TEST_ASSERT_EQUAL_STRING("https://ota.example.com/magtag-1.6.0.bin", d.url);
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, d.reason);
}

/* Parse, select with an explicit cap, and hand back the chosen block's
   url so nothing outlives the cJSON tree. "" when no block was chosen. */
static const char *select_url(const char *json, int cap, int *schema, bool *dup) {
    static char picked[64];
    picked[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    const cJSON *b = select_block(root, cap, schema, dup);
    if (b != NULL) {
        const cJSON *def = cJSON_GetObjectItemCaseSensitive(b, "default");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(def, "url");
        TEST_ASSERT_TRUE(cJSON_IsString(url));
        snprintf(picked, sizeof(picked), "%s", url->valuestring);
    }
    cJSON_Delete(root);
    return picked;
}

/* Two SUPPORTED blocks: the HIGHEST schema wins, regardless of order in
   the file. Both orderings, so "highest" cannot be satisfied by "last".

   Driven through select_block with a cap of 2 rather than through
   decide(), because at OTA_SCHEMA_MAX 1 two supported blocks cannot
   exist — the rule would otherwise sit untested until the day schema 2
   ships, which is precisely the day it has to already work. */
void test_highest_supported_schema_wins(void) {
    int schema = -1;
    bool dup = true;
    const char *ascending =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/old.bin\"}},"
        "{\"schema\":2,\"default\":{\"version\":\"2.0.0\",\"url\":\"https://h/new.bin\"}}]";
    const char *descending =
        "[{\"schema\":2,\"default\":{\"version\":\"2.0.0\",\"url\":\"https://h/new.bin\"}},"
        "{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/old.bin\"}}]";

    TEST_ASSERT_EQUAL_STRING("https://h/new.bin", select_url(ascending, 2, &schema, &dup));
    TEST_ASSERT_EQUAL(2, schema);
    TEST_ASSERT_FALSE(dup);
    TEST_ASSERT_EQUAL_STRING("https://h/new.bin", select_url(descending, 2, &schema, &dup));
    TEST_ASSERT_EQUAL(2, schema);

    /* The same array read by THIS build, whose cap is 1, takes the
       schema-1 block instead: the rolling-fleet property, seen from the
       selector rather than from decide(). */
    TEST_ASSERT_EQUAL_STRING("https://h/old.bin", select_url(ascending, 1, &schema, &dup));
    TEST_ASSERT_EQUAL(1, schema);
}

/* Schema 0 is not a schema. It used to be reachable — the best_schema
   seed of -1 excludes negatives only — so a {"schema":0} block was
   selected and then handed to the schema-1 resolver. A typo, or 0
   reserved to mean "do not read this", would have been installed. */
void test_schema_zero_is_not_selected(void) {
    ota_decision_t d;
    decide("[{\"schema\":0,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_NO_SCHEMA, d.reason);
    TEST_ASSERT_EQUAL(-1, d.schema);
}

/* Non-integral schemas: cJSON derives valueint from valuedouble by
   saturating truncation, so 0.5 reads as 0, 1.9 as 1 and 1e300 as
   INT_MAX — a malformed value silently reinterpreted as a schema this
   build claims to understand. */
void test_non_integral_schema_is_skipped(void) {
    ota_decision_t d;
    const char *cases[] = {"0.5", "1.9", "1e300", "-0.5"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char m[192];
        snprintf(m, sizeof(m), "[{\"schema\":%s,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\"}}]",
                 cases[i]);
        decide(m, DEV, RUNNING, &d);
        TEST_ASSERT_FALSE(d.update);
        TEST_ASSERT_EQUAL(OTA_REASON_NO_SCHEMA, d.reason);
        TEST_ASSERT_EQUAL(-1, d.schema);
    }
    /* 1.0 IS integral and must still read as schema 1 */
    decide("[{\"schema\":1.0,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL(1, d.schema);
}

/* THE rolling-fleet property, and the whole reason the top level is an
   array: a block this firmware cannot read must not strand it — it falls
   back to the newest block it does understand and keeps updating. */
void test_unsupported_newest_block_falls_back_to_the_supported_one(void) {
    ota_decision_t d;
    const char *m =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/one.bin\"}},"
        "{\"schema\":99,\"default\":{\"version\":\"9.9.9\",\"url\":\"https://h/future.bin\"},"
        "\"channels\":{\"beta\":{\"version\":\"9.9.9\"}}}]";
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL(1, d.schema);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
}

void test_no_supported_block_reports_no_schema(void) {
    ota_decision_t d;
    decide("[{\"schema\":2,\"default\":{\"version\":\"2.0.0\",\"url\":\"https://h/f.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_NO_SCHEMA, d.reason);
    TEST_ASSERT_EQUAL(-1, d.schema);
    TEST_ASSERT_EQUAL_STRING("no_schema", ota_policy_reason_str(d.reason));
}

/* Duplicate schema values are malformed input, not a feature: take the
   first and carry on rather than guessing or bailing. The plan also wants
   it logged, which a pure module cannot do — so the fact is reported to
   the caller instead of swallowed. */
void test_duplicate_schema_first_wins_and_is_reported(void) {
    ota_decision_t d;
    const char *m =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/first.bin\"}},"
        "{\"schema\":1,\"default\":{\"version\":\"1.7.0\",\"url\":\"https://h/second.bin\"}}]";
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
    TEST_ASSERT_EQUAL_STRING("https://h/first.bin", d.url);
    TEST_ASSERT_TRUE(d.duplicate_schema);
}

/* The flag tracks the SELECTED schema, not any repeat anywhere in the
   array: a duplicate among blocks that lost the selection says nothing
   about the block actually being read. */
void test_duplicate_schema_flag_is_clear_on_a_clean_manifest(void) {
    ota_decision_t d;
    decide(MANIFEST_DEFAULT_ONLY, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_FALSE(d.duplicate_schema);
    /* two schema-0 blocks, then the schema-1 block that wins */
    const char *m =
        "[{\"schema\":0,\"default\":{\"version\":\"0.1\",\"url\":\"https://h/a.bin\"}},"
        "{\"schema\":0,\"default\":{\"version\":\"0.2\",\"url\":\"https://h/b.bin\"}},"
        "{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/c.bin\"}}]";
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_EQUAL(1, d.schema);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
    TEST_ASSERT_FALSE(d.duplicate_schema);
}

/* The forward-compatibility guarantee, and the easiest one to regress:
   a key added for a later 1.x reader must not make a schema-1 block
   unreadable, at either level. */
void test_unknown_keys_inside_a_supported_block_are_ignored(void) {
    ota_decision_t d;
    const char *m =
        "[{\"schema\":1,\"note\":\"published by CI\",\"min_batt\":40,"
        "\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\",\"sha256\":\"deadbeef\",\"size\":1234}}]";
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
}

void test_top_level_not_an_array_is_no_update(void) {
    ota_decision_t d;
    decide("{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\"}}", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_MANIFEST, d.reason);
    decide("\"just a string\"", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_MANIFEST, d.reason);
}

/* Junk elements alongside a good block must not take the block down —
   same forward-compatibility argument, one level up. */
void test_non_object_and_schemaless_elements_are_skipped(void) {
    ota_decision_t d;
    const char *m =
        "[null,42,\"text\",[1,2],{\"no_schema_key\":true},"
        "{\"schema\":\"1\",\"default\":{\"version\":\"9.9.9\",\"url\":\"https://h/str.bin\"}},"
        "{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\"}}]";
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
}

/* A negative schema is junk, and it must not be selected: the decision
   struct uses -1 to mean "no block was read", so a selected -1 would
   report itself as a non-selection. */
void test_negative_schema_is_skipped(void) {
    ota_decision_t d;
    decide("[{\"schema\":-1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_NO_SCHEMA, d.reason);
    TEST_ASSERT_EQUAL(-1, d.schema);
}

void test_empty_array_reports_no_schema(void) {
    ota_decision_t d;
    decide("[]", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_NO_SCHEMA, d.reason);
}

/* duplicate_schema is only meaningful once a block has been selected;
   on the paths where none was, it must not carry a stale true. */
void test_duplicate_schema_flag_is_false_when_no_block_was_selected(void) {
    ota_decision_t d;
    decide("not json at all", DEV, RUNNING, &d);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_MANIFEST, d.reason);
    TEST_ASSERT_EQUAL(-1, d.schema);
    TEST_ASSERT_FALSE(d.duplicate_schema);

    /* two duplicate blocks, both above this build's cap: nothing is
       selected, so there is nothing for the flag to be about */
    const char *dup_but_unsupported =
        "[{\"schema\":99,\"default\":{\"version\":\"9.9\",\"url\":\"https://h/a.bin\"}},"
        "{\"schema\":99,\"default\":{\"version\":\"9.9\",\"url\":\"https://h/b.bin\"}}]";
    decide(dup_but_unsupported, DEV, RUNNING, &d);
    TEST_ASSERT_EQUAL(OTA_REASON_NO_SCHEMA, d.reason);
    TEST_ASSERT_EQUAL(-1, d.schema);
    TEST_ASSERT_FALSE(d.duplicate_schema);
}

/* ================= resolution ================= */

void test_default_applies_when_the_device_is_not_listed(void) {
    ota_decision_t d;
    const char *m =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/default.bin\"},"
        "\"devices\":{\"" DEV "\":{\"version\":\"1.7.0-rc1\",\"url\":\"https://h/rc.bin\"}}}]";
    decide(m, OTHER_DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
    TEST_ASSERT_EQUAL_STRING("https://h/default.bin", d.url);
}

void test_device_entry_overrides_the_default(void) {
    ota_decision_t d;
    const char *m =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/default.bin\"},"
        "\"devices\":{\"" DEV "\":{\"version\":\"1.7.0-rc1\",\"url\":\"https://h/rc.bin\"}}}]";
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.7.0-rc1", d.version);
    TEST_ASSERT_EQUAL_STRING("https://h/rc.bin", d.url);
}

/* Pinning: how one device is frozen without deleting its entry. Absent
   and explicit-null must behave identically. */
void test_device_entry_without_a_version_is_pinned(void) {
    ota_decision_t d;
    const char *absent =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\"},"
        "\"devices\":{\"" DEV "\":{\"note\":\"held back for testing\"}}}]";
    const char *null_ver =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\"},"
        "\"devices\":{\"" DEV "\":{\"version\":null}}}]";
    decide(absent, DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_PINNED, d.reason);
    decide(null_ver, DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_PINNED, d.reason);
    /* ...and the pin does not leak to another device */
    decide(null_ver, OTHER_DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
}

void test_version_equal_to_running_is_no_update(void) {
    ota_decision_t d;
    decide("[{\"schema\":1,\"default\":{\"version\":\"" RUNNING "\",\"url\":\"https://h/f.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_UP_TO_DATE, d.reason);
}

/* "Different, not newer" — both directions. The downgrade is deliberate:
   it is how a rollback is published, and it must not be "fixed" into a
   greater-than test later. */
void test_a_different_version_updates_in_both_directions(void) {
    ota_decision_t d;
    decide("[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/up.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
    decide("[{\"schema\":1,\"default\":{\"version\":\"1.4.0\",\"url\":\"https://h/down.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.4.0", d.version);
    /* pre-release strings order badly under semver and must not matter */
    decide("[{\"schema\":1,\"default\":{\"version\":\"1.5.0-rc1\",\"url\":\"https://h/rc.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
}

void test_malformed_json_is_no_update(void) {
    ota_decision_t d;
    decide("[{\"schema\":1,,}]", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_MANIFEST, d.reason);
    decide("not json at all", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_MANIFEST, d.reason);
}

/* Truncated at every byte offset, on an unterminated buffer: a short read
   is the realistic HTTP failure and ASAN turns any over-read into a
   failing test rather than a lucky parse. */
void test_truncated_json_is_no_update_at_every_offset(void) {
    const char *full = MANIFEST_DEFAULT_ONLY;
    for (size_t len = 1; len < strlen(full); len++) {
        ota_decision_t d;
        decide_bytes(full, len, DEV, RUNNING, &d);
        TEST_ASSERT_FALSE(d.update);
    }
}

void test_empty_body_is_no_update(void) {
    ota_decision_t d;
    decide_bytes("", 0, DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_MANIFEST, d.reason);
}

/* manifest_len is the authority, not NUL: a body that claims 30 bytes must
   be parsed as 30 bytes even when more bytes follow in the buffer. */
void test_manifest_len_bounds_the_parse(void) {
    ota_decision_t d;
    const char *full = MANIFEST_DEFAULT_ONLY;
    decide_bytes(full, strlen(full) - 1, DEV, RUNNING, &d); /* drop the closing ] */
    TEST_ASSERT_FALSE(d.update);
}

void test_null_inputs_do_not_crash(void) {
    ota_decision_t d;
    ota_decide_in_t in = {0};
    ota_policy_decide(&in, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_MANIFEST, d.reason);
    /* a NULL device id and a NULL running version resolve to "" rather
       than dereferencing: an unknown running version differs from every
       published one, which is the safe direction (attempt, don't skip). */
    decide(MANIFEST_DEFAULT_ONLY, NULL, NULL, &d);
    TEST_ASSERT_TRUE(d.update);
}

void test_missing_or_empty_url_is_rejected(void) {
    ota_decision_t d;
    decide("[{\"schema\":1,\"default\":{\"version\":\"1.6.0\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_URL, d.reason);
    decide("[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_URL, d.reason);
    decide("[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":42}}]", DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_URL, d.reason);
}

/* TLS is structural, not a convention: an unauthenticated firmware
   endpoint is an arbitrary-code-execution channel. The manifest's binary
   URL is held to the same rule as the HA-set endpoint, from the same
   definition (config_is_https_url), so the two cannot drift. */
void test_non_https_url_is_rejected(void) {
    ota_decision_t d;
    char m[256];
    const char *bad[] = {
        "http://h/f.bin", "https:/h/f.bin", "ftp://h/f.bin",     "//h/f.bin",
        "https://",       "h/f.bin",        "https://h/f 1.bin", "https://h/f\\\".bin",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        snprintf(m, sizeof(m), "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"%s\"}}]", bad[i]);
        decide(m, DEV, RUNNING, &d);
        TEST_ASSERT_FALSE(d.update);
        TEST_ASSERT_EQUAL(OTA_REASON_BAD_URL, d.reason);
    }
    /* The scheme compare is case-insensitive (config_validate.c owns that
       rule, and esp_http_client matches schemes with strcasecmp too), so a
       shouted scheme is a legal URL here rather than a silent no-update. */
    snprintf(m, sizeof(m), "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"HTTPS://h/f.bin\"}}]");
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
}

/* The app descriptor's version field is 32 bytes, so 31 chars is the last
   value that can ever match a running version — anything longer is a
   publishing mistake and is rejected rather than truncated. */
void test_version_length_bound(void) {
    ota_decision_t d;
    char m[256];
    char ver[64];
    memset(ver, 'v', 31);
    ver[31] = '\0';
    snprintf(m, sizeof(m), "[{\"schema\":1,\"default\":{\"version\":\"%s\",\"url\":\"https://h/f.bin\"}}]", ver);
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING(ver, d.version);

    memset(ver, 'v', 32);
    ver[32] = '\0';
    snprintf(m, sizeof(m), "[{\"schema\":1,\"default\":{\"version\":\"%s\",\"url\":\"https://h/f.bin\"}}]", ver);
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_VERSION, d.reason);
}

void test_non_string_or_empty_version_is_rejected(void) {
    ota_decision_t d;
    decide("[{\"schema\":1,\"default\":{\"version\":160,\"url\":\"https://h/f.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_VERSION, d.reason);
    decide("[{\"schema\":1,\"default\":{\"version\":\"\",\"url\":\"https://h/f.bin\"}}]", DEV, RUNNING, &d);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_VERSION, d.reason);
}

/* A URL that cannot be stored is a rejection, never a truncation: half a
   URL is a download that fails in a way nobody can diagnose. */
static void url_of_len(char *buf, size_t buflen, size_t len) {
    static const char prefix[] = "https://h/";
    TEST_ASSERT_TRUE(len > sizeof(prefix) - 1);
    TEST_ASSERT_TRUE(len < buflen);
    memcpy(buf, prefix, sizeof(prefix) - 1);
    memset(buf + sizeof(prefix) - 1, 'x', len - (sizeof(prefix) - 1));
    buf[len] = '\0';
}

/* Both sides of the bound, exactly — mirroring test_version_length_bound.
   The longest storable URL must round-trip byte-identical, and one more
   must be REJECTED rather than snprintf-truncated into a URL that is
   silently one character short and fails in a way nobody can diagnose. */
void test_url_length_bound(void) {
    ota_decision_t d;
    char url[OTA_URL_MAX + 32];
    char m[OTA_URL_MAX + 128];

    url_of_len(url, sizeof(url), OTA_URL_MAX - 1); /* 191: the longest that fits */
    snprintf(m, sizeof(m), "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"%s\"}}]", url);
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING(url, d.url);
    TEST_ASSERT_EQUAL_UINT(OTA_URL_MAX - 1, strlen(d.url));

    url_of_len(url, sizeof(url), OTA_URL_MAX); /* 192: one too many */
    snprintf(m, sizeof(m), "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"%s\"}}]", url);
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_URL, d.reason);
    TEST_ASSERT_EQUAL_STRING("", d.url);

    url_of_len(url, sizeof(url), OTA_URL_MAX + 10);
    snprintf(m, sizeof(m), "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"%s\"}}]", url);
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_URL, d.reason);
}

void test_block_with_no_entry_for_this_device_and_no_default(void) {
    ota_decision_t d;
    const char *m =
        "[{\"schema\":1,\"devices\":{\"" OTHER_DEV "\":{\"version\":\"1.6.0\",\"url\":\"https://h/f.bin\"}}}]";
    decide(m, DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_NO_ENTRY, d.reason);
}

/* A malformed device entry — or a malformed `devices` map — falls
   through to the default rather than taking the device out of the fleet. */
void test_malformed_device_targeting_falls_back_to_default(void) {
    ota_decision_t d;
    const char *bad_entry =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/default.bin\"},"
        "\"devices\":{\"" DEV "\":\"1.7.0\"}}]";
    const char *bad_map =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/default.bin\"},"
        "\"devices\":[\"" DEV "\"]}]";
    decide(bad_entry, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
    decide(bad_map, DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
}

/* An entry that IS an object is a deliberate statement about this
   device, so a fault inside it is reported rather than quietly resolved
   to the fleet default — installing a version the publisher explicitly
   did not target here is worse than not updating. The non-object case
   above is the only fallback. Both directions pinned, because the
   asymmetry is the kind of thing a later reader "tidies up". */
void test_an_object_device_entry_is_binding_and_does_not_fall_back(void) {
    ota_decision_t d;
    const char *bad_url =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/default.bin\"},"
        "\"devices\":{\"" DEV "\":{\"version\":\"1.7.0\",\"url\":\"http://h/rc.bin\"}}}]";
    const char *bad_version =
        "[{\"schema\":1,\"default\":{\"version\":\"1.6.0\",\"url\":\"https://h/default.bin\"},"
        "\"devices\":{\"" DEV "\":{\"version\":42,\"url\":\"https://h/rc.bin\"}}}]";
    decide(bad_url, DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_URL, d.reason);
    TEST_ASSERT_EQUAL_STRING("", d.url);
    decide(bad_version, DEV, RUNNING, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_VERSION, d.reason);
    /* and the other device on the same manifest still gets the default */
    decide(bad_url, OTHER_DEV, RUNNING, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("https://h/default.bin", d.url);
}

/* ================= preconditions ================= */

static ota_gate_in_t clear_gate(void) {
    ota_gate_in_t g = {
        .url_set = true,
        .time_valid = true,
        .charge_locked = false,
        .batt_pct = 80,
        .min_batt_pct = 30,
        .free_heap = 80000,
        .min_free_heap = 40000,
    };
    return g;
}

void test_gate_passes_when_all_clear(void) {
    ota_gate_in_t g = clear_gate();
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, ota_policy_check_gate(&g));
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, ota_policy_download_gate(&g));
}

void test_gate_skips_with_an_empty_endpoint(void) {
    ota_gate_in_t g = clear_gate();
    g.url_set = false;
    TEST_ASSERT_EQUAL(OTA_REASON_NO_URL, ota_policy_check_gate(&g));
}

void test_gate_skips_without_ntp(void) {
    ota_gate_in_t g = clear_gate();
    g.time_valid = false;
    TEST_ASSERT_EQUAL(OTA_REASON_NO_TIME, ota_policy_check_gate(&g));
}

void test_gate_skips_when_charge_locked(void) {
    ota_gate_in_t g = clear_gate();
    g.charge_locked = true;
    TEST_ASSERT_EQUAL(OTA_REASON_LOCKED, ota_policy_check_gate(&g));
}

void test_gate_skips_below_the_battery_floor(void) {
    ota_gate_in_t g = clear_gate();
    g.batt_pct = 29;
    TEST_ASSERT_EQUAL(OTA_REASON_LOW_BATT, ota_policy_check_gate(&g));
    g.batt_pct = 30; /* the floor itself is allowed */
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, ota_policy_check_gate(&g));
}

/* An unreadable battery must not become a permanent OTA block — the
   charge lock is the real low-battery defence and it is checked above. */
void test_gate_ignores_an_unknown_battery_reading(void) {
    ota_gate_in_t g = clear_gate();
    g.batt_pct = -1;
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, ota_policy_check_gate(&g));
}

/* Heap is only knowable at the second window, so it gates the download
   and not the check. */
void test_download_gate_skips_on_low_heap(void) {
    ota_gate_in_t g = clear_gate();
    g.free_heap = 39999;
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, ota_policy_check_gate(&g));
    TEST_ASSERT_EQUAL(OTA_REASON_LOW_HEAP, ota_policy_download_gate(&g));
    g.free_heap = 40000;
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, ota_policy_download_gate(&g));
}

/* Conditions drift between the two windows, so the download gate re-runs
   the check gate's questions rather than trusting the earlier answer. */
void test_download_gate_also_reruns_the_check_preconditions(void) {
    ota_gate_in_t g = clear_gate();
    g.batt_pct = 10;
    TEST_ASSERT_EQUAL(OTA_REASON_LOW_BATT, ota_policy_download_gate(&g));
}

void test_gate_null_input_does_not_crash(void) {
    TEST_ASSERT_EQUAL(OTA_REASON_NO_URL, ota_policy_check_gate(NULL));
    TEST_ASSERT_EQUAL(OTA_REASON_NO_URL, ota_policy_download_gate(NULL));
}

/* ================= the retry budget ================= */

static void decide_budget(const char *target_version, const char *counted, uint16_t fails, uint16_t max_fails,
                          ota_decision_t *out) {
    char m[192];
    snprintf(m, sizeof(m), "[{\"schema\":1,\"default\":{\"version\":\"%s\",\"url\":\"https://h/f.bin\"}}]",
             target_version);
    char *buf = malloc(strlen(m));
    TEST_ASSERT_NOT_NULL(buf);
    memcpy(buf, m, strlen(m));
    ota_decide_in_t in = {
        .manifest = buf,
        .manifest_len = strlen(m),
        .device_id = DEV,
        .running_version = RUNNING,
        .counted_target = counted,
        .fails = fails,
        .max_fails = max_fails,
    };
    ota_policy_decide(&in, out);
    free(buf);
}

void test_budget_below_the_limit_attempts(void) {
    ota_decision_t d;
    decide_budget("1.6.0", "1.6.0", 2, 3, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, d.reason);
}

void test_budget_at_the_limit_for_the_same_target_gives_up(void) {
    ota_decision_t d;
    decide_budget("1.6.0", "1.6.0", 3, 3, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_GAVE_UP, d.reason);
    /* the target is still reported: HA has to show what it gave up on */
    TEST_ASSERT_EQUAL_STRING("1.6.0", d.version);
    TEST_ASSERT_EQUAL_STRING("gave_up", ota_policy_reason_str(d.reason));
}

/* Publishing a new version re-arms a device that had given up — this is
   the escape from the sad loop, and it must not need a counter reset from
   anywhere else. */
void test_budget_at_the_limit_for_a_different_target_attempts(void) {
    ota_decision_t d;
    decide_budget("1.7.0", "1.6.0", 3, 3, &d);
    TEST_ASSERT_TRUE(d.update);
    TEST_ASSERT_EQUAL_STRING("1.7.0", d.version);
}

/* A caller that forgets .max_fails must not get unlimited retries: that
   is the daily sad loop the budget exists to prevent, and Kconfig's
   range of 1..10 means 0 can only ever be a caller bug. */
void test_zero_max_fails_falls_back_to_the_compiled_default(void) {
    TEST_ASSERT_FALSE(ota_policy_budget_exhausted("1.6.0", "1.6.0", OTA_MAX_FAILS_DEFAULT - 1, 0));
    TEST_ASSERT_TRUE(ota_policy_budget_exhausted("1.6.0", "1.6.0", OTA_MAX_FAILS_DEFAULT, 0));
    TEST_ASSERT_TRUE(ota_policy_budget_exhausted("1.6.0", "1.6.0", 99, 0));
    /* and end to end, through the decision */
    ota_decision_t d;
    decide_budget("1.6.0", "1.6.0", OTA_MAX_FAILS_DEFAULT, 0, &d);
    TEST_ASSERT_FALSE(d.update);
    TEST_ASSERT_EQUAL(OTA_REASON_GAVE_UP, d.reason);
}

void test_budget_helper_rules(void) {
    TEST_ASSERT_FALSE(ota_policy_budget_exhausted("1.6.0", "1.6.0", 2, 3));
    TEST_ASSERT_TRUE(ota_policy_budget_exhausted("1.6.0", "1.6.0", 3, 3));
    TEST_ASSERT_TRUE(ota_policy_budget_exhausted("1.6.0", "1.6.0", 9, 3));
    TEST_ASSERT_FALSE(ota_policy_budget_exhausted("1.7.0", "1.6.0", 9, 3));
    /* never counted anything yet */
    TEST_ASSERT_FALSE(ota_policy_budget_exhausted("1.6.0", "", 9, 3));
    TEST_ASSERT_FALSE(ota_policy_budget_exhausted(NULL, NULL, 9, 3));
}

void test_next_fail_count_increments_for_the_same_target_and_resets_otherwise(void) {
    TEST_ASSERT_EQUAL_UINT16(1, ota_policy_next_fail_count("1.6.0", "", 0));
    TEST_ASSERT_EQUAL_UINT16(1, ota_policy_next_fail_count("1.6.0", "1.5.9", 7));
    TEST_ASSERT_EQUAL_UINT16(3, ota_policy_next_fail_count("1.6.0", "1.6.0", 2));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ota_policy_next_fail_count("1.6.0", "1.6.0", UINT16_MAX));
    TEST_ASSERT_EQUAL_UINT16(1, ota_policy_next_fail_count("1.6.0", NULL, 5));
}

/* ================= reason-code mapping ================= */

/* The distinction that matters: "I pinned the wrong root / my cert
   expired" needs a serial visit, "the network flaked" fixes itself. A
   single `tls` code makes those indistinguishable from HA. */
void test_a_cert_failure_maps_to_tls_cert_not_tls(void) {
    ota_error_facts_t cert = {.tls_failed = true, .tls_cert_flags = 0x02 /* MBEDTLS_X509_BADCERT_NOT_TRUSTED */};
    ota_error_facts_t flake = {.tls_failed = true, .tls_cert_flags = 0};
    TEST_ASSERT_EQUAL(OTA_REASON_TLS_CERT, ota_policy_reason(&cert));
    TEST_ASSERT_EQUAL(OTA_REASON_TLS, ota_policy_reason(&flake));
    TEST_ASSERT_EQUAL_STRING("tls_cert", ota_policy_reason_str(ota_policy_reason(&cert)));
    TEST_ASSERT_EQUAL_STRING("tls", ota_policy_reason_str(ota_policy_reason(&flake)));
}

void test_reason_mapping_covers_the_failure_table(void) {
    ota_error_facts_t f;
    memset(&f, 0, sizeof(f));
    f.deadline_hit = true;
    TEST_ASSERT_EQUAL(OTA_REASON_TIMEOUT, ota_policy_reason(&f));
    memset(&f, 0, sizeof(f));
    f.image_rejected = true;
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_IMAGE, ota_policy_reason(&f));
    memset(&f, 0, sizeof(f));
    f.http_status = 404;
    TEST_ASSERT_EQUAL(OTA_REASON_HTTP, ota_policy_reason(&f));
    memset(&f, 0, sizeof(f));
    f.transport_failed = true;
    TEST_ASSERT_EQUAL(OTA_REASON_NET, ota_policy_reason(&f));
    memset(&f, 0, sizeof(f));
    f.http_status = 200;
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, ota_policy_reason(&f));
    TEST_ASSERT_EQUAL(OTA_REASON_NONE, ota_policy_reason(NULL));
}

/* Our own deadline outranks whatever the transport reports on the way
   out: the abort is the cause, the socket error is the symptom. */
void test_deadline_outranks_the_transport_error(void) {
    ota_error_facts_t f = {
        .deadline_hit = true, .tls_failed = true, .tls_cert_flags = 0x02, .transport_failed = true, .http_status = 500};
    TEST_ASSERT_EQUAL(OTA_REASON_TIMEOUT, ota_policy_reason(&f));
}

/* http_404 and http_500 are separate answers in the failure table, so the
   status has to survive into the published string. */
void test_reason_text_carries_the_http_status(void) {
    char s[OTA_REASON_TEXT_MAX];
    ota_policy_reason_text(OTA_REASON_HTTP, 404, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("http_404", s);
    ota_policy_reason_text(OTA_REASON_HTTP, 500, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("http_500", s);
    ota_policy_reason_text(OTA_REASON_HTTP, 0, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("http", s);
    ota_policy_reason_text(OTA_REASON_TLS_CERT, 0, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("tls_cert", s);
    ota_policy_reason_text(OTA_REASON_NONE, 0, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("", s);
}

/* Every code must have a distinct, non-empty string (except NONE): the
   whole failure table is only useful if it survives to HA, and a missing
   arm in the switch is silent. */
void test_every_reason_has_a_distinct_string(void) {
    const char *seen[OTA_REASON_COUNT];
    for (int r = 0; r < OTA_REASON_COUNT; r++) {
        const char *s = ota_policy_reason_str((ota_reason_t)r);
        TEST_ASSERT_NOT_NULL(s);
        if (r == OTA_REASON_NONE) {
            TEST_ASSERT_EQUAL_STRING("", s);
        } else {
            TEST_ASSERT_TRUE_MESSAGE(s[0] != '\0', "reason code has no string");
            TEST_ASSERT_TRUE_MESSAGE(strlen(s) < OTA_REASON_TEXT_MAX, "reason string does not fit the NVS field");
        }
        for (int p = 0; p < r; p++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(seen[p], s) != 0, "duplicate reason string");
        }
        seen[r] = s;
    }
    TEST_ASSERT_EQUAL_STRING("", ota_policy_reason_str((ota_reason_t)OTA_REASON_COUNT));
}

/* ================= persistability ================= */

/* The plan's failure table leaves ota_result ALONE for the rows where no
   check ran, so a download failure from the previous window is still
   there to be published in the next one — a download failure cannot
   publish itself, because MQTT is already closed by the time it happens.
   Keeping that rule here means a test can catch getting it wrong;
   in the orchestrator it would be an unwritten convention. */
void test_reason_persistability_matches_the_failure_table(void) {
    /* no check ran, or the check ran and there was nothing to do */
    TEST_ASSERT_FALSE(ota_policy_reason_is_persistable(OTA_REASON_NONE));
    TEST_ASSERT_FALSE(ota_policy_reason_is_persistable(OTA_REASON_NO_URL));
    TEST_ASSERT_FALSE(ota_policy_reason_is_persistable(OTA_REASON_NO_TIME));
    TEST_ASSERT_FALSE(ota_policy_reason_is_persistable(OTA_REASON_LOCKED));
    TEST_ASSERT_FALSE(ota_policy_reason_is_persistable(OTA_REASON_UP_TO_DATE));
    TEST_ASSERT_FALSE(ota_policy_reason_is_persistable(OTA_REASON_PINNED));
    /* the two skips the failure table still names an ota_result for */
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_LOW_BATT));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_LOW_HEAP));
    /* genuine outcomes */
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_GAVE_UP));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_BAD_MANIFEST));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_NO_SCHEMA));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_NO_ENTRY));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_BAD_VERSION));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_BAD_URL));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_HTTP));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_NET));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_TLS));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_TLS_CERT));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_TIMEOUT));
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_BAD_IMAGE));
    /* out of range is not an outcome */
    TEST_ASSERT_FALSE(ota_policy_reason_is_persistable(OTA_REASON_COUNT));
}

/* Every reason must be classified by the switch, so a code added later
   cannot slip through as a default-false and silently stop being
   reported. Paired with the -Wswitch build diagnostic, which is the
   compile-time half of the same guarantee. */
void test_every_persistable_reason_has_a_string(void) {
    for (int r = 0; r < OTA_REASON_COUNT; r++) {
        if (ota_policy_reason_is_persistable((ota_reason_t)r)) {
            TEST_ASSERT_TRUE_MESSAGE(ota_policy_reason_str((ota_reason_t)r)[0] != '\0',
                                     "a persistable reason must have a code to persist");
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    /* block selection */
    RUN_TEST(test_single_schema1_block_is_selected);
    RUN_TEST(test_highest_supported_schema_wins);
    RUN_TEST(test_schema_zero_is_not_selected);
    RUN_TEST(test_non_integral_schema_is_skipped);
    RUN_TEST(test_unsupported_newest_block_falls_back_to_the_supported_one);
    RUN_TEST(test_no_supported_block_reports_no_schema);
    RUN_TEST(test_duplicate_schema_first_wins_and_is_reported);
    RUN_TEST(test_duplicate_schema_flag_is_clear_on_a_clean_manifest);
    RUN_TEST(test_duplicate_schema_flag_is_false_when_no_block_was_selected);
    RUN_TEST(test_unknown_keys_inside_a_supported_block_are_ignored);
    RUN_TEST(test_top_level_not_an_array_is_no_update);
    RUN_TEST(test_non_object_and_schemaless_elements_are_skipped);
    RUN_TEST(test_negative_schema_is_skipped);
    RUN_TEST(test_empty_array_reports_no_schema);
    /* resolution */
    RUN_TEST(test_default_applies_when_the_device_is_not_listed);
    RUN_TEST(test_device_entry_overrides_the_default);
    RUN_TEST(test_device_entry_without_a_version_is_pinned);
    RUN_TEST(test_version_equal_to_running_is_no_update);
    RUN_TEST(test_a_different_version_updates_in_both_directions);
    RUN_TEST(test_malformed_json_is_no_update);
    RUN_TEST(test_truncated_json_is_no_update_at_every_offset);
    RUN_TEST(test_empty_body_is_no_update);
    RUN_TEST(test_manifest_len_bounds_the_parse);
    RUN_TEST(test_null_inputs_do_not_crash);
    RUN_TEST(test_missing_or_empty_url_is_rejected);
    RUN_TEST(test_non_https_url_is_rejected);
    RUN_TEST(test_version_length_bound);
    RUN_TEST(test_non_string_or_empty_version_is_rejected);
    RUN_TEST(test_url_length_bound);
    RUN_TEST(test_block_with_no_entry_for_this_device_and_no_default);
    RUN_TEST(test_malformed_device_targeting_falls_back_to_default);
    RUN_TEST(test_an_object_device_entry_is_binding_and_does_not_fall_back);
    /* preconditions */
    RUN_TEST(test_gate_passes_when_all_clear);
    RUN_TEST(test_gate_skips_with_an_empty_endpoint);
    RUN_TEST(test_gate_skips_without_ntp);
    RUN_TEST(test_gate_skips_when_charge_locked);
    RUN_TEST(test_gate_skips_below_the_battery_floor);
    RUN_TEST(test_gate_ignores_an_unknown_battery_reading);
    RUN_TEST(test_download_gate_skips_on_low_heap);
    RUN_TEST(test_download_gate_also_reruns_the_check_preconditions);
    RUN_TEST(test_gate_null_input_does_not_crash);
    /* retry budget */
    RUN_TEST(test_budget_below_the_limit_attempts);
    RUN_TEST(test_budget_at_the_limit_for_the_same_target_gives_up);
    RUN_TEST(test_budget_at_the_limit_for_a_different_target_attempts);
    RUN_TEST(test_zero_max_fails_falls_back_to_the_compiled_default);
    RUN_TEST(test_budget_helper_rules);
    RUN_TEST(test_next_fail_count_increments_for_the_same_target_and_resets_otherwise);
    /* reason codes */
    RUN_TEST(test_a_cert_failure_maps_to_tls_cert_not_tls);
    RUN_TEST(test_reason_mapping_covers_the_failure_table);
    RUN_TEST(test_deadline_outranks_the_transport_error);
    RUN_TEST(test_reason_text_carries_the_http_status);
    RUN_TEST(test_every_reason_has_a_distinct_string);
    RUN_TEST(test_reason_persistability_matches_the_failure_table);
    RUN_TEST(test_every_persistable_reason_has_a_string);
    return UNITY_END();
}
