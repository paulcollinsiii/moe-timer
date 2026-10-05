/* Device glue for the setup session (setup_session.h). Not host-tested —
   kept thin on purpose: every function here is either one of
   setup_session_ops_t's real implementations, or private plumbing that
   feeds the queue those implementations read from. The decisions (what
   ends the session, what gets stored, what the caller sleeps into) all
   live in setup_session.c; nothing here branches on the session's own
   outcome.

   setup_mode_ops() at the bottom assembles the whole setup_session_ops_t
   from the eight exported functions below and the setup screens' two render
   functions. The one member it cannot supply is extend_awake, whose real
   implementation owns the awake failsafe's esp_timer handle in main.c; the
   caller passes it in, so the table is built here and not in the
   composition root. */
#include "setup_session_idf.h" /* prototypes for the eight exported functions below, checked against their definitions */

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_srp.h" /* components/protocomm/include/crypto/srp6a — public include dir */
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h" /* xTaskGetHandle, uxTaskGetStackHighWaterMark — bench measurement of the httpd stack */
#include "mbedtls/platform_util.h" /* mbedtls_platform_zeroize — non-elidable wipe, already linked via esp-tls */
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"
#include "nvs_config.h"
#include "setup_screens.h" /* the two render functions setup_mode_ops() installs */
#include "setup_session.h"
#include "wifi_session.h" /* wifi_session_sta_netif() — the one shared STA netif */

static const char *TAG = "setup_session_idf";

/* SRP6a salt length: 16 bytes is RFC 5054's own example width and what the
   component's own dev-mode fixture uses (examples/wifi_prov's sec2_salt is
   16 bytes) — not otherwise meaningful, the verifier is what carries the
   security. */
#define SRP_SALT_LEN 16

/* httpd_register_uri_handler's registrations (5 of the manager's own
   fixed endpoints + "mqtt-config" + our /mqtt GET/POST, each one its own
   URI handler at the esp_http_server level — protocomm's httpd transport
   registers a handler per endpoint, same as this file registers one per
   method on /mqtt) comes to exactly 8 today. A few slots of headroom so
   the next endpoint anyone adds does not fail silently against a config
   nobody remembered to grow. */
#define SETUP_SESSION_HTTPD_MAX_URI_HANDLERS 10

/* HTTPD_DEFAULT_CONFIG's own 4096 B is tight against this feature's
   worst case. With the /mqtt page's locals
   moved to the heap (send_mqtt_page below) the two paths left on this
   stack are:
     - our own handler frames: mqtt_form_result_t + a queue_msg_t (now
       just two short wifi strings) + a short status buffer, on the
       order of 500 B;
     - esp_http_server's own dispatch frames plus, on the protocomm
       endpoint path, one mbedtls GCM decrypt and a bounded-depth cJSON
       parse (mqtt_form_parse_json pre-scans nesting before cJSON ever
       runs), estimated 2-2.5 KB combined.
   ~3 KB of estimated worst case; 6144 leaves roughly double that as
   margin without an actual stack trace to measure against
   (uxTaskGetStackHighWaterMark is logged in stop() below so a bench
   measurement can replace this estimate with a real number). */
#define SETUP_SESSION_HTTPD_STACK_SIZE 6144

/* Consecutive HTTPD_SOCK_ERR_TIMEOUT returns the /mqtt POST body read
   tolerates before giving up: unbounded retry pins the single httpd
   task on a client that sent a Content-Length and then went quiet,
   and httpd_stop() in teardown then blocks joining that same task.
   Any successful partial read resets the count,
   so a slow-but-progressing upload is never penalised for it. */
#define SETUP_SESSION_POST_RECV_MAX_TIMEOUTS 3

/* ---- state the ops below share with the plumbing underneath them ------- */

static bool s_wifi_inited;
static bool s_mgr_inited;
static bool s_clear_driver_store_requested; /* D3, deferred — see setup_session_idf_clear_wifi_driver_store() */
/* True from the top of setup_session_idf_stop() until it returns: an
   NETWORK_PROV_END/_DEINIT our OWN stop provokes is not a failure to
   report — see prov_event_handler's NETWORK_PROV_END case. */
static bool s_our_own_stop;
static httpd_handle_t s_httpd;
static esp_event_handler_instance_t s_inst_prov;
static QueueHandle_t s_evt_queue;

static char *s_srp_salt;
static char *s_srp_verifier;
static int s_srp_verifier_len;
static network_prov_security2_params_t s_sec2_params;

/* Captured at NETWORK_PROV_WIFI_CRED_RECV, which carries the credentials;
   NETWORK_PROV_WIFI_CRED_SUCCESS carries none (manager.h's own doc comment
   for that event has no event-data sentence), so this is the only place
   they can be read. Sized from NVS_CONFIG_WIFI_SSID_BUF/_PASS_BUF — the
   same widths wifi_sta_config_t's own ssid[32]/password[64] need plus a
   NUL: the old 64-byte s_cred_pass silently dropped the last byte of a
   64-character hex PSK. */
static char s_cred_ssid[NVS_CONFIG_WIFI_SSID_BUF];
static char s_cred_pass[NVS_CONFIG_WIFI_PASS_BUF];

typedef struct {
    setup_session_event_t event;
    setup_session_poll_out_t out;
} queue_msg_t;

/* ---- the httpd side of the /mqtt form ----------------------------------- */

static bool httpd_chunk_sink(const char *chunk, size_t len, void *ctx) {
    httpd_req_t *req = (httpd_req_t *)ctx;
    return httpd_resp_send_chunk(req, chunk, (ssize_t)len) == ESP_OK;
}

/* The page's own prefill + escape buffers, heap-allocated rather than
   stack locals: at ~1.3 KB combined (uri/user from NVS plus their HTML-
   escaped widenings) they were the single largest consumer of this
   handler's share of the httpd task stack. A transient allocation,
   freed before this function returns — the same
   call/free shape mqtt_post_handler's body buffer already uses below,
   and for the same reason: setup mode is not a path this device needs
   to keep off the heap allocator for timing reasons. */
typedef struct {
    char uri[MQTT_FORM_URI_MAX];
    char user[MQTT_FORM_USER_MAX];
    char uri_esc[MQTT_FORM_HTML_ESCAPED_MAX(MQTT_FORM_URI_MAX)];
    char user_esc[MQTT_FORM_HTML_ESCAPED_MAX(MQTT_FORM_USER_MAX)];
} mqtt_page_scratch_t;

static void send_mqtt_page(httpd_req_t *req, const char *status_msg) {
    mqtt_page_scratch_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        ESP_LOGW(TAG, "/mqtt: out of memory building the page");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return;
    }
    memset(s, 0, sizeof(*s));
    (void)nvs_config_get_mqtt_uri(s->uri, sizeof(s->uri));
    (void)nvs_config_get_mqtt_user(s->user, sizeof(s->user));
    (void)mqtt_form_html_escape(s->uri, s->uri_esc, sizeof(s->uri_esc));
    (void)mqtt_form_html_escape(s->user, s->user_esc, sizeof(s->user_esc));

    setup_session_mqtt_page_in_t in = {
        .uri_escaped = s->uri_esc, .user_escaped = s->user_esc, .status_msg = status_msg};
    httpd_resp_set_type(req, "text/html");
    (void)setup_session_render_mqtt_page(&in, httpd_chunk_sink, req);
    httpd_resp_send_chunk(req, NULL, 0); /* ends the chunked response */
    free(s);
}

static esp_err_t mqtt_get_handler(httpd_req_t *req) {
    send_mqtt_page(req, NULL);
    return ESP_OK;
}

/* Parses body/body_len with `parse`, and on success stores the result
   synchronously (setup_session_apply_mqtt, setup_session.h) and — only
   once it is actually saved — posts SETUP_SESSION_EVENT_MQTT_STORED so
   the session can decide whether that ends it. Shared by both MQTT
   entry points (the /mqtt form and the "mqtt-config" protocomm
   endpoint) so the parse-then-store-then-notify sequence exists in
   exactly one place rather than twice. Writes a
   short human status into `status` (capacity status_cap) that is
   "Saved." only when the store actually happened — never when it was
   merely queued, which is what let a dropped post still claim success
   before this fix. */
static void apply_and_notify(mqtt_form_status_t parse_st, const mqtt_form_result_t *result, char *status,
                             size_t status_cap) {
    bool parsed = (parse_st.err == MQTT_FORM_ERR_NONE);
    bool stored = false;

    if (parsed) {
        /* A one-field ops table built here, not setup_session_run's own:
           this handler has no access to that table (it belongs to
           main.c), and the real device call
           (setup_session_idf_set_mqtt_creds) is this file's own, so
           there is nothing to inject it from. setup_session_apply_mqtt
           only ever reads this one field. */
        setup_session_ops_t apply_ops;
        memset(&apply_ops, 0, sizeof(apply_ops));
        apply_ops.set_mqtt_creds = setup_session_idf_set_mqtt_creds;
        stored = setup_session_apply_mqtt(&apply_ops, result);

        if (stored) {
            queue_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.event = SETUP_SESSION_EVENT_MQTT_STORED;
            /* NVS writes from this (httpd) task are safe — nvs_flash is
               thread-safe across tasks, unlike the esp_wifi driver calls
               this file also makes, which all run on the task that calls
               setup_session_run(). */
            if (s_evt_queue == NULL || xQueueSend(s_evt_queue, &msg, 0) != pdTRUE) {
                /* The data is already in NVS — only the "end the session
                   now" notification is lost, and the session's own
                   timeout still covers that. */
                ESP_LOGW(TAG, "mqtt credentials saved, but the session queue would not take the notification");
            }
        }
    }

    if (!parsed) {
        setup_session_format_mqtt_status(false, parse_st, status, status_cap);
    } else if (!stored) {
        snprintf(status, status_cap, "could not save: flash write failed");
    } else {
        setup_session_format_mqtt_status(true, parse_st, status, status_cap);
    }
}

/* Reads at most MQTT_FORM_BODY_MAX bytes off the socket — on the HEAP,
   neither the stack nor a `static`. Not the stack: this runs on the
   httpd task, whose stack this file sizes explicitly
   (SETUP_SESSION_HTTPD_STACK_SIZE) rather than leaving at protocomm's
   4 KB default, and 1 KB is still a meaningful fraction of either. Not
   `static` either — that trades the stack risk for a permanent DIRAM
   cost, on a device with very little static RAM headroom to spare, for
   a buffer only ever live for the length of one POST. A transient
   malloc/free costs nothing this feature doesn't already spend on
   SRP6a's own allocations (esp_srp_gen_salt_verifier), and setup mode
   is not a path anything here needs to keep off the heap allocator for
   timing reasons. A body longer than the cap is reported as too-long
   without ever being read into memory. */
static esp_err_t mqtt_post_handler(httpd_req_t *req) {
    mqtt_form_status_t st;
    mqtt_form_result_t result;
    memset(&result, 0, sizeof(result));

    if (req->content_len > MQTT_FORM_BODY_MAX) {
        st = (mqtt_form_status_t){MQTT_FORM_ERR_BODY_TOO_LONG, MQTT_FORM_FIELD_NONE};
    } else {
        char *body = malloc(MQTT_FORM_BODY_MAX);
        if (body == NULL) {
            ESP_LOGW(TAG, "/mqtt POST: out of memory reading the body");
            return ESP_ERR_NO_MEM;
        }
        size_t received = 0;
        int timeouts = 0;
        esp_err_t recv_failed = ESP_OK;
        while (received < req->content_len) {
            int r = httpd_req_recv(req, body + received, req->content_len - received);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > SETUP_SESSION_POST_RECV_MAX_TIMEOUTS) {
                    ESP_LOGW(TAG, "/mqtt POST: recv timed out %d times in a row", timeouts);
                    recv_failed = ESP_ERR_TIMEOUT;
                    break;
                }
                continue;
            }
            if (r <= 0) {
                ESP_LOGW(TAG, "/mqtt POST: recv failed (%d)", r);
                recv_failed = ESP_FAIL;
                break;
            }
            timeouts = 0;
            received += (size_t)r;
        }
        if (recv_failed == ESP_ERR_TIMEOUT) {
            mbedtls_platform_zeroize(body, MQTT_FORM_BODY_MAX);
            free(body);
            return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, NULL);
        }
        if (recv_failed != ESP_OK) {
            mbedtls_platform_zeroize(body, MQTT_FORM_BODY_MAX);
            free(body);
            return ESP_FAIL;
        }
        st = mqtt_form_parse_urlencoded(body, received, &result);
        mbedtls_platform_zeroize(body, MQTT_FORM_BODY_MAX); /* held the submitted password in cleartext */
        free(body);
    }

    char status[80];
    apply_and_notify(st, &result, status, sizeof(status));
    mbedtls_platform_zeroize(&result, sizeof(result)); /* result.pass */
    send_mqtt_page(req, status);
    return ESP_OK;
}

/* The protocomm "mqtt-config" endpoint — the esp_prov.py --custom_data
   path (plan, D2's secondary route). Same parse-and-store, same JSON
   instead of a browser form and a short plain-text reply instead of a
   page. */
static esp_err_t mqtt_endpoint_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf,
                                       ssize_t *outlen, void *priv_data) {
    (void)session_id;
    (void)priv_data;
    mqtt_form_result_t result;
    mqtt_form_status_t st = mqtt_form_parse_json((const char *)inbuf, (size_t)inlen, &result);

    char status[80];
    apply_and_notify(st, &result, status, sizeof(status));
    mbedtls_platform_zeroize(&result, sizeof(result)); /* result.pass */

    char *resp = strdup(status);
    if (resp == NULL)
        return ESP_ERR_NO_MEM;
    *outbuf = (uint8_t *)resp; /* protocomm frees this once sent */
    *outlen = (ssize_t)strlen(status) + 1;
    return ESP_OK;
}

/* ---- the manager's own events -------------------------------------------- */

static void prov_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base != NETWORK_PROV_EVENT)
        return;

    queue_msg_t msg;
    memset(&msg, 0, sizeof(msg));

    switch (id) {
        case NETWORK_PROV_WIFI_CRED_RECV: {
            /* No event data to post yet: CRED_SUCCESS carries none of its
               own, so this is captured here and read back below.
               wifi_sta_config_t's ssid[32]/password[64] are NOT
               NUL-terminated strings by contract — memcpy of the real
               length (bounded by strnlen over the source array, not by
               our own buffer) into a buffer this file already zeroed
               before any session's first CRED_RECV keeps both
               NUL-terminated without risking dropping a full-width byte
               the way a `%.*s` through snprintf's own NUL would. */
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)data;
            size_t ssid_len = strnlen((const char *)cfg->ssid, sizeof(cfg->ssid));
            size_t pass_len = strnlen((const char *)cfg->password, sizeof(cfg->password));
            memset(s_cred_ssid, 0, sizeof(s_cred_ssid));
            memset(s_cred_pass, 0, sizeof(s_cred_pass));
            memcpy(s_cred_ssid, cfg->ssid, ssid_len);
            memcpy(s_cred_pass, cfg->password, pass_len);
            return;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL:
            /* Without this, the manager's own prov_state sticks at FAIL
               and refuses every later credential with "already
               received" (manager.c:1387-1391), so a mistyped password
               could never be corrected for the rest of the session —
               confirmed in manager.c:2455-2492, which also resets the
               STA config it wrote on CRED_RECV back to {0}, so a wrong
               password this device joined with briefly does not stay in
               the driver's store either. Safe on
               this task (the event-loop task) — the IDF example
               (app_main.c) calls it from the same place. */
            if (network_prov_mgr_reset_wifi_sm_state_on_failure() != ESP_OK)
                ESP_LOGW(TAG, "reset_wifi_sm_state_on_failure failed: a retry after this failure may be refused");
            msg.event = SETUP_SESSION_EVENT_WIFI_FAIL;
            break;
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            msg.event = SETUP_SESSION_EVENT_WIFI_SUCCESS;
            snprintf(msg.out.wifi_ssid, sizeof(msg.out.wifi_ssid), "%s", s_cred_ssid);
            snprintf(msg.out.wifi_password, sizeof(msg.out.wifi_password), "%s", s_cred_pass);
            mbedtls_platform_zeroize(s_cred_pass, sizeof(s_cred_pass)); /* consumed */
            break;
        case NETWORK_PROV_END:
        case NETWORK_PROV_DEINIT:
            /* Both of these fire as part of OUR OWN setup_session_idf_stop()
               (network_prov_mgr_deinit() posts END then DEINIT once a
               running service is torn down) — expected, and by the time
               they are dispatched nothing is reading this queue any
               more. Anything else is the manager stopping or dying on
               its own, mid-session, which `poll` otherwise has no way
               to observe: this is what makes
               HARD_ERROR a real, reachable outcome rather than a
               documented one. */
            if (s_our_own_stop)
                return;
            msg.event = SETUP_SESSION_EVENT_HARD_ERROR;
            break;
        default:
            return;
    }
    if (s_evt_queue != NULL)
        (void)xQueueSend(s_evt_queue, &msg, 0);
}

/* ---- the ops table's eight real implementations -------------------------- */

uint8_t setup_session_idf_rand_byte(void) {
    return (uint8_t)esp_random();
}

uint32_t setup_session_idf_now_ms(void) {
    /* esp_timer_get_time() is microseconds since boot, monotonic; this
       narrows to milliseconds in a uint32_t, which wraps at ~49.7 days —
       setup_session.c's deadline arithmetic is written to tolerate
       exactly that wrap, and a session with a Kconfig-bounded
       budget of at most 1800 s is nowhere near it either way. */
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Idempotent-by-construction cleanup: every resource is torn down behind
   a null/flag guard, so calling this when `start` was never reached, or
   failed partway through, is a correctly-shaped no-op rather than a
   special case the caller has to know about (setup_session_ops_t's
   contract for `stop`). */
void setup_session_idf_stop(void) {
    s_our_own_stop = true; /* see prov_event_handler's NETWORK_PROV_END/_DEINIT case */

    if (s_mgr_inited) {
        /* network_prov_mgr_deinit() alone — not stop_provisioning() +
           wait() + deinit(). Read from manager.c (1.3.1): deinit() calls
           network_prov_mgr_stop_service(blocking=true) itself, which —
           when blocking — runs the SAME cleanup_delay wait inline
           (prov_stop_and_notify's `if (!is_async) vTaskDelay(...)`
           branch) and returns only once the service is fully stopped.
           stop_provisioning() instead calls stop_service(blocking=false),
           which merely ARMS a timer to do that same work later and
           returns immediately — so the wait() loop after it was doing
           the blocking that deinit() already does on its own, except in
           whole-second steps (its own vTaskDelay(1000ms) poll), costing
           up to a second more than deinit() alone needs. */
        network_prov_mgr_deinit();
        s_mgr_inited = false;
    }
    if (s_httpd != NULL) {
        /* Bench measurement needs a real number rather than the worst-
           case estimate above; esp_http_server names this task "httpd" unconditionally
           (httpd_main.c) and it is gone the instant httpd_stop returns,
           so this is the only point that can still see it. */
        TaskHandle_t httpd_task = xTaskGetHandle("httpd");
        if (httpd_task != NULL) {
            UBaseType_t httpd_stack_free = uxTaskGetStackHighWaterMark(httpd_task);
            ESP_LOGI(TAG, "httpd task stack high-water mark: %u words free", (unsigned)httpd_stack_free);
        }
        httpd_stop(s_httpd);
        s_httpd = NULL;
        /* The scheme keeps this pointer in its own static until told
           otherwise; clearing it here is what stops a dangling handle
           from surviving into the next session this boot. */
        network_prov_scheme_softap_set_httpd_handle(NULL);
    }
    if (s_inst_prov != NULL) {
        esp_event_handler_instance_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, s_inst_prov);
        s_inst_prov = NULL;
    }
    if (s_evt_queue != NULL) {
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
    }
    if (s_srp_salt != NULL) {
        free(s_srp_salt); /* esp_srp_gen_salt_verifier's own contract: plain free(), not esp_srp_free */
        s_srp_salt = NULL;
    }
    if (s_srp_verifier != NULL) {
        free(s_srp_verifier);
        s_srp_verifier = NULL;
    }
    /* D3's clear, deferred from setup_session_idf_clear_wifi_driver_store()
       above: the manager is fully stopped by this point (nothing left to
       disrupt) and esp_wifi is still inited (esp_wifi_restore() needs
       that), which is the one window where this is safe to do. */
    if (s_clear_driver_store_requested) {
        if (s_wifi_inited) {
            esp_err_t ret = network_prov_mgr_reset_wifi_provisioning();
            if (ret != ESP_OK)
                ESP_LOGW(TAG,
                         "reset_wifi_provisioning failed (%s): the driver's own copy may still hold stale credentials",
                         esp_err_to_name(ret));
        } else {
            ESP_LOGW(TAG, "wifi never inited: nothing to clear, but the request implied a successful join");
        }
        s_clear_driver_store_requested = false;
    }
    if (s_wifi_inited) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_inited = false;
    }

    /* Safety net, not the primary wipe: NETWORK_PROV_WIFI_CRED_SUCCESS
       already zeroizes s_cred_pass once it is consumed, but a session
       that ends on WIFI_FAIL/TIMEOUT/HARD_ERROR without ever reaching
       SUCCESS leaves whatever the last CRED_RECV wrote sitting here
       until the next one overwrites it — which, on an idle/never-
       reprovisioned device, could be a long time. */
    mbedtls_platform_zeroize(s_cred_pass, sizeof(s_cred_pass));
    memset(s_cred_ssid, 0, sizeof(s_cred_ssid)); /* not a secret; cleared only for symmetry */

    s_our_own_stop = false; /* a second session this boot starts clean */
}

bool setup_session_idf_start(const char *ap_ssid, const char *ap_password) {
    esp_err_t ret;

    /* netif + the default event loop are boot-global singletons; tolerate
       "already created" the same way wifi_session.c does, because a setup
       session and a normal network window never run in the same wake
       (plan, "The setup session" item 4) but DO share a boot's lifetime of
       these two calls on the rare path where setup runs on a wake that
       skips straight past the normal window's own init. */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(ret));
        return false;
    }

    /* scheme_softap.c sets WIFI_MODE_APSTA but creates neither netif
       itself — network_provisioning 1.3.1's scheme_softap.c and
       manager.c call esp_wifi_set_mode() only, never a netif
       constructor. The STA half is the SAME netif wifi_session.c's own
       network window uses (wifi_session_sta_netif(), wifi_session.h) —
       each used to keep a separate static, and
       esp_netif_create_default_wifi_sta() asserts on a duplicate
       "WIFI_STA_DEF" if_key, so any wake that ran a normal window and
       then entered setup aborted. The AP netif has
       no such collision (nothing else in this image creates one) and
       stays a function-local static here, which already makes a second
       setup session this boot safe: the NULL check only creates it
       once. */
    if (wifi_session_sta_netif() == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return false;
    }
    static esp_netif_t *s_ap_netif;
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
            return false;
        }
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        return false;
    }
    s_wifi_inited = true;

    s_evt_queue = xQueueCreate(4, sizeof(queue_msg_t));
    if (s_evt_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        setup_session_idf_stop();
        return false;
    }

    ret = esp_event_handler_instance_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_handler, NULL,
                                              &s_inst_prov);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event handler register: %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }

    /* Our own httpd instance, so /mqtt can be registered on it (plan:
       "Create the httpd instance yourself and pass it to
       network_prov_scheme_softap_set_httpd_handle"). max_uri_handlers
       and lru_purge_enable both raised from HTTPD_DEFAULT_CONFIG's own
       defaults (8, false): 8 is exactly how many this session registers
       today with zero headroom for the next one, and protocomm's own
       httpd transport sets lru_purge_enable itself (protocomm_httpd.c)
       while ours did not. */
    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.stack_size = SETUP_SESSION_HTTPD_STACK_SIZE;
    httpd_cfg.max_uri_handlers = SETUP_SESSION_HTTPD_MAX_URI_HANDLERS;
    httpd_cfg.lru_purge_enable = true;
    ret = httpd_start(&s_httpd, &httpd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }
    network_prov_scheme_softap_set_httpd_handle(s_httpd);

    httpd_uri_t get_uri = {.uri = "/mqtt", .method = HTTP_GET, .handler = mqtt_get_handler, .user_ctx = NULL};
    httpd_uri_t post_uri = {.uri = "/mqtt", .method = HTTP_POST, .handler = mqtt_post_handler, .user_ctx = NULL};
    ret = httpd_register_uri_handler(s_httpd, &get_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /mqtt): %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }
    ret = httpd_register_uri_handler(s_httpd, &post_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /mqtt): %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }

    network_prov_mgr_config_t mgr_cfg = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ret = network_prov_mgr_init(mgr_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "network_prov_mgr_init: %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }
    s_mgr_inited = true;

    /* disable_auto_stop(1000) turns the manager's OWN 30 s auto-stop
       (CONFIG_NETWORK_PROV_AUTOSTOP_TIMEOUT, armed on CRED_SUCCESS) OFF
       entirely — confirmed in manager.c: once no_auto_stop is set, the
       `if (!no_auto_stop)` guard around starting that timer never runs,
       so there is no 30 s timer left to race. `1000` is `cleanup_delay`,
       a SEPARATE, much smaller number: the delay OUR OWN stop() (via
       network_prov_mgr_deinit()) waits between notifying and actually
       tearing the scheme down, so a client's last response has a moment
       to flush first. It has nothing to do with when we choose to call
       stop() — that is setup_session.c's own SETUP_SESSION_SUCCESS_LINGER_MS
       after a verified join. */
    ret = network_prov_mgr_disable_auto_stop(1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "network_prov_mgr_disable_auto_stop: %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }
    ret = network_prov_mgr_endpoint_create("mqtt-config");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "network_prov_mgr_endpoint_create: %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }

    /* The 3072-bit modexp the plan asks to have timed — esp_timer_get_time()
       is microseconds, logged as milliseconds. */
    int64_t srp_start_us = esp_timer_get_time();
    char *salt = NULL;
    char *verifier = NULL;
    int verifier_len = 0;
    ret = esp_srp_gen_salt_verifier(SETUP_SESSION_QR_USERNAME, (int)strlen(SETUP_SESSION_QR_USERNAME), ap_password,
                                    (int)strlen(ap_password), &salt, SRP_SALT_LEN, &verifier, &verifier_len);
    int64_t srp_us = esp_timer_get_time() - srp_start_us;
    ESP_LOGI(TAG, "SRP6a salt/verifier generated in %lld ms", (long long)(srp_us / 1000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_srp_gen_salt_verifier: %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }
    s_srp_salt = salt;
    s_srp_verifier = verifier;
    s_srp_verifier_len = verifier_len;
    s_sec2_params = (network_prov_security2_params_t){
        .salt = s_srp_salt,
        .salt_len = SRP_SALT_LEN,
        .verifier = s_srp_verifier,
        .verifier_len = (uint16_t)s_srp_verifier_len,
    };

    ret = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_2, &s_sec2_params, ap_ssid, ap_password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "network_prov_mgr_start_provisioning: %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }

    /* scheme_softap.c's own start_wifi_ap hard-codes
       WIFI_AUTH_WPA_WPA2_PSK (accepts a WPA1/TKIP client too) with no
       public API to ask it for WPA2-only instead; re-asserting the
       narrower mode on the AP config the scheme just applied is the
       only way to close that without forking the scheme. Best-effort:
       failing to tighten this is not worth
       failing the whole session over, so only logged. */
    wifi_config_t ap_cfg;
    if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) == ESP_OK) {
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        esp_err_t ap_ret = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (ap_ret != ESP_OK)
            ESP_LOGW(TAG, "could not narrow the SoftAP to WPA2-only: %s", esp_err_to_name(ap_ret));
    }

    ret = network_prov_mgr_endpoint_register("mqtt-config", mqtt_endpoint_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "network_prov_mgr_endpoint_register: %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }
    return true;
}

setup_session_event_t setup_session_idf_poll(uint32_t timeout_ms, setup_session_poll_out_t *out) {
    queue_msg_t msg;
    if (s_evt_queue == NULL)
        return SETUP_SESSION_EVENT_HARD_ERROR; /* start() never got far enough to create it */
    if (xQueueReceive(s_evt_queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return SETUP_SESSION_EVENT_NONE;
    *out = msg.out;
    return msg.event;
}

bool setup_session_idf_set_wifi_creds(const char *ssid, const char *password) {
    esp_err_t r1 = nvs_config_set_wifi_ssid(ssid);
    esp_err_t r2 = nvs_config_set_wifi_pass(password);
    if (r1 != ESP_OK || r2 != ESP_OK) {
        ESP_LOGW(TAG, "wifi credential write failed (%d/%d)", r1, r2);
        return false;
    }
    return true;
}

/* D3: the driver's own flash-backed copy can't disagree with the app's
   NVS keys if it no longer exists. network_prov_mgr_reset_wifi_provisioning()
   is a thin wrapper over esp_wifi_restore() (network_provisioning 1.3.1's
   manager.c:2443-2448) and esp_wifi_restore() is documented to erase
   WiFi's persistent settings, which is exactly the store
   NETWORK_PROV_WIFI_CRED_RECV wrote into via esp_wifi_set_storage(
   WIFI_STORAGE_FLASH) + esp_wifi_set_config() (manager.c:1408-1415).

   NOT CALLED HERE, DELIBERATELY — only requested. esp_wifi_restore()'s own
   doc comment says it also resets esp_wifi_set_mode(), and at the moment
   the pure layer calls this op (right after a verified WIFI_SUCCESS, still
   inside the session's loop, now inside its post-success linger too) the
   manager and the SoftAP are both still live, deliberately, so the phone
   app gets time to poll its own "are you connected now?" query. Resetting
   the WiFi mode out from under that exchange would be the ordering bug
   the manager's own delayed-stop design exists to prevent. So this only
   sets a flag; setup_session_idf_stop() below acts on it after the
   manager has been fully torn down but before esp_wifi itself goes down
   — the one point both "the manager is done with the radio" and
   "esp_wifi is still inited" (esp_wifi_restore() needs that) hold at
   once. */
void setup_session_idf_clear_wifi_driver_store(void) {
    s_clear_driver_store_requested = true;
}

bool setup_session_idf_set_mqtt_creds(const char *uri, const char *user, const char *pass, bool keep_pass) {
    esp_err_t r1 = nvs_config_set_mqtt_uri(uri);
    esp_err_t r2 = nvs_config_set_mqtt_user(user);
    esp_err_t r3 = keep_pass ? ESP_OK : nvs_config_set_mqtt_pass(pass);
    if (r1 != ESP_OK || r2 != ESP_OK || r3 != ESP_OK) {
        ESP_LOGW(TAG, "mqtt credential write failed (%d/%d/%d)", r1, r2, r3);
        return false;
    }
    return true;
}

setup_session_ops_t setup_mode_ops(bool (*extend_awake)(int seconds)) {
    return (setup_session_ops_t){
        .extend_awake = extend_awake,
        .now_ms = setup_session_idf_now_ms,
        .rand_byte = setup_session_idf_rand_byte,
        .start = setup_session_idf_start,
        .stop = setup_session_idf_stop,
        .poll = setup_session_idf_poll,
        .render_setup_screen = setup_screens_render_setup_screen,
        .render_end_screen = setup_screens_render_end_screen,
        .set_wifi_creds = setup_session_idf_set_wifi_creds,
        .clear_wifi_driver_store = setup_session_idf_clear_wifi_driver_store,
        .set_mqtt_creds = setup_session_idf_set_mqtt_creds,
    };
}
