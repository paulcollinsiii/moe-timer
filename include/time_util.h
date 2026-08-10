#pragma once
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

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
