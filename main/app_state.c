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

/* Today's allocation for an IDLE slot: the scheduled/configured base plus
   whatever adjustment is banked on it, clamped at 0 exactly as
   timer_start does when it folds the bank for real.

   The clamp is load-bearing beyond looking tidy: the stats path casts the
   result to uint32_t, so a -120 min adjustment against a 60 min day would
   otherwise publish 4294963696 as HA's "Screen time limit". Both callers
   below take their effective figure from here or from
   timer_slot_allocation(), and timer.c clamps that one too, so no signed
   value ever reaches the cast. */
static uint32_t idle_allocation(uint32_t base, int slot) {
    int32_t a = (int32_t)base + timer_slot_banked_bonus(slot);
    return (a > 0) ? (uint32_t)a : 0;
}

/* Today's EFFECTIVE limit for a slot, whatever state it is in: the banked
   fold while IDLE, and after that the allocation the timer is actually
   running on — timer_start folded the bank into it and timer_adjust moves
   it in place, so this is the one figure that survives both. */
static uint32_t effective_allocation(uint32_t base, int slot) {
    if (timer_slot_state(slot) == TIMER_IDLE)
        return idle_allocation(base, slot);
    int32_t a = timer_slot_allocation(slot);
    return (a > 0) ? (uint32_t)a : 0;
}

display_state_t app_state_display(const app_state_in_t *in, int32_t remaining, time_t now) {
    day_type_t dt = schedule_get_day_type(now);
    /* Extra timers have a fixed configured duration; Screen (slot 0)
       follows the day schedule. */
    const timer_def_t *def = timer_active_def();
    /* The day's DEFAULT, never today's adjusted limit. The panel renders
       the two separately — "Weekday - 60 min (-30 min today)" — because
       folding them together produced a first number that was neither the
       day's default nor anything the family had configured, and the
       default is the half that answers "is this a normal day?".

       It is also the bar's denominator, which is deliberate: a grant then
       simply pins the bar full instead of silently rescaling the day, and
       a deduction drains it against the same scale as every other day of
       the week. display_bar_fill_px clamps both ends, so remaining above
       it is not an error case. */
    uint32_t base = (def != NULL) ? (uint32_t)def->duration_sec : schedule_get_allocation_sec(dt);
    int slot = timer_active_slot();
    uint32_t effective = effective_allocation(base, slot);
    /* Read from the timer's own running total, NOT derived as
       (effective - base). The two inputs to that subtraction are read at
       different times: base is live config, while the effective limit was
       frozen into the slot at timer_start. So anything that moved the
       default underneath a started day — a parent editing the weekday
       minutes, a holiday landing for today, school dates flipping to
       Summer — manufactured an adjustment nobody made, and rewrote a real
       one when there was one. The tracked total survives timer_start's
       fold of the bank, which is the reason the derivation existed. */
    int32_t adjust = timer_slot_adjust_today(slot);
    /* Clamped FOR DISPLAY only. A -120 min deduction against a 60 min day
       empties it and stops; reporting "(-120 min today)" beside a 60 min
       default would describe a day of minus one hour. The stored total
       keeps the untruncated figure — that is the record of what the
       parent asked for — so the clamp lives here, at the seam that has to
       make three numbers on one screen agree. */
    if ((int64_t)base + adjust < 0) {
        adjust = -(int32_t)base;
    }
    /* IDLE shows today's whole allocation rather than 0 (ProductOverview),
       and the EFFECTIVE one: an adjustment banked before the day's first
       start would otherwise show nowhere until someone presses A, which
       reads exactly like a set that never landed. Note this is no longer
       the same thing as a full BAR — the bar divides by `base`, so an
       idle day with -30 on it draws half a bar, which is the point of the
       split. */
    if (timer_get_state() == TIMER_IDLE) {
        remaining = (int32_t)effective;
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
        .allocation_sec = base,
        .adjust_sec = adjust,
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
        .reload_available = timer_reload_allowed(),
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
        /* The EFFECTIVE limit, adjustment included — deliberately NOT the
           split the panel gets. An automation asking "how much screen time
           is there today" wants the number that is actually enforced, and
           changing that would be an HA-visible semantic change owing a
           DISC_SCHEMA_VER bump. The shared helper is also what keeps the
           uint32_t below unsigned all the way down: it clamps, so no
           signed intermediate exists here to leak a -3600 into the cast. */
        uint32_t alloc = effective_allocation((i == 0) ? schedule_get_allocation_sec(dt) : (uint32_t)sd->duration_sec,
                                              i); /* i == 0 short-circuits the NULL sd */
        out->allocation_s[i] = alloc;
        out->remaining_s[i] = timer_slot_remaining(i, now, (int32_t)alloc);
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
