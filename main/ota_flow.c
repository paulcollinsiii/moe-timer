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
#include "ota_timing.h" /* OTA_ABORT_TAIL_MS and the arithmetic behind it */

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

/* OTA_ABORT_TAIL_MS — how much longer than the download's own budget the
   awake failsafe is armed for — now lives in ota_timing.h, next to the
   socket timeout it has to outlast. It was a bare 5000 here while ota.c
   independently set a 10 s timeout_ms, which made the worst-case
   deadline overshoot four times the tail meant to absorb it. The two are
   one decision, so they are one header with a _Static_assert tying them
   together. Read that header before touching either number. */

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

/* Set by the awake failsafe on its way into enter_deep_sleep(); read only
   by ota_flow_confirm_image(). Both run on whichever task is ending the
   wake, and the failsafe is the only writer, so no barrier is needed —
   the write strictly precedes the read on the one path that sets it. */
static bool s_failsafe_sleep;

/* This boot has reported a revert, so ota_result is spoken for. Set by
   note_rollback_if_reverted() and cleared by ota_flow_init(), which makes
   it strictly boot-scoped: a plain static, zeroed by every reset and not
   preserved across deep sleep either. See record() for what it protects
   and what it costs. */
static bool s_revert_reported;

/* ---- shared helpers ----------------------------------------------------- */

/* ota_result is the ONLY channel a download failure has. The second
   window runs after MQTT has closed, so the failure cannot publish
   itself; it has to survive in NVS until the next window's stat payload
   picks it up. Which reasons may be written is ota_policy's rule rather
   than a convention here, precisely so that the "leave an earlier
   failure alone" cases cannot drift — see
   ota_policy_reason_is_persistable.

   Answers whether ota_result now holds this reason, which only the
   rollback detector reads: it must not claim the channel on the strength
   of a write that did not land. */
static bool record(ota_reason_t reason, int http_status) {
    if (!ota_policy_reason_is_persistable(reason))
        return false;

    /* A REVERT REPORTED ON THIS BOOT OUTRANKS EVERYTHING AFTER IT, and
       this guard is the whole of that rule.

       note_rollback_if_reverted() runs from ota_flow_init(), well before
       app_main opens the network window. That window then runs
       ota_flow_check() and publishes, in that order — net_window_task
       calls ota_flow_check() and then mqtt_ha_window() two statements
       later, on consecutive lines of the same wifi_up block. (Named
       rather than numbered: the ":107, :109" that stood here pointed at
       net_window.c's own comment block by the time anyone looked.) So
       without this guard ANY persistable check reason lands on
       top of the rollback in the seconds before the payload is built.
       And the token that proved the revert was consumed at detection, so
       no later wake can say it again: the report is not delayed, it is
       destroyed.

       Not a corner case. low_batt and gave_up are both persistable and
       both are what a device that has been thrashing downloads reports —
       the collision is likeliest in exactly the situation this report
       exists for.

       What gets sacrificed is the check's own reason, deliberately. A
       gate or budget verdict is a LEVEL: the condition is still there
       tomorrow, the next daily check records it again, and it reaches HA
       one day late. A revert is an EVENT and its evidence is spent. The
       cheaper loss is the one that repeats itself.

       Scoped to the BOOT rather than to the publish. Lifting it inside
       ota_flow_stat() would save one more message — the second window's
       download failure, recorded after the payload is built — at the cost
       of hiding a state change in a getter and making the rule depend on
       how many times, and whether, a payload is ever built. That message
       is a level too, and it also recurs. */
    if (s_revert_reported) {
        ESP_LOGI(TAG, "%s not recorded: this boot reported a rollback and that verdict keeps ota_result",
                 ota_policy_reason_str(reason));
        return false;
    }

    char text[OTA_REASON_TEXT_MAX];
    ota_policy_reason_text(reason, http_status, text, sizeof(text));
    esp_err_t ret = nvs_config_set_ota_result(text);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ota_result write failed (%d): this outcome will not reach HA", (int)ret);
        return false;
    }
    return true;
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

/* ---- rollback detection, once per boot ---------------------------------

   The problem this solves: a device that updates, fails to certify the
   new image and gets reverted by the bootloader comes back reporting an
   EMPTY ota_result -- the commit path cleared it (task 13 requires that)
   and the reverted image never got to write anything. Every surface the
   operator has then agrees the update simply did not happen: `fw` and the
   status screen both read the app descriptor and name the old version.

   Why not read esp_ota_get_state_partition() on the other slot, which is
   the obvious detector. Because ESP_OTA_IMG_ABORTED is not an event, it
   is a STATE, and it persists until that slot is rewritten. "The other
   slot is aborted, therefore we rolled back" is true on the boot after
   the revert and equally true on every boot after that, forever -- it
   would latch, and the latch would overwrite each subsequent wake's
   genuine ota_result with a rollback that happened last month. Pairing it
   with "ota_target names a version we are not running" (which task 13
   made available by no longer clearing ota_target) narrows it but does
   not fix it: both halves are states, and both stay true.

   So the detector is a one-shot token instead of a pair of levels.
   ota_pend_ver is written by the commit path, at the one instant that is
   unambiguous -- the boot partition has just moved and nothing has
   certified anything yet -- and it is CONSUMED by whichever of the two
   possible futures arrives first:

     - the new image runs and reaches a clean sleep, so
       ota_flow_confirm_image() certifies it and clears the flag; or
     - a boot comes up on something that is NOT the committed version,
       which is what a revert looks like from in here, and this function
       clears the flag and records the reason.

   A latch is then not merely avoided, it is unrepresentable: the second
   boot after a revert finds no flag. And because the flag is re-armed by
   every commit, a device stuck in the rollback loop that task 13 bounded
   reports EACH revert once, rather than one and then silence.

   The version compare is what separates a revert from the ordinary boot
   right after a successful update, when the token is also set: there, the
   committed version IS the running one, and the token is left for the
   certification at this wake's sleep.

   THE TOKEN IS THE COMMITTED VERSION ITSELF, not a flag beside
   ota_target, and that is a correction rather than a flourish. This
   detector originally compared the running version against ota_target,
   which is the RETRY BUDGET's key: charge_the_attempt() re-points it
   BEFORE every download, by design (task 11/13 — an attempt has to be
   countable even when the wake is killed mid-flight). The two meanings
   diverge the moment any attempt happens between a commit and its
   revert, and an image bad enough to be reverted usually wedges before
   it certifies, so that is the ordinary case rather than the exotic one.
   Both directions were wrong and both were reachable:

     - MISATTRIBUTION. The uncertified boot attempts 1.7.0, so ota_target
       becomes 1.7.0 while 1.6.0 is what was committed. A boot on 1.6.0
       then LOOKS reverted, and a boot on 1.5.0 reports a rollback of a
       version that was never downloaded.
     - THE INVERSE, worse. ota_policy allows downgrades on purpose ("a
       downgrade is how a rollback is published"), so the operator's
       designed recovery is to re-point the manifest at the known-good
       build. That attempt sets ota_target to the version the device is
       about to revert TO, the compare below then matches, and the genuine
       revert is swallowed while the token stays armed.

   Storing the committed version in its own key ends both, and folding
   the flag into it removes a state rather than adding one: there is no
   "armed but unnamed" to reason about, because the arming write IS the
   name. Which also means that every early return below happens BEFORE
   the consume, so no path can skip the consume on its way to a report —
   the property the old ordering had to assert in prose. */
static void note_rollback_if_reverted(void) {
    char committed[CFG_BOUND_OTA_TARGET_MAX];
    esp_err_t ret = nvs_config_get_ota_pend_ver(committed, sizeof(committed));
    if (ret != ESP_OK) {
        /* Unreadable, which for a str read means a stored value wider
           than this buffer or a store that is failing. Consumed anyway
           and NOTHING is reported: naming a rollback we cannot attribute
           would blame whatever version the operator last saw, and leaving
           it armed would re-ask an unanswerable question on every boot
           from here on. The retry budget still converges on a visible
           gave_up, so the device does not go silent. */
        ESP_LOGW(TAG, "ota_pend_ver unreadable (%d): consuming it without reporting a rollback", (int)ret);
        (void)nvs_config_set_ota_pend_ver("");
        return;
    }
    if (committed[0] == '\0')
        return; /* nothing was committed — and no flash spent saying so */

    const char *running = (s_cfg.running_version != NULL) ? s_cfg.running_version : "";
    if (strcmp(committed, running) == 0)
        return; /* it IS running; certification is this wake's job */

    /* CONSUMED BEFORE THE REPORT IS WRITTEN, and the report is abandoned
       if the consume does not land.

       The order alone is the crash argument: if the consume lands and the
       record does not, one revert goes unreported and the retry budget
       still walks to a visible gave_up; the other order would have every
       boot from here on re-report this rollback over whatever ota_result
       had come to hold — the latch, reintroduced through the back door.

       Checking the answer is the write-failure argument, which the order
       alone does NOT cover. A consume that fails leaves the token armed,
       so recording anyway would produce exactly that latch, once per boot
       until the store recovers. Returning instead leaves the token armed
       AND ota_result untouched, which is the same state this boot started
       in — so the next boot detects the same revert and reports it then.
       The revert is deferred rather than lost, and a report is never
       written against a token that is still armed. */
    if (nvs_config_set_ota_pend_ver("") != ESP_OK) {
        ESP_LOGW(TAG,
                 "rollback token could not be consumed: leaving the revert for the next boot rather than "
                 "reporting one that would repeat");
        return;
    }

    if (record(OTA_REASON_ROLLED_BACK, 0)) {
        /* Only once the string is actually in NVS: claiming ota_result
           for a write that failed would suppress this wake's real
           outcomes to protect a verdict that is not there. */
        s_revert_reported = true;
        ESP_LOGW(TAG, "rolled back: %s was committed but never certified; %s is running", committed, running);
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
    s_failsafe_sleep = false;
    s_revert_reported = false;

    /* Last, because it reads s_cfg.running_version and may write NVS.
       Here rather than at a call site of its own because this is the only
       function in this module that a post-rollback boot is guaranteed to
       reach: ota_flow_check() does nothing unless the wake armed a check,
       and the wake that comes back from a revert usually did not arm one.

       Running it from init also puts the verdict in NVS before app_main
       opens the network window — necessary for the verdict to reach this
       wake's payload, but on its own the exact opposite of sufficient,
       and this comment used to claim otherwise. Being FIRST in the wake
       means every persistable outcome the wake produces afterwards is
       written to the same single-slot ota_result before the payload is
       built, and the evidence has already been consumed by then. Earliest
       writer, not last writer, is the losing position. What makes the
       ordering work is record()'s guard, which hands ota_result to the
       revert for the rest of the boot; read that before moving this
       call. */
    note_rollback_if_reverted();
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
       net_apply_open() has two call sites in wake_flow.c: Button B's
       start/resume leg inside the shared dispatch, and the Button D sync
       leg in the EXT1 decode. (Named rather than numbered — the line
       numbers that stood here, ":349" and ":1068", were already several
       hundred lines out and landed in the middle of comment blocks.) The
       rollover window finds 1.6.0 and buffers it, the operator presses
       Button B, the second window arms with a trigger that does not
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

void ota_flow_stat(ota_stat_t *out) {
    if (out == NULL)
        return;
    /* Zeroed first for the reason read_counted_target() spells out:
       hal_nvs_read_str writes NOTHING into a buffer it judges too small
       or a key it cannot find, so without this an unreadable field would
       publish whatever was on the caller's stack. */
    memset(out, 0, sizeof(*out));
    if (nvs_config_get_ota_result(out->result, sizeof(out->result)) != ESP_OK)
        out->result[0] = '\0';
    if (nvs_config_get_ota_target(out->target, sizeof(out->target)) != ESP_OK)
        out->target[0] = '\0';
    (void)nvs_config_get_ota_fails(&out->fails); /* getters leave 0 on any error */
    (void)nvs_config_get_ota_dl_ms(&out->dl_ms);
}

/* ---- rollback: certifying the image this boot came up on ----------------
   ota_flow.h carries the argument for declining on the failsafe path,
   including what it costs and what the user sees. This is only the
   mechanism. */

void ota_flow_note_failsafe_sleep(void) {
    s_failsafe_sleep = true;
}

void ota_flow_confirm_image(void) {
    if (s_ops.mark_valid == NULL) {
        /* Before ota_flow_init: the failsafe is armed as app_main's
           second call, so a boot that wedges ahead of init reaches the
           funnel with an empty ops table. Nothing to certify anyway —
           that boot would decline below in any case. */
        return;
    }
    if (s_failsafe_sleep) {
        ESP_LOGW(TAG, "awake failsafe ended this wake: leaving the image unverified, the next boot decides");
        return;
    }
    s_ops.mark_valid();
    /* The image is certified, so the token the commit path left behind has
       done its job. Retiring it here is what keeps the detector honest
       for the update AFTER this one: left standing, the flag would still
       be set when a later commit fails somewhere it cannot re-arm, and a
       boot on a different version would then read a rollback that had
       already been certified away. Only reached on the certifying path --
       the failsafe decline above leaves the flag standing on purpose,
       because that wake genuinely did not certify anything and the next
       boot has to be able to tell. */
    (void)nvs_config_set_ota_pend_ver("");
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

    /* And to NVS, because s_dl_ms itself can never be published.

       This is the same argument ota_result makes at the top of the file,
       one step further. The download is the SECOND window: it opens after
       net_window.c has closed MQTT, so nothing inside it can publish
       anything. But where a failure at least survives to the next wake in
       RAM-free form, a duration in a plain static does not survive at
       all: the success path ends in restart() (which discards RAM) and
       the failure path ends in deep sleep (which discards it too, this
       being an ordinary static and not RTC-backed -- and RTC would not
       help, since only a deep-sleep wake preserves the RTC segments on
       the S2 and the success path is a restart). Read back in
       mqtt_ha_window it was therefore 0 on every path, always. Off flash
       it reports the last download, one wake late, which is what a
       "is the link getting slower?" number is for.

       Written HERE rather than at the two call sites so that both of them
       get it: the success path, and every failure that goes through
       fail_attempt -- including the deadline abort, which is the single
       most interesting sample the field can carry. A download that never
       started (session_begin failed) does not reach this line at all,
       because s_dl_running is still false, so no zero is written over a
       real earlier measurement. */
    esp_err_t ret = nvs_config_set_ota_dl_ms(s_dl_ms);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "ota_dl_ms write failed (%d): this download's duration will not reach HA", (int)ret);
}

/* Charge the retry budget for an attempt that is ABOUT TO HAPPEN.

   This is the durable half of the fix for "nothing counts an attempt
   that was killed mid-flight", and it is a MOVE: this write used to live
   at the end of fail_attempt, where it only ran for a failure this
   module actually observed.

   Everything that can kill a wake between here and the commit is
   invisible from inside the flow. Two of them are ordinary rather than
   exotic:

     - dl_begin is UNBOUNDED IN TIME. esp_https_ota's _http_connect is
       `do { open; fetch_headers; handle_response } while (redirect)`
       with no hop counter of any kind (esp_https_ota.c:166-218), and its
       read_header does `if (data_read == -ESP_ERR_HTTP_EAGAIN) continue;`
       inside a loop with no bound (esp_https_ota.c:591-617) — a wedged
       reverse proxy that sends headers and then stalls spins there at
       one socket timeout per turn. The loop that ota_flow bounds starts
       AFTER dl_begin returns, so neither is covered by the deadline.
     - a brownout or a battery pull, which no in-flight bound can ever
       cover.

   In every one of those the awake failsafe fires, main.c deep-sleeps
   immediately, dl_abort never runs and record() never happens. Charging
   AFTER an observed failure therefore learns nothing from exactly the
   failures that repeat: the counter stays put, the budget never engages,
   and the identical doomed attempt burns MAX_AWAKE_SEC of radio-on time
   at every rollover forever, on a battery device. Charging BEFORE makes
   the attempt countable on the next boot whatever kills it.

   The cost is one over-count for an attempt that succeeds, and it is
   left standing on purpose — the commit path below no longer clears it,
   because that clear is what would let a rollback loop run unbounded.
   It costs nothing on the healthy path: the device reboots into the new
   image, running_version then equals the target, and ota_policy's
   resolve() answers up_to_date BEFORE it ever consults the budget
   (ota_policy.c — the version compare precedes
   ota_policy_budget_exhausted), so the elevated counter for that target
   is never read again. A death between the commit and the reboot lands
   in the same place for the same reason.

   Order matters between the two writes. fails is written FIRST because
   the value was computed against the OLD counted target: a crash between
   them then leaves a count that does not match the target, which reads
   as stale, re-arms, and is recomputed correctly on the next wake. The
   other order leaves the NEW target wearing the OLD target's count,
   which can give up on a build that was never attempted. */
static void charge_the_attempt(void) {
    char counted[CFG_BOUND_OTA_TARGET_MAX];
    read_counted_target(counted, sizeof(counted));
    uint16_t fails = 0;
    (void)nvs_config_get_ota_fails(&fails); /* getter leaves 0 on any error */

    (void)nvs_config_set_ota_fails(ota_policy_next_fail_count(s_target, counted, fails));
    (void)nvs_config_set_ota_target(s_target);
}

/* Everything a failed attempt owes the next window.

   The radio comes down BEFORE the repaint, which is the same rule the
   paint at the top of the sequence follows and for the same reason. Then
   the reason goes to NVS, where the next window's stat payload is the
   only thing that can publish it. The retry budget is NOT touched here —
   charge_the_attempt already did that before the attempt started, and
   charging again would double-count and halve the effective budget.
   Then the failsafe is re-armed for awake_sec — not "restored", because the re-arm is
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

    record(reason, facts->http_status);

    /* Answer ignored here on purpose: ota_flow_apply already refused to
       start the attempt if the failsafe was unarmable, so reaching this
       line means it was armable. */
    (void)s_ops.extend_awake(s_cfg.awake_sec);
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
       combination that browned out the rail in on-device testing (the
       snapshot rendezvous in net_window_task states it; the ":65-79" that
       stood here now lands in the NTP block above it).

       EXTEND SECOND, before the window and after the paint. Before,
       because an extension applied once a download "looks slow" races
       the failsafe it is protecting against. After, because ~3 s of
       panel time charged to the download's own budget is 3 s the
       download does not get.

       CHARGE FIRST, ahead of both. From this line on the wake can be
       killed without this module ever hearing about it, and the paint
       is already one of the ways: display_ota() blocks on a full refresh
       and a full refresh is a current draw. Everything before this line
       is a gate that declined, which is not an attempt and must not be
       charged. See charge_the_attempt.

       THE FAILSAFE GATE, ahead of even that. Everything below leans on
       the awake failsafe being the thing that ends a wake nothing else
       can end: ota_task_run_apply blocks on portMAX_DELAY, dl_begin is
       unbounded in time, and the deadline loop only starts after it. But
       main.c's arm_awake_failsafe merely LOGS a create/start failure and
       extend_awake_failsafe then null-guards, so on a boot where the
       timer was never created every extension below is a silent no-op
       and the "bounded by the failsafe" argument evaporates — a wedged
       server would hold the radio on until the battery is flat.

       So the extension is asked for FIRST and its answer is believed. It
       is a real re-arm, not a probe: awake_sec absolute-from-now, the
       same bound every ordinary wake gets, which also stops the ~3 s
       paint below being charged to whatever is left of a wake that has
       already run for minutes. And it sits BEFORE charge_the_attempt
       because a refusal is not an attempt: burning retry budget for a
       device that never opened a socket would eventually give up on a
       perfectly good image. */
    if (!s_ops.extend_awake(s_cfg.awake_sec)) {
        ESP_LOGE(TAG, "no awake failsafe to arm: declining the download (nothing would bound it)");
        return; /* nothing painted, nothing charged, nothing to repaint */
    }

    charge_the_attempt();

    s_ops.paint_update(s_cfg.running_version, s_target);

    /* The failsafe and the loop's deadline are two numbers on purpose,
       and a future reader must not fold them back into one.

       extend_awake re-arms the failsafe ABSOLUTELY, from now — main.c's
       extend_awake_failsafe is an esp_timer_stop followed by a
       start_once — so both clocks start at budget_start, on this line.
       They must not start at the same value: if the failsafe wins, the
       device deep-sleeps in the middle of dl_step. dl_abort never runs
       and nothing is recorded. So the failsafe is given max_sec PLUS the
       abort tail, the loop is given max_sec exactly, and the loop is
       therefore guaranteed to stop the transfer first with room left to
       tear it down.

       "Room left to tear it down" is arithmetic, not a hope, and it is
       checked by a _Static_assert in ota_timing.h: the tail has to
       exceed the worst-case overshoot of one dl_step plus dl_abort plus
       session_end. The overshoot term is the socket timeout ota.c sets,
       which is why that number and this one now live in the same header.

       Note what this ordering does NOT cover, so that nobody reads more
       into it than it says: it bounds the LOOP. dl_begin runs before the
       loop and is unbounded, and a brownout is unbounded everywhere.
       Those are covered by charge_the_attempt above, which is why the
       budget is charged up front rather than on an observed failure.

       Anchoring both here also matters. Anchoring the deadline after
       session_begin instead would hand the download the association time
       for free — up to WIFI_CONNECT_TIMEOUT_MS (15 s, wifi_session.c) —
       while the failsafe had already been counting it, which makes the
       deadline unreachable and hands the kill back to the failsafe. */
    int64_t budget_start = s_ops.mono_ms();
    /* Armable was established at the gate above; this is the value. */
    (void)s_ops.extend_awake(s_cfg.max_sec + OTA_ABORT_TAIL_MS / 1000);
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

    /* THE COMMIT TAIL GETS ITS OWN BUDGET, and this line is the only
       thing that gives it one.

       The failsafe was armed for max_sec + OTA_ABORT_TAIL_MS above, and
       ota_timing.h's _Static_assert justifies that tail entirely in
       ABORT-path terms: one worst-case dl_step, plus dl_abort, plus
       session_end. Nothing in it budgets the COMMIT path, which is much
       the more expensive of the two — esp_https_ota_finish runs
       esp_ota_end -> ota_verify_partition (a full ~1.5 MB SHA-256), then
       esp_ota_set_boot_partition runs image_validate() (a SECOND full
       ~1.5 MB SHA-256) plus an ota_data erase-and-write, and three NVS
       writes and a session_end follow. Neither SHA pass has ever been
       pinned to a number without hardware in front of it.

       Losing that race is the single worst outcome in this file, and it
       is worth spelling out because it is the ONLY path in the system
       that yields garbage rather than zeros. If the failsafe fires after
       esp_ota_set_boot_partition has succeeded but before restart(),
       main.c's callback deep-sleeps. The next boot is then a DEEP-SLEEP
       WAKE of the NEW image, and deep-sleep wake is the one reset the
       bootloader does not reload the RTC segments for
       (esp_image_format.c: load_rtc_memory is false only for
       RESET_REASON_CORE_DEEP_SLEEP). The new image would read the OLD
       image's .rtc.data bytes at its own offsets. Every other reset path
       reloads .rtc.data from the image and therefore yields zeros.

       Note what this re-arm does NOT do: it does not EXTEND. It is
       absolute-from-now, so a download that finished quickly walks away
       with 180 s where it had 300 s left. That is deliberate and must
       not be "fixed" back. The commit tail is a handful of seconds
       against 180, and 180 s is the same bound every ordinary wake in
       this firmware already runs under.

       The abort path is untouched by this: fail_attempt re-arms for
       awake_sec itself, so a dl_finish that answers false lands on the
       same budget it would have had. ota_timing.h's assertion is
       likewise unaffected — it says what it always said about the abort
       path, and stays exactly as sound. */
    (void)s_ops.extend_awake(s_cfg.awake_sec);

    memset(&facts, 0, sizeof(facts));
    if (!s_ops.dl_finish(&facts)) {
        /* dl_finish frees its handle whichever way it answers, so this
           path aborts nothing either. It is also the last step that can
           fail: past here the boot partition has moved. */
        fail_attempt(&facts, true);
        return;
    }
    stop_clock();

    /* ota_result is cleared, the retry counter and its target are NOT,
       and the asymmetry is the whole point.

       ota_result has to go: the string in it belongs to the image that is
       about to be replaced, and task 14 reads it back inside the
       mqtt_ha_window that runs BEFORE the next enter_deep_sleep. Left
       standing, the new image's first stat publish would report a failure
       that no longer describes anything.

       The counter is the opposite case. With
       CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y this reboot is not the end
       of the story: if the new image never reaches
       ota_flow_confirm_image(), the bootloader marks it ABORTED and boots
       the OLD slot again. Clearing here would hand that old image a fresh
       budget -- fails=0, no counted target -- so it would re-find the same
       manifest, re-download the same ~1.5 MB, and roll back again, every
       day, with nothing in NVS ever showing why. The retry budget is
       supposed to bound exactly that, and clearing it here is what would
       make it unbounded.

       So the charge raised by charge_the_attempt() when this attempt
       started is deliberately left standing, charged against s_target. Nothing on the
       healthy path needs it gone: ota_policy_evaluate() answers up_to_date
       ahead of the budget check once the device is RUNNING the counted
       version (ota_policy.c: the up_to_date return precedes the budget
       test), a genuinely new target re-arms the budget by itself
       (budget_exhausted() compares target against the counted target, and
       next_fail_count() restarts at 1), and a rolled-back device instead
       walks the counter to max_fails and stops with a visible gave_up. */
    /* The one durable trace this reboot leaves, and the ONLY moment it can
       be written: the boot partition has just moved, and from the next
       line onward this process may cease to exist at any point. Set
       BEFORE the ota_result clear below so that a death between the two
       leaves the recoverable state -- the next boot either certifies the
       new image and clears this, or converts it into a rolled_back that
       overwrites the stale string anyway. The other order would leave no
       flag at all, and the revert it was meant to catch would be exactly
       as invisible as it was before task 14.

       It is set AFTER dl_finish rather than before it for the same reason
       in reverse: a token armed ahead of a commit that then FAILS would
       have the next boot see "committed, not running the target" -- a
       false rollback report, written over the genuine failure reason
       fail_attempt just recorded.

       It stores THE VERSION, and s_target is the only correct value for
       it: this is the line where "what was committed" is a fact rather
       than an inference. The detector must not go looking for that fact
       in ota_target, which by then means "what the retry budget is
       counting", a different question with a different answer -- see
       note_rollback_if_reverted. s_target is non-empty by construction
       (ota_policy rejects an empty manifest version as bad_version, and
       nothing reaches this line without a decision that named one), which
       is what lets one key carry both the arming and the name. */
    (void)nvs_config_set_ota_pend_ver(s_target);
    (void)nvs_config_set_ota_result("");

    ESP_LOGI(TAG, "update to %s committed in %u ms; restarting", s_target, (unsigned)s_dl_ms);
    s_ops.session_end();

    /* The timer snapshot, and the reason this reboot needs one when no
       other path in the firmware does.

       Apart from this injected op, timer_persist_save() runs at exactly
       four places: enter_deep_sleep, the Bed Time lock, a break start and
       the expiry alert. NOT on a day rollover, NOT on a button action,
       NOT on an HA grant. Every one of those — and the charge lock, which
       has no save of its own either — relies on enter_deep_sleep to flush
       eventually, and this reboot never reaches it, because
       maybe_apply_update() sits ahead of the sleep in both wake tails.

       That would not matter if RTC memory survived the restart. It does
       not: on the ESP32-S2 only a deep-sleep wake preserves the RTC
       segments, so g_rtc_state comes back ZEROED and the NVS snapshot is
       the sole survivor — up to a whole wake stale.

       The sharp case is the primary trigger. On a rollover wake the
       snapshot still carries YESTERDAY'S date, so timer_restore_snapshot
       refuses it; g_rtc_state stays zeroed, last_date is empty, and
       timer_is_new_day() answers true on an empty last_date. The new
       firmware then runs the day rollover a SECOND time and publishes
       yesterday's screen-time summary computed from all-zero slots,
       permanently overwriting Home Assistant's record with zeros.

       Saving HERE fixes that outright rather than papering over it:
       timer_make_snapshot copies g_rtc_state.last_date, the rollover has
       already run by this point, so the blob carries TODAY'S date. The
       post-OTA boot restores it cleanly and timer_is_new_day() answers
       false — no second rollover, and no lost completions or pause.

       Safe from this task for the same reason .repaint is: the main task
       is blocked in ota_task_run_apply's join and the network window was
       joined before the apply point was reached. After session_end, so
       the flash write is not competing with the radio.

       One more load this save carries, added after the fact and easy to
       miss: it is also what keeps a ROLLBACK loop down to one attempt a
       day instead of a continuous one. Do not "simplify" it away.

       The chain is the same zeroed g_rtc_state as above, read from the
       other end. wake_flow.c arms the OTA check inside the rollover
       branch, and does so BEFORE the in-rollover
       timer_persist_try_restore(). So the question "is this a new day?"
       is answered off whatever last_date is in RTC at that moment — and
       after a rollback reboot that is empty, which timer_is_new_day()
       reads as yes. Without a valid same-day snapshot to restore,
       EVERY post-rollback boot would arm a fresh check, re-download, roll
       back, and come straight back round with no sleep in between.

       It only works because the wall clock outlives the restart that
       zeroes .rtc.data: with CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y,
       ESP-IDF keeps boot time in the RTC retention REGISTERS
       (RTC_BOOT_TIME_LOW/HIGH_REG, esp_libc's esp_time_impl.c), not in
       the .rtc.data segment. time(NULL) is therefore correct on the next
       boot, the snapshot written here is recognised as today's, and the
       loop is damped to the ordinary daily cadence — which is what makes
       the retry budget above a bound anyone would live to see reached. */
    s_ops.persist_state();
    s_ops.restart();
}

/* ---- outcomes the flow itself never saw --------------------------------- */

void ota_flow_note_spawn_failed(void) {
    /* low_heap, and not a new code of its own. What actually failed is
       xTaskCreate on a 16 KB stack (or the semaphore before it) — the
       largest single allocation this firmware ever asks for — which is a
       heap condition by any reading, presents to the operator exactly as
       the download gate's own low_heap does, and already has a row in
       the plan's failure table ("Free heap too low / low_heap / download
       skipped / next rollover"). Inventing a second string for the same
       physical condition would buy Home Assistant nothing and cost a
       discovery-schema bump.

       Through record() rather than nvs_config_set_ota_result() directly,
       so the "may this overwrite an earlier failure?" question keeps one
       owner (ota_policy_reason_is_persistable) instead of two. */
    ESP_LOGW(TAG, "download task could not be started: reporting low_heap");
    record(OTA_REASON_LOW_HEAP, 0);
}
