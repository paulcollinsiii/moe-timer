#pragma once
#include "ota_policy.h" /* OTA_URL_MAX */

/* Redirect acceptance for the OTA transport — layer 1, pure. One total
   function over its arguments: no clock, no NVS, no ESP-IDF includes, no
   I/O. Host-tested in test_ota_url.

   ---- why this is not a few lines inside ota.c ----

   config_is_https_url() closes the "an unauthenticated firmware endpoint
   is an arbitrary-code-execution channel" hole at the config boundary.
   The transport reopens it one hop later:
   esp_http_client_set_redirection() is esp_http_client_set_url(client,
   client->location) with NO scheme check, and esp_https_ota.c calls it
   for any 3xx. So an endpoint that passed validation, whose host answers
   "302 Location: http://...", is fetched in the clear.

   "Given this Location header and this hop count, do I follow it?" is a
   pure question, and it is the single decision standing between the
   config-time https guarantee and a plaintext firmware fetch. Left inside
   ota.c it would be reachable only from hardware, which is to say never
   asserted. Here it is thirty lines and a suite.

   ---- the rules, and why each one ----

   ABSOLUTE https ONLY. A relative target ("/firmware/x.bin") would in
   fact be safe to follow — esp_http_client keeps the current scheme, so
   the hop stays https and the pinned root still authenticates it. It is
   refused anyway, because accepting it would mean re-implementing
   config_is_https_url's character rule for the schemeless case, and a
   security rule with two implementations is a security rule that will
   drift. Every common server (nginx with the default absolute_redirect,
   Caddy, Apache, GitHub) emits an absolute Location, and a host that does
   not can be pointed at directly from the manifest.

   LENGTH IS A REFUSAL, NOT A TRUNCATION. A target that does not fit
   OTA_URL_MAX is rejected outright: silently following a truncated URL is
   how you end up fetching something nobody named.

   BOUNDED HOPS. esp_https_ota's own connect loop has no redirect cap at
   all (it loops while the status says "redirect", and never consults
   esp_http_client's max_redirection_count), so a host answering 302 with
   a Location pointing at itself would spin until the awake failsafe fired.
   The count is the caller's to keep — this module only judges it. */

#ifdef __cplusplus
extern "C" {
#endif

/* Hops the OTA transport will follow. Three is generous for the one
   shape this is expected to meet in practice (a hosting path that has
   moved once); it exists to bound a loop, not to enable a chain. */
#define OTA_MAX_REDIRECTS 3

typedef enum {
    OTA_REDIRECT_FOLLOW = 0, /* absolute https, in budget: go */
    OTA_REDIRECT_NO_TARGET,  /* 3xx with no Location, or an empty one */
    OTA_REDIRECT_NOT_HTTPS,  /* the whole reason this module exists */
    OTA_REDIRECT_TOO_LONG,   /* will not fit OTA_URL_MAX; refused, not truncated */
    OTA_REDIRECT_TOO_MANY,   /* hop budget spent */
} ota_redirect_t;

/* May this redirect be followed? location is the raw Location header
   value (NULL and "" are answered, not dereferenced past their NUL).
   redirects_done is how many hops this transfer has already taken; a
   negative value is read as a corrupt counter and fails closed.

   The scheme test comes BEFORE the budget test on purpose: "your host
   redirected firmware to plain http" is true however many hops in it
   happens, and it is the one refusal that names a misconfiguration the
   operator has to go fix rather than a limit this firmware chose. */
ota_redirect_t ota_url_redirect_check(const char *location, int redirects_done, int max_redirects);

/* Stable short code for the outcome, for the log line that accompanies a
   refusal. "" for OTA_REDIRECT_FOLLOW and for anything out of range. */
const char *ota_url_redirect_str(ota_redirect_t r);

#ifdef __cplusplus
}
#endif
