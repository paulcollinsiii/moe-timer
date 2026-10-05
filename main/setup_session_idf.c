/* Device glue for the setup session (setup_session.h). Not host-tested —
   kept thin on purpose: every function here is either one of
   setup_session_ops_t's real implementations, or private plumbing that
   feeds the queue those implementations read from. The decisions (what
   ends the session, what gets stored, what the caller sleeps into) all
   live in setup_session.c; nothing here branches on the session's own
   outcome.

   setup_mode_ops() at the bottom assembles the whole setup_session_ops_t
   from the nine exported functions below and the setup screens' two render
   functions. The one member it cannot supply is extend_awake, whose real
   implementation owns the awake failsafe's esp_timer handle in main.c; the
   caller passes it in, so the table is built here and not in the
   composition root. */
#include "setup_session_idf.h" /* prototypes for the nine exported functions below, checked against their definitions */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "dhcpserver/dhcpserver.h" /* OFFER_DNS */
#include "dns_reply.h"
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
#include "freertos/semphr.h"
#include "freertos/task.h" /* xTaskGetHandle, uxTaskGetStackHighWaterMark — bench measurement of the httpd stack */
#include "lwip/sockets.h"
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
   fixed endpoints + "mqtt-config", each one its own URI handler at the
   esp_http_server level, plus this file's page GET/POST on "/" and on the
   "/mqtt" alias, and GET "/status") comes to exactly 11 today. Three
   slots of headroom so the next endpoint anyone adds does not fail
   silently against a config nobody remembered to grow. */
#define SETUP_SESSION_HTTPD_MAX_URI_HANDLERS 14

/* The captive portal's DNS responder task: one UDP socket, one 512 B query
   buffer and one reply buffer on its stack, with the lwip send/receive
   frames below them. Not measured; the high-water mark is logged on stop
   so a bench run can replace this guess. */
#define SETUP_SESSION_DNS_TASK_STACK 3072
#define SETUP_SESSION_DNS_TASK_PRIO 4
/* The receive timeout is how often the responder looks at its stop flag,
   so it bounds how long stop() can wait for it. */
#define SETUP_SESSION_DNS_RECV_TIMEOUT_MS 500
#define SETUP_SESSION_DNS_STOP_WAIT_MS 2000

/* HTTPD_DEFAULT_CONFIG's own 4096 B is tight against this feature's
   worst case. With the setup page's locals
   moved to the heap (send_page below) the two paths left on this
   stack are:
     - our own handler frames: an mqtt_form_setup_t (~370 B) + a
       wifi_config_t for the join + a short status buffer, on the
       order of 700 B;
     - esp_http_server's own dispatch frames plus, on the protocomm
       endpoint path, one mbedtls GCM decrypt and a bounded-depth cJSON
       parse (mqtt_form_parse_json pre-scans nesting before cJSON ever
       runs), estimated 2-2.5 KB combined.
   ~3 KB of estimated worst case; 6144 leaves roughly double that as
   margin without an actual stack trace to measure against
   (uxTaskGetStackHighWaterMark is logged in stop() below so a bench
   measurement can replace this estimate with a real number). */
#define SETUP_SESSION_HTTPD_STACK_SIZE 6144

/* Consecutive HTTPD_SOCK_ERR_TIMEOUT returns the setup POST body read
   tolerates before giving up: unbounded retry pins the single httpd
   task on a client that sent a Content-Length and then went quiet,
   and httpd_stop() in teardown then blocks joining that same task.
   Any successful partial read resets the count,
   so a slow-but-progressing upload is never penalised for it. */
#define SETUP_SESSION_POST_RECV_MAX_TIMEOUTS 3

/* ---- state the ops below share with the plumbing underneath them ------- */

static bool s_wifi_inited;
static bool s_mgr_inited;
static bool s_clear_driver_store_requested; /* deferred — see setup_session_idf_clear_wifi_driver_store() */
/* True from the top of setup_session_idf_stop() until it returns: an
   NETWORK_PROV_END/_DEINIT our OWN stop provokes is not a failure to
   report — see prov_event_handler's NETWORK_PROV_END case. */
static bool s_our_own_stop;
static httpd_handle_t s_httpd;
static esp_event_handler_instance_t s_inst_prov;
static QueueHandle_t s_evt_queue;

/* Where the browser-or-app WiFi join stands, for the page and /status. Written by
   the event-loop task (the manager's events), the httpd task (a submit) and
   the session task (the credential store), read by httpd; setup_join_t's
   own contract keeps a reader from seeing FAILED with a stale reason. */
static setup_join_t s_join;

/* The raw reason of the latest WIFI_EVENT_STA_DISCONNECTED. The manager's
   own reason covers two codes and is stale for the rest, so CRED_FAIL reads
   this instead. Both events run on the event-loop task, and the manager
   posts CRED_FAIL from inside its own handler for that disconnect, so this
   is always written before the CRED_FAIL that reads it. */
static volatile int s_last_disconnect_reason;
static esp_event_handler_instance_t s_inst_wifi_disc;

/* A failed join gets this many tries before the manager gives up. With the
   default (0) the manager retries forever on every reason except the five
   it knows, which leaves it, and this page, stuck at "connecting". */
#define SETUP_SESSION_WIFI_CONN_ATTEMPTS 3

/* Captive portal DNS responder. s_dns_done is given by the task as its last
   act, so stop() knows the socket is closed before the netifs go away. */
static TaskHandle_t s_dns_task;
static SemaphoreHandle_t s_dns_done;
static volatile bool s_dns_stop;

/* The captive portal URI handed to DHCP clients (option 114). The DHCP
   server keeps this pointer rather than copying it, so it must outlive the
   session: static, never freed. */
static char s_portal_uri[] = "http://" SETUP_SESSION_AP_IP;

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

/* ---- the httpd side of the setup page ----------------------------------- */

static bool httpd_chunk_sink(const char *chunk, size_t len, void *ctx) {
    httpd_req_t *req = (httpd_req_t *)ctx;
    return httpd_resp_send_chunk(req, chunk, (ssize_t)len) == ESP_OK;
}

/* The page's own prefill + escape buffers, heap-allocated rather than
   stack locals: at ~1.3 KB combined (uri/user from NVS plus their HTML-
   escaped widenings) they were the single largest consumer of this
   handler's share of the httpd task stack. A transient allocation,
   freed before this function returns — the same
   call/free shape setup_post_handler's body buffer already uses below,
   and for the same reason: setup mode is not a path this device needs
   to keep off the heap allocator for timing reasons. */
typedef struct {
    char ssid[NVS_CONFIG_WIFI_SSID_BUF];
    char uri[MQTT_FORM_URI_MAX];
    char user[MQTT_FORM_USER_MAX];
    char ssid_esc[MQTT_FORM_HTML_ESCAPED_MAX(NVS_CONFIG_WIFI_SSID_BUF)];
    char uri_esc[MQTT_FORM_HTML_ESCAPED_MAX(MQTT_FORM_URI_MAX)];
    char user_esc[MQTT_FORM_HTML_ESCAPED_MAX(MQTT_FORM_USER_MAX)];
} page_scratch_t;

static void send_page(httpd_req_t *req, const char *status_msg) {
    page_scratch_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        ESP_LOGW(TAG, "setup page: out of memory building the page");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return;
    }
    memset(s, 0, sizeof(*s));
    (void)nvs_config_get_wifi_ssid(s->ssid, sizeof(s->ssid));
    (void)nvs_config_get_mqtt_uri(s->uri, sizeof(s->uri));
    (void)nvs_config_get_mqtt_user(s->user, sizeof(s->user));
    (void)mqtt_form_html_escape(s->ssid, s->ssid_esc, sizeof(s->ssid_esc));
    (void)mqtt_form_html_escape(s->uri, s->uri_esc, sizeof(s->uri_esc));
    (void)mqtt_form_html_escape(s->user, s->user_esc, sizeof(s->user_esc));

    setup_session_page_in_t in = {.ssid_escaped = s->ssid_esc,
                                  .uri_escaped = s->uri_esc,
                                  .user_escaped = s->user_esc,
                                  .status_msg = status_msg,
                                  .join_state = s_join.state,
                                  .join_reason = s_join.reason};
    httpd_resp_set_type(req, SETUP_SESSION_HTML_CONTENT_TYPE);
    (void)setup_session_render_page(&in, httpd_chunk_sink, req);
    httpd_resp_send_chunk(req, NULL, 0); /* ends the chunked response */
    free(s);
}

static esp_err_t page_get_handler(httpd_req_t *req) {
    send_page(req, NULL);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    char json[96];
    (void)setup_session_format_status_json(s_join.state, s_join.reason, json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* Phones probe a handful of well-known URLs (generate_204 and the like) to
   decide whether the network is "captive". The DNS responder sends every
   name here, and answering any unknown path with a redirect to the setup
   page is what turns that probe into the sign-in popup. */
static esp_err_t not_found_redirect(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" SETUP_SESSION_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

/* Tells the session an MQTT-only submit has been stored (the session decides
   whether that ends it). NVS writes from the httpd task are safe — nvs_flash
   is thread-safe across tasks. */
static void notify_mqtt_stored(void) {
    queue_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.event = SETUP_SESSION_EVENT_MQTT_STORED;
    if (s_evt_queue == NULL || xQueueSend(s_evt_queue, &msg, 0) != pdTRUE) {
        /* The data is already in NVS — only the "end the session now"
           notification is lost, and the session's own timeout still covers
           that. */
        ESP_LOGW(TAG, "mqtt credentials saved, but the session queue would not take the notification");
    }
}

/* The "mqtt-config" protocomm endpoint's parse-then-store-then-notify.
   Writes a short human status into `status` (capacity status_cap) that is
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

        if (stored)
            notify_mqtt_stored();
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
static esp_err_t setup_post_handler(httpd_req_t *req) {
    mqtt_form_status_t st;
    mqtt_form_setup_t form;
    memset(&form, 0, sizeof(form));

    if (req->content_len > MQTT_FORM_BODY_MAX) {
        st = (mqtt_form_status_t){MQTT_FORM_ERR_BODY_TOO_LONG, MQTT_FORM_FIELD_NONE};
    } else {
        char *body = malloc(MQTT_FORM_BODY_MAX);
        if (body == NULL) {
            ESP_LOGW(TAG, "setup POST: out of memory reading the body");
            return ESP_ERR_NO_MEM;
        }
        size_t received = 0;
        int timeouts = 0;
        esp_err_t recv_failed = ESP_OK;
        while (received < req->content_len) {
            int r = httpd_req_recv(req, body + received, req->content_len - received);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > SETUP_SESSION_POST_RECV_MAX_TIMEOUTS) {
                    ESP_LOGW(TAG, "setup POST: recv timed out %d times in a row", timeouts);
                    recv_failed = ESP_ERR_TIMEOUT;
                    break;
                }
                continue;
            }
            if (r <= 0) {
                ESP_LOGW(TAG, "setup POST: recv failed (%d)", r);
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
        st = mqtt_form_parse_setup(body, received, &form);
        mbedtls_platform_zeroize(body, MQTT_FORM_BODY_MAX); /* held the submitted passwords in cleartext */
        free(body);
    }

    char status[80];
    setup_session_apply_t applied = SETUP_APPLY_NOTHING;
    if (st.err != MQTT_FORM_ERR_NONE) {
        setup_session_format_mqtt_status(false, st, status, sizeof(status));
    } else {
        /* A table of only the two device ops a submit needs, for the same
           reason apply_and_notify builds one: setup_session_run's own table
           belongs to main.c and is out of reach from here. */
        setup_session_ops_t apply_ops;
        memset(&apply_ops, 0, sizeof(apply_ops));
        apply_ops.set_mqtt_creds = setup_session_idf_set_mqtt_creds;
        apply_ops.join_wifi = setup_session_idf_join_wifi;
        applied = setup_session_apply_setup(&apply_ops, &form);
        snprintf(status, sizeof(status), "%s", setup_session_apply_msg(applied));
    }
    mbedtls_platform_zeroize(&form, sizeof(form)); /* form.wifi_pass, form.mqtt.pass */

    if (applied == SETUP_APPLY_MQTT_SAVED)
        notify_mqtt_stored();

    if (applied == SETUP_APPLY_JOINING) {
        /* Post/redirect/get: the join takes seconds, and a reload of this
           POST's own response would resubmit the form (which the manager
           refuses mid-join). Redirecting means a plain reload, with JS off,
           re-reads the join state instead. */
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        return httpd_resp_send(req, NULL, 0);
    }
    send_page(req, status);
    return ESP_OK;
}

/* The protocomm "mqtt-config" endpoint — the esp_prov.py --custom_data
   path, the secondary route to the page. Same parse-and-store, same JSON
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

static void wifi_disconnect_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    s_last_disconnect_reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
}

/* setup_join_on_failed's reset callback: the manager's own reset after a
   failure, whose result only matters for the log. */
static void reset_manager_after_failure(void *ctx) {
    (void)ctx;
    if (network_prov_mgr_reset_wifi_sm_state_on_failure() != ESP_OK)
        ESP_LOGW(TAG, "reset_wifi_sm_state_on_failure failed: a retry after this failure may be refused");
}

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
            /* Whichever route the credentials came by (the page or the
               phone app), the page should show a join in progress. */
            setup_join_on_connecting(&s_join);
            s_last_disconnect_reason = 0;
            /* The manager has just written these credentials, unverified,
               to the driver's flash store. A failure wipes them again, but
               a session that ends mid-join (a timeout) would not, so
               stop() clears the store whenever a join was ever attempted. */
            s_clear_driver_store_requested = true;
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
            /* The reset runs before FAILED is shown: the page invites a
               retry, and the manager refuses one
               (network_prov_mgr_configure_wifi_sta returns ESP_FAIL while
               its state is FAIL) until the reset has run. The reason is the
               raw disconnect code, not the manager's two-value one, which
               is stale for every other reason. */
            setup_join_on_failed(&s_join, s_last_disconnect_reason, reset_manager_after_failure, NULL);
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

/* ---- the ops table's nine real implementations --------------------------- */

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

/* ---- the captive portal's DNS responder -------------------------------- */

/* Answers every A query with the SoftAP's own address (dns_reply_build) so a
   phone's connectivity probe reaches our httpd. Bound to the wildcard
   address because the AP netif's IP may not be up yet when this starts; the
   only traffic that reaches it is the SoftAP's and, while a join runs, the
   joined LAN's, and an answer pointing at ourselves does a LAN client no
   harm. */
static void dns_task(void *arg) {
    (void)arg;
    uint8_t ap_ip[4];
    uint32_t ap_addr = esp_ip4addr_aton(SETUP_SESSION_AP_IP); /* already in network byte order */
    memcpy(ap_ip, &ap_addr, sizeof(ap_ip));
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "dns: socket failed (errno %d); the page needs its address typed", errno);
    } else {
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
        struct timeval tv = {.tv_sec = 0, .tv_usec = SETUP_SESSION_DNS_RECV_TIMEOUT_MS * 1000};
        (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ESP_LOGW(TAG, "dns: bind failed (errno %d); the page needs its address typed", errno);
        } else {
            uint8_t query[DNS_QUERY_MAX];
            uint8_t reply[DNS_REPLY_MAX];
            while (!s_dns_stop) {
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                int n = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&from, &from_len);
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    vTaskDelay(pdMS_TO_TICKS(100)); /* a hard error must not spin */
                    continue;
                }
                if (n <= 0)
                    continue;
                size_t rn = dns_reply_build(query, (size_t)n, ap_ip, reply, sizeof(reply));
                if (rn > 0)
                    (void)sendto(sock, reply, rn, 0, (struct sockaddr *)&from, from_len);
            }
        }
        close(sock);
    }
    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "dns task stack high-water mark: %u words free", (unsigned)stack_free);
    xSemaphoreGive(s_dns_done);
    vTaskDelete(NULL);
}

static void dns_stop(void) {
    if (s_dns_task == NULL)
        return;
    s_dns_stop = true;
    if (xSemaphoreTake(s_dns_done, pdMS_TO_TICKS(SETUP_SESSION_DNS_STOP_WAIT_MS)) == pdTRUE) {
        vSemaphoreDelete(s_dns_done);
        s_dns_done = NULL;
    } else {
        /* The task is still holding its socket. Leaving the semaphore alone
           (a few dozen bytes, once) is what keeps its final give from
           landing on freed memory, and s_dns_task stays set so dns_start
           will not run a second responder over the same globals and port
           while this one is alive. */
        ESP_LOGW(TAG, "dns task did not stop within %d ms", SETUP_SESSION_DNS_STOP_WAIT_MS);
        return;
    }
    s_dns_task = NULL;
}

/* Fail-soft: without the responder the QR still joins the network and the
   panel still shows the address to type; only the popup is lost. */
static void dns_start(void) {
    dns_stop(); /* collects a responder an earlier stop gave up waiting for */
    if (s_dns_task != NULL) {
        ESP_LOGW(TAG, "dns: the previous responder never stopped; not starting another");
        return;
    }
    s_dns_stop = false;
    s_dns_done = xSemaphoreCreateBinary();
    if (s_dns_done == NULL || xTaskCreate(dns_task, "setup_dns", SETUP_SESSION_DNS_TASK_STACK, NULL,
                                          SETUP_SESSION_DNS_TASK_PRIO, &s_dns_task) != pdPASS) {
        ESP_LOGW(TAG, "dns: could not start the responder; the page needs its address typed");
        if (s_dns_done != NULL) {
            vSemaphoreDelete(s_dns_done);
            s_dns_done = NULL;
        }
        s_dns_task = NULL;
    }
}

/* The captive portal's DHCP half, best-effort for the same reason: hand
   clients this device as their DNS server and name the setup page in
   option 114 (the captive-portal URI; whether a given phone acts on it is
   unproven, the DNS answer and the 404 redirect are what carry the popup).
   The AP's
   DHCP server is not running yet (it starts with the AP), so the options
   can be set without stopping it. */
static void dhcp_set_portal_options(esp_netif_t *ap_netif) {
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(SETUP_SESSION_AP_IP);
    uint8_t offer_dns = OFFER_DNS;
    esp_err_t r1 =
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
    esp_err_t r2 = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_err_t r3 = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_portal_uri,
                                          strlen(s_portal_uri));
    if (r1 != ESP_OK || r2 != ESP_OK || r3 != ESP_OK)
        ESP_LOGW(TAG, "dhcp portal options not fully set (%s/%s/%s)", esp_err_to_name(r1), esp_err_to_name(r2),
                 esp_err_to_name(r3));
}

/* Idempotent-by-construction cleanup: every resource is torn down behind
   a null/flag guard, so calling this when `start` was never reached, or
   failed partway through, is a correctly-shaped no-op rather than a
   special case the caller has to know about (setup_session_ops_t's
   contract for `stop`). */
void setup_session_idf_stop(void) {
    s_our_own_stop = true; /* see prov_event_handler's NETWORK_PROV_END/_DEINIT case */

    dns_stop(); /* before the radio goes: it holds a socket on the AP's netif */

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
    if (s_inst_wifi_disc != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_inst_wifi_disc);
        s_inst_wifi_disc = NULL;
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
    /* The driver-store clear, deferred from setup_session_idf_clear_wifi_driver_store()
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
       but DO share a boot's lifetime of
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
    dhcp_set_portal_options(s_ap_netif);
    setup_join_init(&s_join);
    s_last_disconnect_reason = 0;

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
    /* Without the raw reason every failure reads as unknown, which only
       costs the page its advice, so a failed registration is not fatal. */
    ret = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_disconnect_handler, NULL,
                                              &s_inst_wifi_disc);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "disconnect handler register: %s", esp_err_to_name(ret));

    /* Our own httpd instance, so the setup page can be registered on it
       (the manager's scheme takes the handle through
       network_prov_scheme_softap_set_httpd_handle). max_uri_handlers and
       lru_purge_enable are both raised from HTTPD_DEFAULT_CONFIG's own
       defaults (8, false): the 11 handlers this session registers would
       overflow 8, and protocomm's own httpd transport sets
       lru_purge_enable itself (protocomm_httpd.c) while ours did not. */
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
    /* The address of the handle, not the handle: protocomm_httpd keeps this
       as its priv and dereferences it as httpd_handle_t * on every endpoint
       registration. Passing s_httpd itself panics in start_provisioning. */
    network_prov_scheme_softap_set_httpd_handle(&s_httpd);

    /* The page lives at "/", and "/mqtt" stays as an alias so a link or
       bookmark to the old broker form still lands on the same page. */
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get_handler},
        {.uri = "/", .method = HTTP_POST, .handler = setup_post_handler},
        {.uri = "/mqtt", .method = HTTP_GET, .handler = page_get_handler},
        {.uri = "/mqtt", .method = HTTP_POST, .handler = setup_post_handler},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ret = httpd_register_uri_handler(s_httpd, &routes[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_register_uri_handler(%s): %s", routes[i].uri, esp_err_to_name(ret));
            setup_session_idf_stop();
            return false;
        }
    }
    ret = httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, not_found_redirect);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_err_handler(404): %s", esp_err_to_name(ret));
        setup_session_idf_stop();
        return false;
    }

    network_prov_mgr_config_t mgr_cfg = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .network_prov_wifi_conn_cfg = {.wifi_conn_attempts = SETUP_SESSION_WIFI_CONN_ATTEMPTS},
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
    ret = esp_srp_gen_salt_verifier(SETUP_SESSION_PROV_USERNAME, (int)strlen(SETUP_SESSION_PROV_USERNAME), ap_password,
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

    dns_start();
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
        setup_join_on_save_failed(&s_join);
        /* The driver's copy is the only verified one left; keep it. */
        s_clear_driver_store_requested = false;
        return false;
    }
    /* The only place the page may say "saved": the join was verified and the
       app's own keys now hold it. */
    setup_join_on_saved(&s_join);
    return true;
}

/* The page's WiFi submit. Handed to the provisioning manager rather than
   stored: network_prov_mgr_configure_wifi_sta() drives the same state
   machine the phone app does, so the join is verified and the same
   CRED_SUCCESS / CRED_FAIL events (and the CRED_FAIL reset) are the only
   route to storage. It is called straight from the httpd task rather than
   posted to the session task because the manager's own protocomm handler
   makes the very same call from this same task, it serialises on its own
   lock, and it returns before the join (a one-second timer starts the
   connect), so it cannot hold the httpd task. A refusal (ESP_FAIL) means a
   join is already past the point of accepting credentials. */
bool setup_session_idf_join_wifi(const char *ssid, const char *password) {
    wifi_config_t cfg = {0};
    /* memcpy of the real length, as in prov_event_handler: ssid[32] and
       password[64] are not NUL-terminated by contract, and a full-width
       value has no room for a terminator. */
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, password, strnlen(password, sizeof(cfg.sta.password)));
    cfg.sta.scan_method = WIFI_FAST_SCAN; /* what the manager's own handler asks for */

    esp_err_t ret = network_prov_mgr_configure_wifi_sta(&cfg);
    mbedtls_platform_zeroize(&cfg, sizeof(cfg));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "configure_wifi_sta refused the join: %s", esp_err_to_name(ret));
        return false;
    }
    setup_join_on_connecting(&s_join);
    return true;
}

/* The driver's own flash-backed copy can't disagree with the app's
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
        .join_wifi = setup_session_idf_join_wifi,
    };
}
