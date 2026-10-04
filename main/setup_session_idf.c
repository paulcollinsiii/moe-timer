/* Device glue for the setup session (setup_session.h). Not host-tested —
   kept thin on purpose: every function here is either one of
   setup_session_ops_t's real implementations, or private plumbing that
   feeds the queue those implementations read from. The decisions (what
   ends the session, what gets stored, what the caller sleeps into) all
   live in setup_session.c; nothing here branches on the session's own
   outcome.

   Composition is task 6's: main.c assembles a setup_session_ops_t from the
   seven exported functions below plus its own extend_awake_failsafe
   (already injected into ota_flow_ops_t the same way) and task 5's three
   render functions. This file is added to main/CMakeLists.txt SRCS so it
   compiles against the real network_provisioning headers, but nothing
   calls into it yet — gc-sections strips it out of the image until task 6
   wires it in. */
#include "setup_session_idf.h" /* prototypes for the seven exported functions below, checked against their definitions */

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
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"
#include "nvs_config.h"
#include "setup_session.h"

static const char *TAG = "setup_session_idf";

/* SRP6a salt length: 16 bytes is RFC 5054's own example width and what the
   component's own dev-mode fixture uses (examples/wifi_prov's sec2_salt is
   16 bytes) — not otherwise meaningful, the verifier is what carries the
   security. */
#define SRP_SALT_LEN 16

/* ---- state the ops below share with the plumbing underneath them ------- */

static bool s_wifi_inited;
static bool s_mgr_inited;
static bool s_clear_driver_store_requested; /* D3, deferred — see setup_session_idf_clear_wifi_driver_store() */
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
   they can be read. wifi_sta_config_t's own widths. */
static char s_cred_ssid[33];
static char s_cred_pass[64];

typedef struct {
    setup_session_event_t event;
    setup_session_poll_out_t out;
} queue_msg_t;

/* ---- the httpd side of the /mqtt form ----------------------------------- */

static bool httpd_chunk_sink(const char *chunk, size_t len, void *ctx) {
    httpd_req_t *req = (httpd_req_t *)ctx;
    return httpd_resp_send_chunk(req, chunk, (ssize_t)len) == ESP_OK;
}

static void send_mqtt_page(httpd_req_t *req, const char *status_msg) {
    char uri[MQTT_FORM_URI_MAX] = {0};
    char user[MQTT_FORM_USER_MAX] = {0};
    (void)nvs_config_get_mqtt_uri(uri, sizeof(uri));
    (void)nvs_config_get_mqtt_user(user, sizeof(user));
    char uri_esc[MQTT_FORM_HTML_ESCAPED_MAX(MQTT_FORM_URI_MAX)];
    char user_esc[MQTT_FORM_HTML_ESCAPED_MAX(MQTT_FORM_USER_MAX)];
    (void)mqtt_form_html_escape(uri, uri_esc, sizeof(uri_esc));
    (void)mqtt_form_html_escape(user, user_esc, sizeof(user_esc));

    setup_session_mqtt_page_in_t in = {.uri_escaped = uri_esc, .user_escaped = user_esc, .status_msg = status_msg};
    httpd_resp_set_type(req, "text/html");
    (void)setup_session_render_mqtt_page(&in, httpd_chunk_sink, req);
    httpd_resp_send_chunk(req, NULL, 0); /* ends the chunked response */
}

static esp_err_t mqtt_get_handler(httpd_req_t *req) {
    send_mqtt_page(req, NULL);
    return ESP_OK;
}

/* Reads at most MQTT_FORM_BODY_MAX bytes off the socket — on the HEAP,
   neither the stack nor a `static`. Not the stack: this runs on the httpd
   task, whose stack is the thing being protected (plan: "the review
   measured 4 KB for protocomm's default"), and 1 KB plus the two escape
   buffers in send_mqtt_page() is a meaningful fraction of that. Not
   `static` either — that traded the stack risk for a permanent DIRAM one,
   measured at +1 KB of this device's already-96%-full static RAM for a
   buffer only ever live for the length of one POST. A transient
   malloc/free costs nothing this feature doesn't already spend on SRP6a's
   own allocations (esp_srp_gen_salt_verifier), and setup mode is not a
   path anything here needs to keep off the heap allocator for timing
   reasons. A body longer than the cap is reported as too-long without
   ever being read into memory. */
static esp_err_t mqtt_post_handler(httpd_req_t *req) {
    mqtt_form_status_t st;
    mqtt_form_result_t result;

    if (req->content_len > MQTT_FORM_BODY_MAX) {
        st = (mqtt_form_status_t){MQTT_FORM_ERR_BODY_TOO_LONG, MQTT_FORM_FIELD_NONE};
        memset(&result, 0, sizeof(result));
    } else {
        char *body = malloc(MQTT_FORM_BODY_MAX);
        if (body == NULL) {
            ESP_LOGW(TAG, "/mqtt POST: out of memory reading the body");
            return ESP_ERR_NO_MEM;
        }
        size_t received = 0;
        while (received < req->content_len) {
            int r = httpd_req_recv(req, body + received, req->content_len - received);
            if (r == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            if (r <= 0) {
                ESP_LOGW(TAG, "/mqtt POST: recv failed (%d)", r);
                free(body);
                return ESP_FAIL;
            }
            received += (size_t)r;
        }
        st = mqtt_form_parse_urlencoded(body, received, &result);
        free(body);
    }

    bool ok = (st.err == MQTT_FORM_ERR_NONE);
    if (ok) {
        queue_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.event = SETUP_SESSION_EVENT_MQTT_SUBMIT;
        snprintf(msg.out.mqtt_uri, sizeof(msg.out.mqtt_uri), "%s", result.uri);
        snprintf(msg.out.mqtt_user, sizeof(msg.out.mqtt_user), "%s", result.user);
        snprintf(msg.out.mqtt_pass, sizeof(msg.out.mqtt_pass), "%s", result.pass);
        msg.out.mqtt_keep_pass = result.keep_pass;
        if (s_evt_queue != NULL)
            (void)xQueueSend(s_evt_queue, &msg, 0);
    }

    char status[80];
    setup_session_format_mqtt_status(ok, st, status, sizeof(status));
    send_mqtt_page(req, status);
    return ESP_OK;
}

/* The protocomm "mqtt-config" endpoint — the esp_prov.py --custom_data
   path (plan, D2's secondary route). Same parse, same queue post, just
   JSON instead of a browser form and a short plain-text reply instead of
   a page. */
static esp_err_t mqtt_endpoint_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf,
                                       ssize_t *outlen, void *priv_data) {
    (void)session_id;
    (void)priv_data;
    mqtt_form_result_t result;
    mqtt_form_status_t st = mqtt_form_parse_json((const char *)inbuf, (size_t)inlen, &result);
    bool ok = (st.err == MQTT_FORM_ERR_NONE);
    if (ok) {
        queue_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.event = SETUP_SESSION_EVENT_MQTT_SUBMIT;
        snprintf(msg.out.mqtt_uri, sizeof(msg.out.mqtt_uri), "%s", result.uri);
        snprintf(msg.out.mqtt_user, sizeof(msg.out.mqtt_user), "%s", result.user);
        snprintf(msg.out.mqtt_pass, sizeof(msg.out.mqtt_pass), "%s", result.pass);
        msg.out.mqtt_keep_pass = result.keep_pass;
        if (s_evt_queue != NULL)
            (void)xQueueSend(s_evt_queue, &msg, 0);
    }

    char status[80];
    setup_session_format_mqtt_status(ok, st, status, sizeof(status));
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
               own, so this is captured here and read back below. */
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)data;
            snprintf(s_cred_ssid, sizeof(s_cred_ssid), "%.*s", (int)sizeof(cfg->ssid), (const char *)cfg->ssid);
            snprintf(s_cred_pass, sizeof(s_cred_pass), "%.*s", (int)sizeof(cfg->password), (const char *)cfg->password);
            return;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL:
            msg.event = SETUP_SESSION_EVENT_WIFI_FAIL;
            break;
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            msg.event = SETUP_SESSION_EVENT_WIFI_SUCCESS;
            snprintf(msg.out.wifi_ssid, sizeof(msg.out.wifi_ssid), "%s", s_cred_ssid);
            snprintf(msg.out.wifi_password, sizeof(msg.out.wifi_password), "%s", s_cred_pass);
            break;
        default:
            return;
    }
    if (s_evt_queue != NULL)
        (void)xQueueSend(s_evt_queue, &msg, 0);
}

/* ---- the ops table's seven real implementations -------------------------- */

uint8_t setup_session_idf_rand_byte(void) {
    return (uint8_t)esp_random();
}

/* Idempotent-by-construction cleanup: every resource is torn down behind
   a null/flag guard, so calling this when `start` was never reached, or
   failed partway through, is a correctly-shaped no-op rather than a
   special case the caller has to know about (setup_session_ops_t's
   contract for `stop`). */
void setup_session_idf_stop(void) {
    if (s_mgr_inited) {
        network_prov_mgr_stop_provisioning();
        network_prov_mgr_wait();
        network_prov_mgr_deinit();
        s_mgr_inited = false;
    }
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
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
       itself — confirmed by reading network_provisioning 1.3.1's
       scheme_softap.c and manager.c, which call esp_wifi_set_mode() only.
       Both netifs are created once per boot, same pattern as
       wifi_session.c's static STA netif. */
    static esp_netif_t *s_sta_netif;
    static esp_netif_t *s_ap_netif;
    if (s_sta_netif == NULL)
        s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_ap_netif == NULL)
        s_ap_netif = esp_netif_create_default_wifi_ap();

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
       network_prov_scheme_softap_set_httpd_handle"). */
    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    ret = httpd_start(&s_httpd, &httpd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }
    network_prov_scheme_softap_set_httpd_handle(s_httpd);

    httpd_uri_t get_uri = {.uri = "/mqtt", .method = HTTP_GET, .handler = mqtt_get_handler, .user_ctx = NULL};
    httpd_uri_t post_uri = {.uri = "/mqtt", .method = HTTP_POST, .handler = mqtt_post_handler, .user_ctx = NULL};
    httpd_register_uri_handler(s_httpd, &get_uri);
    httpd_register_uri_handler(s_httpd, &post_uri);

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

    /* We own teardown ordering explicitly (setup_session.c's single
       teardown step) rather than the manager's own auto-stop, which would
       otherwise tear the scheme down as soon as CRED_SUCCESS fires and
       race the MQTT form/endpoint still being usable in the same
       session. */
    network_prov_mgr_disable_auto_stop(1000);
    network_prov_mgr_endpoint_create("mqtt-config");

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

    network_prov_mgr_endpoint_register("mqtt-config", mqtt_endpoint_handler, NULL);
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
   manager.c:2443-2448) — confirmed by reading the source rather than
   assumed — and esp_wifi_restore() is documented to erase WiFi's
   persistent settings, which is exactly the store
   NETWORK_PROV_WIFI_CRED_RECV wrote into via esp_wifi_set_storage(
   WIFI_STORAGE_FLASH) + esp_wifi_set_config() (manager.c:1408-1415).

   NOT CALLED HERE, DELIBERATELY — only requested. esp_wifi_restore()'s own
   doc comment says it also resets esp_wifi_set_mode(), and at the moment
   the pure layer calls this op (right after a verified WIFI_SUCCESS, still
   inside the session's loop) the manager and the SoftAP are both still
   live: network_prov_mgr_disable_auto_stop() was called precisely so the
   phone app gets time to poll its final "are you connected now?" query
   before anything stops. Resetting the WiFi mode out from under that
   exchange would be the ordering bug the softap scheme's own delayed-stop
   design exists to prevent. So this only sets a flag; setup_session_idf_stop()
   below acts on it after the manager has been fully stopped and
   deinited, but before esp_wifi itself goes down — the one point both
   "the manager is done with the radio" and "esp_wifi is still inited"
   (esp_wifi_restore() needs that) are simultaneously true. */
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
