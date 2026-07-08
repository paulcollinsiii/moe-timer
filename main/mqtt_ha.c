#include "mqtt_ha.h"

#include <stdio.h>
#include <string.h>

#include "cmd_apply.h"
#include "config_apply.h"
#include "device_id.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "hal_nvs.h"
#include "mqtt_client.h"
#include "nvs_config.h"
#include "timer.h"

static const char *TAG = "mqtt_ha";

/* Bump when entities are added/renamed — discovery configs republish once. */
#define DISC_SCHEMA_VER 1

#define CONNECT_TIMEOUT_MS 5000
#define PUBLISH_DRAIN_TIMEOUT_MS 3000

static EventGroupHandle_t s_eg;
#define EG_CONNECTED BIT0
#define EG_FAILED BIT1

static volatile int s_pub_acks;

/* Retained config document collected during the window (HA→device). The
   broker delivers it right after subscribe; we buffer it here and apply it
   after the stat publishes. Sized for the documented config schema. */
#define CONFIG_BUF_MAX 1024
static char s_config_buf[CONFIG_BUF_MAX];
static volatile bool s_config_received;
static int s_config_topic_len; /* strlen of magtag/<id>/config, for matching */

/* Retained command (HA→device), same collection pattern as config. */
#define CMD_BUF_MAX 256
static char s_cmd_buf[CMD_BUF_MAX];
static volatile bool s_cmd_received;
static int s_cmd_topic_len;
static bool s_locate_pending; /* set when a locate command applied; main.c consumes */

bool mqtt_ha_locate_pending(void) {
    bool p = s_locate_pending;
    s_locate_pending = false;
    return p;
}

/* Pending daily summary (captured at rollover, published next window;
   plain RAM — an unsent summary after a crash is an acceptable loss). */
static struct {
    bool pending;
    char date[11];
    int32_t used_s;
    uint16_t completions[TIMER_EXTRA_SLOTS];
} s_summary;

void mqtt_ha_queue_summary(const char *date, int32_t screen_used_s, const uint16_t completions[TIMER_EXTRA_SLOTS]) {
    snprintf(s_summary.date, sizeof(s_summary.date), "%s", date);
    s_summary.used_s = screen_used_s;
    memcpy(s_summary.completions, completions, sizeof(s_summary.completions));
    s_summary.pending = true;
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            xEventGroupSetBits(s_eg, EG_CONNECTED);
            break;
        case MQTT_EVENT_ERROR:
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupSetBits(s_eg, EG_FAILED);
            break;
        case MQTT_EVENT_PUBLISHED:
            s_pub_acks++;
            break;
        case MQTT_EVENT_DATA:
            /* config and cmd are the only subscriptions; a chunked payload
               (data_len < total_data_len) is copied by absolute offset. An
               empty retained payload (topic cleared) is ignored here. */
            if (ev->topic == NULL || ev->total_data_len <= 0)
                break;
            if (ev->topic_len == s_config_topic_len && ev->total_data_len < CONFIG_BUF_MAX) {
                memcpy(s_config_buf + ev->current_data_offset, ev->data, ev->data_len);
                if (ev->current_data_offset + ev->data_len >= ev->total_data_len) {
                    s_config_buf[ev->total_data_len] = '\0';
                    s_config_received = true;
                }
            } else if (ev->topic_len == s_cmd_topic_len && ev->total_data_len < CMD_BUF_MAX) {
                memcpy(s_cmd_buf + ev->current_data_offset, ev->data, ev->data_len);
                if (ev->current_data_offset + ev->data_len >= ev->total_data_len) {
                    s_cmd_buf[ev->total_data_len] = '\0';
                    s_cmd_received = true;
                }
            }
            break;
        default:
            break;
    }
}

/* QoS-1 retained publish; returns 1 when enqueued (counts toward the
   drain wait), 0 on failure. */
static int publish(esp_mqtt_client_handle_t client, const char *topic, const char *payload, int retain) {
    int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, 1, retain);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish to %s failed", topic);
        return 0;
    }
    return 1;
}

static int publish_discovery(esp_mqtt_client_handle_t client, const char *dev_name, const char *fw) {
    char topic[128];
    char payload[600];
    int count = 0, published = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        const char *name_override = NULL;
        char named[48];
        /* completions_N sensors carry the configured timer's name */
        if (strncmp(ents[i].key, "completions_", 12) == 0) {
            int slot = ents[i].key[12] - '0';
            const timer_def_t *def = timer_slot_def(slot);
            if (def == NULL)
                continue; /* slot disabled: no entity */
            snprintf(named, sizeof(named), "%s runs", def->name);
            name_override = named;
        }
        stats_json_discovery_topic(topic, sizeof(topic), device_id(), &ents[i]);
        int n =
            stats_json_discovery_named(payload, sizeof(payload), device_id(), dev_name, fw, &ents[i], name_override);
        if (n < (int)sizeof(payload)) {
            published += publish(client, topic, payload, 1);
        }
    }
    return published;
}

void mqtt_ha_window(const stats_snapshot_t *snap) {
    char uri[128], user[64], pass[64];
    nvs_config_get_mqtt_uri(uri, sizeof(uri));
    if (uri[0] == '\0') {
        return; /* MQTT disabled */
    }
    nvs_config_get_mqtt_user(user, sizeof(user));
    nvs_config_get_mqtt_pass(pass, sizeof(pass));

    s_eg = xEventGroupCreate();
    if (s_eg == NULL)
        return;
    s_pub_acks = 0;

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = (user[0] != '\0') ? user : NULL,
        .credentials.authentication.password = (pass[0] != '\0') ? pass : NULL,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (client == NULL)
        goto out_eg;
    if (esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL) != ESP_OK)
        goto out_client;
    if (esp_mqtt_client_start(client) != ESP_OK)
        goto out_client;

    EventBits_t bits =
        xEventGroupWaitBits(s_eg, EG_CONNECTED | EG_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (!(bits & EG_CONNECTED)) {
        ESP_LOGW(TAG, "broker connect failed/timed out");
        goto out_started;
    }

    int published = 0;
    char topic[96];
    static char payload[768];

    /* Subscribe to the retained config + cmd topics first, so the broker's
       delivery overlaps the stat publishes below (no separate wait). */
    s_config_received = false;
    s_cmd_received = false;
    char config_topic[96], cmd_topic[96];
    s_config_topic_len = snprintf(config_topic, sizeof(config_topic), "magtag/%s/config", device_id());
    s_cmd_topic_len = snprintf(cmd_topic, sizeof(cmd_topic), "magtag/%s/cmd", device_id());
    esp_mqtt_client_subscribe(client, config_topic, 1);
    esp_mqtt_client_subscribe(client, cmd_topic, 1);

    /* Discovery: once per schema bump (covers new entities and renames) */
    uint16_t disc_ver = 0;
    hal_nvs_read_u16("disc_ver", &disc_ver);
    bool fresh_discovery = (disc_ver != DISC_SCHEMA_VER);
    if (fresh_discovery) {
        char dev_name[64];
        device_name(dev_name, sizeof(dev_name));
        published += publish_discovery(client, dev_name, snap->fw);
    }

    snprintf(topic, sizeof(topic), "magtag/%s/stat", device_id());
    if (stats_json_stat(payload, sizeof(payload), snap) < (int)sizeof(payload)) {
        published += publish(client, topic, payload, 1);
    }

    if (s_summary.pending) {
        snprintf(topic, sizeof(topic), "magtag/%s/summary", device_id());
        if (stats_json_summary(payload, sizeof(payload), s_summary.date, s_summary.used_s, s_summary.completions) <
            (int)sizeof(payload)) {
            published += publish(client, topic, payload, 1);
        }
    }

    /* Drain: wait for QoS-1 acks so the disconnect doesn't drop them */
    int waited = 0;
    while (s_pub_acks < published && waited < PUBLISH_DRAIN_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (s_pub_acks >= published) {
        s_summary.pending = false;
        if (fresh_discovery) {
            hal_nvs_write_u16("disc_ver", DISC_SCHEMA_VER);
        }
        ESP_LOGI(TAG, "published %d messages", published);
    } else {
        ESP_LOGW(TAG, "publish drain incomplete (%d/%d)", s_pub_acks, published);
    }

    /* Retained config + cmd arrive right after subscribe; give them a
       moment past the publish drain to land. */
    int cfg_wait = 0;
    while (!(s_config_received && s_cmd_received) && cfg_wait < 1500) {
        vTaskDelay(pdMS_TO_TICKS(100));
        cfg_wait += 100;
    }

    char ack[256];
    if (s_config_received) {
        /* config_apply writes NVS; a changed timezone/timer def takes
           effect on the next boot/operation. */
        config_result_t r = config_apply(s_config_buf, ack, sizeof(ack));
        if (r != CONFIG_SKIPPED) {
            snprintf(topic, sizeof(topic), "magtag/%s/config_ack", device_id());
            esp_mqtt_client_publish(client, topic, ack, 0, 1, 1); /* retained ack */
            ESP_LOGI(TAG, "config applied (result %d)", (int)r);
        }
    }

    if (s_cmd_received) {
        cmd_action_t act;
        cmd_result_t cr = cmd_apply(s_cmd_buf, &act, ack, sizeof(ack));
        if (cr == CMD_GRANT || cr == CMD_LOCATE) {
            if (cr == CMD_GRANT) {
                /* Pure state change; persisted at the next enter_deep_sleep */
                timer_grant(act.slot, act.sec);
            } else {
                s_locate_pending = true; /* main.c runs the alarm after the window */
            }
            snprintf(topic, sizeof(topic), "magtag/%s/event", device_id());
            esp_mqtt_client_publish(client, topic, ack, 0, 1, 0); /* event ack, not retained */
            /* Clear the retained command so it isn't re-delivered/re-applied */
            snprintf(topic, sizeof(topic), "magtag/%s/cmd", device_id());
            esp_mqtt_client_publish(client, topic, "", 0, 1, 1);
            ESP_LOGI(TAG, "command applied (result %d)", (int)cr);
        }
    }

    /* Final drain so the acks + cleared-topic publishes survive disconnect */
    vTaskDelay(pdMS_TO_TICKS(400));

out_started:
    esp_mqtt_client_stop(client);
out_client:
    esp_mqtt_client_destroy(client);
out_eg:
    vEventGroupDelete(s_eg);
    s_eg = NULL;
}
