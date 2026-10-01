/* Display-state and stats-snapshot assembly, moved from main.c so the
   mapping rules (IDLE reporting the day's whole effective allocation,
   per-slot allocation fallbacks, warn badge, button availability) are
   host-tested (test_app_state). */
#include "app_state.h"

#include <string.h>

#include "battery.h"
#include "battery_policy.h"
#include "chore_store.h"
#include "chores.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "schedule.h"
#include "time_util.h"
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
    /* IDLE shows today's whole allocation rather than 0 (docs/behavior/timers_and_schedule.md, "Timer states"),
       and the EFFECTIVE one: an adjustment banked before the day's first
       start would otherwise show nowhere until someone presses B, which
       reads exactly like a set that never landed.

       This is no longer the same thing as a full BAR, and on a gated day
       it is not a proportional one either. The bar divides by `base`, so
       an idle day with -30 against a 60 min default draws half a bar
       UNGATED. Gated, display_bar_split clamps the free tranche to the
       room the locked block leaves, so 30 effective minutes against a
       20 min tranche saturate and the tranche draws FULL. The bar is a
       statement about the DAY's split, not about the deduction; the
       counter and the status row are what carry that. */
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
       that Button B will then refuse is worse than offering nothing. */
    if (timer_break_active() && timer_eligible_extra_count() == 0) {
        next_def = NULL;
    }
    display_state_t st = (display_state_t){
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

    /* ---- the chore checklist block ------------------------------------
       Filled after the literal rather than inside it because the count has
       to be read before anything that is bounded by it, and because the
       names are COPIED into the struct (display.h says why) — there is no
       initialiser form for that. The designated literal above names none
       of these fields, so C value-initialises all seven: empty rows and a
       count of 0, which is exactly the inert C1 default. Every assignment
       below therefore overwrites a defined value, never stack garbage.

       These are not device reads and so are not injected: chore_store sits
       on hal_nvs and the rest are RTC accessors, all three host-testable
       with the mocks this module already runs under. app_state_in_t stays
       reserved for the ADC, the app descriptor and the reset reason. */

    /* Reads every row on every path — a missing, stale or malformed blob
       leaves the rows "" and the count 0, which is the inert no-chores
       default (C1), so the return code carries nothing this layer acts
       on. */
    chore_store_load_names(st.chore_names, &st.chore_count);

    /* RAW, exactly as timer.h documents it: bits at or above the
       configured count are still set in here. Every chores.c call below is
       handed this byte rather than the masked copy just built, so the
       bounding stays where chores.c keeps it — a test against app_state
       then proves that bounding end to end, instead of proving only that
       app_state masked before asking. (chores_withheld_sec takes the mask
       and ignores it: it is not a term in the formula. It is passed for
       consistency, not because the value matters there.) */
    uint8_t acked_raw = timer_chore_acked();
    for (uint8_t i = 0; i < CHORE_MAX; i++) {
        if (chores_is_acked(acked_raw, i, st.chore_count)) {
            st.chore_acked |= (uint8_t)(1u << i);
        }
    }
    st.chore_outstanding = chores_outstanding(acked_raw, st.chore_count);
    st.chore_released = timer_chore_released();
    /* schedule_get_allocation_sec(dt), NOT `base` above — and the two are
       the same number only while Screen is the selected slot. `base` is
       the ACTIVE slot's allocation, so with an extra timer selected it is
       that timer's fixed configured duration; taking the gate from it
       would subtract the DAY's chore_free tranche from a 15-minute piano
       practice and report a withholding that belongs to no day at all.
       The gate is a statement about slot 0's day allocation and nothing
       else, which is why this reads the schedule a second time rather
       than reusing the figure already in hand. Both lookups are cached
       per wake inside schedule.c, so the second ask costs a branch.
       Seconds in, seconds out: both accessors speak seconds. */
    st.chore_withheld_sec = chores_withheld_sec(schedule_get_allocation_sec(dt), schedule_get_chore_free_sec(dt),
                                                acked_raw, st.chore_count, st.chore_released);
    st.app_mode = timer_mode();
    return st;
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
    out->day_type = schedule_day_type_name(dt);
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

    /* ---- the chore checklist, read-only (design 1.4) ------------------
       The same three reads app_state_display() makes for the panel's chore
       strip, so HA and the glass cannot disagree about what is done. The
       ack byte is RAW (timer.h) and is only ever read through chores.c,
       which ignores bits at or above the configured count — so a stale
       high bit from a longer list reports neither as a done chore nor as a
       lit per-chore sensor. No chores configured: all three are 0, the
       inert C1 reading. */
    char names[CHORE_MAX][CHORE_NAME_BUF];
    uint8_t n = 0;
    chore_store_load_names(names, &n); /* fills n = 0 on any failure; see app_state_display() */
    const uint8_t acked_raw = timer_chore_acked();
    for (uint8_t i = 0; i < CHORE_MAX; i++) {
        if (chores_is_acked(acked_raw, i, n)) {
            out->chore_acked |= (uint8_t)(1u << i);
            out->chores_done++;
        }
    }
    out->chores_left = chores_outstanding(acked_raw, n);

    /* M2-D6's config warning. Every day type, not today's: the blocking
       gate already covers today, and it is the other three that go
       invisible once the next config document overwrites the ack that
       named them. The mask is judged on the RAW stored pair — see
       schedule_chore_free_broken_mask() for why the clamped accessors
       would report every device healthy. Main task, like every schedule
       read; stats_json.h says why it can ride the snapshot. */
    out->chore_free_bad = schedule_chore_free_broken_mask();

    /* NO CLOCK, NO DAY (BUG-14). While the clock is unset, or RAM still
       holds the stand-in day an unset clock dated, the device is behind
       the no-clock lock and hands out no screen time — and everything
       above describes a day nobody can vouch for: a fresh allocation, no
       runs, no chores. Published as it stands, HA would show a refunded
       day. So the stat says what the panel says: state NO_CLOCK, nothing
       remaining and no limit (the stand-in's limit is a fresh day's,
       which is the refund this lock exists to prevent). The other fields
       ride along untouched; with the state saying NO_CLOCK none of them
       reads as a day. This reaches HA only when WiFi and the broker work
       but NTP does not (a LAN with its internet down), because a window
       that cannot associate publishes nothing. A new value of an existing
       field, not a new entity, so the discovery schema is unchanged.

       no_clock is the same verdict for mqtt_ha: it holds the day-scoped
       commands back (stats_json.h). THE DAY ARM IS LOAD-BEARING: it is
       what holds them in a window whose NTP set the clock while RAM still
       holds the stand-in day. Two paths of today's firmware reach that
       (the cycle-2 review, MINOR-4):
         - the charge lock's engage window (lock_gate_check_charge), on a
           device that is also clock-locked, when that window's NTP works:
           it runs at boot, before any gate can settle the day;
         - the lock's own retry window, when NTP lands after the
           after-NTP hook has looked and before the stats are posted.
       Without the arm both would ack and apply onto the stand-in, and
       the release would then wipe it. Do not remove it as a belt.
       The lock's own retry window settles the day BEFORE this snapshot
       (lock_gate.c, net_apply_try_window_then), so its release window
       reports the real day and applies what it holds. */
    const char *const day = timer_current_date();
    if (!time_util_clock_plausible(now) || (day[0] != '\0' && !time_util_day_plausible(day))) {
        out->state = "NO_CLOCK";
        out->no_clock = true;
        for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
            out->remaining_s[i] = 0;
            out->allocation_s[i] = 0;
        }
    }
}
