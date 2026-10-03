#include "mqtt_ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chores.h"
#include "cmd_apply.h"
#include "config_apply.h"
#include "device_id.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "ha_config.h"
#include "ha_day_cmds.h"
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

/* CONFIG_BUF_MAX — the largest retained config document the window will
   take — is defined in config_apply.h. Its sizing is not restated here:
   test_config_apply's test_the_worst_case_document_fits_the_receive_buffer
   builds the compact worst-case document (every parsed field at its
   longest honoured value, 46 holiday dates included), asserts it fits
   under the `total_len < config_cap` gate in mqtt_rx.c, and prints the
   headroom. A hand-written figure here went stale the moment the chore
   fields were added; the test cannot.

   A document past the cap is REFUSED OUT LOUD (config_ack "too_long")
   rather than dropped into the same MQTT_RX_IGNORED bucket as a topic that
   was never ours — a retained over-size document used to be re-delivered
   and re-dropped on every reconnect, forever, with no log and no ack. */
#define CMD_BUF_MAX 256

/* MQTT packet bytes the receive buffer must hold ON TOP of the document:
   1 control byte + up to 4 remaining-length bytes + the 2-byte topic-length
   field + the topic + the 2-byte QoS-1 packet identifier. Our topics are
   built into 96-byte slots, so 128 covers the worst of it.

   This is not decoration — see the buffer.size note at the client config.
   esp-mqtt only fills in `topic` on the FIRST data event of a fragmented
   message unless CONFIG_MQTT_TOPIC_PRESENT_ALL_DATA_EVENTS is set, and it
   is not set (it needs MQTT_USE_CUSTOM_CONFIG, also off). Every later
   fragment therefore arrives with topic == NULL, which mqtt_rx correctly
   ignores — so a document that fragments can never be reassembled here. */
#define MQTT_PACKET_OVERHEAD 128

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
   every wake, so these ~12.7 KB (sizeof(window_mem_t), which SET_MAX
   dominates) no longer sit in .bss permanently. The pointer doubles as
   the "window open" flag for the event handler.

   The two incoming topic strings live here, rather than on
   subscribe_incoming's stack as they used to, because mqtt_rx now matches
   the delivered topic against them BY CONTENT and so needs them for the
   whole window — the same lifetime config_buf already has. */
typedef struct {
    char topic[128];
    char payload[STATS_JSON_PAYLOAD_MAX]; /* stat/summary/discovery payloads */
    char ack[256];
    char cfg_state[HA_CONFIG_STATE_MAX];
    char config_buf[CONFIG_BUF_MAX]; /* retained config document (HA→device) */
    char cmd_buf[CMD_BUF_MAX];       /* retained command document */
    char config_topic[96];           /* subscribed topic, matched by content */
    char cmd_topic[96];
    mqtt_set_kv_t sets[SET_MAX]; /* editable-config sets (set/<key>) */
} window_mem_t;

/* Per-window heap budget. CFG_STR_MAX multiplies through the sets array
   (x SET_MAX), so the ceiling silently controls this figure: at 128 the
   struct is ~12.7 KB, at 512 it would be ~30 KB with nothing else failing.
   Allocation failure here disables the whole MQTT window, so the growth
   has to be a deliberate edit rather than a side effect.

   RAISED 12288 -> 13312, deliberately, and here is the arithmetic it is
   meant to force someone to write down. config_buf went 1024 -> 2048 and
   cfg_state 1280 -> 1536 (both measured — see their own definitions), and
   the two 96-byte topic strings moved in from the stack: 128 + 1024 + 256
   + 1536 + 2048 + 256 + 96 + 96 + 48*152 = 12736 B. 13312 leaves 576 B,
   which is room for a field or two without re-arguing the budget and not
   enough to absorb another kilobyte-scale buffer unnoticed.

   SPENT 256 of that 576 on the chore leg (M3-T1): payload went 1024 ->
   1280 (STATS_JSON_PAYLOAD_MAX says why), so the sum is now 128 + 1280 +
   256 + 1536 + 2048 + 256 + 96 + 96 + 48*152 = 12992 B and 320 B are
   left. The budget itself did not move.

   THE ASSERT CANNOT SEE THE WHOLE COST, so the rest is written down here.
   The same change also grows esp-mqtt's own receive buffer: .buffer.size
   goes from the 1024 B library default to CONFIG_BUF_MAX +
   MQTT_PACKET_OVERHEAD = 2176 B, heap_caps_malloc'd inside
   esp_mqtt_client_init() — which runs AFTER the calloc below and is freed
   only at client destroy, so the two are live together for the whole
   window and their costs add. (The 1024 B transmit buffer does not move:
   .buffer.out_size is pinned, see the note at the client config.) Peak
   heap delta for this change is therefore 1472 + 1152 = 2624 B, not the
   1472 B the arithmetic above accounts for.

   Why 2.6 KB more heap is safe to spend here, rather than merely small:
   both allocations are freed at window teardown, and the same wake already
   runs ota_flow_check() BEFORE mqtt_ha_window() (net_window.c), a TLS
   handshake whose gate refuses to start below CONFIG_MAGTAG_OTA_MIN_FREE_HEAP
   = 40960 B with a structural floor of 32768 B. The two never overlap —
   the download runs in a later window, after MQTT has closed — so this
   window is being measured against headroom the device is already required
   to have an order of magnitude more of. And the failure mode is loud
   either way: the calloc below is checked and logs its size, and a failed
   in_buffer malloc makes esp_mqtt_client_init return NULL, which is
   checked too. */
_Static_assert(sizeof(window_mem_t) <= 13312, "window_mem_t outgrew its per-window heap budget");
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

/* Pending daily summary (captured at rollover, published next window).
   Plain RAM, which deep sleep does not keep: pending is cleared only
   once a window's publishes are acked (below), so the summary is lost
   after a crash AND whenever the device sleeps before any window got it
   out, e.g. when the rollover's own window fails (no Wi-Fi, no broker):
   the next wake starts with pending false.
   That day is then missing from HA, and a bar graph shows it as a zero
   day. Tracked as M4-D1 (a dated pending summary kept across sleep). */
static struct {
    bool pending;
    char date[11];
    int32_t used_s;
    uint16_t completions[TIMER_EXTRA_SLOTS];
    uint8_t chores_done; /* the day's acked chores, of ... */
    int chores;          /* ... this many configured; STATS_JSON_CHORES_UNKNOWN = unread list */
} s_summary;

void mqtt_ha_queue_summary(const char *date, int32_t screen_used_s, const uint16_t completions[TIMER_EXTRA_SLOTS],
                           uint8_t chores_done, int chores) {
    snprintf(s_summary.date, sizeof(s_summary.date), "%s", date);
    s_summary.used_s = screen_used_s;
    memcpy(s_summary.completions, completions, sizeof(s_summary.completions));
    s_summary.chores_done = chores_done;
    s_summary.chores = chores;
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
            /* NO `default:` HERE, deliberately. Every enumerator is
               listed, so the next result added to mqtt_rx_result_t FAILS
               THE BUILD (-Werror=switch; verified by adding a sentinel
               enumerator and watching this line refuse to compile) rather
               than becoming a value this handler drops on the floor. The
               error names the file, the line and the enumerator, so the
               person adding the result is told where to decide what it
               means. A `default: break;` sat here until now, and it is a
               fair description of how MQTT_RX_IGNORED came to mean both
               "not my topic" and "my topic, document too big": the second
               needed a case, and a catch-all meant nobody had to write
               one. */
            switch (mqtt_rx_on_data(&s_rx, ev->topic, ev->topic_len, ev->data, ev->data_len, ev->total_data_len,
                                    ev->current_data_offset)) {
                case MQTT_RX_SETS_FULL:
                    ESP_LOGW(TAG, "set buffer full (%d), edit dropped", SET_MAX);
                    break;
                case MQTT_RX_SET_TOO_LONG:
                    ESP_LOGW(TAG, "set key/value too long, edit dropped");
                    break;
                case MQTT_RX_CONFIG_TOO_LONG:
                    /* Logged here, ACKED from apply_incoming. This runs on
                       the mqtt client task; every publish in this file is
                       counted by drain_acks, which the window task owns. */
                    ESP_LOGW(TAG, "config document too long (%d B, max %d), refused", ev->total_data_len,
                             CONFIG_BUF_MAX - 1);
                    break;
                case MQTT_RX_CMD_TOO_LONG:
                    ESP_LOGW(TAG, "command document too long (%d B, max %d), refused", ev->total_data_len,
                             CMD_BUF_MAX - 1);
                    break;
                case MQTT_RX_BAD_CHUNK:
                    /* Unreachable with correct framing — mqtt_rx checks it
                       so the copies are safe by inspection, not by trust.
                       If this ever prints, the broker or the client library
                       is the story, not the config. */
                    ESP_LOGW(TAG, "malformed chunk (offset %d, %d B, total %d), dropped", ev->current_data_offset,
                             ev->data_len, ev->total_data_len);
                    break;
                case MQTT_RX_OK:
                case MQTT_RX_IGNORED:
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

/* `chores` is the list ha_config_discovery_gate() read and fingerprinted
   for this window — not a second read here, so the pass publishes exactly
   what the stamp will certify. A REJECTED blob is authoritative (n = 0:
   the device runs on 0 chores, so every chore row retires — the inert C1
   reading). A FAILED READ is not: n = -1 skips every chore row, leaving
   the owner's entities as they are, and the gate has already withheld
   the stamp for it, so the next window runs the whole pass again. */
static int publish_discovery(esp_mqtt_client_handle_t client, const char *dev_name, const char *fw,
                             const ha_disc_chores_t *chores) {
    char *topic = s_mem->topic;
    char *payload = s_mem->payload;
    int count = 0, published = 0;
    const ha_entity_t *ents = stats_json_entities(&count);
    for (int i = 0; i < count; i++) {
        const char *name_override = NULL;
        char named[48];
        _Static_assert(sizeof(named) >= STATS_JSON_CHORE_ENTITY_NAME_BUF, "a chore_N display name would truncate");
        /* Per-chore binary sensors: stats_json_chore_discovery() decides
           (and the host suite pins) name, retire or skip; this only
           carries it out. */
        switch (stats_json_chore_discovery(&ents[i], chores->names, chores->n, named, sizeof(named))) {
            case STATS_CHORE_DISC_SKIP:
                continue;
            case STATS_CHORE_DISC_RETIRE:
                stats_json_discovery_topic(topic, sizeof(s_mem->topic), device_id(), &ents[i]);
                published += publish(client, topic, "", 1);
                continue;
            case STATS_CHORE_DISC_PUBLISH:
                name_override = named;
                break;
            case STATS_CHORE_DISC_NOT_CHORE:
                break;
        }
        /* Per-slot sensors (completions_N / day_runs_N / remaining_N /
           limit_N) carry the configured timer's name; disabled slots get
           no entity. stats_json_slot_of() decides which rows (the host
           suite pins it); this carries it out. */
        const char *suffix = NULL;
        const int slot = stats_json_slot_of(&ents[i], &suffix);
        if (slot > 0) {
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
        if (n < (int)sizeof(s_mem->payload)) {
            published += publish(client, topic, payload, 1);
        } else {
            /* The other three producers all say this out loud; this one
               did not, and these are the editable CONTROLS — a number or
               switch that silently never appears in HA reads as a feature
               that was never shipped. The select fields are the widest
               payloads on the device (their options list inline), so this
               is the arm most likely to fire. */
            ESP_LOGW(TAG, "%s config discovery truncated, skipped", fields[i].key);
        }
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
    /* These two payloads are hand-written rather than table-driven, so
       nothing walks them in a host test: the def_ent_id below is held only
       by this comment and by
       test_discovery_default_entity_id_is_the_component_and_unique_id in
       test_stats_json, which pins the RULE the two must follow. Any
       discovery payload added here must carry def_ent_id built as
       "<component>.<uniq_id>", with the component literal matching the one
       handed to mqtt_disc_topic() two lines up — HA keeps only what
       follows the first dot, so the dot is load-bearing and a dotless
       value would register an EMPTY object id. */
    /* number: Screen adjust (min) today (signed; key stays screen_bonus) */
    mqtt_disc_topic(topic, sizeof(s_mem->topic), "number", id, "screen_bonus");
    n = snprintf(payload, sizeof(s_mem->payload),
                 "{\"name\":\"Screen adjust (min) today\",\"uniq_id\":\"%s_screen_bonus\","
                 "\"def_ent_id\":\"number.%s_screen_bonus\","
                 "\"stat_t\":\"magtag/%s/act\",\"val_tpl\":\"{{ value_json.screen_bonus }}\","
                 "\"cmd_t\":\"magtag/%s/set/screen_bonus\",\"retain\":true,\"min\":-%d,\"max\":%d,\"step\":5,"
                 "\"mode\":\"box\",\"optimistic\":true,\"unit_of_meas\":\"min\",\"ent_cat\":\"config\","
                 "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\","
                 "\"mf\":\"Adafruit\",\"mdl\":\"MagTag 2.9\",\"sw\":\"%s\"}}",
                 id, id, id, id, BONUS_MAX_MIN, BONUS_MAX_MIN, id, dn, fw);
    if (n < (int)sizeof(s_mem->payload))
        published += publish(client, topic, payload, 1);
    else
        ESP_LOGW(TAG, "screen_bonus discovery truncated, skipped");
    /* switch: Find my timer. Optimistic like the reload switches (see
       ha_config_discovery — the user chose the two-button assumed-state
       rendering over the snap-back). */
    mqtt_disc_topic(topic, sizeof(s_mem->topic), "switch", id, "locate");
    n = snprintf(payload, sizeof(s_mem->payload),
                 "{\"name\":\"Find my timer\",\"uniq_id\":\"%s_locate\","
                 "\"def_ent_id\":\"switch.%s_locate\","
                 "\"stat_t\":\"magtag/%s/act\","
                 "\"val_tpl\":\"{{ value_json.locate }}\",\"cmd_t\":\"magtag/%s/set/locate\",\"retain\":true,"
                 "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"optimistic\":true,\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\","
                 "\"mf\":\"Adafruit\",\"mdl\":\"MagTag 2.9\",\"sw\":\"%s\"}}",
                 id, id, id, id, id, dn, fw);
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
    /* A target this window HOLDS rather than buffers (BUG-14): reported in
       the act state below, so HA's box keeps the parent's value, but never
       handed to the orchestrator. */
    bool held_target = false;
    int32_t held_target_s = 0;
    int n = s_rx.set_count; /* snapshot: the handler may still be appending */
    for (int i = 0; i < n; i++) {
        const char *k = s_mem->sets[i].key, *v = s_mem->sets[i].value;
        if (strcmp(k, "screen_bonus") == 0) {
            long m = strtol(v, NULL, 10);
            if (m < -BONUS_MAX_MIN)
                m = -BONUS_MAX_MIN;
            if (m > BONUS_MAX_MIN)
                m = BONUS_MAX_MIN;
            /* Day-scoped (BUG-14): the rules are ha_day_cmds.h's. */
            switch (ha_day_bonus_fate(snap->no_clock, s_bonus_clear_pending)) {
                case HA_BONUS_HOLD:
                    /* The device never clears this topic except by the
                       day's clear below, which a no_clock window drops as
                       well, so doing nothing IS leaving it retained: the
                       first window with a day applies it. */
                    held_target = true;
                    held_target_s = (int32_t)m * 60;
                    ESP_LOGI(TAG, "screen adjust target %ld min held: no clock", m);
                    break;
                case HA_BONUS_DROP:
                    /* On a midnight rollover this changes nothing: that
                       day's reset follows the window and wipes the bonus
                       anyway. */
                    ESP_LOGI(TAG, "screen adjust target %ld min dropped: day cleared", m);
                    break;
                case HA_BONUS_BUFFER:
                default:
                    s_bonus_target_s = (int32_t)m * 60; /* applied post-join (idempotent) */
                    s_bonus_target_pending = true;
                    ESP_LOGI(TAG, "screen adjust target %ld min (deferred)", m);
                    break;
            }
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
    /* A clear is day-scoped too (BUG-14): under no_clock it is dropped,
       not deferred (ha_day_cmds.h). The pending flag is consumed either
       way. */
    const bool day_cleared = ha_day_publish_clear(snap->no_clock, s_bonus_clear_pending);
    if (s_bonus_clear_pending && !day_cleared) {
        ESP_LOGI(TAG, "bonus clear dropped: no clock");
    }
    /* Consumed AFTER the set loop, never above it: ha_day_bonus_fate reads
       it there, and cleared early it would BUFFER the cleared day's target
       onto the fresh one. No host suite runs this order (test_ha_day_cmds
       replays it by hand); keep the two in step. */
    s_bonus_clear_pending = false;
    if (day_cleared) {
        /* Rollover: clear the retained bonus target so it doesn't repeat */
        mqtt_topic(topic, sizeof(s_mem->topic), device_id(), "set/screen_bonus");
        published += publish(client, topic, "0", 1);
    }
    /* act state: the Screen-adjust value HA renders + locate off
       (momentary). A target buffered THIS window is reported NOW rather
       than a window later, because this payload is what HA's number box
       shows and `optimistic` does not stop a state message overwriting
       it — publishing the snapshot's (pre-reconcile) applied figure here
       snapped a fresh -45 back to 0 on the very sync that applied it. The
       orchestrator applies the target immediately after joining this
       task, and timer_bonus_reconcile is idempotent, so a window that
       dies before the join simply re-buffers the retained set next time.
       Live timer state stays off-limits on this task; the applied figure
       still comes from the snapshot. */
    const act_state_t act_in = ha_day_act_state(snap->screen_bonus_applied_s, s_bonus_target_pending, s_bonus_target_s,
                                                held_target, held_target_s, day_cleared);
    char act[96];
    mqtt_topic(topic, sizeof(s_mem->topic), device_id(), "act");
    if (stats_json_act(act, sizeof(act), &act_in) < (int)sizeof(act))
        published += publish(client, topic, act, 1);
    else
        ESP_LOGW(TAG, "act payload truncated, not published");
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
    char set_topic[96];
    /* The two document topics are stored, not just measured. The routing
       used to keep only strlen and match `topic_len ==`, which made any
       same-length topic the broker delivered — a sibling device's
       magtag/<other-id>/config, say — indistinguishable from our own. That
       was survivable while the worst outcome was a silent drop; it is not
       now that an over-size document publishes a RETAINED config_ack, which
       would otherwise accuse an operator of a document they never sent.

       Storing the string also closes a quieter hole: mqtt_topic returns the
       would-be length (snprintf semantics), so a device id long enough to
       truncate used to leave config_topic_len LARGER than the topic
       actually subscribed to, and the length match could then never fire.
       Subscribing to and matching the same bytes cannot drift that way. */
    mqtt_topic(s_mem->config_topic, sizeof(s_mem->config_topic), device_id(), "config");
    mqtt_topic(s_mem->cmd_topic, sizeof(s_mem->cmd_topic), device_id(), "cmd");
    s_rx = (mqtt_rx_t){
        .config_buf = s_mem->config_buf,
        .config_cap = CONFIG_BUF_MAX,
        .config_topic = s_mem->config_topic,
        .cmd_buf = s_mem->cmd_buf,
        .cmd_cap = CMD_BUF_MAX,
        .cmd_topic = s_mem->cmd_topic,
        .sets = s_mem->sets,
        .sets_cap = SET_MAX,
        .set_prefix_len = mqtt_topic(set_topic, sizeof(set_topic), device_id(), "set/"),
    };
    if (s_rx.set_prefix_len > 0 && s_rx.set_prefix_len + 1 < (int)sizeof(set_topic)) {
        set_topic[s_rx.set_prefix_len] = '+'; /* single-level wildcard */
        set_topic[s_rx.set_prefix_len + 1] = '\0';
    }
    esp_mqtt_client_subscribe(client, s_mem->config_topic, 1);
    esp_mqtt_client_subscribe(client, s_mem->cmd_topic, 1);
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
       HA-editable timerN_name, and the chore leg for the chore_N binary
       sensors, whose names and existence come from the chore list. Both
       the fingerprint and this gate live in ha_config.c, beside the code
       that writes the block, and are pinned there by host tests.

       ha_config_discovery_gate() reads the chore list ONCE into `chores`,
       fingerprints exactly that, and decides both whether to run the
       passes and whether the result may be stamped (not when the list
       could not be read). `chores` goes to publish_discovery() unchanged,
       and *fresh_discovery — all the post-drain stamp write consults — is
       the gate's `stamp` verbatim. */
    ha_disc_chores_t chores;
    char dev_name[64];
    device_name(dev_name, sizeof(dev_name));
    uint16_t disc_ver = 0, disc_dev = 0;
    hal_nvs_read_u16(NVS_KEY_DISC_VER, &disc_ver);
    hal_nvs_read_u16(NVS_KEY_DISC_NAME, &disc_dev);
    const ha_disc_verdict_t disc =
        ha_config_discovery_gate(dev_name, snap->fw, disc_ver, disc_dev, DISC_SCHEMA_VER, &chores);
    *fresh_discovery = disc.stamp;
    *dev_hash_out = disc.hash;
    if (disc.stale) {
        published += publish_discovery(client, dev_name, snap->fw, &chores);
        published += publish_config_discovery(client, dev_name, snap->fw);
        published += publish_action_discovery(client, dev_name, snap->fw);
    }

    /* PUBLISH TIME, and it has to be here rather than in the snapshot.

       `snap` was filled by stats_collect() on the main task and posted by
       value; net_window_task copied it off the queue BEFORE it called
       ota_flow_check(), so any OTA field carried in it would be this
       wake's snapshot of the PREVIOUS wake's result. Reading NVS on this
       line instead picks up the verdict the check wrote two statements
       earlier — net_window_task's ota_flow_check() call, immediately
       above the mqtt_ha_window() that reaches here. (Named rather than
       numbered: the ":107" that stood here pointed into that file's own
       comment block.) See ota_flow.h's ota_flow_stat() and
       task 12 of docs/planning/implemented/ota.plan.md. */
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
        /* test_stat_payload_worst_case_fits_the_publish_buffer holds the
           worst case under the cap and reports its headroom; this branch
           is for the day that test is out of date. Silence would look
           exactly like a healthy device with nothing to report. */
        ESP_LOGW(TAG, "stat payload truncated (%d >= %d), not published", stat_n, cap);
    }

    if (s_summary.pending) {
        mqtt_topic(s_mem->topic, sizeof(s_mem->topic), device_id(), "summary");
        if (stats_json_summary(s_mem->payload, sizeof(s_mem->payload), s_summary.date, s_summary.used_s,
                               s_summary.completions, s_summary.chores_done,
                               s_summary.chores) < (int)sizeof(s_mem->payload)) {
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
    /* "Settled" per topic, not "done": a refusal is as final an answer as a
       complete document, and waiting out the rest of the timeout for a
       document the receiver has already refused is pure latency on a
       battery device. Per topic rather than one blanket exit, because a
       config refusal says nothing about a cmd that is still inbound —
       leaving early on it would drop a grant.

       Unchanged, and worth saying plainly: a topic with NO retained message
       at all never settles, so a device whose broker holds neither document
       still pays the full RETAINED_RX_TIMEOUT_MS. That is the existing
       behaviour and this loop is not where it gets fixed. */
    while (!((s_rx.config_done || s_rx.config_too_long) && (s_rx.cmd_done || s_rx.cmd_too_long)) &&
           cfg_wait < RETAINED_RX_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        cfg_wait += 100;
    }

    /* The refusals publish from HERE, not from the event handler that
       detected them: the handler runs on the mqtt client task, and every
       publish in this file is counted by drain_acks, which this task owns.
       An ack enqueued off-task would be waited for by nobody and could be
       cut off by the disconnect at window teardown.

       Config refusal goes to config_ack RETAINED, the same slot a normal
       config result uses, so the answer outlives this window and is there
       when the operator next looks. WHAT CLEARS IT is worth stating
       exactly, because nothing here does: the retained ack is overwritten
       only by the NEXT ack published to that topic, and the config block
       below publishes one only when config_apply returns something other
       than CONFIG_SKIPPED. config_apply skips whenever the document's
       `ver` matches the stored cfg_ver (config_apply.c), so an operator
       who reverts an over-size document back to the exact content the
       device already applied gets no ack at all, and the refusal stands
       against a document that is now fine. Bumping `ver` is what clears
       it — docs/home_assistant/troubleshooting.md ("A document did not
       take") tells the operator to, and that is the whole mechanism.
       Deliberate: the alternative is publishing an ack
       for every skipped document on every wake.

       Command refusal goes to event UNRETAINED, the same slot a normal
       command result uses — a stale "your command was too long" pinned on
       the broker would be worse than none.

       Placed BEFORE the apply blocks on purpose. Both can fire in one
       window (a good retained document lands, then an over-size one is
       published live), and config_ack is a retained topic where the last
       publish wins: this order lets the applied result overwrite the
       refusal, rather than a refusal of the second document burying the
       outcome of the first. */
    if (s_rx.config_too_long) {
        config_ack_too_long(s_mem->ack, sizeof(s_mem->ack), s_rx.config_too_long_len, CONFIG_BUF_MAX - 1);
        mqtt_topic(s_mem->topic, sizeof(s_mem->topic), device_id(), "config_ack");
        published += publish(client, s_mem->topic, s_mem->ack, 1);
    }
    if (s_rx.cmd_too_long) {
        config_ack_too_long(s_mem->ack, sizeof(s_mem->ack), s_rx.cmd_too_long_len, CMD_BUF_MAX - 1);
        mqtt_topic(s_mem->topic, sizeof(s_mem->topic), device_id(), "event");
        published += publish(client, s_mem->topic, s_mem->ack, 0);
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
        /* A grant is day-scoped: behind the no-clock lock it is HELD —
           not buffered, not acked, not cleared, its id not recorded — so
           it stays retained for the first window with a settled day (BUG-14,
           owner decision Q1). A locate still runs. */
        cmd_result_t cr = cmd_apply_for_snapshot(s_mem->cmd_buf, &act, s_mem->ack, sizeof(s_mem->ack), snap);
        if (cr == CMD_HELD) {
            ESP_LOGI(TAG, "grant held: no clock, left retained");
        }
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
           URI was never configured. Make it loud. */
        ESP_LOGW(TAG, "MQTT disabled: no broker configured; enter it in setup mode");
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

    /* buffer.size is SET, not defaulted, and this is load-bearing.
       esp-mqtt's default receive buffer is 1024 B (mqtt_config.h
       MQTT_BUFFER_SIZE_BYTE; CONFIG_MQTT_BUFFER_SIZE needs
       MQTT_USE_CUSTOM_CONFIG, which is off), and a PUBLISH whose whole
       packet exceeds it is delivered in fragments. Only the FIRST fragment
       carries `topic` — the rest arrive with topic == NULL and topic_len 0,
       because filling them in needs CONFIG_MQTT_TOPIC_PRESENT_ALL_DATA_EVENTS
       and that is off too (deliver_publish() in mqtt_client.c). mqtt_rx
       ignores a fragment it cannot route, which is correct and which also
       means a fragmented document can never be reassembled here.

       So a receive buffer smaller than the document ceiling does not
       truncate loudly: it drops the tail in silence, config_done is never
       set, config_too_long is never set either (the total fits the buffer
       it never reached), and the retained document is re-dropped on every
       reconnect. That is precisely the silent paralysis this whole change
       exists to remove, and raising CONFIG_BUF_MAX alone would have moved
       it one buffer along rather than fixing it.

       Sized so every ACCEPTED document arrives in one event. A document
       past the ceiling still fragments, but fragment zero carries both the
       topic and the true total_data_len, so it is refused out loud.

       out_size is pinned at the old default instead of being left to follow
       buffer.size, which would silently double this allocation. Outgoing
       messages do not need it: esp_mqtt_client_publish fragments a payload
       larger than the buffer itself and sends the remainder from the
       caller's memory, which is how the 1164 B worst-case cfg_state already
       goes out through a 1024 B buffer. */
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = (user[0] != '\0') ? user : NULL,
        .credentials.authentication.password = (pass[0] != '\0') ? pass : NULL,
        .buffer.size = CONFIG_BUF_MAX + MQTT_PACKET_OVERHEAD,
        .buffer.out_size = 1024,
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
