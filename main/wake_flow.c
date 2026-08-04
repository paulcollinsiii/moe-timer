/* The wake orchestration, lifted out of main.c so its edges carry tests.
   What each entry point guarantees, and why, is in wake_flow.h; what
   lives here is the flow. */
#include "wake_flow.h"

#include "alerts.h"
#include "audio.h"
#include "bedtime.h"
#include "button_actions.h"
#include "button_latch.h"
#include "buttons.h"
#include "config_cache.h"
#include "display.h"
#include "hal_time.h"
#include "lock_gate.h"
#include "mqtt_ha.h"
#include "neopixel.h"
#include "net_apply.h"
#include "net_window.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "sleep_plan.h"
#include "status_led.h"
#include "time_util.h"
#include "timer.h"
#include "timer_persist.h"
#include "wake_policy.h"

#ifndef NATIVE
#include "esp_log.h"
#else
/* These discard their varargs, so ANY function call made inside a log
   argument is invisible to every host test — it is never evaluated here.
   FIVE call sites exist today, and every one is a verified pure read:
     - timer_active_slot()   x2 (the break-over snap, the Button C swap)
     - timer_get_state()        (the join poll's "state %d -> %d")
     - timer_run_accum(now)     (the break gate's accrual)
     - timer_current_date()     (the day rollover)
   Before putting a sixth call in a log line, check it has no side effect
   AND update the list above: a state change smuggled in as a %d argument
   would run on device and be unobservable in the suite, and an
   enumeration nobody maintains is what lets that happen unnoticed. */
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

/* Kconfig bool as a C expression (defined as 1 when =y, absent when =n),
   the same shape main.c and buttons.c use. sdkconfig.h arrives with
   timer.h on firmware; the host build has no sdkconfig and the flag reads
   false there, which is the shipping configuration. */
#if CONFIG_MAGTAG_PARENT_TESTING
#define PARENT_TESTING true
#else
#define PARENT_TESTING false
#endif

static const char *TAG = "wake_flow";

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
        ESP_LOGI(TAG, "Break over: chimed, selection back to slot %d", timer_active_slot());
    } else {
        ESP_LOGI(TAG, "Break over: chimed");
    }
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
            switch (button_a_apply(*now)) {
                case BTN_A_STARTED:
                case BTN_A_RESUMED:
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
                case BTN_A_PAUSED:
                    return true;
                default:
                    return false; /* EXPIRED: renders only (wake path) */
            }
        case BTN_B:
            /* Reset the selected timer to full: reloadable extras without
               ParentTesting, anything else with it — never while RUNNING
               (B is dropped from the wake mask then, same as C; this guard
               covers presses that ride in on another wake). */
            if (!timer_reload_allowed(PARENT_TESTING) || !timer_reload()) {
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
                ESP_LOGI(TAG, "button C: selected slot %d", timer_active_slot());
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
    if (button_a_apply(hal_time_now()) == BTN_A_NONE)
        return false;
    ESP_LOGI(TAG, "button A during join: state %d -> %d", (int)st, (int)timer_get_state());
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
    ESP_LOGI(TAG, "Screen break due (accum %ld s)", (long)timer_run_accum(now));
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
    ESP_LOGW(TAG, "Day rollover (last_date='%s', now=%lld)", timer_current_date(), (long long)*now);
    queue_rollover_summary();    /* yesterday's stats, before any reset */
    mqtt_ha_queue_bonus_clear(); /* clear the retained HA bonus target this window */
    /* Fail-open: reset to IDLE with today's allocation even if sync fails */
    net_apply_try_window();
    *now = hal_time_now();
    /* Power cycling must not refund the allocation: with the clock now
       corrected, a same-day NVS snapshot beats a reset. Only a genuine
       date change (or Button B in parent mode) resets the day. */
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
