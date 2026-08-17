/* The decisions that were hiding inside the OTA driver.
 *
 * The plan's architecture table described main/ota.c as "thin — every
 * decision is already made upstream", and said its one testable decision
 * had been extracted to ota_url.c. Review found that to be false: about a
 * fifth of that file was pure judgement needing no radio, no TLS peer and
 * no flash partition, sitting where only a hardware smoke test could
 * reach it. None of it was WRONG. All of it was unasserted, which is a
 * different problem with the same eventual cost.
 *
 * Four rules live here now, and this suite is why:
 *   - which esp-tls codes are the TLS layer and which are a socket that
 *     never came up (`tls` sends an operator to a serial cable, `net`
 *     tells them to wait — getting it backwards is expensive);
 *   - which esp_err_t values mean "these bytes are not a firmware image";
 *   - the fallback-fact rule, which is what ota_flow's "the driver failed
 *     but named no fact" log rests on;
 *   - whether a short manifest body is a dead transport, a chunked stream
 *     that stopped early, or a manifest that has outgrown the reader.
 *
 * Every case is written against a specific way of getting it wrong, and
 * each was mutation-checked: introduce the defect, the suite must fail,
 * revert. A case that survives its own mutation is not evidence.
 */
#include <string.h>
#include <unity.h>

/* Single-TU. ota_policy.c comes along because the fallback-fact rule's
   whole purpose is a property OF ota_policy_reason — "no failure that
   reaches ota_facts_fail can map to OTA_REASON_NONE" — and asserting
   that against the real classifier rather than a restatement of it is
   the difference between pinning the guarantee and pinning a paraphrase.
   cJSON rides in for ota_policy.c's manifest parser, which nothing here
   calls. */
// clang-format off
#include "cJSON.h"
#include "../../main/config_validate.c"
#include "../../main/ota_policy.c"
#include "../../main/ota_facts.c"
// clang-format on

void setUp(void) {}
void tearDown(void) {}

/* ---- TLS layer vs the socket underneath it ------------------------------ */

/* The first column of the split. Everything here is "the network flaked,
   it will fix itself" — a name that did not resolve, a socket that would
   not open, a connect that timed out. Classify any of them as TLS and an
   operator reading `tls` goes looking at certificates for a DNS
   problem. */
void test_socket_failures_are_not_tls_failures(void) {
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME));
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET));
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_UNSUPPORTED_PROTOCOL_FAMILY));
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST));
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED));
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT));
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_TCP_CLOSED_FIN));
}

/* The second column: the session itself. Handshake, certificate parsing,
   key setup, session tickets, record read/write. */
void test_session_failures_are_tls_failures(void) {
    TEST_ASSERT_TRUE(ota_facts_tls_layer_failure(ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED));
    TEST_ASSERT_TRUE(ota_facts_tls_layer_failure(ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED));
    TEST_ASSERT_TRUE(ota_facts_tls_layer_failure(ESP_ERR_MBEDTLS_CERT_PARTLY_OK));
    TEST_ASSERT_TRUE(ota_facts_tls_layer_failure(ESP_ERR_MBEDTLS_SSL_SETUP_FAILED));
    TEST_ASSERT_TRUE(ota_facts_tls_layer_failure(ESP_ERR_MBEDTLS_SSL_READ_FAILED));
    /* The Secure Element path is crypto, not plumbing. */
    TEST_ASSERT_TRUE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_SE_FAILED));
}

/* The deliberate boundary pair, and the reason the carve-out list is a
   list rather than a range. These two sit either side of a line drawn on
   MEANING, not on numbering: CONNECTION_TIMEOUT covers the whole
   low-level connect and reads as a flaky network; SERVER_HANDSHAKE_TIMEOUT
   is unambiguously the TLS exchange and reads as a peer that will not
   negotiate. Their codes are adjacent-ish and their answers are
   opposite. */
void test_the_two_timeouts_are_classified_oppositely(void) {
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT));
    TEST_ASSERT_TRUE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT));
}

/* The range, at both ends. An UNKNOWN code answers false and reports as
   `net`, which is the safe direction: "wait, it may fix itself" costs an
   operator a day, "go check your certificates" costs them a serial
   cable and a trip. The _Static_assert in ota_facts.c pins the upper
   bound's value; this pins the behaviour on either side of it. */
void test_codes_outside_the_esp_tls_range_are_not_claimed(void) {
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_OK));
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_FAIL));
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_ESP_TLS_BASE - 1));
    TEST_ASSERT_TRUE(ota_facts_tls_layer_failure(ESP_ERR_MBEDTLS_SSL_READ_FAILED));
    /* One past the last code esp_tls_errors.h defines. A future IDF that
       appends here starts reporting as `net` rather than `tls` — the
       safe direction, and the reason the bound is tight rather than
       widened to the whole 0x8000 block. */
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_MBEDTLS_SSL_READ_FAILED + 1));
    /* An OTA code is not esp-tls's to claim either. */
    TEST_ASSERT_FALSE(ota_facts_tls_layer_failure(ESP_ERR_OTA_VALIDATE_FAILED));
}

/* ---- "not a firmware image" vs "the bytes did not arrive" --------------- */

void test_image_errors_are_named_exactly(void) {
    /* Chip id or chip revision mismatch, reported from the FIRST perform.
       An operator who published an ESP32-S3 build must not be sent to
       look at their wifi. */
    TEST_ASSERT_TRUE(ota_facts_image_error(ESP_ERR_INVALID_VERSION));
    /* Image validation, from finish. */
    TEST_ASSERT_TRUE(ota_facts_image_error(ESP_ERR_OTA_VALIDATE_FAILED));
    TEST_ASSERT_TRUE(ota_facts_image_error(ESP_ERR_IMAGE_INVALID));
}

/* The forward-looking one. Task 13 turns on
   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, after which esp_ota_begin
   refuses to start while the RUNNING app is still PENDING_VERIFY, and
   that refusal arrives from the first esp_https_ota_perform with no other
   fact set. Unclassified it reports as `net` — a network diagnosis for a
   condition that has nothing to do with the network. Classified now,
   because the alternative is a mislabel that only shows up once rollback
   is switched on and is then very hard to recognise. */
void test_a_pending_verify_refusal_is_not_reported_as_a_network_fault(void) {
    TEST_ASSERT_TRUE(ota_facts_image_error(ESP_ERR_OTA_ROLLBACK_INVALID_STATE));
}

void test_transport_errors_are_not_image_errors(void) {
    TEST_ASSERT_FALSE(ota_facts_image_error(ESP_OK));
    TEST_ASSERT_FALSE(ota_facts_image_error(ESP_FAIL));
    TEST_ASSERT_FALSE(ota_facts_image_error(ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST));
    TEST_ASSERT_FALSE(ota_facts_image_error(ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED));
    /* Neighbours of the codes above, so the classification is a set and
       not a range that happens to fit. */
    TEST_ASSERT_FALSE(ota_facts_image_error(ESP_ERR_OTA_PARTITION_CONFLICT));
    TEST_ASSERT_FALSE(ota_facts_image_error(ESP_ERR_OTA_SELECT_INFO_INVALID));
    TEST_ASSERT_FALSE(ota_facts_image_error(ESP_ERR_OTA_ROLLBACK_FAILED));
    TEST_ASSERT_FALSE(ota_facts_image_error(ESP_ERR_IMAGE_FLASH_FAIL));
}

/* ---- draining the handler's observations into facts --------------------- */

void test_context_facts_carry_across(void) {
    ota_http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ota_error_facts_t f;
    memset(&f, 0, sizeof(f));

    ctx.status = 404;
    ctx.tls_failed = true;
    ctx.tls_cert_flags = 0x08;
    ctx.transport_failed = true;
    ota_facts_apply_ctx(&ctx, &f);

    TEST_ASSERT_EQUAL(404, f.http_status);
    TEST_ASSERT_TRUE(f.tls_failed);
    TEST_ASSERT_EQUAL(0x08, f.tls_cert_flags);
    TEST_ASSERT_TRUE(f.transport_failed);
}

/* A refused redirect is its OWN fact. It used to fold into
   transport_failed, which made the one shape that can never succeed by
   itself — a host emitting a relative `Location` — report as `net` at
   every rollover until the retry budget capped, indistinguishable from a
   flaky link, with a fix that is not guessable from `net`. */
void test_a_refused_redirect_is_its_own_fact_not_a_transport_failure(void) {
    ota_http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ota_error_facts_t f;
    memset(&f, 0, sizeof(f));

    ctx.status = 302;
    ctx.blocked = true;
    ota_facts_fail(&ctx, &f);

    TEST_ASSERT_TRUE(f.redirect_refused);
    TEST_ASSERT_FALSE_MESSAGE(f.transport_failed, "a refusal must not be shadowed by the generic fallback");
    TEST_ASSERT_EQUAL(OTA_REASON_BAD_REDIRECT, ota_policy_reason(&f));
}

/* The one unconditional write in apply_ctx, and the reason ota.h now
   REQUIRES the caller to zero `facts` before every call. Everything else
   ORs in; http_status is assigned. Reuse one struct across the manifest
   fetch and the download and the second call silently resets the first's
   404 to 0, turning a missing manifest into `net`. */
void test_http_status_is_overwritten_not_merged(void) {
    ota_http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ota_error_facts_t f;
    memset(&f, 0, sizeof(f));

    f.http_status = 404;
    ctx.status = 0; /* a second transfer that never saw a status line */
    ota_facts_apply_ctx(&ctx, &f);
    TEST_ASSERT_EQUAL_MESSAGE(0, f.http_status, "http_status is an assignment; ota.h documents the consequence");
}

void test_null_arguments_are_answered_not_dereferenced(void) {
    ota_error_facts_t f;
    memset(&f, 0, sizeof(f));
    ota_facts_apply_ctx(NULL, &f);
    ota_facts_fail(NULL, &f);
    ota_facts_apply_ctx(NULL, NULL);
    ota_facts_fail(NULL, NULL);
    /* fail() with a NULL ctx still owes the caller a fact, because its
       whole job is that a failure is never fact-less. */
    TEST_ASSERT_TRUE(f.transport_failed);
}

/* ---- the fallback rule, which is a guarantee rather than a convenience -- */

/* ota_flow logs "the driver failed but named no fact" as a BUG in ota.c
   and falls back to `net`. This is the rule that makes that branch
   unreachable from any path that ends in ota_facts_fail — and asserting
   it against the real ota_policy_reason is the point, because the
   guarantee is stated in terms of that function. It was previously only
   reasoned about in a comment. */
void test_no_failure_shape_can_produce_no_reason(void) {
    /* Every combination of the four booleans the handler can set, times
       a few statuses. 2^4 * 4 = 64 shapes, all of which must name
       something. */
    const int statuses[] = {0, 200, 302, 500};
    for (unsigned bits = 0; bits < 16u; bits++) {
        for (unsigned s = 0; s < sizeof(statuses) / sizeof(statuses[0]); s++) {
            ota_http_ctx_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.tls_failed = (bits & 1u) != 0;
            ctx.transport_failed = (bits & 2u) != 0;
            ctx.blocked = (bits & 4u) != 0;
            ctx.tls_cert_flags = (bits & 8u) ? 0x08 : 0;
            ctx.status = statuses[s];

            ota_error_facts_t f;
            memset(&f, 0, sizeof(f));
            ota_facts_fail(&ctx, &f);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(OTA_REASON_NONE, ota_policy_reason(&f),
                                          "a failure that named no fact would be logged as an ota.c defect");
        }
    }
}

/* The emptiest possible failure: nothing observed at all. This is the
   case the fallback exists for. */
void test_a_failure_that_observed_nothing_still_reports_net(void) {
    ota_http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ota_error_facts_t f;
    memset(&f, 0, sizeof(f));
    ota_facts_fail(&ctx, &f);
    TEST_ASSERT_TRUE(f.transport_failed);
    TEST_ASSERT_EQUAL(OTA_REASON_NET, ota_policy_reason(&f));
}

/* ...and the other half: the fallback must not fire when something more
   specific is already known, or every tls_cert would arrive carrying a
   transport_failed it did not earn. */
void test_the_fallback_defers_to_every_more_specific_fact(void) {
    ota_http_ctx_t ctx;
    ota_error_facts_t f;

    memset(&ctx, 0, sizeof(ctx));
    memset(&f, 0, sizeof(f));
    ctx.tls_failed = true;
    ota_facts_fail(&ctx, &f);
    TEST_ASSERT_FALSE(f.transport_failed);

    memset(&ctx, 0, sizeof(ctx));
    memset(&f, 0, sizeof(f));
    ctx.tls_cert_flags = 0x08;
    ota_facts_fail(&ctx, &f);
    TEST_ASSERT_FALSE(f.transport_failed);

    memset(&ctx, 0, sizeof(ctx));
    memset(&f, 0, sizeof(f));
    ctx.status = 500;
    ota_facts_fail(&ctx, &f);
    TEST_ASSERT_FALSE(f.transport_failed);

    /* image_rejected is set by the CALLER before fail() runs — that is
       how ota.c reports a bad header — so the rule has to respect a fact
       it did not put there itself. */
    memset(&ctx, 0, sizeof(ctx));
    memset(&f, 0, sizeof(f));
    f.image_rejected = true;
    ota_facts_fail(&ctx, &f);
    TEST_ASSERT_FALSE(f.transport_failed);

    /* A status below 400 is NOT a fact — ota_policy only folds >= 400 —
       so this one must still fall back. */
    memset(&ctx, 0, sizeof(ctx));
    memset(&f, 0, sizeof(f));
    ctx.status = 302;
    ota_facts_fail(&ctx, &f);
    TEST_ASSERT_TRUE_MESSAGE(f.transport_failed, "a 3xx with no other fact would otherwise report nothing at all");
}

/* ---- was the transport clean? ------------------------------------------ */

/* A link that drops inside the first 1 KB of a 1.44 MB download used to
   report `bad_image`, because esp_https_ota returns the SAME bare
   ESP_FAIL for "the app-descriptor magic is wrong" and "the socket
   closed before the header arrived". Since image_rejected outranks
   transport_failed, the operator was told to go and rebuild and
   republish a binary that was perfectly fine. */
void test_a_dropped_socket_is_not_a_clean_transport(void) {
    ota_http_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.transport_failed = true;
    TEST_ASSERT_FALSE(ota_facts_transport_was_clean(&ctx));

    memset(&ctx, 0, sizeof(ctx));
    ctx.tls_failed = true;
    TEST_ASSERT_FALSE(ota_facts_transport_was_clean(&ctx));

    memset(&ctx, 0, sizeof(ctx));
    ctx.tls_cert_flags = 0x08;
    TEST_ASSERT_FALSE(ota_facts_transport_was_clean(&ctx));

    /* Claiming a clean transport is the expensive direction, so no
       evidence answers "not clean". */
    TEST_ASSERT_FALSE(ota_facts_transport_was_clean(NULL));
}

/* The other half, and the reason this is a discriminator rather than a
   blanket suppression: a host that serves an HTML error page with a
   valid Content-Length produces NO error event. The bytes arrived
   perfectly; they are simply not a firmware image. That must still read
   as bad_image. */
void test_a_clean_transport_carrying_the_wrong_bytes_stays_a_bad_image(void) {
    ota_http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.status = 200;
    TEST_ASSERT_TRUE(ota_facts_transport_was_clean(&ctx));
    /* A redirect refusal is not a transport complaint either — it is its
       own fact, handled on its own path. */
    ctx.blocked = true;
    TEST_ASSERT_TRUE(ota_facts_transport_was_clean(&ctx));
}

/* ---- manifest completeness --------------------------------------------- */

#define BUF 100

/* The reason this rule exists at all: read_response stops at the buffer
   end OR at a socket that died and reports both as "this is what I got".
   Content-Length is the only thing that separates them. */
void test_a_body_that_promised_more_than_it_delivered_is_a_transport_failure(void) {
    TEST_ASSERT_EQUAL(OTA_BODY_TRUNCATED, ota_facts_body(30, 50, BUF, false));
    TEST_ASSERT_EQUAL(OTA_BODY_TRUNCATED, ota_facts_body(0, 50, BUF, false));
    /* Even a body whose declared length was larger than the buffer: if
       the reader did not even manage to FILL the buffer, the socket died
       and that is a transport fault, not an oversized manifest. */
    TEST_ASSERT_EQUAL(OTA_BODY_TRUNCATED, ota_facts_body(60, 500, BUF, false));
}

/* The case review flagged as a possible silent success, and the answer
   is that it is neither silent nor a success: a manifest whose declared
   length exceeds the reader gets its own verdict, and the bytes are
   deliberately returned anyway so cJSON refuses the cut-off array and
   the operator reads `bad_manifest`.
   Calling this TRUNCATED instead would report `net` and send someone to
   look at a network that is working perfectly. */
void test_a_manifest_larger_than_the_buffer_is_oversize_not_a_transport_failure(void) {
    TEST_ASSERT_EQUAL(OTA_BODY_OVERSIZE, ota_facts_body(BUF, 500, BUF, false));
    TEST_ASSERT_EQUAL(OTA_BODY_OVERSIZE, ota_facts_body(BUF, BUF + 1, BUF, false));
    /* Chunked, so no declared length: filling the buffer without the
       parser ever reporting completion means the same thing. */
    TEST_ASSERT_EQUAL(OTA_BODY_OVERSIZE, ota_facts_body(BUF, 0, BUF, false));
}

void test_a_complete_body_is_complete(void) {
    TEST_ASSERT_EQUAL(OTA_BODY_OK, ota_facts_body(50, 50, BUF, false));
    TEST_ASSERT_EQUAL(OTA_BODY_OK, ota_facts_body(0, 0, BUF, true));
    /* Chunked and complete, at any length. */
    TEST_ASSERT_EQUAL(OTA_BODY_OK, ota_facts_body(40, 0, BUF, true));
}

/* A body that is EXACTLY the buffer width and arrived whole. The old
   rule tested `n_read == len` on its own and warned "it will not parse"
   about a manifest that parses perfectly well. Content-Length is
   authoritative when it is present, and the completeness flag is
   authoritative when it is not — neither is "the buffer happens to be
   full". */
void test_a_body_that_exactly_fills_the_buffer_is_not_accused_of_overflowing(void) {
    TEST_ASSERT_EQUAL(OTA_BODY_OK, ota_facts_body(BUF, BUF, BUF, false));
    TEST_ASSERT_EQUAL(OTA_BODY_OK, ota_facts_body(BUF, 0, BUF, true));
}

/* Chunked responses report no length, so the parser's own flag is the
   only evidence there is that the stream finished. */
void test_a_chunked_stream_that_stopped_early_is_a_transport_failure(void) {
    TEST_ASSERT_EQUAL(OTA_BODY_SHORT_CHUNKED, ota_facts_body(40, 0, BUF, false));
    TEST_ASSERT_EQUAL(OTA_BODY_SHORT_CHUNKED, ota_facts_body(0, 0, BUF, false));
}

/* Values ota.c cannot actually produce, answered anyway because a total
   function has to be total. */
void test_impossible_inputs_get_the_cautious_answer(void) {
    TEST_ASSERT_EQUAL(OTA_BODY_TRUNCATED, ota_facts_body(-1, 50, BUF, true));
    TEST_ASSERT_EQUAL(OTA_BODY_TRUNCATED, ota_facts_body(0, 0, 0, true));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_socket_failures_are_not_tls_failures);
    RUN_TEST(test_session_failures_are_tls_failures);
    RUN_TEST(test_the_two_timeouts_are_classified_oppositely);
    RUN_TEST(test_codes_outside_the_esp_tls_range_are_not_claimed);

    RUN_TEST(test_image_errors_are_named_exactly);
    RUN_TEST(test_a_pending_verify_refusal_is_not_reported_as_a_network_fault);
    RUN_TEST(test_transport_errors_are_not_image_errors);

    RUN_TEST(test_context_facts_carry_across);
    RUN_TEST(test_a_refused_redirect_is_its_own_fact_not_a_transport_failure);
    RUN_TEST(test_http_status_is_overwritten_not_merged);
    RUN_TEST(test_null_arguments_are_answered_not_dereferenced);

    RUN_TEST(test_no_failure_shape_can_produce_no_reason);
    RUN_TEST(test_a_failure_that_observed_nothing_still_reports_net);
    RUN_TEST(test_the_fallback_defers_to_every_more_specific_fact);

    RUN_TEST(test_a_dropped_socket_is_not_a_clean_transport);
    RUN_TEST(test_a_clean_transport_carrying_the_wrong_bytes_stays_a_bad_image);

    RUN_TEST(test_a_body_that_promised_more_than_it_delivered_is_a_transport_failure);
    RUN_TEST(test_a_manifest_larger_than_the_buffer_is_oversize_not_a_transport_failure);
    RUN_TEST(test_a_complete_body_is_complete);
    RUN_TEST(test_a_body_that_exactly_fills_the_buffer_is_not_accused_of_overflowing);
    RUN_TEST(test_a_chunked_stream_that_stopped_early_is_a_transport_failure);
    RUN_TEST(test_impossible_inputs_get_the_cautious_answer);
    return UNITY_END();
}
