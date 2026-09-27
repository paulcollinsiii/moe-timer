#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NATIVE
#include "esp_err.h" /* esp_err_t; under NATIVE it comes from esp_compat.h */
#endif

#include "ota_policy.h" /* ota_error_facts_t, OTA_URL_MAX */

/* OTA transport classification — layer 1, pure. Host-tested in
   test_ota_facts.

   ---- why this is its own module ----

   The plan's architecture table called `ota.c` "thin — every decision is
   already made upstream", and named `ota_url.c` as the one decision that
   had been extracted. That was not true. Roughly a fifth of `ota.c` was
   pure judgement that needs no radio, no TLS peer and no flash
   partition, and it was sitting where only a hardware smoke test could
   reach it:

     - which esp-tls error codes are the TLS layer and which are just a
       socket that never came up (get this wrong and every DNS failure
       tells the operator to go look at certificates);
     - which esp_err_t values mean "these bytes are not a firmware image
       for this device" rather than "the bytes did not arrive";
     - the fallback-fact rule, which is what `ota_flow`'s "the driver
       failed but named no fact" log rests on — a guarantee that was
       asserted ABOUT rather than asserted;
     - whether a manifest body that came up short is a dead transport, a
       chunked stream that stopped early, or simply a manifest that has
       outgrown the reader.

   None of these were wrong. They were untested, which is a different
   problem with the same eventual cost.

   Everything here is a total function over its arguments. */

#ifdef __cplusplus
extern "C" {
#endif

/* What the HTTP event handler observed about one transfer.

   Lives here rather than inside `ota.c` because the two functions that
   turn it into an `ota_error_facts_t` are the ones worth testing, and a
   struct they both take has to be visible to the suite. One instance per
   transfer, addressed through `esp_http_client_config_t`'s `user_data`
   so the manifest GET (a stack local) and the image download (a
   module-static, because the transfer outlives the call) share one
   handler with no possibility of crosstalk. */
typedef struct {
    int status;                 /* last HTTP status line seen; 0 if none */
    int redirects;              /* hops followed so far, this transfer */
    char location[OTA_URL_MAX]; /* last ACCEPTED redirect target */
    bool have_location;
    bool blocked; /* a redirect was refused: this transfer is poisoned */
    bool tls_failed;
    int tls_cert_flags;
    bool transport_failed;
} ota_http_ctx_t;

/* Is this esp-tls code the TLS layer itself, or a socket that never came
   up? Everything that answers true reports to Home Assistant as `tls`,
   and an operator who reads `tls` goes looking at certificates — so DNS
   and connect failures must answer false.

   An UNKNOWN code answers false, which is the safe direction: `net`
   says "the network flaked, it will fix itself", `tls` sends someone to
   a serial cable. See the note in ota_facts.c about the upper bound. */
bool ota_facts_tls_layer_failure(esp_err_t err);

/* Is this esp_err_t "the bytes are not a firmware image for this
   device"? True lands on `bad_image`; false lands wherever the transport
   facts point, which is normally `net`. */
bool ota_facts_image_error(esp_err_t err);

/* Drain the handler's observations into the struct ota_policy will
   classify. Facts are OR-ed in so a caller may accumulate across calls;
   `http_status` is the exception and is overwritten unconditionally,
   which is why ota.h requires the caller to zero `facts` first.

   Never sets deadline_hit: that fact is ota_flow's, and layer 3 cannot
   see it. */
void ota_facts_apply_ctx(const ota_http_ctx_t *ctx, ota_error_facts_t *facts);

/* The same, plus the fallback rule: a failure that named nothing at all
   is recorded as a bare transport failure, so that ota_flow's "the
   driver failed but named no fact" branch is unreachable from any path
   that ends here. Every failure return in ota.c that got as far as
   opening a socket calls this. */
void ota_facts_fail(const ota_http_ctx_t *ctx, ota_error_facts_t *facts);

/* Did this transfer get all the way here without the transport
   complaining?

   The discriminator for "is a rejected image header really a rejected
   image". esp_https_ota's get_description_from_image returns a bare
   ESP_FAIL for two unrelated causes (esp_https_ota.c:663-666 and
   :672-675): a wrong app-descriptor magic, which is a genuinely bad
   image, and read_header failing because the socket closed before
   IMAGE_HEADER_SIZE bytes arrived (:618-621). Since ota_policy ranks
   image_rejected above transport_failed, treating both as bad_image
   tells an operator whose link drops in the first 1 KB of a 1.44 MB
   download to go and rebuild a binary that was never the problem.

   Sound because esp_http_client_read dispatches HTTP_EVENT_ERROR on a
   hard read failure (esp_http_client.c:1443-1446), so a dropped socket
   has already set one of these three flags. A short-but-CLEAN body — an
   HTML error page with a valid Content-Length — sets none of them and
   correctly stays bad_image. */
bool ota_facts_transport_was_clean(const ota_http_ctx_t *ctx);

/* Was the manifest body complete? */
typedef enum {
    OTA_BODY_OK = 0,        /* the whole body arrived */
    OTA_BODY_TRUNCATED,     /* Content-Length promised more than turned up */
    OTA_BODY_SHORT_CHUNKED, /* chunked, and the stream ended mid-body */
    OTA_BODY_OVERSIZE,      /* larger than the reader's buffer; not a transport fault */
} ota_body_t;

/* n_read      bytes the reader actually produced (never negative:
               esp_http_client_read_response returns a running count)
   content_len the Content-Length header, or 0 for a chunked response
   buf_len     the reader's buffer, which is also the cap on n_read
   complete    esp_http_client_is_complete_data_received()

   TRUNCATED and SHORT_CHUNKED are transport failures and report as
   `net`. OVERSIZE is NOT: the bytes arrived fine, there were just too
   many of them, and returning the prefix so cJSON refuses it puts
   `bad_manifest` in front of the operator — which names the real
   problem, where `net` would send them to look at their network. */
ota_body_t ota_facts_body(int n_read, int64_t content_len, size_t buf_len, bool complete);

#ifdef __cplusplus
}
#endif
