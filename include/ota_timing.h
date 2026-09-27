#pragma once

/* OTA timing — the numbers that have to agree with each other, in one
   place, with the agreement checked by the compiler.

   ---- why this header exists ----

   Two constants used to live in two files and were compatible only by
   coincidence. `ota.c` set a per-socket `timeout_ms`; `ota_flow.c` armed
   the awake failsafe for `max_sec` plus an "abort tail" and gave the
   download loop `max_sec` exactly. The tail is what pays for everything
   that happens AFTER the deadline expires, and the socket timeout is the
   dominant term in that bill — but nothing tied them together, so a
   10 s timeout sat behind a 5 s tail and the failsafe could beat the
   deadline it was supposed to outlast. When the failsafe wins the device
   deep-sleeps mid-transfer: no abort, no `ota_result`, nothing observed.

   They are one decision, so they are one header, and the `_Static_assert`
   at the bottom is what stops them drifting apart again.

   ---- what the tail has to pay for ----

     deadline expires
       -> the in-flight dl_step has to RETURN before the loop can look at
          the clock at all                     ... OTA_DL_STEP_WORST_MS
       -> dl_abort discards the partial image  ... OTA_DL_ABORT_MS
       -> session_end brings the radio down    ... OTA_SESSION_END_MS
     failsafe must still not have fired

   ---- deriving OTA_DL_STEP_WORST_MS honestly ----

   One `dl_step` is one `esp_https_ota_perform`, which is one
   `esp_http_client_read` into a buffer of
   `MAX(http_config.buffer_size, DEFAULT_OTA_BUF_SIZE)`
   (esp_https_ota.c:561). That read is a `while (need_read > 0 &&
   is_data_remain)` loop (esp_http_client.c:1398-1458) calling
   `esp_transport_read(..., client->timeout_ms)` per iteration, capped at
   `client->buffer_size_rx` bytes — which `esp_http_client_init` takes
   straight from `config->buffer_size` (esp_http_client.c:574), falling
   back to DEFAULT_HTTP_BUF_SIZE (512) when it is left at 0.

   So the ONE config field below sets both halves of the ratio, and
   leaving it unset was the actual defect: the OTA buffer was 1024 while
   the per-iteration cap was 512, making the worst case TWO full timeout
   periods rather than one.

   ---- the case this does NOT bound, stated rather than implied ----

   An iteration that times out having already accumulated bytes returns
   the partial count (esp_http_client.c:1428-1435), which ends the loop —
   so a link that simply DIES is bounded by one timeout period, and that
   is the case the tail is sized for. A peer that trickles one byte just
   before each timeout expires is a different matter: every such
   iteration succeeds, `need_read` drops by one, and the loop goes round
   again. That is bounded only by OTA_DL_BUF_SIZE iterations, which no
   choice of timeout makes small.

   Nothing here fixes that, and nothing can. What covers it is
   ota_flow.c's pre-charge of the retry budget: the attempt is written to
   NVS BEFORE it starts, so even an attempt the failsafe kills outright
   is counted on the next boot and the budget still converges. See the
   comment above the pre-charge in ota_flow_apply. */

/* Per-socket network timeout, and the granularity of ota_flow's deadline
   check. Lowering it is cheap — it yields more, shorter dl_step calls,
   which makes the deadline FINER, and a read that times out with nothing
   to show returns EAGAIN to a caller that handles it. Raising it is not
   cheap: it is multiplied by OTA_DL_STEP_READS into the tail below, and
   the assertion at the bottom is what will stop you.

   Three seconds is generous for a TLS handshake over a marginal link
   (this same field bounds the connect) and small against
   CONFIG_MAGTAG_OTA_MAX_SEC, which defaults to 300. */
#define OTA_HTTP_TIMEOUT_MS 3000

/* esp_http_client_config_t.buffer_size, set EXPLICITLY rather than
   inherited. It is both the OTA read buffer and the per-iteration
   transport read cap, so pinning it is what makes OTA_DL_STEP_READS a
   known constant instead of a property of whatever default the IDF
   happens to ship. An assertion resting on an inherited default is a
   weak assertion. */
#define OTA_DL_BUF_SIZE 1024

/* esp_https_ota's own DEFAULT_OTA_BUF_SIZE (esp_https_ota.c:26, which is
   IMAGE_HEADER_SIZE, 1024 at IDF 6). Mirrored because the allocation is
   a MAX against it and the header is private to that .c file. If a
   future IDF raises it past OTA_DL_BUF_SIZE the ratio below grows, and
   the assertion is what notices. */
#define OTA_DL_IDF_MIN_BUF 1024

#define OTA_DL_ALLOC_BYTES ((OTA_DL_BUF_SIZE) > (OTA_DL_IDF_MIN_BUF) ? (OTA_DL_BUF_SIZE) : (OTA_DL_IDF_MIN_BUF))

/* Inner esp_transport_read iterations one dl_step can be made to take by
   a peer that stops answering. 1 while the two constants above are
   equal. */
#define OTA_DL_STEP_READS (((OTA_DL_ALLOC_BYTES) + (OTA_DL_BUF_SIZE)-1) / (OTA_DL_BUF_SIZE))

/* The longest a single dl_step can block against a link that has gone
   quiet — the overshoot ota_flow's deadline can suffer, because the
   clock is only consulted between steps. */
#define OTA_DL_STEP_WORST_MS (OTA_DL_STEP_READS * OTA_HTTP_TIMEOUT_MS)

/* esp_https_ota_abort: closes the client and frees the handle. Fast, but
   it can put a TLS close-notify on a socket that is already misbehaving,
   so it is budgeted rather than assumed free. */
#define OTA_DL_ABORT_MS 1000

/* wifi_session_end waits up to a second for the stack to answer. */
#define OTA_SESSION_END_MS 1000

/* How much longer than the download's own budget the awake failsafe is
   armed for. NOT slack — it is the abort tail costed above, and it must
   cover every one of those terms with room to spare.
   ota_flow_apply arms `max_sec + OTA_ABORT_TAIL_MS / 1000` seconds while
   the loop's deadline is `max_sec` exactly, which is why the assertion
   below also insists this divides evenly into seconds: a remainder would
   be truncated away by that division and the tail actually armed would
   be shorter than the one asserted here.

   The headroom over the strict minimum is deliberate, and it is what
   absorbs a future IDF whose DEFAULT_OTA_BUF_SIZE outgrows
   OTA_DL_BUF_SIZE (which doubles OTA_DL_STEP_WORST_MS) without an
   immediate build break. */
#define OTA_ABORT_TAIL_MS 10000

_Static_assert(OTA_ABORT_TAIL_MS % 1000 == 0,
               "the abort tail is armed in whole seconds; a remainder is silently truncated away");
_Static_assert(OTA_ABORT_TAIL_MS > OTA_DL_STEP_WORST_MS + OTA_DL_ABORT_MS + OTA_SESSION_END_MS,
               "the awake failsafe can fire before the download's own deadline finishes tearing the transfer down: "
               "lower OTA_HTTP_TIMEOUT_MS, or raise OTA_ABORT_TAIL_MS");
