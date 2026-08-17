/* Display-state and stats-snapshot assembly, moved from main.c so the
   mapping rules (IDLE full bar, per-slot allocation fallbacks, warn
   badge, button availability) are host-tested (test_app_state). */
#include "app_state.h"

#include <string.h>

#include "battery.h"
#include "battery_policy.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "schedule.h"
#include "timer.h"

static const char *timer_state_str(timer_state_t st) {
    switch (st) {
        case TIMER_RUNNING:
            return "RUNNING";
        case TIMER_PAUSED:
            return "PAUSED";
        case TIMER_EXPIRED:
            return "EXPIRED";
        case TIMER_BREAK:
            return "BREAK";
        default:
            return "IDLE";
    }
}

static const char *day_type_name(day_type_t dt) {
    switch (dt) {
        case DAY_WEEKEND:
            return "Weekend";
        case DAY_HOLIDAY:
            return "Holiday";
        case DAY_SUMMER:
            return "Summer";
        default:
            return "Weekday";
    }
}

display_state_t app_state_display(const app_state_in_t *in, int32_t remaining, time_t now) {
    day_type_t dt = schedule_get_day_type(now);
    /* Extra timers have a fixed configured duration; Screen (slot 0)
       follows the day schedule. */
    const timer_def_t *def = timer_active_def();
    uint32_t alloc = (def != NULL) ? (uint32_t)def->duration_sec : schedule_get_allocation_sec(dt);
    /* IDLE shows today's full allocation (full bar), not 0 (ProductOverview) */
    if (timer_get_state() == TIMER_IDLE) {
        remaining = (int32_t)alloc;
    }
    int pct = battery_percent_from_mv(in->batt_mv);
    uint16_t break_dur = NVS_DEFAULT_BREAK_DURATION_MIN;
    nvs_config_get_break_duration_min(&break_dur);
    /* The break lives on slot 0 and keeps running behind whatever timer
       is selected. With Screen selected the break SCREEN is drawn, so the
       chip would say the same thing twice; everywhere else it is the only
       cue the break is still counting down. */
    bool banner = timer_break_active() && timer_active_slot() != 0;
    /* Swap hint (break screen): slot 0 has no def, so a wrap back to
       Screen reads as "no name" — which is right, the hint only renders
       while Screen is the selected slot. */
    const timer_def_t *next_def = timer_slot_def(timer_next_slot());
    /* ...and during a break it must reflect whether a STARTABLE timer
       exists, not merely another enabled slot: offering a swap to a chore
       that Button A will then refuse is worse than offering nothing. */
    if (timer_break_active() && timer_eligible_extra_count() == 0) {
        next_def = NULL;
    }
    return (display_state_t){
        .remaining_sec = remaining,
        .allocation_sec = alloc,
        .timer_state = timer_get_state(),
        .day_type = dt,
        .wall_time = now,
        .last_sync_time = timer_last_ntp_sync(),
        .battery_pct = (uint8_t)pct,
        .break_remaining_sec = timer_break_remaining(now),
        .break_duration_sec = (uint32_t)break_dur * 60,
        .break_banner = banner,
        .swap_next_name = (next_def != NULL) ? next_def->name : NULL,
        .timer_name = (def != NULL) ? def->name : NULL,
        .charge_warn = battery_policy_evaluate(pct, false) != BATT_OK,
        .completions = timer_completions(),
        .reloadable = (def != NULL) && def->reloadable,
        .swap_available = timer_swap_allowed(),
        .reload_available = timer_reload_allowed(in->parent_testing),
        .start_available = timer_start_allowed(),
        /* Rendered on the battery row. The app descriptor is a device
           read, so it arrives injected — app_state stays host-testable
           and display_screens stays ESP-free. */
        .fw_version = in->fw_version,
    };
}

void app_state_stats(const app_state_in_t *in, time_t now, stats_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    out->batt_mv = in->batt_mv;
    out->batt_pct = battery_percent_from_mv(in->batt_mv);
    out->light_mv = in->light_mv;
    out->state = timer_state_str(timer_get_state());
    const timer_def_t *def = timer_active_def();
    out->active_timer = (def != NULL) ? def->name : "Screen";
    day_type_t dt = schedule_get_day_type(now);
    out->day_type = day_type_name(dt);
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        const timer_def_t *sd = timer_slot_def(i);
        if (i > 0 && sd == NULL) {
            out->remaining_s[i] = 0;
            out->allocation_s[i] = 0;
            continue;
        }
        int32_t alloc;
        if (timer_slot_state(i) != TIMER_IDLE) {
            alloc = timer_slot_allocation(i);
        } else {
            alloc = (i == 0) ? (int32_t)schedule_get_allocation_sec(dt) : sd->duration_sec;
        }
        out->allocation_s[i] = (uint32_t)alloc;
        out->remaining_s[i] = timer_slot_remaining(i, now, alloc);
    }
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        out->completions[i] = timer_slot_completions(1 + i);
    }
    out->charge_lock = in->charge_locked;
    out->break_remaining_s = timer_break_remaining(now); /* slot 0; 0 = no break */
    out->accum_s = timer_run_accum(now);                 /* clamped read: never negative */
    out->fw = in->fw_version;
    out->screen_bonus_applied_s = timer_screen_bonus_applied();
    out->reset_reason = in->reset_reason;
}
