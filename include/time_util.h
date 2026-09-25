#pragma once
#include <stdbool.h>
#include <time.h>

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
