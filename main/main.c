#include <stdlib.h>
#include <time.h>

#include "alerts.h"
#include "audio.h"
#include "battery.h"
#include "buttons.h"
#include "config_cache.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_app_desc.h" /* esp_app_get_description(): the running version */
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_nvs.h"
#include "lock_gate.h"
#include "neopixel.h"
#include "net_apply.h"
#include "net_window.h"
#include "nvs_config.h"
#include "nvs_flash.h"
#include "ota.h"
#include "ota_flow.h"
#include "panic_diag.h"
#include "panic_soak.h"
#include "setup_session_idf.h"
#include "sleep_plan.h"
#include "timer.h"
#include "timer_persist.h"
#include "wake_flow.h"
#include "wifi_session.h"

/* This file is the composition root and nothing else. The rule it is held
   to (.claude/CLAUDE.md) is that it may contain wiring and device calls
   but may not contain a DECISION. Every symbol below with an executable
   statement names the numbered reason that admits it; the rest — the log
   TAG and the NET_APPLY_OPS and OTA_FLOW_OPS tables — are pure wiring,
   admitted by the headline rule rather than by a number, because a
   construct with nothing to execute has nothing to decide. The numbered
   reasons, quoted from the rule rather than paraphrased — the rule is not
   negotiable against the code that has to satisfy it:

     1. Boot ordering is a hardware contract.
     2. It runs in an ISR or esp_timer context where a module API is not
        safe.
     3. It owns an ESP-IDF handle with no module home.
     4. It is a <=3-line, branch-free thunk adapting a module ABI to
        another module's callback signature.

   "It's only a few lines" and "it's just plumbing" are not reasons. An
   `if` here that is not a null-guard on an injected pointer is a review
   blocker. Three that used to be — the panic quiet, the sleep-time
   break-end drain and the wake-cause decode — are host-tested calls into
   wake_flow.c as of the residency audit. Every branch that is LEFT,
   enumerated so the next reviewer can check the claim instead of
   trusting it:

     - app_main's NVS re-init, on esp_err_t: ESP-IDF's documented idiom.
     - arm_awake_failsafe's two, on esp_err_t from the esp_timer handle
       this file owns (reason 3).
     - extend_awake_failsafe's, a null-guard on that same handle.
     - enter_deep_sleep's button-release wait,
       `i < 30 && buttons_scan_held() != 0`. This one branches on a
       MODULE API — neither an esp_err_t nor the handle above — and the
       3 s cap is a policy number. It names NO reason on the list. It is
       recorded here as DEBT rather than given a label; the comment at
       the loop says where it belongs.

   One PREPROCESSOR conditional is also left, and it is deliberately not
   on the list above because it is not a branch: the
   `#if MAGTAG_PANIC_SOAK` at the end of app_main. It tests a
   hand-flipped compile-time constant that ships at 0, so the ordinary
   image does not contain the code at all — there is no runtime state it
   can consult and no path it can choose between. Named here anyway so
   the next reviewer does not have to re-derive that;
   include/panic_soak.h carries the rest of the argument, including why
   it is a #define rather than a Kconfig symbol.

   Adding a line here means naming its reason in the review. */

static const char *TAG = "main";

/* Timezone default lives in nvs_defaults.h (NVS_DEFAULT_TZ); the active TZ
   comes from NVS at boot so HA can change it (docs/home_assistant/configuring.md, "The controls"). */

/* Residency 4. Adapts the wake-scoped quiet-hours cache to neopixel.c's
   bool(void) callback ABI, which has nowhere to take the clock from. One
   line, no branch; the quiet-hours rule itself is in config_cache.c. */
static bool status_leds_quiet(void) {
    return config_cache_quiet_active(time(NULL));
}

/* Residency 3, and the largest thing here that earns it: deep sleep is an
   ESP-IDF contract with no module home — the wake sources, the RTC pad
   holds and esp_deep_sleep_start() itself. Declared in lock_gate.h,
   because the gates there end a wake by calling this. Does not return.

   Everything in it that is a POLICY has already left: which buttons arm,
   how long to sleep and why are sleep_plan.c's, and the break-end report
   below is wake_flow's. What remains is the ORDER, which is the part that
   cannot move — each step below is a hardware precondition for the next. */
void enter_deep_sleep(wake_sleep_mode_t mode) {
    /* Breadcrumb: everything below is the sleep funnel. First statement
       in the function so the whole funnel is covered, including the
       failsafe's entry from esp_timer context. No branch and no
       decision — which slot the phase belongs to is panic_diag's, and
       the phase table is host-tested there. */
    (void)panic_diag_enter(PANIC_PHASE_SLEEP);
    /* Late-wake forensics repeat: the boot-time log of this line is often
       lost to USB CDC re-enumeration; by sleep entry the console has had
       the whole wake to come up. */
    const esp_reset_reason_t rst = esp_reset_reason();
    ESP_LOGI(TAG, "this boot: reset %s", wake_flow_reset_reason_str(rst));
    /* Never sleep with the network task alive: it holds WiFi and may be
       mid-publish. Normal paths finished the window already (no-op here);
       this covers cut-short paths. Bounded — on the failsafe path the
       network task may BE the wedge, and deep sleep then powers the radio
       down regardless. No pause polling: this can run in esp_timer
       context. */
    net_window_join(15000, NULL);
    net_window_log_last(); /* timing repeat: the boot-time line is often lost to CDC */
    /* Rollback: this wake reached sleep, so keep the image it came up on.
       Early in the funnel on purpose — every step below it can block (the
       3 s release wait, neopixel_stop_sync's 500 ms ack, the break-end
       repaint) and this wake is the image's only chance to be certified.
       It does not need to be above hal_nvs_close() at the bottom, though
       it also is: the mark writes the ota_data partition through the
       esp_ota APIs, not through hal_nvs. Whether it certifies or declines
       is ota_flow's, host-tested; the funnel decides nothing. */
    ota_flow_confirm_image();
    timer_persist_save();
    /* EXT1 ANY_LOW is level-triggered: a still-held button would re-wake
       instantly and re-fire its action. Wait (bounded) for release before
       the wake sources are armed at the bottom of this function;
       wake_flow's continuation guard is what handles a timeout.

       DEBT, named rather than labelled. The loop branches on
       buttons_scan_held() — buttons.c's API, not an esp_err_t and not the
       esp_timer handle this file owns — and 30 x 100 ms encodes a 3 s cap,
       which is a number somebody chose. Reason 3 admits the sleep entry
       AROUND it (the wake sources, the pad holds, esp_deep_sleep_start);
       it does not reach in here. A host-tested
       buttons_wait_for_release(cap_ms) in buttons.c is where this goes,
       and until it does this loop is a rule violation on record rather
       than a survivor with a reason. */
    for (int i = 0; i < 30 && buttons_scan_held() != 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Snapshot still-held buttons for the continuation guard (the guard
       itself, and the RTC memory behind it, live in wake_flow). Must read
       while the pads are still digital: buttons_configure_wakeup_if() at
       the end of this function is what hands them to the RTC mux, and
       gpio_get_level is unreliable afterwards. */
    wake_flow_note_sleep_entry();

    /* No code path may sleep with the NeoPixel gate LOW — the hold below
       would keep the LEDs powered all night. Ack'd stop: waits for the LED
       task to confirm; on timeout the gate GPIO is forced HIGH without an
       RMT transmit (safe from the failsafe's esp_timer context too). */
    neopixel_stop_sync(500);

    /* Digital pads float in deep sleep; hold the power-control pins so the
       NeoPixel gate (21, HIGH = off) and amp enable (16, LOW = off) cannot
       drift on and drain the battery. neopixel_init() releases the gate
       hold on every wake; the amp hold stays until the (lazy) audio_init
       actually needs the pin — silent wakes leave it held. Re-holding an
       already-held pin is a no-op. */
    gpio_hold_en(GPIO_NUM_21);
    gpio_hold_en(GPIO_NUM_16);
    gpio_deep_sleep_hold_en();

    /* Safety net: a latched break end that nothing drained reaches sleep
       here. The take, the report, and the reasons it does none of the
       things the break-end owner would do (no tick, no chime, no snap
       back — the panel is finished, and this can run from the failsafe's
       esp_timer context where audio is not safe) are all in wake_flow.c
       with a host test, so the edge keeps one owner. What stays here is
       the POSITION, which is the load-bearing part: it has to sit after
       every path that could have drained the edge, or "undrained" would
       mean nothing. */
    wake_flow_report_undrained_break_end();

    /* Last NVS write (snapshot) is behind us on every path below; release
       the wake-scoped handle. */
    hal_nvs_close();

    /* All sleep-duration policy lives in the pure, host-tested planner
       (sleep_plan.c): minute-boundary alignment for clean renders, the
       NTP early-wake lead, the expiry/break-end event lead, and which of
       the readings below each state actually uses. Alignment precision is
       bounded by the S2's RC-oscillator sleep drift — the periodic NTP
       sync keeps it honest.
       Every reading is taken unconditionally: all seven are side-effect
       free getters, so gathering them costs nothing and keeps the choice
       of which ones matter on the tested side of the seam. */
    time_t plan_now = time(NULL);
    sleep_plan_timer_in_t plan_readings = {
        .state = timer_get_state(),
        .now = plan_now,
        .expiry_wall = timer_expiry_wall(),
        .ntp_recheck_due = timer_needs_ntp_sync(plan_now + SLEEP_PLAN_SYNC_LOOKAHEAD_SEC),
        .break_active = timer_break_active(),
        .break_remaining_sec = timer_break_remaining(plan_now),
        .extra_running = timer_any_extra_running(),
    };
    sleep_plan_in_t plan_in = sleep_plan_from_timer(&plan_readings);
    /* The mode picks between the planner and a lock's fixed interval, and
       carries the button decision with it — the readings above are pure
       getters, so gathering them on a locked wake costs nothing and keeps
       this path straight. */
    sleep_outcome_t out = sleep_plan_outcome(mode, &plan_in);
    buttons_configure_wakeup_if(out.enable_buttons);
    sleep_plan_arm_timer(out.seconds);
    ESP_LOGI(TAG, "Entering deep sleep (%s%lu s)", out.reason, (unsigned long)out.seconds);
    esp_deep_sleep_start();
}

/* ---- network window (WiFi → NTP → snapshot rendezvous → MQTT) ----------
   Mechanics (task, completion signals) live in net_window.c; the
   orchestration (pre-window def capture, post-join reconcile/apply) in
   net_apply.c. main.c only supplies the device effects below. */

/* Residency 4. Join poll: Button B stays live while the MQTT tail drains
   — the screen is already painted and a dropped press would read as
   broken. One line, no branch; which states a press acts on is decided in
   wake_flow. Never passed from the failsafe's esp_timer context. */
static void poll_button_b_cb(void) {
    (void)wake_flow_poll_button_b_action();
}

/* The composition root proper: wiring, and the one construct here with no
   executable statement at all, so there is nothing in it to decide. It is
   not admitted by a numbered reason because it does not need one — the
   test a move is supposed to buy does not exist. Asserting that a field
   holds the function it was just assigned is a tautology, and binding
   this inside net_apply.c instead would give that module link-time
   dependencies on audio, alerts, config_cache and wake_flow, making its
   own suite harder to stub rather than easier. That is the rule's purpose
   pointing the other way. */
static const net_apply_ops_t NET_APPLY_OPS = {
    .join_poll = poll_button_b_cb,
    .on_config_applied = config_cache_invalidate,
    .on_active_reset_chirp = audio_break_over_chime,
    .on_active_expired_alert = wake_flow_fire_expiry_alert,
    .post_stats = wake_flow_post_stats_snapshot,
    .on_locate = alert_run_locate,
};

/* Residency 2 — the canonical case for it. Last-resort battery
   protection: no wake may run forever (WiFi driver hang, stuck BUSY,
   firmware bug) — the CPU would otherwise stay awake until the battery
   dies. Runs in the esp_timer task, where most module APIs are not safe
   to call, which is exactly why the callback is here and does nothing but
   log and sleep. enter_deep_sleep persists the snapshot first, so no
   allocation is lost. A mid-refresh force-sleep can leave the panel
   scruffy for one frame — acceptable for a path that only fires when
   something is already wedged. */
static void awake_failsafe_cb(void *arg) {
    (void)arg;
    ESP_LOGE(TAG, "Awake failsafe: still awake after %d s - forcing deep sleep", CONFIG_MAGTAG_MAX_AWAKE_SEC);
    /* Announce the path before entering the funnel: this sleep must not
       certify a pending image. A wake that had to be killed is the worst
       possible evidence for the firmware running it, and the funnel's
       ota_flow_confirm_image() has no other way to tell this caller from
       the healthy ones. One line, no branch — ota_flow.h owns the
       argument and ota_flow.c the decision. */
    ota_flow_note_failsafe_sleep();
    enter_deep_sleep(lock_gate_sleep_mode());
}

/* Residency 3 for the three symbols below: this file owns the
   esp_timer_handle_t, and an esp_timer has no module home. The `if`s in
   arm_ are on esp_err_t from that handle and cannot be separated from
   owning it — creating a timer and starting it are two fallible ESP-IDF
   calls, and the second is only meaningful if the first succeeded. The
   `if` in extend_ is a null-guard on the same handle. None of the three
   decides anything about the wake; the cap itself is a Kconfig value. */
static esp_timer_handle_t s_failsafe_timer;

static void arm_awake_failsafe(void) {
    static const esp_timer_create_args_t args = {.callback = awake_failsafe_cb, .name = "awake_cap"};
    esp_err_t ret = esp_timer_create(&args, &s_failsafe_timer);
    if (ret == ESP_OK) {
        ret = esp_timer_start_once(s_failsafe_timer, (uint64_t)CONFIG_MAGTAG_MAX_AWAKE_SEC * 1000000ULL);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "awake failsafe not armed: %s", esp_err_to_name(ret));
    }
}

/* Push the awake failsafe out so a long deliberate awake stretch (the
   locate alarm) isn't cut short by it. HOW LONG to push is the caller's,
   and this only applies it.

   ANSWERS WHETHER THERE WAS A FAILSAFE TO PUSH, which used to be
   swallowed. arm_awake_failsafe above only logs a create/start failure,
   so on such a boot s_failsafe_timer stays NULL and this is a silent
   no-op for the rest of the wake. Harmless for the locate alarm — with
   no failsafe there is nothing to cut it short — but not for OTA:
   ota_task_run_apply blocks on portMAX_DELAY and names this failsafe as
   its only bound, so ota_flow_apply has to be able to ask, and declines
   the download when the answer is no. */
static bool extend_awake_failsafe(int seconds) {
    if (s_failsafe_timer != NULL) {
        esp_timer_stop(s_failsafe_timer);
        esp_timer_start_once(s_failsafe_timer, (uint64_t)seconds * 1000000ULL);
        return true;
    }
    return false;
}

/* Residency 4. alerts.c's extender callback is void(int) — it has no use
   for the answer, and widening its ABI to carry one nobody reads would
   be the tail wagging the dog. One line, no branch.
   This also keeps the property the note above used to ask for by hand:
   each of the two installs still has exactly one static symbol behind
   it, so -Werror=unused-function is what notices a dropped install
   (see refactor.bugdiscoveries.md). */
static void alerts_extend_awake_cb(int seconds) {
    (void)extend_awake_failsafe(seconds);
}

/* ---- OTA ---------------------------------------------------------------
   Same shape as the network window above: the sequence is ota_flow.c's
   (host-tested against injected counters), the transport is ota.c's, and
   what belongs here is only the binding between them plus the budgets.

   Note where the call sites are NOT: this file does not call
   ota_flow_arm, ota_flow_check or ota_flow_apply. Arming and applying are
   wake_flow.c's, checking is net_window.c's, and the apply is
   deliberately kept off enter_deep_sleep() — awake_failsafe_cb reaches
   that funnel from the esp_timer task, and a wedged wake is the last one
   that should start a 1.5 MB download. The reasoning is written out at
   maybe_apply_update() in wake_flow.c.

   The rollback confirmation goes the OTHER way, and the contrast is the
   point rather than an inconsistency. ota_flow_confirm_image() IS on
   enter_deep_sleep(), because skipping the apply defers an update by a
   day while skipping the confirmation REVERTS one — so it has to sit on
   the funnel every sleep passes through, early-outs included. The
   failsafe still must not certify anything, which is why
   awake_failsafe_cb announces itself first; ota_flow.h argues that
   choice out in full. */

/* Residency 4. wifi_session_begin answers esp_err_t; ota_flow_ops_t wants
   a bool, because the host suite has no esp_err_t. One line, no branch. */
static bool ota_session_begin(void) {
    return wifi_session_begin() == ESP_OK;
}

/* Residency 4. esp_timer counts microseconds; ota_flow measures its
   download deadline in milliseconds. One line, no branch. */
static int64_t ota_mono_ms(void) {
    return esp_timer_get_time() / 1000;
}

/* Wiring, no executable statement — the same standing as NET_APPLY_OPS
   above, and admitted by the headline rule for the same reason: a
   construct with nothing to execute has nothing to decide.

   .repaint is wake_flow's public entry point rather than main.c
   implementing the paint itself; wake_flow.h records why that direction
   is the one the residency audit landed on. */
static const ota_flow_ops_t OTA_FLOW_OPS = {
    .manifest_get = ota_manifest_get,
    .dl_begin = ota_download_begin,
    .dl_step = ota_download_step,
    .dl_finish = ota_download_finish,
    .dl_abort = ota_download_abort,
    .session_begin = ota_session_begin,
    .session_end = wifi_session_end,
    .paint_update = display_ota,
    .repaint = wake_flow_repaint_current_state,
    .extend_awake = extend_awake_failsafe,
    /* The OTA reboot skips enter_deep_sleep() entirely — maybe_apply_update
       sits ahead of it in both wake tails — so this is the only thing
       that flushes the timer snapshot before esp_restart() wipes RTC
       memory. ota_flow.h says what the stale snapshot would cost. */
    .persist_state = timer_persist_save,
    .restart = esp_restart,
    .free_heap = esp_get_free_heap_size,
    .mono_ms = ota_mono_ms,
    /* Cancels the pending-verify rollback. In ota.c rather than here
       because esp_ota_mark_app_valid_cancel_rollback() is a bare call
       with no handle, which the residency rule keeps out of this file;
       reached through the ops table so the "should this wake certify?"
       decision stays in host-tested ota_flow.c. */
    .mark_valid = ota_mark_valid_if_pending,
};

/* Residency 1, whole-function: this is the boot sequence, and the order
   of it is a hardware contract at every step — the NeoPixel gate before
   any peripheral touches GPIO 21, NVS before anything reads config, TZ
   before any date comparison, timer_defs_install() before the first
   timer_* call, and the wake-cause read while the register still holds
   this boot's value. The sequence cannot be host-tested because it IS the
   ordering; what it must not contain is a decision, and after the audit
   it contains one branch, the ESP-IDF-documented NVS re-init idiom. */
void app_main(void) {
    /* Reason 1, and it must be the first statement in the function.
       Latches the previous boot's panic breadcrumb out of RTC memory
       BEFORE anything can overwrite it, and starts this boot's. Anything
       executed ahead of it is unattributable — a panic there would be
       reported against the phase the PREVIOUS wake ended in.

       Does NOT displace neopixel_init()'s claim on the line below: that
       claim is on the first PERIPHERAL call, and this touches only RTC
       memory, the reset-reason register and the monotonic timer. It
       writes no NVS either, because NVS is not up yet; panic_diag_commit()
       below is the other half. */
    panic_diag_init();

    /* MUST be first peripheral call: GPIO 21 power gate HIGH (NeoPixels off) */
    neopixel_init();
    arm_awake_failsafe();
    neopixel_set_quiet_cb(status_leds_quiet);
    neopixel_set_status_brightness(CONFIG_MAGTAG_STATUS_LED_BRIGHTNESS);

    /* Panic-loop breaker. WHICH reset reason earns a quiet window and how
       long it lasts are decided in wake_flow.c and host-tested; what is
       reason 1 here is only the position — it has to happen before this
       boot's first console output, which is what the quiet is for.

       DELIBERATELY UNMARKED, and it is the one unmarked span in boot
       that is not small: 2,000 ms of blocking delay, on exactly the
       post-panic boots this diagnostic measures. panic_diag.h argues why
       a mark here could not report anything, and what the delay does to
       the published `panic_uptime_s`. */
    wake_flow_boot_quiet_after_panic();

    /* Boot sub-phase 1 of 6. panic_diag.h argues why BOOT was subdivided at all; what belongs here
       is why THIS span is one phase. Everything between this mark and its exit is flash work on
       the NVS partition — the documented re-init idiom below (whose erase branch is the most
       destructive act the firmware can perform), the defaults seed, and panic_diag_commit(), which
       files the previous boot's breadcrumb. If panics are landing in flash, this is the label that
       says so.

       ENTER/EXIT rather than a bare mark, and the exit restores what the
       enter returned rather than hard-coding PANIC_PHASE_BOOT back. Two
       reasons, both real: the slot goes back to whatever it actually
       held, so the unclaimed wiring calls between these spans still read
       as plain BOOT; and the awake failsafe is already armed a few lines
       above and marks SLEEP from the esp_timer task, so a restore has to
       be conditional. panic_diag_rec_exit does that comparison — this
       file only has to hand back the value it was given. Reason 1 in the
       same sense as the AWAKE mark at the bottom of this function: the
       whole content of a phase mark is WHERE it sits, so it cannot live
       anywhere but here. */
    const panic_phase_t boot_prev_nvs = panic_diag_enter(PANIC_PHASE_BOOT_NVS);
    /* The one surviving branch. ESP-IDF's documented NVS init idiom: a flash image whose NVS
       partition is full or was written by a newer version cannot be opened until it is erased, and
       there is nowhere to put this but in front of the first nvs call. The erase takes the owner's
       WiFi and MQTT credentials with it, same as every other stored setting; nothing reseeds them
       from a build-time default any more (nvs_defaults.h). The device comes back up with no SSID,
       which is exactly what sends it into setup mode to get them re-entered. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_config_init_defaults());

    /* Reason 1: the other half of panic_diag_init(), and its position is
       the whole content of it — the first point in the boot where NVS
       can be written. Bumps the panic counter and files the breadcrumb
       so the evidence outlives RTC memory, which only reaches the NEXT
       boot; this device does not open a network window on every wake, so
       without this the record would routinely be dropped before anything
       could publish it. A no-op unless this boot followed a panic. */
    panic_diag_commit();
    panic_diag_exit(PANIC_PHASE_BOOT_NVS, boot_prev_nvs);

    /* TZ from NVS (HA config-in) with the compile-time default as fallback */
    char tz[48];
    nvs_config_get_tz(tz, sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();

    /* Boot sub-phase 2 of 6, and the one that closes a hole rather than
       naming a single call. BOOT_NVS exited a few lines above and
       BOOT_OTA does not begin until well below, so this span used to
       read as plain BOOT - and it is not more small wiring. The rule
       drawing the span is: RUNS ON the timer state carried across a
       deep-sleep wake, or INSTALLS THE TABLE that state is interpreted
       against. The guard and the restore are the first kind (both go
       through g_rtc_state, which is RTC_DATA_ATTR); timer_defs_install()
       is the second, and reads NVS into plain statics without touching
       RTC memory at all.

       That split is also which soak proved what. esp_restart() zeroes
       RTC memory but leaves NVS intact, so the reset-loop soak in
       panic_soak.h exercised timer_defs_install() fully and cleared it,
       while running the guard and the restore against a blank slate on
       every iteration - the panics being chased are all genuine wakes
       with the state intact.

       The TZ read above is deliberately OUTSIDE the span even though
       timer_persist_try_restore() depends on it having happened;
       panic_diag.h argues why. In one line: it is upstream of all three
       calls rather than one of them, and half the firmware consumes it.

       Enter/exit with the value the enter returned, for the same two
       reasons as sub-phase 1: the unclaimed calls on either side go back
       to reading plain BOOT, and the awake failsafe armed at the top of
       this function marks SLEEP from the esp_timer task, so the restore
       has to be the conditional one panic_diag_rec_exit performs. */
    const panic_phase_t boot_prev_tmr = panic_diag_enter(PANIC_PHASE_BOOT_TMR);

    /* Before anything reads the timer state, and in particular before the
       restore below: g_rtc_state now carries a magic and a version, and
       zeroing a struct that fails them is what makes last_date empty and
       so hands the boot to the NVS snapshot. Answers whether it fired;
       nothing here acts on that, because on a cold boot and on every
       esp_restart the answer is yes by construction. The one case it is
       actually defending against is written out above rtc_state_t. */
    (void)timer_rtc_state_guard();

    /* Slot definitions come from the NVS timer table (Kconfig fallback), not RTC
       memory — install them before the first timer_* call on every boot/wake. */
    timer_defs_install();

    /* Must run after TZ is set (date comparison) and before the wake
       handlers (whose rollover check would otherwise reset the timer). */
    timer_persist_try_restore(time(NULL));
    panic_diag_exit(PANIC_PHASE_BOOT_TMR, boot_prev_tmr);

    /* Paired with the line below: net_apply_init is what makes .on_locate
       dispatchable, so this is where a missing install would bite. Order
       against arm_awake_failsafe is free — the extender null-guards. */
    alerts_set_extend_awake(alerts_extend_awake_cb);
    net_apply_init(&NET_APPLY_OPS);
    /* Wiring in the same bucket as the install above and the ota_cfg below:
       a data table assembled by a builder in setup_session_idf.c (the only
       member main.c owns is the failsafe extender) and handed over once, with
       no branch and nothing decided here. Under reason 1 as part of the boot
       sequence — wake_flow_handle_wake below is where it is first needed. */
    const setup_session_ops_t setup_ops = setup_mode_ops(extend_awake_failsafe);
    wake_flow_set_setup_ops(&setup_ops);
    /* Before wake_flow_handle_wake at the bottom of this function, which
       is where the first ota_flow_arm() happens — the module's contract
       is "once, before any other call", and this is the only point that
       satisfies it on every wake path.

       The CONFIG_MAGTAG_* symbols are READ HERE and passed in rather than
       included by ota_flow.c, which is the arrangement ota_flow.h asks
       for: the host suite asserts on the deadline and the retry budget
       against its own numbers, and a host build that supplied its own
       CONFIG_ defines would be asserting against values this firmware
       does not necessarily use.

       running_version is the app descriptor's, which version.txt at the
       project root stamps (1.5.0). It is the SAME string the panel and
       the `fw` stat publish, and that is the point: it is also the OTA
       comparison key, so the two cannot disagree. Without version.txt IDF
       falls back to `git describe`, which yields something no manifest
       could ever match and very nearly overflows the 32-byte field. */
    const ota_flow_cfg_t ota_cfg = {
        .max_sec = CONFIG_MAGTAG_OTA_MAX_SEC,
        .awake_sec = CONFIG_MAGTAG_MAX_AWAKE_SEC,
        .min_batt_pct = CONFIG_MAGTAG_OTA_MIN_BATT_PCT,
        .max_fails = CONFIG_MAGTAG_OTA_MAX_FAILS,
        .min_free_heap = CONFIG_MAGTAG_OTA_MIN_FREE_HEAP,
        .running_version = esp_app_get_description()->version,
    };
    /* Boot sub-phase 3 of 6. ota_flow_init() runs the rollback detector,
       and what that detector actually does is an NVS READ and, on an
       ordinary boot, nothing else: note_rollback_if_reverted() reads the
       ota_pend_ver key and compares it against s_cfg.running_version. It
       does NOT read partition state — esp_ota_get_running_partition()
       and esp_ota_get_state_partition() live in ota.c and are reached
       later, through .mark_valid — and it does NOT write either way: an
       empty key returns before any flash is spent saying so. The writes
       happen only on the boot after something was committed.

       Which is the reason it earns a label rather than an argument
       against one. It is the only part of boot whose behaviour depends
       on the PREVIOUS boot having been an OTA, so "did it die in the
       rollback detector?" is a question worth answering separately from
       the rest of init — and the boot that answers yes is the boot that
       is doing the flash work. */
    const panic_phase_t boot_prev_ota = panic_diag_enter(PANIC_PHASE_BOOT_OTA);
    ota_flow_init(&OTA_FLOW_OPS, &ota_cfg);
    panic_diag_exit(PANIC_PHASE_BOOT_OTA, boot_prev_ota);

    buttons_init();

    /* Boot sub-phase 4 of 6. Two ADC calls and nothing else, which is
       exactly why it gets its own label rather than sharing one with the
       charge gate below: if a panic ever reports BOOT_BATT, the search
       space is a oneshot unit handle and a channel config. Kept separate
       from BOOT_LOCK for that reason and because display_init() runs
       between them — one phase could not have covered both without
       swallowing the framebuffer allocation as well. */
    const panic_phase_t boot_prev_batt = panic_diag_enter(PANIC_PHASE_BOOT_BATT);
    battery_init();
    panic_diag_exit(PANIC_PHASE_BOOT_BATT, boot_prev_batt);

    /* audio + light init lazily on first use (most wakes need neither);
       until then the amp pin stays under its deep-sleep hold (off). */

    /* Boot sub-phase 5 of 6, and still the leading suspect — but not for
       the reason first written here. The LVGL draw buffer is not "the
       largest allocation on the device"; it is not an allocation at all.
       display.c's s_lvbuf is a link-time static in .bss, 8 + 296*128/8 =
       4,744 B, and it costs the runtime heap nothing. What genuinely
       runs on the heap in here is lv_init()'s own allocator pool and
       lv_display_create(), sitting on top of an SSD1680 bring-up that
       drives GPIO, an SPI bus and a hardware BUSY wait.

       It keeps the "leading suspect" billing on those grounds instead:
       this is the only boot phase that both talks to a peripheral over a
       bus and asks a third-party library to stand up its own allocator,
       and a fault in either is indistinguishable from every other boot
       fault while they all report BOOT. The label is "BOOT_DISP" and not
       "BOOT_DISPLAY" for a hard reason, spelled out in panic_diag.h: a
       main-slot label gets nine characters, because "BOOT_DISP" has to
       be able to sit next to "OTA_CHECK" inside DIAG_PHASE_MAX. */
    const panic_phase_t boot_prev_disp = panic_diag_enter(PANIC_PHASE_BOOT_DISP);
    display_init();
    panic_diag_exit(PANIC_PHASE_BOOT_DISP, boot_prev_disp);

    /* Heap headroom check: the LED + network task stacks now ride
       alongside WiFi and the LVGL framebuffer — regressions show up here
       long before an alloc fails in the field. */
    const uint32_t heap_free = esp_get_free_heap_size();
    const uint32_t heap_min = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "free heap after init: %lu B (min ever %lu B)", (unsigned long)heap_free, (unsigned long)heap_min);

    uint32_t causes = esp_sleep_get_wakeup_causes();
    /* Reset reason distinguishes a real cold boot from an external reset
       (e.g. monitor DTR/RTS) — both report wake cause UNDEFINED. */
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Wakeup causes: 0x%08lx, reset reason: %d", (unsigned long)causes, (int)reset_reason);

    /* Boot sub-phase 6 of 6, and by far the heaviest. Battery gate
       before any wake work: does not return while locked. On the locked
       path lock_gate_check_charge() pauses the timer, runs a FULL e-ink
       refresh (display_charge_me), opens an entire network window
       (net_apply_try_window — wifi, SNTP, the update check, MQTT) and
       then enters deep sleep — all of it inside what used to be
       undifferentiated BOOT. That is also why "BOOT_LOCK+OTA_CHECK" is a
       genuinely reachable label rather than a theoretical worst case,
       and it is the pairing that fills the published field to the byte.

       No exit on the locked path, and that is correct: the function does
       not return, so the breadcrumb should still read BOOT_LOCK if the
       device dies in there. The exit below only runs when the gate
       declined to lock. */
    const panic_phase_t boot_prev_lock = panic_diag_enter(PANIC_PHASE_BOOT_LOCK);
    lock_gate_check_charge();
    panic_diag_exit(PANIC_PHASE_BOOT_LOCK, boot_prev_lock);

    /* Reason 3 covers the READ above and stops there. The causes register
       is boot-scoped — valid only until something re-arms a wake source —
       so esp_sleep_get_wakeup_causes() has to be called here; but what
       the value MEANS is a decision, and it used to be an `if` in this
       file choosing between two non-returning handlers, which is the
       largest fork in the firmware and had no test at all. The decode now
       lives in wake_flow.c with one. Does not return. */
#if MAGTAG_PANIC_SOAK
    /* Reset-loop soak, compiled out unless a human sets MAGTAG_PANIC_SOAK
       to 1 in include/panic_soak.h. That header carries the whole
       argument — why a #define rather than a Kconfig symbol (this
       project's sdkconfig is hand-maintained and a reconfigure would
       rewrite it), what the evidence looks like without a network, and
       the fact that display_init() runs every iteration while no render
       ever does.

       The POSITION is the content, exactly as for the phase marks above:
       here and nowhere else is the point where the entire BOOT window
       has run and nothing of the wake has. Restarting from here loops
       the span under investigation at maximum rate; restarting later
       would drag the wake handler in, and it does not return. */
    const int soak_settle_ms = MAGTAG_PANIC_SOAK_SETTLE_MS;
    ESP_LOGE(TAG, "PANIC SOAK: boot window survived; restarting in %d ms", soak_settle_ms);
    vTaskDelay(pdMS_TO_TICKS(soak_settle_ms));
    esp_restart();
#endif

    /* Boot is over; everything past this line is the wake itself. Marked
       here rather than inside wake_flow.c because this is the exact
       boundary — wake_flow_handle_wake() does not return, so there is no
       other point that means "init finished". */
    (void)panic_diag_enter(PANIC_PHASE_AWAKE);
    wake_flow_handle_wake(causes);
}
