/* OTA transport classification — see include/ota_facts.h for why these
   four decisions are their own module rather than branches inside
   ota.c. */
#include "ota_facts.h"

#ifndef NATIVE
#include "esp_image_format.h" /* ESP_ERR_IMAGE_INVALID */
#include "esp_ota_ops.h"      /* ESP_ERR_OTA_* */
#include "esp_tls_errors.h"   /* ESP_ERR_ESP_TLS_* / ESP_ERR_MBEDTLS_* */
#endif
/* Under NATIVE the same codes come from test/mocks/esp_compat.h, which
   mirrors the real values in IDF's declaration order. The assertion
   below is what keeps that mirror honest: it is checked in BOTH builds,
   so a drift between the mock and the real header fails the host suite
   rather than waiting for hardware. */

/* The upper bound of the esp-tls range, pinned.

   tls_layer_failure below reads "anything in [BASE, READ_FAILED] that is
   not carved out as a socket failure is the TLS layer". That rests on
   ESP_ERR_MBEDTLS_SSL_READ_FAILED being the LAST code esp_tls_errors.h
   defines, which is a property of a header this tree does not own.

   What this assertion catches: a renumbering, and a mock that has
   drifted from the real header. What it does NOT catch, stated plainly
   rather than implied: an IDF that APPENDS a code at +0x1E. There is no
   count marker in that header to assert against, so nothing can.

   The consequence of the case it cannot catch is bounded and points the
   safe way: a newly appended code falls OUTSIDE the range, answers
   false, and reports as `net` — "the network flaked, it will fix
   itself". Widening the range to the whole 0x8000..0x80FF block would
   catch such a code automatically but would classify a newly added
   SOCKET failure as `tls`, which sends an operator to a serial cable
   over a DNS problem. Wrong in the expensive direction, so the tight
   bound stays. */
_Static_assert(ESP_ERR_MBEDTLS_SSL_READ_FAILED == ESP_ERR_ESP_TLS_BASE + 0x1D,
               "esp_tls_errors.h has been renumbered or extended: re-check tls_layer_failure's upper bound");

bool ota_facts_tls_layer_failure(esp_err_t err) {
    switch (err) {
        /* The socket, not the session. Everything in this list reads as
           "the network flaked". The boundary cases are deliberate:
           CONNECTION_TIMEOUT covers the whole low-level connect, while
           SERVER_HANDSHAKE_TIMEOUT is unambiguously the TLS exchange and
           is therefore NOT here. */
        case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:
        case ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET:
        case ESP_ERR_ESP_TLS_UNSUPPORTED_PROTOCOL_FAMILY:
        case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:
        case ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED:
        case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:
        case ESP_ERR_ESP_TLS_TCP_CLOSED_FIN:
            return false;
        default:
            break;
    }
    /* Everything else esp-tls defines is the TLS layer itself:
       handshake, certificate parsing, session setup. Anything outside
       the range is not esp-tls's to claim. */
    return err >= ESP_ERR_ESP_TLS_BASE && err <= ESP_ERR_MBEDTLS_SSL_READ_FAILED;
}

bool ota_facts_image_error(esp_err_t err) {
    switch (err) {
        /* "The bytes are not a firmware image for this device."
           esp_https_ota reports a chip id or chip revision mismatch as
           ESP_ERR_INVALID_VERSION from the FIRST perform, and a failed
           image validation as ESP_ERR_OTA_VALIDATE_FAILED from finish.
           Both must land on bad_image, not net, or an operator who
           published an ESP32-S3 build goes looking at their wifi. */
        case ESP_ERR_INVALID_VERSION:
        case ESP_ERR_OTA_VALIDATE_FAILED:
        case ESP_ERR_IMAGE_INVALID:
            return true;
        /* The odd one out, and it is here on purpose rather than by
           analogy. Once CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is set
           (plan task 13), esp_ota_begin refuses to start while the
           RUNNING app is still PENDING_VERIFY (esp_ota_ops.c:166-171),
           and that refusal surfaces from the first esp_https_ota_perform
           (esp_https_ota.c:738) — i.e. from dl_step, with no other fact
           set, so it would otherwise be reported as `net`.

           It is not literally a bad image: it says this DEVICE is not in
           a state to accept one. bad_image is the least wrong bucket
           available — it at least tells the operator the problem is not
           their network — and a dedicated reason code is more surface
           than a condition ota_mark_valid_if_pending should already have
           cleared by the next wake. Classified now rather than after
           task 13 lands, because the alternative is a mislabel that only
           appears once rollback is switched on. */
        case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
            return true;
        default:
            return false;
    }
}

void ota_facts_apply_ctx(const ota_http_ctx_t *ctx, ota_error_facts_t *facts) {
    if (ctx == NULL || facts == NULL)
        return;
    /* The one unconditional overwrite in here, and the reason ota.h
       requires a zeroed struct per call: a caller that reused one across
       the manifest fetch and the download would have a 404 from the
       first quietly reset to 0 by the second. */
    facts->http_status = ctx->status;
    if (ctx->tls_cert_flags != 0)
        facts->tls_cert_flags = ctx->tls_cert_flags;
    if (ctx->tls_failed)
        facts->tls_failed = true;
    if (ctx->transport_failed)
        facts->transport_failed = true;
    /* A refused redirect is its OWN fact, not a transport failure. It
       used to fold into transport_failed, which meant the one shape that
       can never succeed on its own — a host answering with a relative
       Location — reported as `net` every rollover until the budget
       capped, indistinguishable from a flaky link. */
    if (ctx->blocked)
        facts->redirect_refused = true;
}

void ota_facts_fail(const ota_http_ctx_t *ctx, ota_error_facts_t *facts) {
    if (facts == NULL)
        return;
    ota_facts_apply_ctx(ctx, facts);
    /* The fallback. ota_flow logs "the driver failed but named no fact"
       as a bug in ota.c; this is what makes that branch unreachable from
       any path that ends here, and test_ota_facts asserts it directly
       rather than reasoning about it. */
    if (!facts->tls_failed && facts->tls_cert_flags == 0 && !facts->image_rejected && !facts->redirect_refused &&
        facts->http_status < 400)
        facts->transport_failed = true;
}

bool ota_facts_transport_was_clean(const ota_http_ctx_t *ctx) {
    /* A NULL context is no evidence of a clean transport, and the
       expensive mistake here is claiming one. */
    if (ctx == NULL)
        return false;
    return !ctx->tls_failed && !ctx->transport_failed && ctx->tls_cert_flags == 0;
}

ota_body_t ota_facts_body(int n_read, int64_t content_len, size_t buf_len, bool complete) {
    /* Neither can happen through ota.c — read_response returns a running
       count and the buffer is a fixed static — but a total function has
       to answer anyway, and "I cannot vouch for these bytes" is the
       honest answer. */
    if (n_read < 0 || buf_len == 0)
        return OTA_BODY_TRUNCATED;
    size_t got = (size_t)n_read;

    if (content_len > 0) {
        /* A body that cannot fit whatever happens. Separated from the
           short-read case below because they are different problems
           with different fixes: this one is a manifest that has outgrown
           the device, and calling it a transport failure would send the
           operator to look at their network. The bytes are returned
           anyway — cJSON refuses the cut-off array and the outcome
           reaches Home Assistant as bad_manifest, which names it. */
        if ((uint64_t)content_len > (uint64_t)buf_len)
            return (got == buf_len) ? OTA_BODY_OVERSIZE : OTA_BODY_TRUNCATED;
        /* Content-Length is the only thing that separates "the buffer
           ended" from "the socket died", so a body that promised a
           length and did not deliver it is a transport failure. */
        return ((int64_t)got < content_len) ? OTA_BODY_TRUNCATED : OTA_BODY_OK;
    }

    /* No Content-Length: a chunked response, where the parser's own
       completeness flag is the only evidence there is. Checked BEFORE
       the buffer-full test, so a chunked body that happens to be exactly
       buf_len bytes and arrived whole is not accused of overflowing. */
    if (complete)
        return OTA_BODY_OK;
    return (got == buf_len) ? OTA_BODY_OVERSIZE : OTA_BODY_SHORT_CHUNKED;
}
