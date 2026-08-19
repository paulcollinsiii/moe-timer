#include "mqtt_ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd_apply.h"
#include "config_apply.h"
#include "device_id.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "ha_config.h"
#include "hal_nvs.h"
#include "mqtt_client.h"
#include "mqtt_rx.h"
#include "mqtt_topics.h"
#include "nvs_config.h"
#include "nvs_keys.h"
#include "ota_flow.h"
#include "panic_diag.h"
#include "timer.h"

static const char *TAG = "mqtt_ha";

/* Bump when entities are added/renamed — discovery configs republish once. */
/* STATS_JSON_DISC_SCHEMA_VER lives in stats_json.h, beside the entity
   table it versions, so that adding a row cannot miss the bump -- the
   number and the table are pinned to each other by test_stats_json. It
   was here until v18 and nothing else changed about how it is used. */
#define DISC_SCHEMA_VER STATS_JSON_DISC_SCHEMA_VER

#define CONNECT_TIMEOUT_MS 5000
#define PUBLISH_DRAIN_TIMEOUT_MS 3000
#define RETAINED_RX_TIMEOUT_MS 1500 /* config/cmd usually land right after subscribe */

static EventGroupHandle_t s_eg;
#define EG_CONNECTED BIT0
#define EG_FAILED BIT1

static volatile int s_pub_acks;

#define CONFIG_BUF_MAX 1024
#define CMD_BUF_MAX 256
/* Registry fields + screen_bonus + locate, with headroom so a duplicate
   (retained + a fresh in-window edit) can't silently drop. No longer a
   hand-counted number: ha_config.c static-asserts its own FIELDS array
   against HA_CONFIG_SET_SLOTS, so adding a field breaks the build rather
   than dropping edits with a "set buffer full" log nobody reads. The old
   hand-maintained count had gone stale twice (25 while the real count was
   past 32). */
#define SET_MAX HA_CONFIG_SET_SLOTS

/* Window-scoped buffers: allocated at window start, freed at teardown —
   the radio is off (and none of this is needed) for the vast majority of
   every wake, so these ~11 KB (sizeof(window_mem_t), which SET_MAX
   dominates) no longer sit in .bss permanently. The pointer doubles as
   the "window open" flag for the event handler. */
typedef struct {
    char topic[128];
    char payload[STATS_JSON_PAYLOAD_MAX]; /* stat/summary/discovery payloads */
    char ack[256];
    char cfg_state[HA_CONFIG_STATE_MAX];
    char config_buf[CONFIG_BUF_MAX]; /* retained config document (HA→device) */
    char cmd_buf[CMD_BUF_MAX];       /* retained command document */
    mqtt_set_kv_t sets[SET_MAX];     /* editable-config sets (set/<key>) */
} window_mem_t;

/* Per-window heap budget. CFG_STR_MAX multiplies through the sets array
   (x SET_MAX), so the ceiling silently controls this figure: at 128 the
   struct is ~11 KB, at 512 it would be ~29 KB with nothing else failing.
   Allocation failure here disables the whole MQTT window, so the growth
   has to be a deliberate edit rather than a side effect. */
_Static_assert(sizeof(window_mem_t) <= 12288, "window_mem_t outgrew its per-window heap budget");
/* config_apply.h says callers must not pass less than CONFIG_ACK_MIN; this
   is the only caller, and it was exactly 256 by coincidence. */
_Static_assert(sizeof(((window_mem_t *)0)->ack) >= CONFIG_ACK_MIN, "ack buffer is below CONFIG_ACK_MIN");

static window_mem_t *s_mem;
static mqtt_rx_t s_rx; /* routing/reassembly context; buffers point into s_mem */

static bool s_locate_pending;      /* set when a locate command applied; main.c consumes */
static bool s_bonus_clear_pending; /* rollover: clear the retained bonus target next window */

void mqtt_ha_queue_bonus_clear(void) {
    s_bonus_clear_pending = true;
}

bool mqtt_ha_locate_pending(void) {
    bool p = s_locate_pending;
    s_locate_pending = false;
    return p;
}

/* Timer effects parsed during the window are BUFFERED, never applied here:
   the window runs on the network task, and only the orchestrator may mutate
   timer state (it applies these after joining the task). */
static bool s_bonus_target_pending; /* set/screen_bonus target buffered
                                       (the value is signed, so no
                                       in-band none sentinel) */
static int32_t s_bonus_target_s;
static int s_grant_slot = -1; /* cmd grant; -1 = none */
static int32_t s_grant_sec;

bool mqtt_ha_take_bonus_target(int32_t *target_sec) {
    if (!s_bonus_target_pending)
        return false;
    *target_sec = s_bonus_target_s;
    s_bonus_target_pending = false;
    return true;
}

bool mqtt_ha_take_grant(int *slot, int32_t *sec) {
    if (s_grant_slot < 0)
        return false;
    *slot = s_grant_slot;
    *sec = s_grant_sec;
    s_grant_slot = -1;
    return true;
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
            /* Routing + chunk reassembly live in mqtt_rx.c (host-tested,
               incl. the set/tz-vs-config same-length misroute case). */
            if (s_mem == NULL)
                break;
            switch (mqtt_rx_on_data(&s_rx, ev->topic, ev->topic_len, ev->data, ev->data_len, ev->total_data_len,
                                    ev->current_data_offset)) {
                case MQTT_RX_SETS_FULL:
                    ESP_LOGW(TAG, "set buffer full (%d), edit dropped", SET_MAX);
                    break;
                case MQTT_RX_SET_TOO_LONG:
                    ESP_LOGW(TAG, "set key/value too long, edit dropped");
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

/* QoS-1 publish; returns 1 when enqueued (counts toward the drain wait),
   0 on failure. EVERY window publish goes through here so the final drain
   can wait on the full ack count instead of a blind delay. */
static int publish(esp_mqtt_client_handle_t client, const char *topic, const char *payload, int retain) {
    int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, 1, retain);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish to %s failed", topic);
        return 0;
    }
    return 1;
}

/* Wait (bounded) for the QoS-1 acks so a disconnect can't drop messages. */
static bool drain_acks(int published, int timeout_ms) {
    int waited = 0;
    while (s_pub_acks < published && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return s_pub_acks >= published;
}

static int publish_discovery(esp_mqtt_client_handle_t client, const char *dev_name, const char *fw) {
    char *topic = s_mem->topic;
    char *payload = s_mem->payload;
    int count = 0, published = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        const char *name_override = NULL;
        char named[48];
        /* Per-slot sensors (completions_N / remaining_N / limit_N) carry
           the configured timer's name; disabled slots get no entity. */
        const char *suffix = NULL;
        int slot = 0;
        if (strncmp(ents[i].key, "completions_", 12) == 0) {
            slot = ents[i].key[12] - '0';
            suffix = "runs";
        } else if (strncmp(ents[i].key, "remaining_", 10) == 0) {
            slot = ents[i].key[10] - '0';
            suffix = "remaining";
        } else if (strncmp(ents[i].key, "limit_", 6) == 0) {
            slot = ents[i].key[6] - '0';
            suffix = "limit";
        }
        if (suffix != NULL) {
            const timer_def_t *def = timer_slot_def(slot);
            if (def == NULL) {
                /* Slot disabled: RETIRE the entity rather than just
                   skipping it. Skipping leaves the previous occupant's
                   retained discovery config on the broker, so clearing
                   timer2_name left `Reading remaining/limit/runs` in HA
                   forever-unavailable. An empty retained payload on the
                   discovery topic is how MQTT discovery deletes. The
                   RETIRED[] loop below only covers three legacy keys and
                   never covered these. */
                stats_json_discovery_topic(topic, sizeof(s_mem->topic), device_id(), &ents[i]);
                published += publish(client, topic, "", 1);
                continue;
            }
            snprintf(named, sizeof(named), "%s %s", def->name, suffix);
            name_override = named;
        }
        stats_json_discovery_topic(topic, sizeof(s_mem->topic), device_id(), &ents[i]);
        int n = stats_json_discovery_named(payload, sizeof(s_mem->payload), device_id(), dev_name, fw, &ents[i],
                                           name_override);
        if (n < (int)sizeof(s_mem->payload)) {
            published += publish(client, topic, payload, 1);
        } else {
            /* The action-discovery path below already says this out loud;
               the entity table is where the eleven diagnostic entities
               land, so it has to as well. An entity that overflows here
               simply never appears in HA, which on a diagnostics feature
               is the one failure that must not be silent. */
            ESP_LOGW(TAG, "%s discovery truncated, skipped", ents[i].key);
        }
    }
    /* Retire replaced entities (v9): clear their retained discovery configs
       so HA drops them instead of showing them forever-unavailable. */
    static const char *RETIRED[] = {"remaining", "allocation", "screen_used"};
    for (size_t i = 0; i < sizeof(RETIRED) / sizeof(RETIRED[0]); i++) {
        mqtt_disc_topic(topic, sizeof(s_mem->topic), "sensor", device_id(), RETIRED[i]);
        published += publish(client, topic, "", 1);
    }
    return published;
}

/* Editable-config entity discovery (number/text/switch with command topics). */
static int publish_config_discovery(esp_mqtt_client_handle_t client, const char *dev_name, const char *fw) {
    char *topic = s_mem->topic;
    char *payload = s_mem->payload;
    int count = 0, published = 0;
    const cfg_field_t *fields = ha_config_fields(&count);
    for (int i = 0; i < count; i++) {
        ha_config_discovery_topic(topic, sizeof(s_mem->topic), device_id(), &fields[i]);
        int n = ha_config_discovery(payload, sizeof(s_mem->payload), device_id(), dev_name, fw, &fields[i]);
        if (n < (int)sizeof(s_mem->payload))
            published += publish(client, topic, payload, 1);
    }
    return published;
}

/* Editable actions: a signed "Screen adjust (min) today" number (idempotent
   target — negative takes time back, e.g. chores not done) and a "Find my
   timer" switch. Discovery + state ride the magtag/<id>/act topic; commands
   come in on the shared set/+ subscription (screen_bonus, locate). */
#define BONUS_MAX_MIN 240
static int publish_action_discovery(esp_mqtt_client_handle_t client, const char *dev_name, const char *fw) {
    char *topic = s_mem->topic;
    char *payload = s_mem->payload;
    const char *id = device_id();
    /* The device name is user-editable free text (HA "name" entity), so it
       must be JSON-escaped before interpolation. */
    char dn[128];
    ha_config_json_escape(dn, sizeof(dn), dev_name);
    int published = 0, n;
    /* number: Screen adjust (min) today (signed; key stays screen_bonus) */
    mqtt_disc_topic(topic, sizeof(s_mem->topic), "number", id, "screen_bonus");
    n = snprintf(payload, sizeof(s_mem->payload),
                 "{\"name\":\"Screen adjust (min) today\",\"uniq_id\":\"%s_screen_bonus\","
                 "\"stat_t\":\"magtag/%s/act\",\"val_tpl\":\"{{ value_json.screen_bonus }}\","
                 "\"cmd_t\":\"magtag/%s/set/screen_bonus\",\"retain\":true,\"min\":-%d,\"max\":%d,\"step\":5,"
                 "\"mode\":\"box\",\"optimistic\":true,\"unit_of_meas\":\"min\",\"ent_cat\":\"config\","
                 "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\","
                 "\"mf\":\"Adafruit\",\"mdl\":\"MagTag 2.9\",\"sw\":\"%s\"}}",
                 id, id, id, BONUS_MAX_MIN, BONUS_MAX_MIN, id, dn, fw);
    if (n < (int)sizeof(s_mem->payload))
        published += publish(client, topic, payload, 1);
    else
        ESP_LOGW(TAG, "screen_bonus discovery truncated, skipped");
    /* switch: Find my timer. Optimistic like the reload switches (see
       ha_config_discovery — the user chose the two-button assumed-state
       rendering over the snap-back). */
    mqtt_disc_topic(topic, sizeof(s_mem->topic), "switch", id, "locate");
    n = snprintf(payload, sizeof(s_mem->payload),
                 "{\"name\":\"Find my timer\",\"uniq_id\":\"%s_locate\",\"stat_t\":\"magtag/%s/act\","
                 "\"val_tpl\":\"{{ value_json.locate }}\",\"cmd_t\":\"magtag/%s/set/locate\",\"retain\":true,"
                 "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"optimistic\":true,\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\","
                 "\"mf\":\"Adafruit\",\"mdl\":\"MagTag 2.9\",\"sw\":\"%s\"}}",
                 id, id, id, id, dn, fw);
    if (n < (int)sizeof(s_mem->payload))
        published += publish(client, topic, payload, 1);
    else
        ESP_LOGW(TAG, "locate discovery truncated, skipped");
    return published;
}

/* Apply the buffered editable-config sets + actions, republish cfg + act.
   Returns the number of publishes enqueued (all counted for the drain). */
static int apply_sets(esp_mqtt_client_handle_t client, const stats_snapshot_t *snap) {
    char *topic = s_mem->topic;
    char *ack = s_mem->ack;
    int published = 0;
    int n = s_rx.set_count; /* snapshot: the handler may still be appending */
    for (int i = 0; i < n; i++) {
        const char *k = s_mem->sets[i].key, *v = s_mem->sets[i].value;
        if (strcmp(k, "screen_bonus") == 0) {
            long m = strtol(v, NULL, 10);
            if (m < -BONUS_MAX_MIN)
                m = -BONUS_MAX_MIN;
            if (m > BONUS_MAX_MIN)
                m = BONUS_MAX_MIN;
            s_bonus_target_s = (int32_t)m * 60; /* applied post-join (idempotent) */
            s_bonus_target_pending = true;
            ESP_LOGI(TAG, "screen adjust target %ld min (deferred)", m);
        } else if (strcmp(k, "locate") == 0) {
            if (strcmp(v, "ON") == 0) {
                s_locate_pending = true;
                /* clear the retained switch command so it fires once */
                mqtt_topic(topic, sizeof(s_mem->topic), device_id(), "set/locate");
                published += publish(client, topic, "OFF", 1);
            }
        } else {
            ha_cfg_result_t r = ha_config_set(k, v, ack, sizeof(s_mem->ack));
            ESP_LOGI(TAG, "set %s: %s", k, ack);
            /* Clear the retained set command once applied. HA drives the
               control's state from the cfg topic (republished below), not
               from this command topic, so clearing it is invisible to HA
               but stops a stale set from re-overriding the bulk config
               document every window. Leave rejected/unknown values retained
               so the error is visible. */
            if (r == HA_CFG_OK) {
                char suffix[32];
                snprintf(suffix, sizeof(suffix), "set/%s", k);
                mqtt_topic(topic, sizeof(s_mem->topic), device_id(), suffix);
                published += publish(client, topic, "", 1);
            }
        }
    }
    if (s_bonus_clear_pending) {
        /* Rollover: clear the retained bonus target so it doesn't repeat */
        mqtt_topic(topic, sizeof(s_mem->topic), device_id(), "set/screen_bonus");
        published += publish(client, topic, "0", 1);
        s_bonus_clear_pending = false;
    }
    /* act state: confirmed bonus (applied minutes, from the orchestrator's
       snapshot — live timer state is off-limits on this task) + locate off
       (momentary). A target buffered THIS window confirms next window; the
       HA number is optimistic, so it doesn't snap back meanwhile. */
    char act[96];
    mqtt_topic(topic, sizeof(s_mem->topic), device_id(), "act");
    snprintf(act, sizeof(act), "{\"screen_bonus\":%ld,\"locate\":\"OFF\"}", (long)(snap->screen_bonus_applied_s / 60));
    published += publish(client, topic, act, 1);
    /* cfg state: current editable-config values. Skip a truncated doc —
       publishing invalid JSON would knock every editable control offline. */
    mqtt_topic(topic, sizeof(s_mem->topic), device_id(), "cfg");
    if (ha_config_state_json(s_mem->cfg_state, sizeof(s_mem->cfg_state)) >= (int)sizeof(s_mem->cfg_state)) {
        ESP_LOGW(TAG, "cfg state JSON truncated (%d B buffer), not published", (int)sizeof(s_mem->cfg_state));
        return published;
    }
    published += publish(client, topic, s_mem->cfg_state, 1);
    return published;
}

/* Build the incoming-topic strings, point the rx context at the window
   buffers, and subscribe. set/+ is a wildcard: one subscription covers
   every editable field. Subscribing FIRST lets the broker's retained
   delivery overlap the stat publishes (no separate wait). */
static void subscribe_incoming(esp_mqtt_client_handle_t client) {
    char config_topic[96], cmd_topic[96], set_topic[96];
    s_rx = (mqtt_rx_t){
        .config_buf = s_mem->config_buf,
        .config_cap = CONFIG_BUF_MAX,
        .config_topic_len = mqtt_topic(config_topic, sizeof(config_topic), device_id(), "config"),
        .cmd_buf = s_mem->cmd_buf,
        .cmd_cap = CMD_BUF_MAX,
        .cmd_topic_len = mqtt_topic(cmd_topic, sizeof(cmd_topic), device_id(), "cmd"),
        .sets = s_mem->sets,
        .sets_cap = SET_MAX,
        .set_prefix_len = mqtt_topic(set_topic, sizeof(set_topic), device_id(), "set/"),
    };
    if (s_rx.set_prefix_len > 0 && s_rx.set_prefix_len + 1 < (int)sizeof(set_topic)) {
        set_topic[s_rx.set_prefix_len] = '+'; /* single-level wildcard */
        set_topic[s_rx.set_prefix_len + 1] = '\0';
    }
    esp_mqtt_client_subscribe(client, config_topic, 1);
    esp_mqtt_client_subscribe(client, cmd_topic, 1);
    esp_mqtt_client_subscribe(client, set_topic, 1);
}

/* Discovery gate + stat + summary. Returns publishes enqueued;
 *fresh_discovery / *dev_hash feed the post-drain stamp write. */
static int publish_states(esp_mqtt_client_handle_t client, const stats_snapshot_t *snap, bool *fresh_discovery,
                          uint16_t *dev_hash_out) {
    int published = 0;
    /* Discovery: once per schema bump (covers new entities and renames), OR
       when the discovery `dev` block's mutable fields changed. That block
       carries dev.name (editable from HA) and dev.sw (the running firmware
       version), and NEITHER is covered by DISC_SCHEMA_VER — so both are
       folded into one 16-bit fingerprint, cheaply.

       The fw leg is what keeps the HA device card honest across an OTA
       update. With only the name in the hash, a schema bump refreshes the
       version exactly once and every subsequent update leaves the card
       showing the old version permanently — the sw field would go stale
       and stay stale. The slot-name leg does the same job for the
       per-timer stat entities below, whose published names come from the
       HA-editable timerN_name. Both the fingerprint and this gate live in
       ha_config.c, beside the code that writes the block, and are pinned
       there by host tests. */
    char dev_name[64];
    device_name(dev_name, sizeof(dev_name));
    uint16_t dev_hash = ha_config_discovery_hash(dev_name, snap->fw);
    uint16_t disc_ver = 0, disc_dev = 0;
    hal_nvs_read_u16(NVS_KEY_DISC_VER, &disc_ver);
    hal_nvs_read_u16(NVS_KEY_DISC_NAME, &disc_dev);
    *fresh_discovery = ha_config_discovery_stale(disc_ver, disc_dev, DISC_SCHEMA_VER, dev_hash);
    *dev_hash_out = dev_hash;
    if (*fresh_discovery) {
        published += publish_discovery(client, dev_name, snap->fw);
        published += publish_config_discovery(client, dev_name, snap->fw);
        published += publish_action_discovery(client, dev_name, snap->fw);
    }

    /* PUBLISH TIME, and it has to be here rather than in the snapshot.

       `snap` was filled by stats_collect() on the main task and posted by
       value; net_window_task copied it off the queue BEFORE it called
       ota_flow_check(), so any OTA field carried in it would be this
       wake's snapshot of the PREVIOUS wake's result. Reading NVS on this
       line instead picks up the verdict the check wrote two statements
       earlier at net_window.c:107. See ota_flow.h's ota_flow_stat() and
       task 12 of docs/planning/ota.plan.md. */
    ota_stat_t ota;
    ota_flow_stat(&ota);
    /* Same line of reasoning, one file further. The panic breadcrumb has
       to come off NVS — it was latched at BOOT, on the main task, before
       this window existed — and the live heap/stack/NVS readings have to
       be taken HERE, on the network task, mid-window: a snapshot field
       would carry the main task's pre-radio view of both, which is
       exactly the reading that cannot show TLS pressure. See
       panic_diag_stat() and the diag_stat_t comment in stats_json.h. */
    diag_stat_t diag;
    panic_diag_stat(&diag);

    mqtt_topic(s_mem->topic, sizeof(s_mem->topic), device_id(), "stat");
    const int cap = (int)sizeof(s_mem->payload);
    const int stat_n = stats_json_stat(s_mem->payload, sizeof(s_mem->payload), snap, &ota, &diag);
    if (stat_n < cap) {
        published += publish(client, s_mem->topic, s_mem->payload, 1);
    } else {
        /* Headroom here is 86 B at the measured worst case, so this is
           one added field away from firing. Silence would look exactly
           like a healthy device with nothing to report. */
        ESP_LOGW(TAG, "stat payload truncated (%d >= %d), not published", stat_n, cap);
    }

    if (s_summary.pending) {
        mqtt_topic(s_mem->topic, sizeof(s_mem->topic), device_id(), "summary");
        if (stats_json_summary(s_mem->payload, sizeof(s_mem->payload), s_summary.date, s_summary.used_s,
                               s_summary.completions) < (int)sizeof(s_mem->payload)) {
            published += publish(client, s_mem->topic, s_mem->payload, 1);
        }
    }
    return published;
}

/* Wait for the retained config/cmd, then apply: bulk config document,
   command (grant/locate — buffered for the orchestrator), editable sets.
   Returns publishes enqueued (acks, event, retained clears, act + cfg). */
static int apply_incoming(esp_mqtt_client_handle_t client, const stats_snapshot_t *snap) {
    int published = 0;
    /* Retained config + cmd arrive right after subscribe; give them a
       moment past the publish drain to land. */
    int cfg_wait = 0;
    while (!(s_rx.config_done && s_rx.cmd_done) && cfg_wait < RETAINED_RX_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        cfg_wait += 100;
    }

    if (s_rx.config_done) {
        /* config_apply writes NVS; a changed timezone/timer def takes
           effect on the next boot/operation. */
        config_result_t r = config_apply(s_mem->config_buf, s_mem->ack, sizeof(s_mem->ack));
        if (r != CONFIG_SKIPPED) {
            mqtt_topic(s_mem->topic, sizeof(s_mem->topic), device_id(), "config_ack");
            published += publish(client, s_mem->topic, s_mem->ack, 1); /* retained ack */
            ESP_LOGI(TAG, "config applied (result %d)", (int)r);
        }
    }

    if (s_rx.cmd_done) {
        cmd_action_t act;
        cmd_result_t cr = cmd_apply(s_mem->cmd_buf, &act, s_mem->ack, sizeof(s_mem->ack));
        if (cr == CMD_GRANT || cr == CMD_LOCATE) {
            if (cr == CMD_GRANT) {
                /* Buffered: the orchestrator applies it after joining this
                   task; persisted at the next enter_deep_sleep. */
                s_grant_slot = act.slot;
                s_grant_sec = act.sec;
            } else {
                s_locate_pending = true; /* main.c runs the alarm after the window */
            }
            mqtt_topic(s_mem->topic, sizeof(s_mem->topic), device_id(), "event");
            published += publish(client, s_mem->topic, s_mem->ack, 0); /* event ack, not retained */
            /* Clear the retained command so it isn't re-delivered/re-applied */
            mqtt_topic(s_mem->topic, sizeof(s_mem->topic), device_id(), "cmd");
            published += publish(client, s_mem->topic, "", 1);
            ESP_LOGI(TAG, "command applied (result %d)", (int)cr);
        }
    }

    /* Apply any editable-config edits and (always) republish current values
       so HA's number/text controls reflect the confirmed state. */
    published += apply_sets(client, snap);
    return published;
}

void mqtt_ha_window(const stats_snapshot_t *snap) {
    /* Zero-initialised because a value longer than these buffers makes
       nvs_get_str return ESP_ERR_NVS_INVALID_LENGTH and leave them
       UNTOUCHED, and neither call below checks the return. Uninitialised
       stack would reach esp_mqtt_client_config_t.broker.address.uri. */
    char uri[128] = {0}, user[64] = {0}, pass[64] = {0};
    nvs_config_get_mqtt_uri(uri, sizeof(uri));
    if (uri[0] == '\0') {
        /* Was silent — the #1 reason "nothing shows up in HA": the broker
           URI was never configured (NVS_DEFAULT_MQTT_URI / MAGTAG_MQTT_URI
           empty). Make it loud. */
        ESP_LOGW(TAG, "MQTT disabled: no broker URI in NVS (set NVS_DEFAULT_MQTT_URI or MAGTAG_MQTT_URI)");
        return;
    }
    nvs_config_get_mqtt_user(user, sizeof(user));
    nvs_config_get_mqtt_pass(pass, sizeof(pass));
    ESP_LOGI(TAG, "MQTT window: connecting to %s", uri);

    s_mem = calloc(1, sizeof(*s_mem));
    if (s_mem == NULL) {
        ESP_LOGE(TAG, "window buffer alloc failed (%u B)", (unsigned)sizeof(*s_mem));
        return;
    }
    s_eg = xEventGroupCreate();
    if (s_eg == NULL)
        goto out_mem;
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

    subscribe_incoming(client);

    bool fresh_discovery = false;
    uint16_t dev_hash = 0;
    int published = publish_states(client, snap, &fresh_discovery, &dev_hash);

    /* Drain: wait for the QoS-1 acks so the disconnect doesn't drop them */
    if (drain_acks(published, PUBLISH_DRAIN_TIMEOUT_MS)) {
        s_summary.pending = false;
        if (fresh_discovery) {
            hal_nvs_write_u16(NVS_KEY_DISC_VER, DISC_SCHEMA_VER);
            hal_nvs_write_u16(NVS_KEY_DISC_NAME, dev_hash);
        }
        ESP_LOGI(TAG, "published %d messages", published);
    } else {
        ESP_LOGW(TAG, "publish drain incomplete (%d/%d)", s_pub_acks, published);
    }

    published += apply_incoming(client, snap);

    /* Final drain: the acks, event, retained clears, and act/cfg states
       must survive the disconnect too (was a blind 400 ms delay — QoS-1
       messages could drop on a slow broker). */
    if (!drain_acks(published, PUBLISH_DRAIN_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "final drain incomplete (%d/%d)", s_pub_acks, published);
    }

out_started:
    esp_mqtt_client_stop(client);
out_client:
    esp_mqtt_client_destroy(client);
out_eg:
    vEventGroupDelete(s_eg);
    s_eg = NULL;
out_mem:
    free(s_mem);
    s_mem = NULL;
}
