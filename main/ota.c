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

/* Per-socket network timeout. Two jobs, and the second is the one that
   picked the number: it is the longest a single esp_https_ota_perform can
   block, so it is the granularity of ota_flow's deadline check and the
   most that deadline can overshoot. Ten seconds is generous for a TLS
   handshake over a marginal link and small against
   CONFIG_MAGTAG_OTA_MAX_SEC (300 by default). */
#define OTA_HTTP_TIMEOUT_MS 10000

/* ---- what the HTTP layer told us -------------------------------------- */

/* Filled by the event handler, drained into ota_error_facts_t at the
   point of failure. It exists because the interesting facts are only
   observable from inside the callback: esp_https_ota owns its
   esp_http_client and frees it before returning, so by the time a failed
   esp_https_ota_begin hands control back there is no handle left to ask.

   One instance per transfer, addressed through esp_http_client_config_t's
   user_data rather than a file-static, so the manifest GET (a stack local)
   and the image download (the module-static below, because the transfer
   outlives the call) share one handler with no possibility of crosstalk. */
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

/* Which esp-tls failures are TLS failures and which are just a socket
   that never came up. Getting this wrong is not cosmetic: everything in
   the second column reports as `tls`, and an operator who reads `tls`
   goes looking at certificates. DNS and connect failures belong in the
   first. The boundary cases are deliberate — CONNECTION_TIMEOUT covers
   the whole low-level connect and reads as "the network flaked", while
   SERVER_HANDSHAKE_TIMEOUT is unambiguously the TLS exchange. */
static bool tls_layer_failure(esp_err_t err) {
    switch (err) {
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
    /* Everything else esp-tls defines is the TLS layer itself: handshake,
       certificate parsing, session setup. Anything outside the range is
       not esp-tls's to claim. */
    return err >= ESP_ERR_ESP_TLS_BASE && err <= ESP_ERR_MBEDTLS_SSL_READ_FAILED;
}

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
    if (tls_layer_failure(last)) {
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
           attack as well as the shape of a misconfiguration, and the
           reason code it eventually reports (net) cannot say which. */
        ESP_LOGE(TAG, "refusing %d redirect: %s", ctx->status, ota_url_redirect_str(verdict));
        ctx->blocked = true;
        ctx->have_location = false;
        return;
    }
    ctx->redirects++;
    snprintf(ctx->location, sizeof(ctx->location), "%s", location);
    ctx->have_location = true;
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
            if (evt->header_key != NULL && strcasecmp(evt->header_key, "Location") == 0)
                note_location(ctx, evt->header_value);
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* Drain the context into the struct ota_policy will classify. Never sets
   deadline_hit: that fact is ota_flow's, and this module cannot see it. */
static void apply_ctx_facts(const ota_http_ctx_t *ctx, ota_error_facts_t *facts) {
    facts->http_status = ctx->status;
    if (ctx->tls_cert_flags != 0)
        facts->tls_cert_flags = ctx->tls_cert_flags;
    if (ctx->tls_failed)
        facts->tls_failed = true;
    if (ctx->transport_failed)
        facts->transport_failed = true;
}

/* Every failure path ends here, so that "the driver failed but named no
   fact" — which ota_flow logs as a bug in this file — is unreachable by
   construction rather than by review. */
static void fail_facts(const ota_http_ctx_t *ctx, ota_error_facts_t *facts) {
    apply_ctx_facts(ctx, facts);
    if (!facts->tls_failed && facts->tls_cert_flags == 0 && !facts->image_rejected && facts->http_status < 400)
        facts->transport_failed = true;
}

static esp_http_client_config_t client_config(const char *url, ota_http_ctx_t *ctx) {
    esp_http_client_config_t cfg = {
        .url = url,
        .cert_pem = (const char *)ota_ca_pem_start,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
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
    if (content_len < 0) {
        ESP_LOGW(TAG, "manifest headers failed: %d", (int)content_len);
        ctx->transport_failed = true;
        goto done;
    }
    /* A refused redirect. note_location has already logged the specific
       reason; the fact it becomes is fail_facts's transport_failed,
       because ota_error_facts_t has no way to say "the transport refused
       to follow this" — see the plan's task 11 entry. */
    if (ctx->blocked)
        goto done;
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

    n_read = esp_http_client_read_response(client, buf, (int)len);
    if (n_read < 0) {
        ctx->transport_failed = true;
        goto done;
    }
    /* read_response stops at the buffer end OR at a socket that died,
       and reports both as "this is what I got". Content-Length is the
       only thing that separates them, so a body that promised a length
       and did not deliver it is a transport failure rather than a short
       manifest. A chunked response reports 0 here and is checked against
       the parser's own completeness flag instead. */
    if (content_len > 0 && (int64_t)n_read < content_len && (size_t)n_read < len) {
        ESP_LOGW(TAG, "manifest truncated: %d of %d bytes", n_read, (int)content_len);
        ctx->transport_failed = true;
        goto done;
    }
    if (content_len == 0 && (size_t)n_read < len && !esp_http_client_is_complete_data_received(client)) {
        ESP_LOGW(TAG, "chunked manifest ended early after %d bytes", n_read);
        ctx->transport_failed = true;
        goto done;
    }
    if ((size_t)n_read == len) {
        /* Returned anyway, not failed: cJSON will refuse it and the
           operator reads bad_manifest, which names the real problem. */
        ESP_LOGW(TAG, "manifest filled the %u-byte buffer; it will not parse", (unsigned)len);
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
        fail_facts(&ctx, facts);
        return -1;
    }
    /* The endpoint passed config_is_ota_url on the way into NVS, but this
       is the last frame before a socket opens and the check costs a
       string walk. A hand-edited NVS partition, or a future writer that
       forgets the validator, stops here rather than at the peer. */
    if (!config_is_https_url(current)) {
        ESP_LOGE(TAG, "manifest endpoint is not https: refusing to fetch");
        fail_facts(&ctx, facts);
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

    fail_facts(&ctx, facts);
    return -1;
}

/* ---- the four download primitives -------------------------------------- */

/* One transfer in flight at a time, so the handle and its context are
   file-statics: ota_flow drives begin/step/finish across separate calls
   and holds no handle of its own. */
static esp_https_ota_handle_t s_dl;
static ota_http_ctx_t s_dl_ctx;

/* Errors that mean "the bytes are not a firmware image for this device",
   as opposed to "the bytes did not arrive". esp_https_ota reports a chip
   id or chip revision mismatch as ESP_ERR_INVALID_VERSION from the FIRST
   perform, and a failed image validation as ESP_ERR_OTA_VALIDATE_FAILED
   from finish. Both must land on bad_image, not net, or an operator who
   published an ESP32-S3 build goes looking at their wifi. */
static bool image_error(esp_err_t err) {
    return err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_OTA_VALIDATE_FAILED || err == ESP_ERR_IMAGE_INVALID;
}

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
        fail_facts(&s_dl_ctx, facts);
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
        if (image_error(err))
            facts->image_rejected = true;
        fail_facts(&s_dl_ctx, facts);
        return false;
    }
    if (s_dl_ctx.blocked) {
        /* A redirect was refused after esp_https_ota had already followed
           it. This is the one part of the defence that is after the fact,
           and it is after the fact because esp_https_ota's redirect is
           internal to begin(): the flag cannot be consulted until begin
           returns. What it costs is one connection and the 1 KB image
           header; what it prevents is the commit, which is the only thing
           that matters. Abort here rather than returning false with the
           handle live — a false begin means nothing is in flight. */
        ESP_LOGE(TAG, "aborting: the transfer followed a redirect this device refuses");
        (void)esp_https_ota_abort(s_dl);
        fail_facts(&s_dl_ctx, facts);
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
       bad_image. */
    esp_app_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    err = esp_https_ota_get_img_desc(s_dl, &desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image header rejected: %s", esp_err_to_name(err));
        facts->image_rejected = true;
        fail_facts(&s_dl_ctx, facts);
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
        fail_facts(&s_dl_ctx, facts);
        return OTA_STEP_FAIL;
    }

    esp_err_t err = esp_https_ota_perform(s_dl);
    if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        return OTA_STEP_MORE;
    if (err == ESP_OK)
        return OTA_STEP_DONE;

    ESP_LOGE(TAG, "download step failed: %s", esp_err_to_name(err));
    if (image_error(err))
        facts->image_rejected = true;
    fail_facts(&s_dl_ctx, facts);
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
        fail_facts(&s_dl_ctx, facts);
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
        if (image_error(err))
            facts->image_rejected = true;
        fail_facts(&ctx, facts);
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
