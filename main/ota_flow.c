/* OTA sequencing — the order an update happens in. What each step means
   is ota_policy.c's (pure, host-tested); how each step is performed is
   ota.c's (the transport). This file owns only the sequence, because the
   sequence is where the hazards are: a paint inside an open radio window
   browns out the rail, a failsafe extended after the download has started
   races the thing it protects against, and a commit that runs after a
   partial transfer points the boot partition at half an image.

   Every device effect is injected (ota_flow_ops_t), so test_ota_flow
   asserts the whole order on the host as a string. */
#include "ota_flow.h"

#include <stdio.h>
#include <string.h>

#include "config_validate.h" /* CFG_BOUND_OTA_* — the reader buffer widths */
#include "device_id.h"
#include "nvs_config.h"

#ifndef NATIVE
#include "esp_log.h"
#else
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#endif

static const char *TAG = "ota_flow";

/* Static rather than stack or heap. The check runs on the net_win task,
   whose 10 KB stack is already carrying an mbedtls session, and a 2 KB
   heap allocation taken DURING that session is exactly the pressure the
   low_heap gate exists to avoid. 2 KB holds a default entry plus roughly
   forty targeted devices; a manifest longer than that is truncated, which
   cJSON then rejects as bad_manifest — a loud, reported failure rather
   than a silently half-read targeting table. */
#define OTA_MANIFEST_MAX 2048
static char s_manifest[OTA_MANIFEST_MAX];

/* How much longer than the download's own budget the awake failsafe is
   armed for. It is not slack: it is the abort tail — dl_abort discards
   the partial image, then session_end brings the radio down and waits up
   to a second for the stack to answer. The failsafe firing inside that
   tail is the same outcome as it firing mid-transfer. */
#define OTA_ABORT_TAIL_MS 5000

static ota_flow_ops_t s_ops;
static ota_flow_cfg_t s_cfg;

/* Per-wake, set by ota_flow_arm on the main task and read by
   ota_flow_check on the network task. The handover is safe for the same
   reason every other network→device effect in this tree is: the window
   task is spawned after arming and joined before anything reads back. */
static bool s_armed;
static int s_batt_pct;
static bool s_charge_locked;

/* Buffered by the check, consumed by the apply — the same buffer-then-
   apply split net_apply.c uses for grants and bonuses, and for the same
   reason: the network task decides nothing and touches no device state. */
static bool s_pending;
static char s_target[OTA_VERSION_MAX];
static char s_image_url[OTA_URL_MAX];

static int64_t s_dl_started_ms;
static bool s_dl_running;
static uint32_t s_dl_ms;

/* ---- shared helpers ----------------------------------------------------- */

/* ota_result is the ONLY channel a download failure has. The second
   window runs after MQTT has closed, so the failure cannot publish
   itself; it has to survive in NVS until the next window's stat payload
   picks it up. Which reasons may be written is ota_policy's rule rather
   than a convention here, precisely so that the "leave an earlier
   failure alone" cases cannot drift — see
   ota_policy_reason_is_persistable. */
static void record(ota_reason_t reason, int http_status) {
    if (!ota_policy_reason_is_persistable(reason))
        return;
    char text[OTA_REASON_TEXT_MAX];
    ota_policy_reason_text(reason, http_status, text, sizeof(text));
    esp_err_t ret = nvs_config_set_ota_result(text);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ota_result write failed (%d): this outcome will not reach HA", (int)ret);
    }
}

/* hal_nvs_read_str writes NOTHING into a buffer it judges too small, so a
   failed read leaves the caller holding uninitialised bytes. For
   ota_target that is not cosmetic: garbage never matches the manifest's
   version, so the retry budget would never converge and the device would
   retry a doomed build forever. Both readers below therefore substitute
   "" explicitly rather than trusting the buffer. */
static void read_counted_target(char *buf, size_t len) {
    if (nvs_config_get_ota_target(buf, len) != ESP_OK) {
        ESP_LOGW(TAG, "ota_target unreadable: treating the retry count as unarmed");
        buf[0] = '\0';
    }
}

static void fill_gate(ota_gate_in_t *g, bool url_set, bool time_valid, int batt_pct, bool charge_locked) {
    *g = (ota_gate_in_t){
        .url_set = url_set,
        .time_valid = time_valid,
        .charge_locked = charge_locked,
        .batt_pct = batt_pct,
        .min_batt_pct = s_cfg.min_batt_pct,
        .free_heap = s_ops.free_heap(),
        .min_free_heap = s_cfg.min_free_heap,
    };
}

/* ---- init and arming ---------------------------------------------------- */

void ota_flow_init(const ota_flow_ops_t *ops, const ota_flow_cfg_t *cfg) {
    s_ops = *ops;
    s_cfg = *cfg;
    s_armed = false;
    s_pending = false;
    s_target[0] = '\0';
    s_image_url[0] = '\0';
    s_dl_started_ms = 0;
    s_dl_running = false;
    s_dl_ms = 0;
}

void ota_flow_arm(ota_trigger_t trigger, int batt_pct, bool charge_locked) {
    s_batt_pct = batt_pct;
    s_charge_locked = charge_locked;
    s_armed = false;

    switch (trigger) {
        case OTA_TRIGGER_ROLLOVER:
            s_armed = true;
            break;
        case OTA_TRIGGER_SYNC: {
            uint16_t on = 0;
            /* An unreadable flag reads as off. A check is a convenience;
               spending a radio burst on a setting we could not read is
               the wrong direction to be wrong in. */
            s_armed = (nvs_config_get_ota_on_sync(&on) == ESP_OK && on != 0);
            break;
        }
        default:
            break;
    }

    /* Cleared only when this call is actually going to run a check. A
       check may legitimately replace what it finds; an arm that will not
       check has nothing to replace it WITH, and throwing the buffer away
       there loses a real update for the day.

       That is not a corner case. Two network sessions in one wake is the
       ordinary path, not the exotic one — wifi_session.c:95 names "day
       rollover + mandatory start sync" as the routine example, and
       net_apply_open() has two call sites (wake_flow.c:349, :1068). The
       rollover window finds 1.6.0 and buffers it, the operator presses
       Button A, the second window arms with a trigger that does not
       check, and the update vanishes silently until tomorrow.

       This used to clear unconditionally, justified by keeping a buffer
       out of a LATER WAKE. That threat is mostly imaginary: these are
       plain statics with no RTC_DATA_ATTR, so deep sleep does not
       preserve them, and a reboot certainly does not. The staleness that
       IS real — minutes and a full-panel repaint between the two windows
       of one wake — is handled where it can actually be measured, by
       ota_flow_apply's second gate re-sampling battery, charge lock and
       heap immediately before the download. */
    if (s_armed) {
        s_pending = false;
        s_target[0] = '\0';
        s_image_url[0] = '\0';
    }
}

/* ---- the check (network task, window 1) --------------------------------- */

void ota_flow_check(bool time_valid) {
    if (!s_armed)
        return;
    s_armed = false; /* one check per wake, whatever happens below */

    char url[CFG_BOUND_OTA_URL_MAX];
    if (nvs_config_get_ota_url(url, sizeof(url)) != ESP_OK) {
        ESP_LOGW(TAG, "ota_url unreadable: skipping the check");
        return;
    }

    ota_gate_in_t gate;
    fill_gate(&gate, url[0] != '\0', time_valid, s_batt_pct, s_charge_locked);
    ota_reason_t reason = ota_policy_check_gate(&gate);
    if (reason != OTA_REASON_NONE) {
        ESP_LOGI(TAG, "check skipped: %s", ota_policy_reason_str(reason));
        record(reason, 0);
        return;
    }

    ota_error_facts_t facts;
    memset(&facts, 0, sizeof(facts));
    int n = s_ops.manifest_get(url, s_manifest, sizeof(s_manifest), &facts);
    if (n < 0) {
        reason = ota_policy_reason(&facts);
        if (reason == OTA_REASON_NONE) {
            /* Loud on purpose. The fallback keeps the outcome reportable,
               but a driver that fails without filling a single fact is a
               bug in ota.c, and "net" would otherwise hide it behind the
               most ordinary reason there is. */
            ESP_LOGE(TAG, "manifest_get failed but named no fact; reporting net");
            reason = OTA_REASON_NET;
        }
        ESP_LOGW(TAG, "manifest fetch failed: %s", ota_policy_reason_str(reason));
        /* Deliberately NOT counted against the retry budget. A manifest
           that never arrived names no version, and the counter is keyed
           on a target — counting this would let a week of DNS trouble
           burn the budget for a build the device was never offered. */
        record(reason, facts.http_status);
        return;
    }

    /* A buffer's owner enforces its own bound: ota.c is trusted to
       honour sizeof(s_manifest), and this is what happens if it does
       not. */
    if (n > (int)sizeof(s_manifest))
        n = (int)sizeof(s_manifest);

    char counted[CFG_BOUND_OTA_TARGET_MAX];
    read_counted_target(counted, sizeof(counted));
    uint16_t fails = 0;
    (void)nvs_config_get_ota_fails(&fails); /* getter leaves 0 on any error */

    ota_decide_in_t in = {
        .manifest = s_manifest,
        .manifest_len = (size_t)n,
        .device_id = device_id(),
        .running_version = s_cfg.running_version,
        .counted_target = counted,
        .fails = fails,
        .max_fails = s_cfg.max_fails,
    };
    ota_decision_t dec;
    ota_policy_decide(&in, &dec);

    /* The plan's "take the first and log it", split across the layer
       boundary: ota_policy cannot log, so it reports the duplicate and
       this is where it becomes visible. */
    if (dec.duplicate_schema) {
        ESP_LOGW(TAG, "manifest carries two schema %d blocks; using the first", dec.schema);
    }

    record(dec.reason, 0);
    if (!dec.update) {
        ESP_LOGI(TAG, "no update: %s", ota_policy_reason_str(dec.reason));
        return;
    }
    snprintf(s_target, sizeof(s_target), "%s", dec.version);
    snprintf(s_image_url, sizeof(s_image_url), "%s", dec.url);
    s_pending = true;
    ESP_LOGI(TAG, "update available: %s -> %s", s_cfg.running_version, s_target);
}

bool ota_flow_pending(void) {
    return s_pending;
}

uint32_t ota_flow_last_dl_ms(void) {
    return s_dl_ms;
}

/* ---- the apply (main task, window 2) ------------------------------------ */

static void stop_clock(void) {
    if (!s_dl_running)
        return;
    s_dl_running = false;
    /* Clamped because the subtraction is unsigned once it lands in
       s_dl_ms: a clock that went backwards would publish ~4e9 ms in the
       stat payload, which reads as a catastrophic link rather than as
       the clock fault it is. esp_timer_get_time() cannot do that, but
       mono_ms is injected and the next implementation might. */
    int64_t elapsed = s_ops.mono_ms() - s_dl_started_ms;
    if (elapsed < 0)
        elapsed = 0;
    s_dl_ms = (uint32_t)elapsed;
}

/* Everything a failed attempt owes the next window.

   The radio comes down BEFORE the repaint, which is the same rule the
   paint at the top of the sequence follows and for the same reason. Then
   the reason and the target go to NVS, where the next window's stat
   payload is the only thing that can publish them. Then the failsafe is
   re-armed for awake_sec — not "restored", because the re-arm is
   absolute from now and a wake that has already burned time walks away
   with a fresh full budget. What it buys is that the ~3 s repaint below
   is charged to a freshly armed failsafe rather than to whatever is left
   of a download budget that has just been spent. Harmless in practice:
   everything after this point is repaint-then-sleep.

   session_up is false only for a session that never came up: net_window.c
   does not call wifi_session_end() after a failed begin either, and
   tearing down a driver that was never initialised is its own fault
   class. */
static void fail_attempt(const ota_error_facts_t *facts, bool session_up) {
    stop_clock();
    ota_reason_t reason = ota_policy_reason(facts);
    if (reason == OTA_REASON_NONE) {
        /* Same reasoning as the manifest path: the fallback keeps the
           attempt reportable and countable, but a download primitive
           that failed while naming nothing is an ota.c defect, and the
           log line is the only way to tell it apart from a genuinely
           flaky link. */
        ESP_LOGE(TAG, "download failed but named no fact; reporting net");
        reason = OTA_REASON_NET;
    }

    if (session_up)
        s_ops.session_end();

    char counted[CFG_BOUND_OTA_TARGET_MAX];
    read_counted_target(counted, sizeof(counted));
    uint16_t fails = 0;
    (void)nvs_config_get_ota_fails(&fails);

    record(reason, facts->http_status);
    (void)nvs_config_set_ota_target(s_target);
    (void)nvs_config_set_ota_fails(ota_policy_next_fail_count(s_target, counted, fails));

    s_ops.extend_awake(s_cfg.awake_sec);
    s_ops.repaint();
    ESP_LOGW(TAG, "update to %s failed: %s", s_target, ota_policy_reason_str(reason));
}

void ota_flow_apply(int batt_pct, bool charge_locked) {
    if (!s_pending)
        return; /* the common path: one comparison, no window, no paint */
    s_pending = false;

    /* The second gate, on facts re-sampled by the caller. The first
       window closed minutes and a full-panel repaint ago, so its answer
       is not evidence — and the heap headroom a TLS session needs is only
       knowable here, immediately before it is taken. */
    ota_gate_in_t gate;
    fill_gate(&gate, true, true, batt_pct, charge_locked);
    ota_reason_t gate_reason = ota_policy_download_gate(&gate);
    if (gate_reason != OTA_REASON_NONE) {
        ESP_LOGW(TAG, "download skipped: %s", ota_policy_reason_str(gate_reason));
        record(gate_reason, 0);
        return; /* nothing painted, so nothing to repaint */
    }

    /* The order below is the whole point of this module.

       PAINT FIRST, with the radio down. display_ota() carries no
       net_window_active() guard and its flush blocks for a full refresh;
       painting inside an open window is the panel-current-plus-TX-burst
       combination that browned out the rail in on-device testing
       (net_window.c:65-79).

       EXTEND SECOND, before the window and after the paint. Before,
       because an extension applied once a download "looks slow" races
       the failsafe it is protecting against. After, because ~3 s of
       panel time charged to the download's own budget is 3 s the
       download does not get. */
    s_ops.paint_update(s_cfg.running_version, s_target);

    /* The failsafe and the loop's deadline are two numbers on purpose,
       and a future reader must not fold them back into one.

       extend_awake re-arms the failsafe ABSOLUTELY, from now — main.c's
       extend_awake_failsafe is an esp_timer_stop followed by a
       start_once — so both clocks start at budget_start, on this line.
       They must not start at the same value: if the failsafe wins, the
       device deep-sleeps in the middle of dl_step. dl_abort never runs,
       nothing is recorded, the retry budget never advances, and the same
       doomed download is attempted again at every rollover forever,
       which is exactly the sad loop the budget exists to end. So the
       failsafe is given max_sec PLUS the abort tail, the loop is given
       max_sec exactly, and the loop is therefore guaranteed to stop the
       transfer first with room left to tear it down.

       Anchoring both here also matters. Anchoring the deadline after
       session_begin instead would hand the download the association time
       for free — up to WIFI_CONNECT_TIMEOUT_MS (15 s, wifi_session.c) —
       while the failsafe had already been counting it, which makes the
       deadline unreachable and hands the kill back to the failsafe. */
    int64_t budget_start = s_ops.mono_ms();
    s_ops.extend_awake(s_cfg.max_sec + OTA_ABORT_TAIL_MS / 1000);
    int64_t deadline_ms = budget_start + (int64_t)s_cfg.max_sec * 1000;

    ota_error_facts_t facts;
    memset(&facts, 0, sizeof(facts));
    if (!s_ops.session_begin()) {
        facts.transport_failed = true;
        fail_attempt(&facts, false);
        return;
    }
    /* The duration METRIC, not the deadline: what the stat payload
       publishes is the transfer's own wall time, so it starts once the
       link is up. The budget above already covers the association. */
    s_dl_started_ms = s_ops.mono_ms();
    s_dl_running = true;

    memset(&facts, 0, sizeof(facts));
    if (!s_ops.dl_begin(s_image_url, &facts)) {
        /* No abort: a dl_begin that answers false has already cleaned up
           after itself (see ota_flow_ops_t), and aborting a transfer that
           was never opened is a double free. */
        fail_attempt(&facts, true);
        return;
    }

    for (;;) {
        memset(&facts, 0, sizeof(facts));
        ota_step_t step = s_ops.dl_step(&facts);
        if (step == OTA_STEP_DONE)
            break;
        if (step == OTA_STEP_FAIL) {
            s_ops.dl_abort();
            fail_attempt(&facts, true);
            return;
        }
        /* The deadline, and the reason this is a loop rather than one
           blocking call. Aborting HERE discards the partial image and
           leaves the boot partition alone; being killed by the awake
           failsafe mid-write does neither, and the retry budget never
           learns that anything went wrong.

           Checked after a chunk rather than before one, so the budget is
           measured against time that has actually passed and every
           download gets at least one chunk's chance. The loop needs no
           iteration cap: dl_step blocks on the socket, so time always
           advances between checks. */
        if (s_ops.mono_ms() >= deadline_ms) {
            memset(&facts, 0, sizeof(facts));
            facts.deadline_hit = true;
            s_ops.dl_abort();
            fail_attempt(&facts, true);
            return;
        }
    }

    memset(&facts, 0, sizeof(facts));
    if (!s_ops.dl_finish(&facts)) {
        /* dl_finish frees its handle whichever way it answers, so this
           path aborts nothing either. It is also the last step that can
           fail: past here the boot partition has moved. */
        fail_attempt(&facts, true);
        return;
    }
    stop_clock();

    /* Cleared before the reboot, not after: the image about to run is a
       different version, so a counter left behind would be counting
       against a target that is no longer in this device's future. */
    (void)nvs_config_set_ota_result("");
    (void)nvs_config_set_ota_target("");
    (void)nvs_config_set_ota_fails(0);

    ESP_LOGI(TAG, "update to %s committed in %u ms; restarting", s_target, (unsigned)s_dl_ms);
    s_ops.session_end();
    s_ops.restart();
}
