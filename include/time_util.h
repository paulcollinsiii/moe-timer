#pragma once
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "date_fmt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The earliest wall-clock instant this firmware will believe:
   2026-01-01 00:00:00 UTC. It predates the repo's first commit
   (2026-05-17), so no build of this code can run on a correctly set
   clock that reads earlier.

   WHAT IT CATCHES is a clock that was never set. A genuine power-on reset
   (battery pulled, cell run flat, first power-up) clears the RTC epoch
   offset, and until NTP lands the clock reads the 1970 epoch plus uptime
   (BUG-11). Panics, esp_restart() and brownouts do NOT clear it: the
   offset lives in RTC retention registers.

   A fixed floor and not the build time (__DATE__ or the app descriptor):
   a floor that moves with each build breaks reproducible builds and makes
   the host tests depend on the day they were compiled, and a tighter
   floor catches nothing more. An unset clock reads near 1970, decades
   below either one.

   NOT "NTP has set the clock this session". That is the OTA gate's
   question (ota_gate_in_t::time_valid) and it is false on most wakes,
   because NTP runs hourly. This one asks whether the clock has ever been
   set. */
#define TIME_UTIL_CLOCK_FLOOR ((time_t)1767225600)

/* True once the wall clock reads a time this firmware could be running
   at: at or after TIME_UTIL_CLOCK_FLOOR. A decision that compares the
   time of day against a configured window should refuse to act on a
   false answer (lock_gate.c's bed-time check and the break planner's
   bed-time crossing in wake_flow.c both do). */
static inline bool time_util_clock_plausible(time_t t) {
    return t >= TIME_UTIL_CLOCK_FLOOR;
}

/* The same question asked of a stored local ISO date ("YYYY-MM-DD", as
   timer_record_date() writes last_date): could a plausible clock have
   dated it? True iff some instant of that local day passes
   time_util_clock_plausible() — which holds exactly for the day that
   contains the floor and every day after it. So the one threshold is
   rendered through the one date format and compared lexicographically,
   the way date_fmt.h compares every ISO date; there is no second floor.

   WHAT IT CATCHES is a day that an unset clock opened (BUG-14): after a
   power-on without NTP the rollover dates the day "1970-01-01" (or
   "1969-12-31" west of UTC). Such a day is a placeholder, not a day the
   device can vouch for, and nothing dated by it may overwrite a real
   day's record. An empty string (no day recorded) answers false too. */
static inline bool time_util_day_plausible(const char *iso) {
    const time_t floor_t = TIME_UTIL_CLOCK_FLOOR;
    struct tm tm_floor;
    localtime_r(&floor_t, &tm_floor);
    char floor_day[11];
    date_fmt_iso(floor_day, sizeof(floor_day), &tm_floor);
    return iso != NULL && strlen(iso) == 10 && strcmp(iso, floor_day) >= 0;
}

/* Local wall-clock minutes since midnight — the unit every schedule
   policy (bed time, quiet hours, break windows) compares against. Local,
   not UTC: the device runs on the TZ configured in NVS, and a UTC reading
   would put bed time hours off. Header-only because it is one localtime_r
   call and the callers are in different modules; module-prefixed because
   that puts a bare `minutes_of_day` into every TU that includes it, and
   the bedtime/quiet-hours modules want the name themselves. */
static inline int time_util_minutes_of_day(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

#ifdef __cplusplus
}
#endif
