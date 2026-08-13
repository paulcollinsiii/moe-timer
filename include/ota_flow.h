#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota_policy.h"
#include "stats_json.h" /* ota_stat_t — this module owns the keys behind it */

/* OTA sequencing — layer 2. Owns the ORDER of an update: when a check may
   run, where the manifest fetch sits inside the existing network window,
   when the panel is painted relative to the radio, how long the download
   is allowed, and what is persisted when it fails. Every device effect
   arrives through ota_flow_ops_t, so the whole sequence is host-tested in
   test_ota_flow with no ESP-IDF present.

   The decisions themselves are not here: ota_policy.c answers "may I" and
   "should I", ota.c moves the bytes. What this module contributes is the
   sequence those two are placed in, which is where the hazards live.

   ---- the two windows, and why the split is load-bearing ----

   A check is one small HTTPS GET that RIDES the window the wake was
   already opening (ota_flow_check, on the network task, after the
   snapshot rendezvous and before the MQTT phase). A download is a second
   window, opened only on the days an update actually exists
   (ota_flow_apply, on the main task, after the first window has closed).

   Between them the panel is painted, and that gap is the entire reason
   for the split. display_ota() carries no net_window_active() guard and
   its flush blocks for a full refresh; painting inside an open window
   walks into the brownout this project already paid for once
   (net_window.c:65-79). So: window 1 closes -> paint -> window 2 opens.
   ota_flow_apply enforces that ordering rather than leaving it to the
   call site, and test_ota_flow asserts it.

   ---- threading, and the stack the download needs ----

   ota_flow_init, ota_flow_arm and ota_flow_pending run on the main task.
   ota_flow_check runs on the network task. The handover between them is
   the same one every other network->device effect in this tree uses: the
   check BUFFERS its result, and the main task reads it only after
   net_window_join(), which is the barrier.

   ota_flow_apply is the exception, and it is a hardware requirement
   rather than a preference: it MUST be called from its own task with a
   stack of roughly 16 KB, NOT from the main task. The download loop now
   lives inside this function, so esp_https_ota_perform() — mbedTLS
   record buffers plus the flash write path — runs on whatever stack
   calls it. CONFIG_ESP_MAIN_TASK_STACK_SIZE is 7168 B
   (sdkconfig.defaults), which does not fit; net_window.c already spawns
   a dedicated 10240 B task for a strictly smaller job. Overflowing here
   presents on the bench as an unexplained reboot, because USB CDC eats
   the panic output. See docs/planning/ota.plan.md, "Task and stack
   sizing", and task 12, which owns the call site. */

#ifdef __cplusplus
extern "C" {
#endif

/* What opened this window, as far as the OTA check is concerned. The
   trigger is the caller's fact; whether it earns a check is this
   module's (ota_flow_arm), because the sync case depends on a runtime
   flag that only NVS knows. */
typedef enum {
    OTA_TRIGGER_NONE = 0, /* tick wake, break end, final-minute sync: never checks */
    OTA_TRIGGER_ROLLOVER, /* day rollover: always checks */
    OTA_TRIGGER_SYNC,     /* Button D full sync: checks iff ota_on_sync */
} ota_trigger_t;

/* One turn of the download loop. */
typedef enum {
    OTA_STEP_MORE = 0, /* a chunk landed, more to come */
    OTA_STEP_DONE,     /* the whole image has been received */
    OTA_STEP_FAIL,     /* transport gave up; facts are filled */
} ota_step_t;

/* Everything this module cannot do itself. Injected once at init, exactly
   like net_apply_ops_t, so the sequence above is exercised on the host
   with counters in place of a radio and a panel.

   The download is FOUR calls rather than one because the deadline check
   lives in the loop between them, and because "committed the image" and
   "discarded it" have to be separately observable: the guarantee that a
   timed-out download never switches the boot partition is only a
   guarantee if a test can see it being kept. dl_finish is the commit
   (esp_https_ota_finish, which is what sets the boot partition);
   dl_abort discards. Exactly one of the two runs per begun download. */
typedef struct {
    /* Manifest GET. Returns bytes written to buf, or < 0 on failure with
       facts filled. Does not NUL-terminate on the caller's behalf — the
       length is authoritative, as ota_policy_decide requires. */
    int (*manifest_get)(const char *url, char *buf, size_t len, ota_error_facts_t *facts);

    /* Open the image transfer and validate its header. One transfer is
       in flight at a time, so the handle stays inside the driver.

       false means NOTHING is in flight: a driver that got part-way has
       already cleaned up after itself, so dl_abort must not follow. Same
       for dl_finish, which frees its handle whichever way it answers.
       dl_abort therefore pairs only with a dl_step that failed or a
       deadline that expired. */
    bool (*dl_begin)(const char *url, ota_error_facts_t *facts);
    ota_step_t (*dl_step)(ota_error_facts_t *facts);
    bool (*dl_finish)(ota_error_facts_t *facts); /* COMMIT: sets the boot partition */
    void (*dl_abort)(void);                      /* discard a partial image */

    bool (*session_begin)(void); /* wifi_session_begin() == ESP_OK */
    void (*session_end)(void);

    void (*paint_update)(const char *from, const char *to); /* display_ota */
    void (*repaint)(void);                                  /* back to the normal screen */

    /* Push the awake failsafe out (main.c's extend_awake_failsafe).
       ABSOLUTE, from the moment of the call — an esp_timer_stop followed
       by a start_once — so an extension applied to a wake that has
       already burned time hands it a fresh full budget rather than
       adding to what is left.

       Answers FALSE when there is no failsafe to push. main.c creates
       the timer once, at the top of app_main, and only LOGS a create or
       start failure; the extender then null-guards and silently does
       nothing for the rest of the boot. That is harmless everywhere
       else and not here: ota_task_run_apply blocks on portMAX_DELAY and
       names this failsafe as its only bound, while dl_begin is
       unbounded in time by construction (see ota_flow.c). A no-op
       extender therefore turns a wedged server into a device that stays
       awake until the battery is flat. ota_flow_apply refuses to start
       an attempt nothing can end. */
    bool (*extend_awake)(int seconds);

    /* Flush volatile state to durable storage (timer_persist_save).
       Called once, immediately before restart() and after the radio is
       down, because the OTA reboot bypasses enter_deep_sleep() — which
       is the ONLY other caller of timer_persist_save on the rollover and
       button paths — and RTC memory does not survive esp_restart() on
       the ESP32-S2 (only a deep-sleep wake preserves the RTC segments;
       every other reset reloads them from the image, i.e. zeroed). See
       ota_flow_apply for what the stale snapshot would otherwise cost. */
    void (*persist_state)(void);
    void (*restart)(void); /* esp_restart */
    uint32_t (*free_heap)(void);
    int64_t (*mono_ms)(void); /* monotonic; only differences are read */

    /* Certify the running image (ota.c's ota_mark_valid_if_pending, which
       cancels the pending-verify rollback). Not part of the download
       sequence above — it runs on the NEXT boot, the one that came up on
       what the sequence wrote — and injected here for the same reason
       everything else is: ota_flow_confirm_image() below decides whether
       to call it, and that decision needs a host test. */
    void (*mark_valid)(void);
} ota_flow_ops_t;

/* The numbers the sequence is measured against. Passed in rather than
   read from sdkconfig.h here, because test_ota_flow asserts on the
   deadline and the retry budget: a host build that supplied its own
   CONFIG_ defines would be asserting against values the firmware does
   not necessarily use. app_main passes the real symbols in one line. */
typedef struct {
    int max_sec;                 /* CONFIG_MAGTAG_OTA_MAX_SEC: the download's own budget */
    int awake_sec;               /* CONFIG_MAGTAG_MAX_AWAKE_SEC: re-armed after a failure */
    int min_batt_pct;            /* CONFIG_MAGTAG_OTA_MIN_BATT_PCT */
    uint16_t max_fails;          /* CONFIG_MAGTAG_OTA_MAX_FAILS */
    uint32_t min_free_heap;      /* headroom the TLS session needs */
    const char *running_version; /* esp_app_get_description()->version */
} ota_flow_cfg_t;

/* Install the effects and the budgets. Once, before any other call.

   NOT purely an installer: it also runs this boot's rollback check, and
   that placement is load-bearing. A revert is only visible from the boot
   that comes back on the old image, and that boot has no other reason to
   enter this module at all -- ota_flow_check() is a no-op unless the wake
   armed one, and the wake after a rollback usually did not. Running the
   check from init puts the verdict in NVS before app_main has opened
   anything, which is the only way it reaches the stat payload of the same
   wake. Requires NVS: main.c calls nvs_flash_init() and
   nvs_config_init_defaults() well ahead of this. */
void ota_flow_init(const ota_flow_ops_t *ops, const ota_flow_cfg_t *cfg);

/* Main task, before the window opens: decide whether this wake's trigger
   earns a check, and sample the facts the check will gate on. batt_pct <
   0 means "unreadable", which does not gate (see ota_gate_in_t). The
   battery and charge-lock facts are sampled HERE, on the main task,
   because the network task must not touch the ADC or the lock gate.

   A buffered update is discarded only when this call arms a check, since
   a check may replace it. Arming for a trigger that will NOT check
   leaves any buffer alone: two windows in one wake is a routine path
   (day rollover plus a Button A sync), and the second arm must not throw
   away what the first window found.

   KNOWN, ACCEPTED, AND DELIBERATELY NOT FIXED: the one wake that hits
   day rollover AND Button D with ota_on_sync=1 arms twice for a check,
   so the second arm clears what the rollover window buffered. It
   self-heals — that second arm is followed by a second CHECK, which
   re-finds the same update — and only a wake where the second check
   also fails (a manifest fetch that times out, say) loses the update,
   for one day, until the next rollover. The clear-on-arm rule is not
   worth loosening for that: it is what stops a withdrawn offer standing
   after a check has replaced it, which is the hazard with teeth. */
void ota_flow_arm(ota_trigger_t trigger, int batt_pct, bool charge_locked);

/* Network task, inside the window: gate, GET the manifest, decide, and
   buffer an update for the main task. Placed after the snapshot
   rendezvous (the panel is idle by then, which is the brownout
   condition) and before mqtt_ha_window (so a failure reaches Home
   Assistant in the same window that produced it). No-op unless armed. */
void ota_flow_check(bool time_valid);

/* Main task, after net_window_join: did the check buffer an update? */
bool ota_flow_pending(void);

/* Main task, at the late pre-sleep point, with the first window CLOSED:
   paint, extend the failsafe, open the second window, download, commit,
   reboot. Returns without opening anything when nothing is pending, so
   the common path costs one comparison.

   The battery facts are sampled AGAIN by the caller rather than carried
   over from ota_flow_arm: the two windows are minutes and a full-panel
   repaint apart, and a cell that has crossed into the charge lock in
   between is exactly the one that must not be asked for a sustained
   radio burst followed by a flash write. The clock is not re-sampled —
   a pending update implies the check gate passed, and a clock, once
   set, stays set for the boot.

   THREE WAYS THIS RETURNS WITHOUT ATTEMPTING ANYTHING, and all three
   FORFEIT the buffered update for this wake — s_pending is cleared on
   entry, so nothing re-offers it until the next check finds it again:

     - the second gate answers low_heap. Recorded to ota_result (a
       human acts on it), nothing painted, retry budget untouched.
     - the second gate answers locked (the cell crossed into the charge
       lock between the windows). Deliberately NOT recorded: a
       non-attempt must not overwrite the only copy of a real failure.
     - extend_awake answers false, i.e. there is no awake failsafe to
       arm. Logged, not recorded, budget untouched — see ota_flow_ops_t.

   The forfeit is cheap by construction: the buffer is a plain static
   that neither deep sleep nor a reboot preserves, so "lost" means "found
   again at the next rollover check", which is at most a day.

   Does not return on success — the device reboots into the new image. */
void ota_flow_apply(int batt_pct, bool charge_locked);

/* The caller could not even START the apply — ota_task_run_apply failed
   to create the download task or its semaphore, so ota_flow_apply never
   ran. Records the outcome so an update that WAS found and announced
   does not then vanish without trace on every wake, forever.

   Deliberately does not touch the retry budget: nothing was attempted,
   and charging a heap condition against a target would eventually give
   up on a perfectly good image. Routed through this module rather than
   written by the caller so that which outcomes may overwrite ota_result
   stays a single rule (ota_policy_reason_is_persistable). */
void ota_flow_note_spawn_failed(void);

/* Last download's wall time THIS BOOT, 0 if none. RAM only, and for
   logging only -- what the stat payload publishes is the NVS copy, via
   ota_flow_stat() below.

   The distinction is not pedantry, it is the whole reason the NVS copy
   exists. Every download runs in the SECOND window, which opens after
   net_window.c has already closed MQTT, so nothing can publish from
   inside it; its success path then ends in restart() and its failure
   path in deep sleep, and both discard this variable. Read from
   mqtt_ha_window, this function is therefore 0 on every path that has
   ever existed -- a sensor that is structurally always zero. stop_clock()
   writes the value to NVS instead, on the failure path as well as the
   success one, because a link creeping toward the deadline is exactly
   the trend the field is for and a timeout is its last data point. */
uint32_t ota_flow_last_dl_ms(void);

/* The four OTA fields for the stat payload, read from NVS at the moment
   of the call.

   CALL IT AT PUBLISH TIME, from inside mqtt_ha_window -- not at snapshot
   collection. net_window.c runs ota_flow_check() and then
   mqtt_ha_window(), so the check's verdict is in NVS by the time this
   runs; a copy taken any earlier (a stats_snapshot_t field, say) is a
   copy of the PREVIOUS wake's result, because the snapshot is filled on
   the main task and posted by value before the check happens. See
   stats_json.h's ota_stat_t comment and task 12 of the plan.

   Lives here rather than in mqtt_ha.c because this module is the only
   writer of all four keys, and because that keeps the read testable:
   test_ota_flow drives the real NVS accessors over the mock store and
   can therefore assert the publish-time property directly.

   Never fails: an unreadable field reads as ""/0, which is what a device
   that has never attempted an update reports anyway. */
void ota_flow_stat(ota_stat_t *out);

/* ---- rollback: certifying the image this boot came up on ----------------

   With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y (sdkconfig.defaults),
   ota_flow_apply's commit leaves the new slot in PENDING_VERIFY. On the
   NEXT boot the bootloader looks at that state: if the app has not called
   esp_ota_mark_app_valid_cancel_rollback() by the time it reboots, the
   image is marked ABORTED and the OTHER slot — the old firmware — boots
   instead.

   CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP is OFF, so a deep-sleep
   wake re-runs the 2nd-stage bootloader like any other reset. That makes
   a deep-sleep wake a rollback opportunity, which in turn means the
   device gets EXACTLY ONE WAKE to certify a new image. Any path to sleep
   that skips the call silently undoes a successful update.

   That is why the call site is enter_deep_sleep() (main.c) — the single
   funnel every sleep passes through — and not the two wake-handler tails
   where ota_flow_apply lives. The two placements answer to opposite
   pressures and both are right: the tails deliberately let the early-out
   sleeps (a break starting mid-wake, a still-held button) skip the
   APPLY, because skipping merely defers an update to the next check;
   letting those same paths skip the CONFIRM would revert one. */

/* Record that this sleep is the awake failsafe's, not an ordinary one.
   Called by main.c's awake_failsafe_cb before it enters the funnel.

   Its only effect is on ota_flow_confirm_image() below. Kept as a fact
   the failsafe ANNOUNCES rather than something the funnel infers: a
   runtime "am I on the esp_timer task?" test would be a check, not a
   guarantee (the same reasoning maybe_apply_update() in wake_flow.c uses
   to stay off this funnel entirely). */
void ota_flow_note_failsafe_sleep(void);

/* Main task, early in enter_deep_sleep(): this wake worked, so cancel the
   pending-verify rollback and keep the image. A no-op on every boot that
   is not PENDING_VERIFY, which is all of them until an OTA has actually
   run.

   DECLINES on the awake-failsafe path, and this is the substantive
   decision in the rollback wiring, so the argument is here rather than
   in a commit message.

   The failsafe fires when a wake has burned CONFIG_MAGTAG_MAX_AWAKE_SEC
   (180 s) without reaching sleep — i.e. when something is already
   wedged. If that happens on the first wake of a new image, then the
   only evidence anyone has about that image is that its first wake had
   to be killed. Certifying it there would cancel the rollback in exactly
   the scenario the rollback was built for, and would delete the single
   strongest signal available that the update is bad. So the failsafe
   path certifies nothing, the next boot reverts, and the device comes up
   on firmware that is known to work.

   THE COST, stated plainly because it is real: a one-off wedge that had
   nothing to do with the new firmware — a hung WiFi driver, an AP that
   went away mid-window — throws away a perfectly good update. The device
   reverts, re-finds the same version at the next check and downloads
   ~1.5 MB again. That is a battery cost on a wrong guess, and it is the
   direction to be wrong in: it fails back to known-good firmware, which
   is recoverable over the air, rather than certifying an image that may
   need a USB cable and a board with no UART bridge chip to recover.

   WHAT THE USER SEES when it happens: the panel comes back showing the
   OLD version (the `fw` stat and the status screen both read the app
   descriptor, so they agree), an "Awake failsafe: still awake after
   180 s" line in the log, and no "first wake on the new image completed"
   line. The update then reappears on a later check.

   One more property falls out of declining, rather than being designed
   in: the failsafe can fire while ota_task is still mid-attempt (see
   hal_nvs.c, which records the same overlap), and esp_https_ota_finish
   writes ota_data. Declining here means the funnel never issues a
   competing ota_data write from the esp_timer task while that is in
   flight.

   Null-safe before ota_flow_init: main.c arms the failsafe as the second
   call in app_main, well before ota_flow_init, so the one boot where
   init never happens must not fault on a NULL op pointer.

   Also retires the "an image is awaiting certification" flag that the
   commit path set, and only on the path that actually certifies -- the
   failsafe decline above leaves it standing so the next boot can still
   tell a revert from a normal wake. Note that this is NOT the clear task
   13 ruled out ("do not defer the clears to the confirmation"): that
   objection was that mark_valid() is void, so an unconditional clear here
   could not tell "I just certified" from "there was nothing to certify"
   and would wipe the retry budget on every ordinary sleep. The flag is
   the discriminator mark_valid's return could not provide -- it is set
   only by a commit, so on an ordinary sleep there is nothing here to
   clear, and the budget keys are not touched by any of this. */
void ota_flow_confirm_image(void);

#ifdef __cplusplus
}
#endif
