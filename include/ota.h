#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "ota_flow.h"   /* ota_step_t — these functions ARE ota_flow_ops_t */
#include "ota_policy.h" /* ota_error_facts_t */

/* OTA transport — layer 3. Moves bytes and nothing else: every decision
   this driver acts on was made upstream (ota_policy answers "may I" and
   "should I", ota_flow owns the order and the deadline), so what is left
   here is esp_http_client, esp_https_ota, one call into app_update, and
   an honest account of what went wrong.

   The five download-side functions exist to be assigned straight into
   ota_flow_ops_t, so their signatures are ota_flow.h's and the contracts
   stated there are contracts on THIS file:

     - a dl_begin that answers false has already cleaned up; nothing is in
       flight and no abort follows;
     - dl_finish frees its handle whichever way it answers;
     - exactly one of finish/abort runs per begun transfer, so the
       esp_https_ota handle is a module-static and one transfer is in
       flight at a time.

   ---- CALLERS MUST ZERO `facts` BEFORE EVERY CALL ----

   Not a style preference. Facts are OR-ed in, so a struct reused across
   two calls accumulates — except `http_status`, which is written
   UNCONDITIONALLY (ota_facts_apply_ctx). Reusing one struct for the
   manifest fetch and then the download would let the second call reset a
   404 the first one recorded to 0, and the operator would read `net` for
   a missing manifest. ota_flow.c memsets before every call today; this
   line is here so that stays true by requirement rather than by luck.

   ---- what this module IS and IS NOT tested by ----

   Hardware smoke test for everything that touches the radio, the TLS
   peer or the flash partition. The decisions that touch none of those
   were extracted and DO have suites:

     - ota_url.c   -- "may I follow this redirect", the security-
                      load-bearing half (test_ota_url);
     - ota_facts.c -- which esp-tls codes are TLS, which esp_err_t values
                      mean "not a firmware image", the fallback-fact
                      rule, and manifest completeness (test_ota_facts).

   The second of those was added after review pointed out that this
   header's earlier claim -- that ota_url.c was the ONE testable decision
   in here -- was simply false; roughly a fifth of ota.c was pure
   judgement reachable only from hardware.

   ---- what bounds a download, and what does not ----

   The dl_step LOOP is bounded by ota_flow's deadline, and the awake
   failsafe outlasts it by an abort tail whose arithmetic is asserted in
   ota_timing.h.

   dl_begin is NOT bounded. esp_https_ota's connect loop has no hop
   counter, and its read_header retries EAGAIN forever, so a wedged peer
   can hold it past the failsafe. Closing the client from the event
   handler bounds the redirect case; nothing bounds the stalled-header
   case. What covers both -- and brownout and battery pull with them --
   is ota_flow charging the retry budget BEFORE the attempt rather than
   after an observed failure, so an attempt killed mid-flight is still
   counted on the next boot. */

#ifdef __cplusplus
extern "C" {
#endif

/* GET the manifest into buf. Returns bytes written, or < 0 with facts
   filled.

   Does NOT NUL-terminate: the return value is authoritative, because
   that is what ota_policy_decide reads and because a truncated body is
   the realistic failure. A body larger than buf is returned truncated
   rather than as an error — cJSON will refuse it and the outcome reaches
   Home Assistant as bad_manifest, which is the right thing to tell an
   operator whose manifest has outgrown the device. */
int ota_manifest_get(const char *url, char *buf, size_t len, ota_error_facts_t *facts);

/* Open the image transfer, validate the app descriptor, leave the socket
   ready for the first step. false means nothing is in flight. */
bool ota_download_begin(const char *url, ota_error_facts_t *facts);

/* Exactly one esp_https_ota_perform per call — the loop belongs to
   ota_flow, which checks its deadline between calls. */
ota_step_t ota_download_step(ota_error_facts_t *facts);

/* COMMIT. esp_https_ota_finish is what sets the boot partition. Frees the
   handle whether it answers true or false. */
bool ota_download_finish(ota_error_facts_t *facts);

/* Discard the partial image and free the handle. Pairs only with a failed
   step or an expired deadline. */
void ota_download_abort(void);

/* Cancel the pending-verify rollback, if this boot is one.

   Called from wake_flow's pre-sleep point (task 12 owns the call site)
   once the wake has demonstrably worked. Lives here rather than in main.c
   because esp_ota_mark_app_valid_cancel_rollback() is a bare call, not a
   handle, and the residency rule keeps those out of main.c.

   A no-op on every boot that is not PENDING_VERIFY, which is all of them
   until CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is turned on (plan task
   13). Checking the state first is what makes the "_if_pending" in the
   name true, and what keeps the log line meaningful. */
void ota_mark_valid_if_pending(void);

#ifdef __cplusplus
}
#endif
