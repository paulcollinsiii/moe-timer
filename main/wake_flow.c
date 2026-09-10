/* The wake orchestration, lifted out of main.c so its edges carry tests.
   What each entry point guarantees, and why, is in wake_flow.h; what
   lives here is the flow. */
#include "wake_flow.h"

#include "alerts.h"
#include "app_state.h"
#include "audio.h"
#include "battery.h"
#include "bedtime.h"
#include "button_actions.h"
#include "button_latch.h"
#include "buttons.h"
#include "config_cache.h"
#include "display.h"
#include "hal_time.h"
#include "light.h"
#include "lock_gate.h"
#include "mqtt_ha.h"
#include "neopixel.h"
#include "net_apply.h"
#include "net_window.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "ota_flow.h"
#include "ota_task.h"
#include "sleep_plan.h"
#include "status_led.h"
#include "time_util.h"
#include "timer.h"
#include "timer_persist.h"
#include "wake_policy.h"

#ifndef NATIVE
#include "esp_app_desc.h" /* esp_app_get_description(), for the stat snapshot */
#include "esp_attr.h"     /* RTC_DATA_ATTR */
#include "esp_bit_defs.h" /* BIT() */
#include "esp_log.h"
#include "esp_sleep.h"  /* esp_sleep_source_t, for the wake-cause decode */
#include "esp_system.h" /* esp_reset_reason() */
#else
/* The host build has no esp_system.h; wake_flow.h only pulls it in on
   device. The reset reason gates the render-grid wait, so the suite has
   to be able to answer it. */
esp_reset_reason_t esp_reset_reason(void);

/* Nor esp_app_desc.h. The stat snapshot publishes the running firmware
   version, so the suite has to be able to answer this too; the struct
   itself is the host shim in test/mocks/esp_compat.h, alongside the
   reset-reason enum, because the stub has to name the type before this
   file is included. */
const esp_app_desc_t *esp_app_get_description(void);

/* Wake causes, in ESP-IDF's declaration order so the bit positions match
   the real enum (esp_sleep.h) — the same arrangement esp_compat.h uses
   for the reset reasons. Trimmed to what the decode names; the causes
   this device cannot produce (touchpad, ULP, UART, BT) exist only on
   silicon and would land on the same tick arm there.

   Note what the suite can and cannot prove with these: the DECODE is
   tested here, but no test can catch a wrong VALUE, because it reads the
   constant from this same enum and drift moves both sides of the
   comparison together. That gap is closed by the _Static_assert below,
   not by a test — do not renumber these without reading it. */
typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_ALL,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
} esp_sleep_source_t;
#define BIT(nr) (1UL << (nr))

/* These discard their varargs, which is only half the story: on DEVICE
   ESP_LOGx also wraps its arguments in a compile-time level guard, so a
   call left in a log argument stops being executed there too as soon as
   the level is compiled out. Host and device agree by accident, which is
   why no test and no differential sweep can see it.
   The census that used to live here — seven pure reads, maintained by
   hand, and never propagated to the other files that needed it — has been
   replaced by scripts/check-log-args.py, which refuses the whole class at
   pre-commit time and carries the allowlist. There is nothing to keep in
   step here any more: put a call in a log argument and the hook says so. */
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

/* Deliberately OUTSIDE the split above, which is what makes it a pin
   rather than a restatement: on device this checks the real esp_sleep.h,
   on host it checks the fallback, and both are held to the same numbers.
   The suite alone cannot catch a wrong constant — it takes
   BIT(ESP_SLEEP_WAKEUP_EXT1) from the same enum as the decode, so drift
   moves both sides of the comparison together and every wake-cause test
   keeps passing while asserting the inverse of what the silicon does.
   Values are esp_sleep.h's as of IDF 6.0.1. If IDF ever renumbers, the
   firmware build is where you want to find out. */
_Static_assert(ESP_SLEEP_WAKEUP_EXT1 == 3, "wake-cause decode assumes IDF's numbering; host fallback mirrors it");
_Static_assert(ESP_SLEEP_WAKEUP_TIMER == 4, "wake-cause decode assumes IDF's numbering; host fallback mirrors it");

/* IDLE shows only the clock — sync on the menuconfig cadence (default
   hourly) instead of every 10 min. The S2 has no crystal-backed RTC; its
   RC-oscillator timekeeping can drift minutes/day, so don't set this too
   long. The fallback matches the Kconfig default and only ever applies to
   the host build, which has no sdkconfig — same shape timer_defs.c uses
   for its slot definitions. */
#ifndef CONFIG_MAGTAG_IDLE_SYNC_INTERVAL_MIN
#define CONFIG_MAGTAG_IDLE_SYNC_INTERVAL_MIN 60
#endif
#define IDLE_SYNC_INTERVAL_SEC (CONFIG_MAGTAG_IDLE_SYNC_INTERVAL_MIN * 60)

static const char *TAG = "wake_flow";

/* The post-action render half, defined with the rest of the tail at the
   bottom of this file. Declared here because the break tail's press poll
   — which sits with the other guard-matrix entry points above it —
   reaches it after a dispatched press, and the tail in turn calls the
   expiry alert and the break-end drain that are declared further down. */
static void render_action_result(button_id_t btn, timer_state_t before, time_t now, bool selection_changed);

/* ---- the two renders this module used to reach through main.c ----------

   Both were seams declared in wake_flow.h and implemented in main.c, on
   the grounds that the battery ADC read at the bottom of them "has no
   host answer". The residency audit's review struck that reason: it is
   not one of the four the rule lists, and lock_gate.c already reads the
   same ADC and is host-tested (test_lock_gate stubs battery_read_mv).
   With that gone neither could name an admissible reason, so both moved
   here, where every other render in the module already lives.

   Both are static: nothing outside this file called either of them, and
   an exported symbol with no out-of-module caller is a seam that exists
   only to be stubbed. The suite reaches them the same way it reaches
   paint_break_started() — through the stubs one level down
   (battery_read_mv, app_state_display, display_full_refresh), which is
   what pushes them from "trusted" to "tested". */

/* The state assembly. main.c had this as make_state() plus a one-line
   make_display_state() thunk over it; with no ABI to adapt to on this
   side the thunk collapses into the body it wrapped.

   Only the battery ADC read is device-side — the assembly rules are
   app_state.c's and are host-tested there. Light and reset reason are
   stats-only and deliberately not read here: no extra ADC work per paint.
   fw_version is NOT in that category — the main screen renders it on the
   battery row, so it must be read on the paint path too. It is a cheap
   pointer into the app descriptor in flash, not a device read, so the
   "no extra work per paint" argument never applied to it. It was omitted
   here when the field was added and the version silently rendered blank
   on hardware; test_paint_carries_the_firmware_version pins it.

   Unchanged by the move APART FROM THE LOG TAG: the debug line below used
   to print under main.c's TAG="main" and now prints under "wake_flow".
   Serial output only. */
static display_state_t make_display_state(int32_t remaining, time_t now) {
    int mv = battery_read_mv();
    /* Hoisted out of the ESP_LOGD argument (HAZ-1). This one is the live
       case, not the hypothetical: CONFIG_LOG_MAXIMUM_LEVEL is INFO, so the
       call inside the argument list was already not running on the device.
       Hoisting makes the conversion happen unconditionally — a real
       behaviour change, in the safe direction (a pure integer map that now
       runs where it previously did not), and the only way the line means
       what it says at any log level. */
    const int batt_pct = battery_percent_from_mv(mv);
    (void)batt_pct; /* the host stub discards its varargs */
    ESP_LOGD(TAG, "battery: %d mV (%d%%)", mv, batt_pct);
    app_state_in_t in = {
        .batt_mv = mv,
        .fw_version = esp_app_get_description()->version,
    };
    return app_state_display(&in, remaining, now);
}

/* Tick the timer and full-refresh the panel with the result. Reached by
   the break-end owner after it drains an edge, and by the expiry alert
   once the alarm has finished; the ORDER — drain first, then this — is
   the caller's and is tested at the caller.

   Character for character main.c's body, with one substitution: its
   `time(NULL)` is `hal_time_now()` here, which wraps time(NULL) in
   production (hal_time.c) and so is the same call on device. Every other
   clock read in this module already goes through it, and a raw time(NULL)
   in a host-built TU would read the real wall clock straight past the
   suite's injected one. timer_tick() stays exactly where it was: inside
   the assembly's argument, so the tick and the number painted cannot
   drift apart. */
static void paint_current_state_full(void) {
    time_t now = hal_time_now();
    display_state_t st = make_display_state(timer_tick(now), now);
    display_full_refresh(&st);
}

/* ---- the OTA seams ------------------------------------------------------ */

/* ota_flow_ops_t's `repaint`: a failed download owes the panel the normal
   screen back, and this is the same body the break-end and expiry-alert
   tails already reach.

   Exposed, and that is NOT a reversal of the residency decision recorded
   at the bottom of wake_flow.h. What that decision says is that main.c no
   longer IMPLEMENTS these seams — the four it names moved here and stay
   here. This is the other pattern that header already describes, for
   wake_flow_fire_expiry_alert and wake_flow_post_stats_snapshot: an entry
   point that is ADDRESS-TAKEN by a composition-root ops table and
   therefore has to have external linkage. paint_current_state_full stays
   static; main.c gets a name, not the paint.

   Runs on the OTA task rather than the main task (ota_task.c). Serialised
   rather than concurrent: the main task is blocked inside
   ota_task_run_apply for the whole attempt, so the tick, the ADC read and
   the panel flush underneath have the same single-owner guarantees they
   have everywhere else in this file. The timer_tick() is safe to repeat —
   it ticks to now — and enter_deep_sleep's timer_persist_save() still runs
   afterwards, on the main task, once the join has returned. */
void wake_flow_repaint_current_state(void) {
    paint_current_state_full();
}

/* State of charge for the OTA gates, with a failed ADC read kept distinct
   from a flat cell.

   battery.h documents <= 0 mV as a failed read, and battery_percent_from_mv
   maps everything at or below 3300 mV to 0 %. Passing that 0 to the gate
   would read as "below the battery floor" and would refuse every update
   check forever, silently, on a device whose ADC broke. ota_gate_in_t
   spells out the alternative it wants instead: batt_pct < 0 means
   "unreadable" and does NOT gate, because the charge lock is the real
   protection and a gate that cannot tell the two apart reports the wrong
   reason to Home Assistant. */
static int ota_batt_pct(void) {
    int mv = battery_read_mv();
    return (mv <= 0) ? -1 : battery_percent_from_mv(mv);
}

/* ALLOWLISTED in scripts/check-log-args.py — see the note on
   ota_policy_reason_str(). Must stay a pure enum -> string literal map.
   Note the allowlist covers THIS function only: esp_reset_reason(), which
   reads a hardware register, is hoisted at both of its log sites. */
const char *wake_flow_reset_reason_str(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP";
        case ESP_RST_POWERON:
            return "POWERON";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_PANIC:
            return "PANIC";
        case ESP_RST_INT_WDT:
            return "INT_WDT";
        case ESP_RST_TASK_WDT:
            return "TASK_WDT";
        case ESP_RST_WDT:
            return "WDT";
        case ESP_RST_SW:
            return "SW";
        case ESP_RST_EXT:
            return "EXT";
        default:
            return "UNKNOWN";
    }
}

/* How long to stay off the console after a panic. Long enough for a host
   port-open to complete against silence, short enough that a device stuck
   in a genuine panic loop still reboots visibly rather than looking
   dead. */
#define PANIC_QUIET_MS 2000

void wake_flow_boot_quiet_after_panic(void) {
    if (esp_reset_reason() != ESP_RST_PANIC) {
        return;
    }
    hal_delay_ms(PANIC_QUIET_MS);
}

/* ---- break end --------------------------------------------------------- */

/* A Screen Break runs on slot 0 and keeps running behind whatever timer
   is selected, so its end is an event in its own right: the panel changes
   (the BREAK chip vanishes, and a chiming end also snaps the selection
   back) without the active slot's state changing at all. Sticky for the
   whole wake — every render after it must be a full refresh. A plain
   static: the next wake is a fresh boot. */
static bool s_break_ended;

bool wake_flow_break_ended_this_wake(void) {
    return s_break_ended;
}

bool wake_flow_break_end(void) {
    time_t now = hal_time_now();
    timer_break_tick(now);
    int32_t overdue = 0;
    if (!timer_break_take_ended(now, &overdue)) {
        return false;
    }
    s_break_ended = true;

    bool extra_running = timer_any_extra_running();
    if (!wake_policy_break_chime(extra_running, overdue)) {
        ESP_LOGI(TAG, "Break ended silently (%s, %ld s late)", extra_running ? "timer running" : "observed late",
                 (long)overdue);
        return true;
    }

    audio_break_over_chime();
    /* The chime and the return are the same event: the break is over, so
       you go back to whatever it interrupted — which is not necessarily
       Screen, since a break can now be earned entirely by a non-eligible
       extra (rule 8). Cannot be refused here: a refusal means a RUNNING
       timer, which suppressed the chime above. */
    int interrupted = timer_break_interrupted_slot();
    if (timer_active_slot() != interrupted && timer_select_interrupted()) {
        /* Read AFTER the select — the log reports where selection landed,
           not where it started — and outside the argument list (HAZ-1). */
        const int selected = timer_active_slot();
        (void)selected;
        ESP_LOGI(TAG, "Break over: chimed, selection back to slot %d", selected);
    } else {
        ESP_LOGI(TAG, "Break over: chimed");
    }
    return true;
}

/* Deliberately a RAW take and not wake_flow_break_end(): by the time
   enter_deep_sleep() calls this the panel is done and the network task is
   gone, so every side effect the owner above performs — the tick, the
   chime, the snap back, the wake-sticky flag — would land after the last
   render and be seen by nobody. What is left is the report.

   Behaviour is unchanged by the move out of main.c APART FROM THE LOG
   TAG: this warning used to print under main.c's TAG="main" and now
   prints under "wake_flow". Serial output only — nothing parses it — but
   it is the one observable difference, so it is recorded here rather
   than left for someone to find while grepping a capture. */
bool wake_flow_report_undrained_break_end(void) {
    int32_t overdue = 0;
    if (!timer_break_take_ended(hal_time_now(), &overdue)) {
        return false;
    }
    ESP_LOGW(TAG, "break end reached sleep undrained (%ld s late)", (long)overdue);
    return true;
}

bool wake_flow_break_end_repaint(void) {
    /* Drain BEFORE the paint, never after: the paint reads the ACTIVE
       slot, and a drain that snaps the selection back changes which slot
       that is. Painting first rendered the previous timer's remaining
       under the snapped-back timer's name and allocation. */
    if (!wake_flow_break_end()) {
        return false; /* nothing ended, or it was already consumed this wake */
    }
    paint_current_state_full();
    return true;
}

/* ---- the button guard matrix ------------------------------------------- */

/* What each entry point guarantees is in wake_flow.h — in particular why
   dispatch's signature is shaped the way it is, which is the part a
   future reader is most likely to want to "clean up". */

bool wake_flow_dispatch_button_action(button_id_t btn, time_t *now, timer_state_t before, bool allow_net_window,
                                      bool *selection_changed) {
    *selection_changed = false;
    switch (btn) {
        case BTN_A:
            if (before == TIMER_BREAK) {
                ESP_LOGI(TAG, "button A ignored during screen break");
                return false;
            }
            /* Start/resume immediately — waiting on NTP first confused
               users. Sync runs after; any clock step is applied to the
               expiry via timer_shift_expiry (measured against the
               monotonic clock, which NTP cannot step). */
            switch (button_b_apply(*now)) {
                case BTN_B_STARTED:
                case BTN_B_RESUMED:
                    /* Hold the pre-press colour briefly so the WHITE/AMBER ->
                       GREEN transition is visible as an acknowledgement */
                    hal_delay_ms(250);
                    status_led_show_timer_state();

                    /* NTP-gated paint: wait only for the sync (seconds) so the
                       panel renders once, with the corrected clock and shifted
                       expiry. The MQTT phase is released AFTER the paint (the
                       snapshot post in the finish tail) and joined before
                       sleep. Fail-open: on sync failure the timer keeps
                       running on the uncorrected clock — remaining time is
                       still a consistent duration; only the shown clock may
                       be off. If the sync settles late (during the MQTT
                       tail), the finish applies the step instead. */
                    if (allow_net_window && net_apply_open()) {
                        if (net_window_wait_ntp()) {
                            timer_shift_expiry(net_window_take_clock_step());
                        } else {
                            net_apply_note_start_unsynced();
                        }
                    }
                    *now = hal_time_now();
                    return true;
                case BTN_B_PAUSED:
                    return true;
                case BTN_B_RELOADED:
                    /* Bare, like the pause arm and like the reload the
                       BTN_B arm below reaches: the slot really did move
                       (EXPIRED -> IDLE at full duration), so the caller
                       owes it an LED and a repaint. Reporting false here
                       would let the same timer_reload() mean "acted" or
                       "did nothing" depending only on which button got
                       there. */
                    return true;
                default:
                    return false; /* BTN_B_NONE — BREAK, or an EXPIRED slot that
                                     cannot reload: renders only (wake path) */
            }
        case BTN_B:
            /* Reset the selected timer to full: only a reloadable extra
               qualifies, and never while RUNNING (B is dropped from the
               wake mask then, same as C; this guard covers presses that
               ride in on another wake). */
            if (!timer_reload_allowed() || !timer_reload()) {
                ESP_LOGI(TAG, "Button B reset unavailable (state %d)", (int)before);
                return false;
            }
            return true;
        case BTN_C:
            /* Swap timer type; refused only while RUNNING (pause first).
               A Screen Break deliberately does NOT refuse — going and
               running Piano is what the break time is for.

               Report the swap instead of rewriting `before`: landing on
               an already-EXPIRED timer must not re-fire its alert, but
               `before` is also the only record of WHICH LAYOUT was
               painted, and both directions of a swap during a break cross
               the full-screen inversion. Overwriting it made those
               renders partial, which ghosts the panel. */
            if (timer_select_next()) {
                const int selected = timer_active_slot(); /* after the swap */
                (void)selected;
                ESP_LOGI(TAG, "button C: selected slot %d", selected);
                *selection_changed = true;
                return true;
            }
            ESP_LOGI(TAG, "button C swap unavailable (state %d)", (int)before);
            return false;
        default:
            return false;
    }
}

bool wake_flow_poll_pause_button(void) {
    /* Masked take: only the A bit is consumed — latched B/C presses stay
       in the latch for the tick-wake drain (a poll during the grid wait
       must not eat them).

       The take is unconditional and runs AHEAD of the state guard. That
       ordering is observable in exactly one direction: an A press made
       while the timer is not RUNNING is consumed here and thrown away,
       so nothing later in the wake can act on it.

       It is NOT what prevents a double pause. timer_pause() moves
       RUNNING -> PAUSED, so a second poll is turned away by its own
       state guard whichever order these two lines are in — guard-first
       pauses exactly once too. The only thing the shipped order buys is
       the discarded press above, and that is a known latent defect, not
       a feature: wake_flow_wait_for_render_grid() can poll for up to
       25 s, during which an A press while PAUSED/IDLE/EXPIRED does
       nothing at all. Left exactly as it shipped and pinned by
       test_row6_a_press_while_not_running_is_eaten_KNOWN_BUG (BUG-2 in
       docs/planning/refactor.bugdiscoveries.md); fixing it is a behaviour
       change and belongs in its own commit, which must flip that test
       deliberately. */
    bool a_pressed = buttons_take_pressed_mask(1u << BTN_A) != 0;
    if (timer_get_state() != TIMER_RUNNING || !a_pressed)
        return false;
    time_t now = hal_time_now();
    timer_pause(now);
    ESP_LOGI(TAG, "button A while awake: paused");
    return true;
}

bool wake_flow_poll_button_a_action(void) {
    if (buttons_take_pressed_mask(1u << BTN_A) == 0)
        return false;
    timer_state_t st = timer_get_state();
    /* The log line below is st's only reader, and it compiles away on the
       host — where this module, unlike main.c, IS built. Explicitly
       consumed rather than deleted: on device it is the pre-apply half of
       "state %d -> %d", which is how a mis-mapped press is diagnosed. */
    (void)st;
    if (button_b_apply(hal_time_now()) == BTN_B_NONE)
        return false;
    const timer_state_t after = timer_get_state(); /* post-apply half of the pair */
    (void)after;
    ESP_LOGI(TAG, "button A during join: state %d -> %d", (int)st, (int)after);
    status_led_show_timer_state();
    return true;
}

bool wake_flow_poll_break_buttons(void) {
    /* Unmasked take, unlike the two polls above: this is the last
       consumer before sleep, so anything left latched is discarded
       anyway. D is excluded from the PICK rather than from the take. */
    int pick = button_latch_pick(buttons_take_pressed(), (1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C));
    if (pick < 0)
        return false;
    time_t now = hal_time_now();
    timer_state_t before = timer_get_state();
    bool swapped = false;
    if (!wake_flow_dispatch_button_action((button_id_t)pick, &now, before, false, &swapped))
        return false;
    status_led_show_timer_state();
    render_action_result((button_id_t)pick, before, now, swapped);
    return true;
}

/* ---- the eye-rest break gate -------------------------------------------- */

/* The break screen's paint, and the ORDER is the point: the LED is lit
   BETWEEN the state assembly and the flush, which is what holds the panel
   blue for the whole multi-second e-paper refresh instead of only after
   it. Lit after the refresh it would be a blink nobody sees; that is
   behaviour, not formatting, so it is pinned by a test here.

   Lived in main.c until the residency audit moved it: the battery ADC
   read underneath admits a device CALL in the composition root, but not
   an ordering, and an ordering is a decision. make_display_state() —
   which followed it out of main.c when the audit's own review found that
   the ADC admitted nothing either — is how this reaches the read, exactly
   as the five other renders in this file do.

   Deliberately not paint_current_state_full() with an argument. That one
   re-reads the wall clock, where a starting break must paint against the
   instant it started — the gate's callers do not agree on which instant
   they mean, and two of them are ~15 s stale by the time they ask. */
static void paint_break_started(time_t now) {
    display_state_t st = make_display_state(timer_tick(now), now);
    status_led_show_timer_state(); /* blue during the refresh */
    display_full_refresh(&st);     /* inverted SCREEN BREAK layout */
}

/* Returns true when a break was started (caller should go straight to
   sleep). Persists BREAK before the alarm, same rationale as the EXPIRED
   at-transition save. */
bool wake_flow_maybe_start_break(time_t now) {
    uint16_t interval_min = NVS_DEFAULT_BREAK_INTERVAL_MIN, duration_min = NVS_DEFAULT_BREAK_DURATION_MIN;
    nvs_config_get_break_interval_min(&interval_min);
    nvs_config_get_break_duration_min(&duration_min);
    if (interval_min == 0) /* eye-rest breaks disabled */
        return false;
    if (!timer_break_due(now, (int32_t)interval_min * 60))
        return false;
    /* A break that would still be running at bedtime is pointless - the
       device would lock mid-break. Skip it and go straight to Bed Time,
       audibly (this is the one alerting path that starts before the
       threshold itself is reached). */
    if (bedtime_break_would_cross(time_util_minutes_of_day(now), (int)duration_min, config_cache_bedtime_minutes())) {
        ESP_LOGW(TAG, "Screen break due but would cross bed time");
        lock_gate_bedtime_engage(now, true); /* no return */
    }
    /* Before timer_start_break() rebases the balance, and outside the log
       argument list (HAZ-1). */
    const int32_t accum = timer_run_accum(now);
    (void)accum;
    ESP_LOGI(TAG, "Screen break due (accum %ld s)", (long)accum);
    timer_start_break(now, (int32_t)duration_min * 60);
    timer_persist_save();
    paint_break_started(now); /* blue LED through the inverted SCREEN BREAK refresh */
    alert_run(ALERT_BREAK);   /* pulse end darkens the pixels */
    return true;              /* caller sleeps; stop_sync guards the gate */
}

/* ---- the expiry alert --------------------------------------------------- */

void wake_flow_fire_expiry_alert(void) {
    /* Persist EXPIRED before the ~15 s alert + redraw, not at the eventual
       enter_deep_sleep: an EN reset or power cut mid-alert would otherwise
       restore the stale RUNNING snapshot and replay the final minute. */
    timer_persist_save();
    display_timesup();
    alert_run(ALERT_EXPIRY);
    /* Back to the main layout. The tail is character for character the
       break-end repaint's paint half — re-read the clock, tick, full
       refresh — so it reaches the panel through that same seam instead of
       carrying a second copy of it. */
    paint_current_state_full();
}

/* ---- day rollover ------------------------------------------------------- */

/* Yesterday's usage numbers for HA, captured BEFORE the rollover resets
   the slots; published by the rollover's own network window. */
static void queue_rollover_summary(void) {
    if (timer_current_date()[0] == '\0') {
        return; /* cold boot / restored-from-nothing: no day to report */
    }
    int32_t used = timer_screen_used_sec(hal_time_now());
    uint16_t comp[TIMER_EXTRA_SLOTS];
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        comp[i] = timer_slot_completions(1 + i);
    }
    mqtt_ha_queue_summary(timer_current_date(), used, comp);
}

void wake_flow_handle_day_rollover(time_t *now) {
    if (!timer_is_new_day(*now))
        return;
    /* last_date + wall time in the log: if a rollover ever fires when the
       date has NOT actually changed, this pinpoints why (bad stored date
       vs. stepped clock). */
    const char *last_date = timer_current_date();
    (void)last_date;
    ESP_LOGW(TAG, "Day rollover (last_date='%s', now=%lld)", last_date, (long long)*now);
    queue_rollover_summary();    /* yesterday's stats, before any reset */
    mqtt_ha_queue_bonus_clear(); /* clear the retained HA bonus target this window */
    /* The update check rides the window opened on the next line, so the
       arm has to precede it: ota_flow_check runs on the network task and
       reads what is sampled here. The rollover is THE checking trigger —
       the device is already awake, already opening a radio session, and
       nobody is waiting on the panel.

       Battery and charge-lock are sampled on this (the main) task
       because the network task must not touch the ADC or the lock gate.
       They gate the check only; the download re-samples its own. */
    ota_flow_arm(OTA_TRIGGER_ROLLOVER, ota_batt_pct(), lock_gate_charge_locked());
    /* Fail-open: reset to IDLE with today's allocation even if sync fails */
    net_apply_try_window();
    *now = hal_time_now();
    /* Power cycling must not refund the allocation: with the clock now
       corrected, a same-day NVS snapshot beats a reset. Only a genuine
       date change resets the day. */
    if (timer_persist_try_restore(*now)) {
        return;
    }
    timer_reset();
    timer_record_date(*now);
}

/* ---- the awake watches -------------------------------------------------- */

/* The planner lands the pre-event wake ~SLEEP_PLAN_EVENT_LEAD_SEC out;
   any wake inside SLEEP_PLAN_WATCH_SEC stays awake so the expiry (TIME'S
   UP) or break end (chime + PAUSED) fires within a tick of wall time. */

/* Absorb the wake residue so the render lands on the state's grid:
   RUNNING/BREAK on the countdown's round minute (the display truly reads
   1:11:00), clock-only states on the wall :00. Bounded — wakes that are
   legitimately off-grid (event watch handoff, slow sync) render where
   they are and self-correct next cycle. Aborts early on a pause press
   (the caller then renders PAUSED, off-grid but honest). */
void wake_flow_wait_for_render_grid(int max_wait_sec) {
    time_t now = hal_time_now();
    timer_state_t st = timer_get_state();
    int32_t event_remaining = 0;
    if (st == TIMER_RUNNING) {
        event_remaining = (int32_t)(timer_expiry_wall() - (int64_t)now);
    } else if (st == TIMER_BREAK) {
        event_remaining = timer_break_remaining(now);
    }
    int32_t to = wake_policy_grid_wait_sec(st, event_remaining, (int)(now % 60), max_wait_sec);
    for (int32_t i = 0; i < to * 10; i++) {
        if (wake_flow_poll_pause_button())
            return;
        hal_delay_ms(100);
    }
}

/* BREAK tail: stay awake through the last seconds so the end (chime +
   repaint, and the snap back to Screen) lands within a tick of wall time.
   Keyed on slot 0, so it covers a break running behind another selected
   timer just as well as the break screen itself. */
/* The tail's press poll is wake_flow_poll_break_buttons: a press it
   accepts has already repainted, so the watch stops there and the sleep
   planner re-schedules the break end — or, if the press started an
   eligible extra, the end is suppressed and there is nothing left to wait
   for, exactly as the top-of-watch guard below decides. */
void wake_flow_watch_break_end(void) {
    if (!timer_break_active())
        return;
    if (timer_any_extra_running()) {
        /* Suppressed end: no chime, no snap — there is nothing to wait
           for. The chip simply disappears at the next tick wake. */
        return;
    }
    int32_t brem = timer_break_remaining(hal_time_now());
    if (brem > SLEEP_PLAN_WATCH_SEC)
        return; /* not our tail; the planner will wake us closer */
    if (brem > 0) {
        ESP_LOGI(TAG, "Break ends in %ld s: staying awake", (long)brem);
        status_led_show_timer_state();
        while (timer_break_remaining(hal_time_now()) > 0) {
            if (wake_flow_poll_break_buttons())
                return; /* repainted; the planner owns the end from here */
            hal_delay_ms(250);
        }
    }
    wake_flow_break_end_repaint(); /* chime + snap back, then repaint */
}

/* RUNNING tail: own the final minute — countdown partials, binary LEDs,
   the pause poll, and the expiry alert at zero. */
void wake_flow_watch_final_minute(void) {
    time_t now = hal_time_now();
    int64_t remaining = timer_expiry_wall() - (int64_t)now;
    if (remaining <= 0 || remaining > SLEEP_PLAN_WATCH_SEC)
        return;

    ESP_LOGI(TAG, "Final minute: staying awake (%lld s remaining)", (long long)remaining);
    buttons_take_pressed(); /* only presses made DURING the watch may pause */
    /* Expiry is a wall time, so a clock step here directly sharpens the
       moment the alert fires. Skip when recently synced or when the sync
       itself (~5-9 s) would blow past the expiry. */
    if (timer_needs_ntp_sync(now) && remaining > 15) {
        net_apply_try_window();
        /* The window's reconcile may have reset/expired the timer (config
           edit); the alert (if any) already fired — don't watch a countdown
           that no longer exists, and never double-fire the alert below. */
        if (timer_get_state() != TIMER_RUNNING) {
            return;
        }
    }
    status_led_show_timer_state();

    /* Break config read once — the loop below spins at 250 ms. Short
       allocations can put break-due INSIDE this watch (e.g. 3 min screen
       with a 2 min interval: due lands at exactly 60 s remaining); the
       per-wake check in the handlers has already passed by then, so the
       loop must keep checking or the break is silently swallowed by the
       expiry. */
    uint16_t break_interval_min = NVS_DEFAULT_BREAK_INTERVAL_MIN;
    nvs_config_get_break_interval_min(&break_interval_min);

    /* Countdown: partial display steps at the quarter-minute marks (the
       step schedule lives in wake_policy — a late wake skips passed
       marks), and the last 15 s on the pixels as a binary count (status
       class: light green, brightness-scaled, muted by quiet hours). */
    int64_t rem = timer_expiry_wall() - (int64_t)hal_time_now();
    int next_step = wake_policy_first_countdown_step((int32_t)rem);
    int32_t leds_shown = -1;
    while ((rem = timer_expiry_wall() - (int64_t)hal_time_now()) > 0) {
        /* The event watch owns the whole final minute — without this poll
           a pause press here would be lost and the expiry unavoidable. */
        if (wake_flow_poll_pause_button()) {
            neopixel_stop(); /* clear the binary-countdown pixels */
            time_t pnow = hal_time_now();
            display_state_t st = make_display_state(timer_tick(pnow), pnow);
            status_led_show_timer_state(); /* amber through the refresh until sleep */
            display_full_refresh(&st);
            return;
        }
        if (break_interval_min != 0 && timer_break_due(hal_time_now(), (int32_t)break_interval_min * 60)) {
            neopixel_stop(); /* clear the binary-countdown pixels */
            /* Back-to-back renders are safe: display.c absorbs the
               driver's refresh-rate guard interval instead of letting the
               frame be dropped. */
            if (wake_flow_maybe_start_break(hal_time_now())) {
                return; /* BREAK painted + alarm run; caller sleeps through it */
            }
        }
        if (next_step < WAKE_COUNTDOWN_STEPS && rem <= (int64_t)wake_policy_countdown_step(next_step)) {
            time_t step_now = hal_time_now();
            display_state_t st = make_display_state(wake_policy_countdown_step(next_step), step_now);
            display_update(&st); /* partial; ~2-3 s, well under the 15 s spacing */
            next_step++;
        }
        if (rem <= 15 && (int32_t)rem != leds_shown) {
            neopixel_status_binary4((uint8_t)rem, 20, 60, 20); /* light green */
            leds_shown = (int32_t)rem;
        }
        hal_delay_ms(250);
    }
    timer_tick(hal_time_now()); /* RUNNING -> EXPIRED */
    wake_flow_fire_expiry_alert();
}

/* ---- the post-action tail ----------------------------------------------- */

/* What each of these guarantees is on the declarations in wake_flow.h for
   the two that have one; the three statics below are reached only through
   the wake handlers at the bottom of this file. */

/* Fill a stat snapshot for the HA session: gather the four device reads
   the assembly cannot make for itself and hand them to app_state.c, where
   the assembly rules are host-tested. Deliberately side-effect free — no
   timer_tick, because a stat read must never transition the state
   machine; whether to collect at all is the caller's branch, below.

   Was main.c's, declared in wake_flow.h and called only from here, which
   made main.c implement a seam its own code never used. It claimed
   residency reason 3 on esp_app_get_description() and esp_reset_reason(),
   and neither is a HANDLE — the reason as written admits handles, so the
   claim was dead and the rule says such code moves. It did NOT move to
   app_state.c: that module takes batt_mv and light_mv as INPUTS and never
   reads an ADC, and pushing the reads down there would force its suite to
   grow device stubs, inverting a seam that is deliberate.

   Character for character main.c's body, with one substitution: its
   `time(NULL)` is `hal_time_now()` here, for the same reason as
   paint_current_state_full above — every clock read in this module goes
   through it, and a raw time(NULL) in a host-built TU would read the real
   wall clock straight past the suite's injected one. */
static void stats_collect(stats_snapshot_t *out) {
    app_state_in_t in = {
        .batt_mv = battery_read_mv(),
        .light_mv = light_read_mv(),
        .charge_locked = lock_gate_charge_locked(),
        .fw_version = esp_app_get_description()->version,
        .reset_reason = wake_flow_reset_reason_str(esp_reset_reason()),
    };
    app_state_stats(&in, hal_time_now(), out);
}

/* Collect and hand off the stats snapshot (no-op without a window). */
void wake_flow_post_stats_snapshot(void) {
    if (!net_window_active())
        return;
    stats_snapshot_t snap;
    stats_collect(&snap);
    net_window_post_snapshot(&snap);
}

/* Render half of the post-action tail, with no network work: drain a
   break end, tick, and paint the result under the render policy. Shared
   with the break-tail poll above, which runs after the window has already
   been joined and so must not touch the MQTT phase — which is why the
   render is its own half rather than the top of the function below. */
static void render_action_result(button_id_t btn, timer_state_t before, time_t now, bool selection_changed) {
    /* A break can elapse mid-wake (a slow sync, a long press sequence).
       Order-independent now that the edge is latched — this drains early
       so the chime accompanies THIS paint rather than the one after. */
    wake_flow_break_end();
    int32_t remaining = timer_tick(now);
    display_state_t st = make_display_state(remaining, now);

    /* Button D is the user-facing "refresh everything" button — it always
       gets a real full refresh regardless of the render policy. */
    bool force_full = (btn == BTN_D);
    /* One read feeds both the policy call and the log below. It has to
       leave the log argument list anyway (HAZ-1); sharing it with the
       policy call is the version that also guarantees the line reports the
       state the policy actually saw. Safe because wake_policy_render() and
       wake_flow_break_ended_this_wake() are both pure reads, so nothing
       between the two former call sites could have moved the state. */
    const timer_state_t after = timer_get_state();
    wake_render_t bwr = wake_policy_render(before, after, true, wake_flow_break_ended_this_wake(), selection_changed);
    if (bwr == WAKE_RENDER_EXPIRY_ALERT) {
        wake_flow_fire_expiry_alert(); /* alert owns the NeoPixels (red pulse) */
    } else {
        /* Includes EXPIRED: any button returns the display to the main
           layout (empty bar, TIME'S UP state). */
        ESP_LOGI(TAG, "button %d: state %d -> %d, %s refresh", (int)btn, (int)before, (int)after,
                 (force_full || bwr == WAKE_RENDER_FULL) ? "full" : "partial");
        status_led_show_timer_state(); /* resulting state, lit until sleep */
        if (force_full || bwr == WAKE_RENDER_FULL) {
            display_full_refresh(&st);
        } else {
            display_update(&st); /* partial cadence: every Nth is promoted */
        }
    }
}

/* Post-action tail shared by both wake handlers and the tick-wake latch
   drain: render the resulting state, release the MQTT phase, join the
   window, and re-render when the join changed what the panel shows. */
static void finish_action_and_render(button_id_t btn, timer_state_t before, time_t now, bool selection_changed) {
    render_action_result(btn, before, now, selection_changed);
    bool force_full = (btn == BTN_D);

    /* Paint done: release the MQTT phase (display refresh current and
       radio TX bursts must never coincide — brownout), then join, apply
       the buffered network→timer effects, reconcile a redefined timer.
       Re-render only when something changed what the panel shows (a
       Button A action landed during the join, a config edit moved the
       timer, or the expiry passed while draining). */
    wake_flow_post_stats_snapshot();
    timer_state_t painted = timer_get_state();
    net_finish_t nf = net_apply_finish();
    if (nf != NET_FINISH_ALERTED && (nf == NET_FINISH_CHANGED || timer_get_state() != painted)) {
        time_t rnow = hal_time_now();
        /* Drain BEFORE the tick that feeds the render: the window can
           span the break end, and a drain that snaps the selection back
           to Screen must be reflected in the remaining below — otherwise
           the Screen layout renders the previous timer's number. */
        wake_flow_break_end();
        int32_t rrem = timer_tick(rnow);
        /* selection_changed is false: `painted` already reflects the
           post-swap slot, and an expiry that landed DURING the window is
           a real transition that must still alert. */
        wake_render_t rwr =
            wake_policy_render(painted, timer_get_state(), true, wake_flow_break_ended_this_wake(), false);
        if (rwr == WAKE_RENDER_EXPIRY_ALERT) {
            wake_flow_fire_expiry_alert();
        } else {
            display_state_t rst = make_display_state(rrem, rnow);
            status_led_show_timer_state();
            if (force_full || rwr == WAKE_RENDER_FULL) {
                display_full_refresh(&rst);
            } else {
                display_update(&rst);
            }
        }
    }
}

/* Shared post-action tail: if the action pushed the accrual past the
   break interval (e.g. a resume landing after it), paint the break and
   sleep through it; otherwise render the action's result and drain the
   window. */
static void finish_or_break(button_id_t btn, timer_state_t before, time_t now, bool selection_changed) {
    /* Note the ordering: this runs BEFORE finish_action_and_render, whose
       tick is what would detect an expiry. So a break due here wins over
       a colliding expiry alert, which is the opposite of the tick
       handler's post-render check. Not shown to be reachable — the press
       that got here has just been dispatched, and the final minute
       belongs to wake_flow_watch_final_minute — but it is not a
       guarantee. */
    if (wake_flow_maybe_start_break(now)) {
        wake_flow_post_stats_snapshot(); /* break screen painted: release MQTT */
        net_apply_finish();              /* drain + apply deferred before sleeping */
        enter_deep_sleep(lock_gate_sleep_mode());
    }
    finish_action_and_render(btn, before, now, selection_changed);
}

/* ---- the pre-sleep event watch ------------------------------------------ */

/* Both watches live above (what each one guarantees is on its declaration
   in wake_flow.h); this is only the choice between them, plus the drain
   that has to happen whichever one ran. */
static void maybe_wait_for_event(void) {
    if (timer_get_state() == TIMER_RUNNING) {
        /* The final-minute watch may ignore a break ending inside the
           same window, and that is correct: a RUNNING active slot is
           either Screen (which cannot be on its own break at the same
           time) or an extra timer — and a RUNNING extra suppresses the
           chime anyway. The break still ends on its own wall clock at
           the next wake. */
        wake_flow_watch_final_minute();
    } else {
        wake_flow_watch_break_end(); /* a no-op unless a break ends inside the window */
    }

    /* GUARANTEED DRAIN. Everything above can end the break as a side
       effect of a timer_tick — most sharply the expiry alert, which holds
       the CPU for ~15 s and can straddle the break's wall end. The latch
       means those ticks no longer lose the edge; this is the one point
       both wake handlers reach after all timer work and before sleep, so
       draining here is what makes "every tick is safe" true rather than a
       promise each new call site has to keep. */
    wake_flow_break_end_repaint();
}

/* ---- the OTA apply point ------------------------------------------------ */

/* The late pre-sleep point: paint, extend the failsafe, open the second
   window, download, commit, reboot — all of it inside ota_flow_apply,
   which owns that order. Reached from the tail of both wake handlers and
   from nowhere else, and the "nowhere else" is the design.

   WHY THIS IS NOT INSIDE enter_deep_sleep(). That function is the single
   funnel every sleep goes through, which makes it the obvious home and
   the wrong one: it is also awake_failsafe_cb's path (main.c). That
   callback runs in the esp_timer task and exists precisely because
   something is ALREADY wedged and the battery needs protecting — painting
   the panel, opening a WiFi session and pulling ~1.5 MB from there is the
   exact opposite of what it is for. The same funnel is lock_gate.c's exit
   too, where the device is refusing to do work at all. A runtime "am I on
   the main task?" test would be a check, not a guarantee. Calling from
   here instead makes the failsafe STRUCTURALLY incapable of reaching a
   download: the esp_timer task never enters a wake handler.

   Three properties hold at this point, and they are why it is this point
   rather than any other spot in the tail:

     - the network window is JOINED. Both tails reach here only through
       net_apply_try_window() or net_apply_finish(), each of which joins,
       and ota_flow.h requires the pending flag to be read after that
       barrier.
     - NVS is still OPEN. hal_nvs_close() is inside enter_deep_sleep and
       therefore after this; ota_flow_apply charges the retry budget —
       ota_fails and ota_target — BEFORE it attempts anything, and those
       writes are the whole reason the budget engages after a wake that
       was killed mid-download.
     - the wake's panel work is finished. maybe_wait_for_event() has run,
       so nothing is left to repaint over the update screen.

   What it deliberately does NOT cover: the early-out sleeps (a break
   starting mid-wake, the still-held-button continuation). A pending
   update is left for the next check there rather than painting an update
   screen over a break screen the device has just decided to sleep
   through. The cost is bounded — s_pending is a plain static that deep
   sleep discards, so the next rollover simply finds the update again.

   The pending test lives here rather than inside ota_task_run_apply so
   the ordinary wake pays one comparison instead of a 16 KB stack
   allocation, and so the two facts below are sampled only when they are
   going to be used. */
static void maybe_apply_update(void) {
    if (!ota_flow_pending())
        return;
    /* Sampled HERE, not carried over from ota_flow_arm. The first window
       closed minutes and a full-panel repaint ago, and a cell that has
       crossed into the charge lock in between is exactly the one that
       must not be asked for a sustained radio burst followed by a flash
       write. Sampled on THIS task, because the ADC and the lock gate are
       main-task concerns while the download is not. */
    if (!ota_task_run_apply(ota_batt_pct(), lock_gate_charge_locked())) {
        /* The spawn itself failed — a 16 KB stack is the largest single
           allocation this firmware makes, so a fragmented heap really can
           refuse it. ota_flow_apply never ran, which means NOTHING was
           recorded: an update was found, announced to Home Assistant by
           the check, and then the device would say nothing about it on
           this wake or any wake after, forever.

           Reported, not charged. ota_task.c's reasoning for leaving the
           retry budget alone here is right and stays: giving up on a
           perfectly good image because the heap was tight one evening is
           the wrong failure. What was missing was only the observability,
           and that is all this adds. */
        ota_flow_note_spawn_failed();
    }
}

/* ---- the wake handlers --------------------------------------------------- */

/* Held-through-sleep guard: EXT1 ANY_LOW is level-triggered, so a button
   still held when the release-wait in enter_deep_sleep() times out (3 s)
   re-wakes the chip instantly and would re-fire its action. Record what
   was held at sleep entry; an immediate re-wake by one of those buttons
   is a continuation to ignore, not a new press. RTC memory, because the
   whole point is to survive the sleep that separates the write from the
   read. */
static RTC_DATA_ATTR uint8_t s_held_mask_at_sleep;
static RTC_DATA_ATTR int64_t s_sleep_entry_time;

/* Why enter_deep_sleep() calls this rather than wake_flow doing it itself
   is on the declaration in wake_flow.h: the read must happen while the
   pads are still digital. */
void wake_flow_note_sleep_entry(void) {
    s_held_mask_at_sleep = buttons_scan_held();
    s_sleep_entry_time = (int64_t)hal_time_now();
}

void wake_flow_handle_timer_tick(void) {
    time_t now = hal_time_now();
    wake_flow_handle_day_rollover(&now);
    lock_gate_check_bedtime(now); /* may not return; before the sync block so a
                           locked re-wake runs exactly one net window
                           (the rare release-by-edit fall-through repaints
                           and may add this wake's regular sync) */
    /* After rollover + bedtime (both of which want to see a live break),
       and before `before` is captured below — so a snap back to Screen is
       invisible to the before/after comparison and the wake-sticky
       break-ended promotion is what forces the full refresh. */
    wake_flow_break_end();

    bool synced_this_wake = false;
    if (wake_policy_sync_due(timer_get_state(), timer_needs_ntp_sync(now), now, timer_last_ntp_sync(),
                             IDLE_SYNC_INTERVAL_SEC)) {
        net_apply_try_window();
        now = hal_time_now();
        synced_this_wake = true;
    }

    /* Cold boot / external reset only: the rollover + sync above already
       showed the WiFi pixel, but the grid wait + first paint below can
       hold a blank panel for tens of seconds more with buttons still
       wake-press-only — a dark, silent device reads as hung (field
       report). Deep-sleep tick wakes stay dark: a dim blink every minute,
       all day, isn't worth the battery. */
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        status_led_show_timer_state();
    }

    /* Fast path: a break already due on arrival, before the grid wait and
       the paint — so the panel isn't refreshed with a main layout we are
       about to replace with the break screen.

       This runs BEFORE this wake's timer_tick, so it cannot see an expiry
       that the tick is about to detect; with both pending the break would
       win here and the expiry alert would be skipped entirely
       (enter_deep_sleep does not return). The post-render call below is
       what actually provides the expiry-then-break ordering. An expiry
       reaching this point unprocessed has not been shown to be reachable
       — the planner's 70 s event lead plus wake_flow_watch_final_minute
       own the final minute — but this call carries no such guarantee, so
       do not add one to this comment. */
    if (wake_flow_maybe_start_break(now)) {
        enter_deep_sleep(lock_gate_sleep_mode()); /* break just started; sleep through it */
    }

    /* Land the render on the state's grid — the planner woke us on (or,
       when a sync was due, ~20 s before) the grid point; absorb the
       residue here. 25 s covers the sync lead without stalling
       event-watch wakes. Captured BEFORE the wait: a pause press during
       it must register as a state change (full refresh). Skipped on
       power-on/reset: the panel is blank and holding it dark for up to
       25 more seconds (field: 19 s) is worse than one off-minute render
       — the next tick wake re-aligns. */
    timer_state_t before = timer_get_state();
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        wake_flow_wait_for_render_grid(25);
    }
    now = hal_time_now();

    /* The grid wait above can span the break end; drain before the paint
       so the chime lands with it (the latch means a later drain would
       still work, just a paint too late). */
    wake_flow_break_end();
    int32_t remaining = timer_tick(now);

    /* The grid wait makes the true remaining a round minute at render
       time; snap away wake/render jitter so 1:10:59 never shows.
       Genuinely off-grid renders (slow sync) stay honest. */
    int32_t shown = remaining;
    if (timer_get_state() == TIMER_RUNNING) {
        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC);
    }
    display_state_t st = make_display_state(shown, now);
    /* Same jitter snap for the break countdown, whether it is the break
       screen's own big clock or the chip behind another timer. */
    if (st.timer_state == TIMER_BREAK || st.break_banner) {
        st.break_remaining_sec = wake_policy_snap_minute(st.break_remaining_sec, SLEEP_PLAN_WATCH_SEC);
    }

    wake_render_t wr = wake_policy_render(before, timer_get_state(), false, wake_flow_break_ended_this_wake(), false);
    wr = lock_gate_promote_render(wr); /* a lock released this wake owes the panel a full one */
    switch (wr) {
        case WAKE_RENDER_EXPIRY_ALERT:
            wake_flow_fire_expiry_alert();
            break;
        case WAKE_RENDER_FULL:
            display_full_refresh(&st);
            break;
        default:
            display_update(&st); /* partial; policy promotes every 5th to full */
            break;
    }

    /* A break earned in the SAME tick that expired a timer. The fast-path
       check above runs before timer_tick, so the expiry that pushed the
       balance over the interval is invisible to it.
       wake_flow_fire_expiry_alert returns (it repaints the main layout),
       so re-checking HERE — after the render switch — is what yields
       expiry-then-break in one wake, which is the order the alerts have
       to arrive in. This is the only call site that provides that
       ordering: finish_or_break has the opposite shape, its break gate
       running before the tick in finish_action_and_render. A no-op unless
       the balance is genuinely over, so it costs nothing on every other
       path.

       Re-read the clock first: `now` predates the render, and an expiry
       alert holds the CPU for ~15 s before returning. */
    now = hal_time_now();
    if (wake_flow_maybe_start_break(now)) {
        enter_deep_sleep(lock_gate_sleep_mode()); /* break just started; sleep through it */
    }

    /* A press that landed while this wake was awake (sync, grid wait,
       e-ink flush) is in the latch — act on it now or it evaporates at
       deep sleep (losing the start/pause race against the minute render).
       Same guards as a wake press via the shared dispatch; D stays
       wake-press-only. Must run BEFORE maybe_wait_for_event: the
       final-minute watch discards pre-watch latched presses at entry. */
    int pick = button_latch_pick(buttons_take_pressed(), (1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C));
    if (pick >= 0) {
        timer_state_t painted = timer_get_state();
        bool swapped = false;
        if (wake_flow_dispatch_button_action((button_id_t)pick, &now, painted, !synced_this_wake, &swapped)) {
            status_led_show_timer_state();
            finish_or_break((button_id_t)pick, painted, now, swapped);
        }
    }
    maybe_wait_for_event();
    maybe_apply_update(); /* second window; does not return when it commits */
    enter_deep_sleep(lock_gate_sleep_mode());
}

void wake_flow_handle_button_wake(void) {
    button_id_t btn = buttons_get_wakeup_button();

    /* Continuation of a hold, not a new press: same button as at sleep
       entry and the sleep lasted no time at all. Skip all action AND
       display work (a hold would otherwise churn the panel every ~3 s)
       and go back to waiting for release. */
    if (btn != BTN_NONE && (s_held_mask_at_sleep & (1u << (int)btn)) &&
        (int64_t)hal_time_now() - s_sleep_entry_time <= 2) {
        ESP_LOGI(TAG, "button %d still held from previous wake - ignoring", (int)btn);
        enter_deep_sleep(lock_gate_sleep_mode()); /* does not return */
    }

    /* Immediate "button heard" ack — current state colour, updated to the
       resulting state below once the action has run. */
    status_led_show_timer_state();

    time_t now = hal_time_now();
    wake_flow_handle_day_rollover(&now);
    /* IDLE overnight: the threshold crossing may first be observed on a
       button press (idle wakes are up to an hour apart). The press is
       swallowed and the transition is silent per the alert rules. */
    lock_gate_check_bedtime(now); /* may not return */
    /* Same ordering as the tick handler: after rollover + bedtime, before
       `before` is captured, so a snap back to Screen rides the
       wake-sticky break-ended promotion rather than confusing the state
       diff. */
    wake_flow_break_end();
    timer_state_t before = timer_get_state();
    bool swapped = false;

    switch (btn) {
        case BTN_A:
        case BTN_B:
        case BTN_C:
            wake_flow_dispatch_button_action(btn, &now, before, true, &swapped);
            break;
        case BTN_D:
            /* The second checking trigger, and the only one a person can
               reach on purpose — which is what makes it the one you use
               while testing an update. Whether it ACTUALLY checks is
               ota_flow_arm's answer, not this call site's: it depends on
               the ota_on_sync runtime flag, which only NVS knows.

               Deliberately before net_apply_open, for the same reason as
               the rollover, and deliberately NOT on Button A: A is the
               start/resume path, where the user is waiting on the panel
               and a manifest GET would sit between the press and the
               render. */
            ota_flow_arm(OTA_TRIGGER_SYNC, ota_batt_pct(), lock_gate_charge_locked());
            /* NTP-gated paint, same as BTN A: sync now, MQTT after paint */
            if (net_apply_open()) {
                net_window_wait_ntp();
            }
            now = hal_time_now();
            break;
        case BTN_NONE:
        default:
            break;
    }

    /* Drain latch: the wake press itself was handled via the EXT1 decode
       above; its release bounce (or a second tap during the action) must
       not replay through the awake-press consumers below — e.g. a resume
       with <70 s remaining flows straight into the final-minute watch,
       where a stale A edge would instantly re-pause. */
    buttons_take_pressed();

    finish_or_break(btn, before, now, swapped); /* e.g. resume with accrual already past the interval */
    maybe_wait_for_event();
    maybe_apply_update(); /* second window; does not return when it commits */
    enter_deep_sleep(lock_gate_sleep_mode());
}

void wake_flow_handle_wake(uint32_t causes) {
    /* A BITMASK, not a value: the silicon can report several sources for
       one wake, so a press that coincided with the RTC alarm carries the
       timer bit too. Testing for equality would send it to the tick
       handler, where the press is never decoded and is thrown away at
       the next sleep. */
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        wake_flow_handle_button_wake();
    } else {
        /* Everything else, including a cold boot — which reports NO
           cause at all (esp_sleep_get_wakeup_causes() returns 0 when the
           reset was not an exit from deep sleep). The tick handler is
           what installs the day, syncs the clock and paints, so a power
           on has to land here and not on a wake button nobody pressed. */
        wake_flow_handle_timer_tick();
    }
}
