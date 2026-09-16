#include "button_actions.h"

#include "chore_store.h"
#include "chores.h"
#include "date_fmt.h" /* date_fmt_iso: a static inline, so no new link edge */
#include "schedule.h"
#include "timer.h"

/* Same shape as chore_store.c's: this file is compiled into host suites
   that have no ESP-IDF, and the one log line below is a diagnostic, so it
   compiles away there rather than dragging a shim in. */
#ifndef NATIVE
#include "esp_log.h"
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

/* NOT named TAG, which is this project's convention everywhere else. Both
   host suites that compile this file (test_button_actions and test_timer)
   are single-TU builds that pull in chore_store.c beside it, and that
   file has a file-scope `TAG` of its own — a plain `TAG` here is a
   redefinition in both. The prefix is the file's, so the next module to
   join either TU cannot collide with it either. */
static const char *BTN_ACT_TAG = "btn_act";

bool button_a_toggle_allowed(void) {
    /* THE RUNNING HALF OF timer_swap_allowed(), and only that half —
       design 4.2 spells out that the extras condition does not come with
       it. Written as the comparison rather than as a call so the two
       cannot be "simplified" into one: a device with no extra timers
       configured has swap_allowed false forever and must still reach its
       chore list.

       Only the active slot can ever be RUNNING (timer.c), so this is
       simultaneously "no timer is running" and "the active slot is not
       RUNNING". TIMER_BREAK is not TIMER_RUNNING, which is the whole of
       why chore mode is reachable throughout a screen break; a background
       break with an extra running arrives here as TIMER_RUNNING on the
       ACTIVE slot and is refused. */
    if (timer_get_state() == TIMER_RUNNING) {
        return false;
    }
    /* FIRST because it is free (one RTC byte) and this is not: the
       sleep-entry caller reaches NVS only when the cheap half says
       "maybe". See buttons.c for why that ordering is worth writing down
       there rather than only here.

       The names are loaded and thrown away because the count is what is
       being asked for and chore_store has no count-only entry point —
       adding one would put a second reader of the blob layout beside the
       one that already validates it. 64 bytes of stack (CHORE_MAX 3 x
       CHORE_NAME_BUF 21 = 63, plus the count) against a second place for
       the version rule to be got wrong — the arithmetic is spelled out so
       the number cannot drift silently when either constant moves. */
    char names[CHORE_MAX][CHORE_NAME_BUF];
    uint8_t n = 0;
    /* Return code deliberately discarded, exactly as app_state.c does:
       every failure path in chore_store_load_names() sets n = 0 first, so
       "unreadable" and "not configured" land on the same answer and there
       is nothing for this layer to do differently. Design 4.2 records the
       cost of that (an NVS fault refuses the toggle while it lasts) and
       accepts it; the refusal stores nothing, so it self-heals on the
       next good read. */
    (void)chore_store_load_names(names, &n);
    return n > 0;
}

btn_a_action_t button_a_apply(void) {
    if (!button_a_toggle_allowed()) {
        return BTN_A_NONE;
    }
    /* Toggle, not a set: A is the only control the user has over the
       mode, so it has to be able to leave chore mode as well as enter it.
       Written against APP_MODE_CHORES rather than APP_MODE_TIMERS because
       the stored byte is NOT clamped (timer.h) — anything that is not
       APP_MODE_CHORES is Timers, so a byte outside the enum toggles INTO
       chores and the next press brings it back, instead of the mode
       getting stuck on an unpaintable value. */
    const app_mode_t next = (timer_mode() == APP_MODE_CHORES) ? APP_MODE_TIMERS : APP_MODE_CHORES;
    timer_set_mode(next);
    return (next == APP_MODE_CHORES) ? BTN_A_CHORES : BTN_A_TIMERS;
}

/* The configured chore list, as a (names, count) pair. Three call sites
   want it and each wants a different part, so the load lives here rather
   than being written out three times; the 64 bytes of names are the
   caller's stack either way, because chores_list_hash() needs the rows
   and not just the count.

   Return code discarded exactly as button_a_toggle_allowed() discards it:
   every failure path in chore_store_load_names() sets n = 0 first, so
   "unreadable" and "not configured" land on the same inert answer. */
static uint8_t load_chore_names(char names[CHORE_MAX][CHORE_NAME_BUF]) {
    uint8_t n = 0;
    (void)chore_store_load_names(names, &n);
    return n;
}

/* Today's withheld seconds for the SCREEN timer. Every term is the DAY's
   — schedule_get_allocation_sec() is slot 0's allocation and nothing
   else's — because the gate withholds screen time. Reading the active
   slot's allocation here instead would size the gate from an extra
   timer's fixed duration, which is not the day. */
static uint32_t chore_withheld_now(time_t now, uint8_t n) {
    const day_type_t dt = schedule_get_day_type(now);
    return chores_withheld_sec(schedule_get_allocation_sec(dt), schedule_get_chore_free_sec(dt), timer_chore_acked(), n,
                               timer_chore_released());
}

int32_t button_b_start_allocation(time_t now) {
    const timer_def_t *def = timer_active_def();
    if (def != NULL) {
        return def->duration_sec; /* an extra: the gate is Screen's alone */
    }
    /* THE LIVE CAP (see the header). alloc - withheld is `chore_free`
       while the gate is shut and the whole allocation once `released` is
       latched, so the day totals one allocation however it is split.
       chores_withheld_sec() saturates at alloc_sec, so the subtraction
       can neither wrap nor go negative, and it returns 0 outright when no
       chores are configured — row C1's inert default, and the reason this
       is byte-for-byte the old behaviour on every device in the field. */
    const uint32_t alloc = schedule_get_allocation_sec(schedule_get_day_type(now));
    char names[CHORE_MAX][CHORE_NAME_BUF];
    const uint8_t n = load_chore_names(names);
    return (int32_t)(alloc - chore_withheld_now(now, n));
}

bool button_chore_ack_allowed(uint8_t idx) {
    /* FIRST because it is free (one RTC byte) and the count below is not:
       on every device that is not looking at the checklist this answers
       without touching flash, which matters because the sleep-entry
       caller in buttons.c reaches this on every sleep. */
    if (timer_mode() != APP_MODE_CHORES) {
        return false;
    }
    char names[CHORE_MAX][CHORE_NAME_BUF];
    return chores_index_valid(idx, load_chore_names(names));
}

btn_ack_action_t button_chore_ack_apply(uint8_t idx, time_t now) {
    /* The gate is re-evaluated here rather than the caller being trusted
       to have called button_chore_ack_allowed(): the wake flow routes on
       the MODE alone (a cheap RTC read that needs no flash), so the
       row-exists half of the gate is checked nowhere else, and Button D's
       arm does not go through the shared dispatch at all. */
    if (timer_mode() != APP_MODE_CHORES) {
        return BTN_ACK_NONE;
    }
    char names[CHORE_MAX][CHORE_NAME_BUF];
    const uint8_t n = load_chore_names(names);
    if (!chores_index_valid(idx, n)) {
        return BTN_ACK_NONE;
    }

    /* The working copy moves first: everything below reads the new mask,
       and chore_withheld_now() reads it back out of RTC. */
    const uint8_t next = chores_toggle_ack(timer_chore_acked(), idx, n);
    timer_chore_set_acked(next);

    btn_ack_action_t act = BTN_ACK_TOGGLED;
    if (chores_release_due(next, n, timer_chore_released())) {
        /* THE ORDER IS THE WHOLE TRAP (chores.h, CALLER CONTRACT).
           chores_withheld_sec() returns 0 the moment `released` is true,
           so the amount has to be read HERE, before the latch two lines
           down. Latch first and the release grants nothing — and every
           assertion that only checks "released is true" still passes,
           which is why test_the_last_ack_releases_the_withheld_seconds
           asserts the seconds and not the flag.

           timer_release_gated() and NOT timer_adjust(): the seconds land
           through the same state machine, but adjust_today_sec is left
           alone. That field is the record of what a PARENT asked for and
           paints the panel's "(+40 min today)"; a gate writing into it
           would manufacture an adjustment nobody made (design 5.2). The
           release is not hidden by that — the locked block vanishing and
           the bar going full width is the feedback.

           The cast is written out because neither build enables
           -Wconversion: the value is bounded by the day's allocation,
           which schedule.c derives from a uint16_t of minutes. */
        const uint32_t owed = chore_withheld_now(now, n);
        (void)timer_release_gated((int32_t)owed);
        timer_chore_set_released(true); /* latched: row C8, nothing re-locks the day */
        act = BTN_ACK_RELEASED;
    }

    /* Flash is the AUTHORITY (design 5.1) and gets ONE write carrying
       both fields — the toggle and, on the release edge, the latch. The
       date is derived from `now` rather than taken from
       timer_current_date(), which is RTC_DATA_ATTR and reads "" on the
       wake after an esp_restart; date_fmt_iso() cannot render fewer than
       ten characters, so every one of chore_store_save_ack()'s date
       refusals is unreachable from here.

       A failed write is reported and nothing is rolled back: the seconds
       are already granted and `released` is latched by design, so a
       rollback would leave a cleared checkbox against a released day. The
       ack stands in RTC, survives deep sleep, and only an esp_restart
       before the next successful write would lose it. */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[11];
    date_fmt_iso(today, sizeof(today), &tm_now);
    const chore_ack_t rec = {.acked = next, .released = timer_chore_released()};
    if (chore_store_save_ack(today, chores_list_hash(names, n), rec) != ESP_OK) {
        ESP_LOGW(BTN_ACT_TAG, "chore ack %u not persisted", (unsigned)idx);
    }
    return act;
}

btn_b_action_t button_b_apply(time_t now) {
    /* EXPIRED first, and deliberately AHEAD of the break gate below: on an
       expired slot B has no start/pause/resume job, so that is where Reload
       lives, and a reload is not a start. timer_reload() returns the slot to
       IDLE at full duration rather than running anything, and the direct
       dispatch this leg replaces was gated only by timer_reload_allowed() —
       putting it behind timer_start_allowed() would silently stop a
       non-break-eligible expired extra from being reloaded during a break.
       timer_reload_allowed() carries the rest: never while RUNNING, and only
       a reloadable def, so Screen (no def) can never be reset here. */
    if (timer_get_state() == TIMER_EXPIRED) {
        return (timer_reload_allowed() && timer_reload()) ? BTN_B_RELOADED : BTN_B_NONE;
    }
    /* A Screen Break refuses to START anything that is not a genuine break
       activity (rule 7) — including Screen itself, which is how the break
       has always been enforced. Pausing is never gated: stopping is always
       safe, and a RUNNING slot during a break is break-eligible anyway. */
    if (timer_get_state() != TIMER_RUNNING && !timer_start_allowed()) {
        return BTN_B_NONE;
    }
    switch (timer_get_state()) {
        case TIMER_RUNNING:
            timer_pause(now);
            return BTN_B_PAUSED;
        case TIMER_IDLE:
            timer_start(now, button_b_start_allocation(now));
            return BTN_B_STARTED;
        case TIMER_PAUSED:
            timer_resume(now);
            return BTN_B_RESUMED;
        default:
            return BTN_B_NONE; /* TIMER_BREAK: EXPIRED already returned above */
    }
}
