/* OTA transport — layer 3. See include/ota.h for the contracts; this
   file is the half of them that only hardware can check.

   Two things in here are load-bearing beyond "it moves bytes", and both
   exist because nothing below this layer can do them:

   1. THE https GUARANTEE DOES NOT SURVIVE THE TRANSPORT.
      esp_http_client_set_redirection() is esp_http_client_set_url(client,
      client->location) with no scheme check, and esp_https_ota.c calls it
      for any 3xx. A URL that config_is_https_url() blessed, whose host
      answers "302 Location: http://...", is fetched in the clear — the
      arbitrary-code-execution channel reopened one hop after it was
      closed. The defence has three parts, below, and the decision half of
      it lives in ota_url.c where a host test can reach it.

   2. THE FACTS ARE WHAT THE OPERATOR SEES.
      ota_error_facts_t is the only evidence that survives to Home
      Assistant: the second window runs after MQTT has closed, so a
      download failure reaches a human only as a reason code read out of
      NVS the next morning. tls_cert vs tls in particular is the
      difference between "the pinned root is wrong and needs a serial
      visit" and "the network flaked, it will fix itself" — so the mbedtls
      verify bitmask is read from the one place and the one moment it is
      populated, rather than approximated. */
#include "ota.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "config_validate.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_tls.h"
#include "ota_facts.h"  /* the pure classification this file used to inline */
#include "ota_timing.h" /* the socket timeout, coupled to ota_flow's abort tail */
#include "ota_url.h"
#include "sdkconfig.h"

/* Structural, not advisory. With ALLOW_HTTP set, esp_https_ota_begin
   accepts a config carrying no server verification at all, which would
   make every defence in this file decorative. It is `not set` today and
   the only way it becomes set is a deliberate menuconfig edit — this is
   what makes that edit fail loudly instead of silently. Same for
   ESP_TLS_INSECURE, which gates the skip-verification switch. */
#ifdef CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
#error "CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP must stay unset: it lets the OTA client fetch firmware without TLS."
#endif
#ifdef CONFIG_ESP_TLS_INSECURE
#error "CONFIG_ESP_TLS_INSECURE must stay unset: the OTA CA pin is the only thing authenticating a firmware image."
#endif

static const char *TAG = "ota";

/* The pinned root, embedded by main/CMakeLists.txt's EMBED_TXTFILES.
   TXTFILES appends the NUL that cert_pem requires; the symbol is derived
   from the filename, so renaming certs/ota_ca.pem breaks the link, which
   is the intended behaviour. */
extern const uint8_t ota_ca_pem_start[] asm("_binary_ota_ca_pem_start");

/* ---- what the HTTP layer told us -------------------------------------- */

/* ota_http_ctx_t — filled by the event handler, drained into
   ota_error_facts_t at the point of failure — now lives in
   include/ota_facts.h, along with the four decisions that used to be
   inlined here (which esp-tls codes are TLS, which esp_err_t values mean
   "not a firmware image", the fallback-fact rule, and manifest
   completeness). All four are pure, all four needed no radio, and all
   four were unasserted while they sat in this file. See ota_facts.h.

   What is left below is the part only hardware can check. */

/* The "and clear" in esp_tls_get_and_clear_last_error is why this is read
   here and not later: the record is zeroed by the read, and the handle
   belongs to a transport that esp_http_client_cleanup is about to free.
   HTTP_EVENT_ERROR is dispatched from esp_http_client_open with exactly
   that handle as evt->data, which is the one moment it is both populated
   and still alive. */
static void note_tls_error(ota_http_ctx_t *ctx, esp_tls_error_handle_t handle) {
    int code = 0;
    int flags = 0;
    if (handle == NULL) {
        /* No record to read: whatever failed, failed below TLS. */
        ctx->transport_failed = true;
        return;
    }
    /* Exactly one read. A second would return zeros — the first cleared
       the record — and would quietly downgrade a tls_cert to a net. */
    esp_err_t last = esp_tls_get_and_clear_last_error(handle, &code, &flags);

    if (flags != 0) {
        /* The chain was REJECTED. This is the fact that earns tls_cert,
           and it is worth more than everything else in this struct: it
           says the pinned root does not accept what the host served, and
           no amount of waiting for the network to settle will fix it. */
        ctx->tls_cert_flags = flags;
        ESP_LOGE(TAG, "TLS cert verify flags 0x%08x: the pinned root does not accept this host", (unsigned)flags);
    }
    if (ota_facts_tls_layer_failure(last)) {
        ctx->tls_failed = true;
        ESP_LOGW(TAG, "TLS failure 0x%x (detail 0x%x)", (unsigned)last, (unsigned)code);
    } else {
        ctx->transport_failed = true;
        if (last != ESP_OK)
            ESP_LOGW(TAG, "transport failure 0x%x (detail 0x%x)", (unsigned)last, (unsigned)code);
    }
}

/* A Location header. Only a 3xx makes one a redirect — a stray Location
   on a 200 is not a hop and must not spend the budget. */
static void note_location(ota_http_ctx_t *ctx, const char *location) {
    if (ctx->status < 300 || ctx->status > 399)
        return;

    ota_redirect_t verdict = ota_url_redirect_check(location, ctx->redirects, OTA_MAX_REDIRECTS);
    if (verdict != OTA_REDIRECT_FOLLOW) {
        /* Loud, and at error level, because this is the shape of an
           attack as well as the shape of a misconfiguration. The log
           names WHICH rule refused it (not_https / too_many / too_long /
           no_target); the reason code that reaches Home Assistant is
           bad_redirect, which says a refusal happened but not which
           kind. That is the split on purpose: the code has to fit an NVS
           field and a stat payload, the detail belongs in the log. */
        ESP_LOGE(TAG, "refusing %d redirect: %s", ctx->status, ota_url_redirect_str(verdict));
        ctx->blocked = true;
        ctx->have_location = false;
        return;
    }
    ctx->redirects++;
    snprintf(ctx->location, sizeof(ctx->location), "%s", location);
    ctx->have_location = true;
}

/* Stop a transfer from INSIDE the callback. The only lever there is, and
   it exists because the obvious one does not work: returning non-ESP_OK
   from an event handler is discarded. Neither http_on_header_event
   (esp_http_client.c:246-277) nor the ON_CONNECTED dispatch
   (esp_http_client.c:1705) propagates a handler error to anything that
   would act on it.

   What this bounds, and it is the whole reason it is here:
   esp_https_ota's _http_connect is
   `do { open; fetch_headers; handle_response } while (process_again(status))`
   with NO hop counter of any kind (esp_https_ota.c:166-218) — the only
   exits are error returns. A host answering 302 with a Location pointing
   at itself would spin there until the awake failsafe fired. The refusal
   verdict cannot terminate that loop from where ota_download_begin reads
   it, because it is only readable after esp_https_ota_begin RETURNS.
   Closing the client here makes the in-flight fetch_headers fail, so
   _http_connect returns an error on this turn rather than looping.

   esp_http_client_close is idempotent (it early-returns unless
   state > HTTP_STATE_INIT, esp_http_client.c:1907-1916), so the close
   manifest_hop does on its way out is still safe.

   IT DOES NOT bound the other unbounded path, and saying so is the
   point: read_header (esp_https_ota.c:591-617) does
   `if (data_read == -ESP_ERR_HTTP_EAGAIN) continue;` inside a loop with
   no cap, so a server that sends headers and then stalls spins there at
   one socket timeout per turn with no redirect involved and nothing here
   to notice. Only ota_flow's up-front budget charge covers that one. */
static void kill_this_transfer(esp_http_client_handle_t client) {
    if (client == NULL)
        return;
    (void)esp_http_client_close(client);
}

static esp_err_t http_event(esp_http_client_event_t *evt) {
    ota_http_ctx_t *ctx = (ota_http_ctx_t *)evt->user_data;
    if (ctx == NULL)
        return ESP_OK;

    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            note_tls_error(ctx, (esp_tls_error_handle_t)evt->data);
            break;
        case HTTP_EVENT_ON_CONNECTED: {
            /* The backstop, and the last point at which a plaintext hop
               has cost nothing but a TCP handshake: no request line has
               been written yet. It catches anything the Location rule
               missed — a relative form resolved differently than we read
               it, or a future IDF that redirects somewhere new.

               A failed get_url is NOT treated as a refusal. It can only
               happen if the client has no scheme/host/path, which the
               URL we handed it rules out, and failing closed on an
               unreachable condition would trade a real capability for an
               imagined one. The Location rule is the primary defence. */
            char url[OTA_URL_MAX];
            url[0] = '\0';
            if (esp_http_client_get_url(evt->client, url, (int)sizeof(url)) != ESP_OK) {
                ESP_LOGW(TAG, "could not read back the connected URL; relying on the Location check");
            } else if (!config_is_https_url(url)) {
                ESP_LOGE(TAG, "connected to a non-https URL (%s): refusing to continue", url);
                ctx->blocked = true;
            }
            break;
        }
        case HTTP_EVENT_ON_STATUS_CODE:
            if (evt->data != NULL)
                ctx->status = *(const int *)evt->data;
            break;
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key != NULL && strcasecmp(evt->header_key, "Location") == 0) {
                note_location(ctx, evt->header_value);
                if (ctx->blocked)
                    kill_this_transfer(evt->client);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* Every failure path that got as far as opening a socket ends in
   ota_facts_fail (ota_facts.c), so that "the driver failed but named no
   fact" — which ota_flow logs as a bug in this file — is unreachable
   from any of them.

   The claim is worth QUALIFYING rather than stating flat, which is how
   it read before. Three argument-validation early returns below
   (ota_manifest_get, ota_download_step and ota_download_finish each
   reject a NULL facts pointer) return failure without calling it. They
   are unreachable through ota_flow as written — it passes the address of
   a stack local every time — so the guarantee holds in practice; it is
   just not a property of the control flow alone. */

static esp_http_client_config_t client_config(const char *url, ota_http_ctx_t *ctx) {
    esp_http_client_config_t cfg = {
        .url = url,
        .cert_pem = (const char *)ota_ca_pem_start,
        /* Coupled to ota_flow's abort tail; ota_timing.h holds both and
           the _Static_assert that keeps them compatible. Do not raise
           this here. */
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        /* Set EXPLICITLY, and this is load-bearing rather than tidy.
           esp_http_client_init takes client->buffer_size_rx straight
           from this field (esp_http_client.c:574) and falls back to 512
           when it is 0, while esp_https_ota allocates
           MAX(buffer_size, DEFAULT_OTA_BUF_SIZE) = 1024
           (esp_https_ota.c:561). Leaving it unset therefore made one
           esp_http_client_read take TWO inner esp_transport_read
           iterations, each with its own full timeout — doubling the
           worst-case overshoot of ota_flow's deadline, silently, via a
           default neither file names. Matching the two makes it one.
           See ota_timing.h. */
        .buffer_size = OTA_DL_BUF_SIZE,
        .event_handler = http_event,
        .user_data = ctx,
        .method = HTTP_METHOD_GET,
        /* Intent, and honestly inert on both paths: this flag is only
           consulted by esp_http_client_perform, and neither path calls
           it — the manifest loop below drives open/fetch_headers/read
           itself, and esp_https_ota does its own redirect handling that
           bypasses the flag entirely. It is set so that a future switch
           to perform() does not silently acquire auto-redirect, and so
           that the config states the policy the code enforces. */
        .disable_auto_redirect = true,
    };
    return cfg;
}

/* ---- manifest GET ------------------------------------------------------ */

/* One hop. A fresh client per hop rather than esp_http_client's redirect
   machinery: redirects then cannot be followed by accident, because
   nothing in this path is capable of following one. The cost is a TLS
   handshake per hop, on a path that normally takes zero hops. */
static int manifest_hop(const char *url, char *buf, size_t len, ota_http_ctx_t *ctx, bool *follow) {
    esp_http_client_handle_t client;
    esp_http_client_config_t cfg;
    esp_err_t err;
    int64_t content_len;
    int n_read;
    int out = -1;

    ctx->status = 0;
    ctx->have_location = false;
    *follow = false;

    cfg = client_config(url, ctx);
    client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "http client init failed");
        ctx->transport_failed = true;
        return -1;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        /* note_tls_error has already run from HTTP_EVENT_ERROR, so any
           cert flags are captured; this only records that the socket
           never produced a response. */
        ESP_LOGW(TAG, "manifest open failed: %s", esp_err_to_name(err));
        ctx->transport_failed = true;
        goto done;
    }

    content_len = esp_http_client_fetch_headers(client);
    /* Checked BEFORE the header result, not after, and the order is
       load-bearing now: note_location closes the client from inside the
       handler (see kill_this_transfer), so a refused redirect makes this
       very fetch_headers report a broken read. Testing content_len first
       would relabel every refusal as a bare transport failure and shadow
       the fact below. note_location has already logged which rule
       refused it; the fact it becomes is redirect_refused, which
       ota_policy reports as bad_redirect. */
    if (ctx->blocked)
        goto done;
    if (content_len < 0) {
        ESP_LOGW(TAG, "manifest headers failed: %d", (int)content_len);
        ctx->transport_failed = true;
        goto done;
    }
    if (ctx->status >= 300 && ctx->status <= 399) {
        if (ctx->have_location) {
            *follow = true;
        } else {
            ESP_LOGW(TAG, "HTTP %d with no usable Location", ctx->status);
            ctx->transport_failed = true;
        }
        goto done;
    }
    if (ctx->status < 200 || ctx->status > 299) {
        /* The status IS the fact: ota_policy folds anything >= 400 into
           http_404 / http_500, which is the most actionable thing this
           module can hand an operator. */
        ESP_LOGW(TAG, "manifest returned HTTP %d", ctx->status);
        goto done;
    }

    /* Never negative: esp_http_client_read_response returns a running
       count that starts at 0 and only grows (esp_http_client.c:2070-2081),
       so there is no error return to test for. It used to be tested
       anyway; the branch was dead and is gone. ota_facts_body answers
       for the values it CAN produce. */
    n_read = esp_http_client_read_response(client, buf, (int)len);

    /* read_response stops at the buffer end OR at a socket that died,
       and reports both as "this is what I got". Separating those is a
       decision over four numbers and no I/O, so it lives in ota_facts.c
       where test_ota_facts walks the whole matrix. */
    switch (ota_facts_body(n_read, content_len, len, esp_http_client_is_complete_data_received(client))) {
        case OTA_BODY_TRUNCATED:
            ESP_LOGW(TAG, "manifest truncated: %d of %d bytes", n_read, (int)content_len);
            ctx->transport_failed = true;
            goto done;
        case OTA_BODY_SHORT_CHUNKED:
            ESP_LOGW(TAG, "chunked manifest ended early after %d bytes", n_read);
            ctx->transport_failed = true;
            goto done;
        case OTA_BODY_OVERSIZE:
            /* Returned anyway, not failed: cJSON refuses the cut-off
               array and the operator reads bad_manifest, which names the
               real problem where `net` would misdirect them. */
            ESP_LOGW(TAG, "manifest is larger than the %u-byte buffer; it will not parse", (unsigned)len);
            break;
        case OTA_BODY_OK:
            break;
    }
    out = n_read;

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return out;
}

int ota_manifest_get(const char *url, char *buf, size_t len, ota_error_facts_t *facts) {
    if (url == NULL || buf == NULL || len == 0 || facts == NULL)
        return -1;

    ota_http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    char current[OTA_URL_MAX];
    if (snprintf(current, sizeof(current), "%s", url) >= (int)sizeof(current)) {
        ESP_LOGE(TAG, "manifest endpoint longer than %u bytes", (unsigned)sizeof(current));
        ota_facts_fail(&ctx, facts);
        return -1;
    }
    /* The endpoint passed config_is_ota_url on the way into NVS, but this
       is the last frame before a socket opens and the check costs a
       string walk. A hand-edited NVS partition, or a future writer that
       forgets the validator, stops here rather than at the peer. */
    if (!config_is_https_url(current)) {
        ESP_LOGE(TAG, "manifest endpoint is not https: refusing to fetch");
        ota_facts_fail(&ctx, facts);
        return -1;
    }

    /* The loop is bounded by OTA_MAX_REDIRECTS twice over: here, and
       inside ota_url_redirect_check, which is what actually refuses the
       hop. Two bounds because they answer to different owners — this one
       keeps the loop finite even if the check is ever loosened. */
    for (int hop = 0; hop <= OTA_MAX_REDIRECTS; hop++) {
        bool follow = false;
        int n = manifest_hop(current, buf, len, &ctx, &follow);
        if (n >= 0) {
            ESP_LOGI(TAG, "manifest: %d bytes, HTTP %d, %d redirect(s)", n, ctx.status, ctx.redirects);
            return n;
        }
        if (!follow || ctx.blocked)
            break;
        ESP_LOGI(TAG, "following HTTP %d redirect (hop %d)", ctx.status, ctx.redirects);
        snprintf(current, sizeof(current), "%s", ctx.location);
    }

    ota_facts_fail(&ctx, facts);
    return -1;
}

/* ---- the four download primitives -------------------------------------- */

/* One transfer in flight at a time, so the handle and its context are
   file-statics: ota_flow drives begin/step/finish across separate calls
   and holds no handle of its own. */
static esp_https_ota_handle_t s_dl;
static ota_http_ctx_t s_dl_ctx;

static void drop_handle(void) {
    s_dl = NULL;
    memset(&s_dl_ctx, 0, sizeof(s_dl_ctx));
}

bool ota_download_begin(const char *url, ota_error_facts_t *facts) {
    if (url == NULL || facts == NULL)
        return false;
    if (s_dl != NULL) {
        /* Unreachable through ota_flow, which pairs every begin with a
           finish or an abort. Refusing rather than leaking is the cheap
           half of "one transfer in flight at a time". */
        ESP_LOGE(TAG, "download already in flight");
        facts->transport_failed = true;
        return false;
    }
    memset(&s_dl_ctx, 0, sizeof(s_dl_ctx));

    /* The image URL came out of the manifest, where ota_policy already
       required https. Re-checked for the same reason as the endpoint:
       this is the frame before the socket. */
    if (!config_is_https_url(url)) {
        ESP_LOGE(TAG, "image URL is not https: refusing to download");
        ota_facts_fail(&s_dl_ctx, facts);
        return false;
    }

    esp_http_client_config_t http_cfg = client_config(url, &s_dl_ctx);
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
        /* Sequential erase, not bulk. A bulk erase of the whole 1.8 MB
           slot would run inside the first esp_https_ota_perform, which
           is one dl_step — and ota_flow can only check its deadline
           between steps. Erasing 4 K at a time as the writes advance
           keeps every step short, which is what makes the deadline
           mean anything. */
        .bulk_flash_erase = false,
    };

    esp_err_t err = esp_https_ota_begin(&ota_cfg, &s_dl);
    if (err != ESP_OK) {
        /* esp_https_ota_begin cleans up its own client and frees its own
           handle on every failure path, and NULLs the out-param. Nothing
           is in flight, which is exactly what ota_flow_ops_t promises a
           false answer here means. */
        ESP_LOGE(TAG, "https_ota begin failed: %s", esp_err_to_name(err));
        s_dl = NULL;
        if (ota_facts_image_error(err))
            facts->image_rejected = true;
        ota_facts_fail(&s_dl_ctx, facts);
        return false;
    }
    if (s_dl_ctx.blocked) {
        /* A redirect was refused inside begin(). Reached only when
           kill_this_transfer's close did NOT already make
           esp_https_ota_begin fail above — belt to that braces. The flag
           cannot be consulted any earlier than this, because
           esp_https_ota's redirect handling is internal to begin(). What
           it costs is one connection and the image header; what it
           prevents is the commit, which is the only thing that matters.
           Abort here rather than returning false with the handle live —
           a false begin means nothing is in flight. */
        ESP_LOGE(TAG, "aborting: the transfer followed a redirect this device refuses");
        (void)esp_https_ota_abort(s_dl);
        ota_facts_fail(&s_dl_ctx, facts);
        drop_handle();
        return false;
    }

    /* Header validation, and the reason it is here rather than left to
       the first step: get_img_desc reads the image header and checks the
       app-descriptor magic, so a URL serving an HTML error page or a
       truncated file is refused before esp_ota_begin erases the slot.
       The CHIP ID is not checkable here — esp_https_ota verifies it
       inside the first perform() and there is no API to pull that
       forward — so ota_download_step maps ESP_ERR_INVALID_VERSION onto
       the same image_rejected fact. Either way the operator reads
       bad_image.

       The header it reads is IMAGE_HEADER_SIZE, which is 1024 bytes —
       but the descriptor itself sits in the first ~288 (an
       esp_image_header_t plus an esp_image_segment_header_t plus an
       esp_app_desc_t). The 1 KB figure is what the transfer costs, not
       what the check needs. */
    esp_app_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    err = esp_https_ota_get_img_desc(s_dl, &desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image header rejected: %s", esp_err_to_name(err));
        /* ONLY when the transport was clean. get_description_from_image
           returns a bare ESP_FAIL for two unrelated causes
           (esp_https_ota.c:663-666 and :672-675): a wrong app-descriptor
           magic, which is a genuinely bad image, and read_header failing
           because the socket closed before 1024 bytes arrived
           (esp_https_ota.c:618-621). Since ota_policy ranks
           image_rejected above transport_failed, treating both as
           bad_image tells an operator whose link drops in the first 1 KB
           of a 1.44 MB download to go rebuild and republish a binary
           that was never the problem.

           The discriminator is sound: esp_http_client_read dispatches
           HTTP_EVENT_ERROR on a hard read failure
           (esp_http_client.c:1443-1446), so a dropped socket has already
           set one of these three. A short-but-CLEAN body — an HTML error
           page served with a valid Content-Length — produces no error
           event and correctly stays bad_image. The rule itself is
           ota_facts_transport_was_clean, where test_ota_facts can reach
           it. */
        if (ota_facts_transport_was_clean(&s_dl_ctx))
            facts->image_rejected = true;
        ota_facts_fail(&s_dl_ctx, facts);
        (void)esp_https_ota_abort(s_dl);
        drop_handle();
        return false;
    }

    ESP_LOGI(TAG, "downloading %.32s (%d bytes, %d redirect(s))", desc.version, esp_https_ota_get_image_size(s_dl),
             s_dl_ctx.redirects);
    return true;
}

ota_step_t ota_download_step(ota_error_facts_t *facts) {
    if (facts == NULL)
        return OTA_STEP_FAIL;
    if (s_dl == NULL) {
        ESP_LOGE(TAG, "step with no transfer in flight");
        facts->transport_failed = true;
        return OTA_STEP_FAIL;
    }
    if (s_dl_ctx.blocked) {
        /* Belt and braces: with partial_http_download off, esp_https_ota
           never reconnects mid-transfer, so nothing should set this after
           begin. If a future IDF changes that, the transfer stops here
           rather than at the commit. */
        ESP_LOGE(TAG, "refused redirect mid-transfer");
        ota_facts_fail(&s_dl_ctx, facts);
        return OTA_STEP_FAIL;
    }

    esp_err_t err = esp_https_ota_perform(s_dl);
    if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        return OTA_STEP_MORE;
    if (err == ESP_OK)
        return OTA_STEP_DONE;

    ESP_LOGE(TAG, "download step failed: %s", esp_err_to_name(err));
    if (ota_facts_image_error(err))
        facts->image_rejected = true;
    ota_facts_fail(&s_dl_ctx, facts);
    return OTA_STEP_FAIL;
}

bool ota_download_finish(ota_error_facts_t *facts) {
    if (facts == NULL)
        return false;
    if (s_dl == NULL) {
        ESP_LOGE(TAG, "finish with no transfer in flight");
        facts->transport_failed = true;
        return false;
    }

    /* The same fact read from the other side. esp_https_ota_perform only
       answers ESP_OK once the client reports a complete body, so this
       cannot currently be false; it is the IDF-documented guard before a
       commit, and the cost of keeping it is one comparison against the
       cost of committing a short image. Aborting rather than finishing
       still honours "frees its handle either way". */
    if (!esp_https_ota_is_complete_data_received(s_dl)) {
        ESP_LOGE(TAG, "refusing to commit: the image is incomplete");
        (void)esp_https_ota_abort(s_dl);
        ota_facts_fail(&s_dl_ctx, facts);
        drop_handle();
        return false;
    }

    esp_err_t err = esp_https_ota_finish(s_dl);
    /* esp_https_ota_finish frees the handle on every path it can reach
       from here, so the local copy must go whatever it answered. */
    ota_http_ctx_t ctx = s_dl_ctx;
    drop_handle();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(err));
        if (ota_facts_image_error(err))
            facts->image_rejected = true;
        ota_facts_fail(&ctx, facts);
        return false;
    }
    ESP_LOGI(TAG, "image committed; the boot partition now points at it");
    return true;
}

void ota_download_abort(void) {
    if (s_dl == NULL)
        return; /* a failed begin already cleaned up; see ota_flow_ops_t */
    esp_err_t err = esp_https_ota_abort(s_dl);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "abort reported %s; the boot partition is untouched either way", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "partial image discarded");
    drop_handle();
}

/* ---- rollback ---------------------------------------------------------- */

void ota_mark_valid_if_pending(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL)
        return;

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) {
        /* ESP_ERR_NOT_SUPPORTED on a factory build, which is every build
           until an OTA has actually run. Not worth a warning. */
        ESP_LOGD(TAG, "no image state for the running partition: %s", esp_err_to_name(err));
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY)
        return;

    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK)
        ESP_LOGI(TAG, "first wake on the new image completed; rollback cancelled");
    else
        ESP_LOGE(TAG, "could not cancel rollback (%s); the next boot reverts", esp_err_to_name(err));
}
