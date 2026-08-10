/* Wake-scoped caches for the two HA-editable time policies that are read
 * far more often than they change: the quiet-hours window and bed time.
 *
 * Both are plain (non-RTC) statics, so a deep sleep drops them and every
 * wake starts cold — "once per wake" is the whole contract. The quiet
 * callback in particular fires from the LED task on every pixel update,
 * which is not a place to put a flash read.
 */
#pragma once
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Are the status pixels inside the configured quiet window at `now`?
   Loads the window from NVS on the first call of the wake. `now` is
   passed in rather than read here so the comparison is testable; the
   fold to local minutes-of-day happens inside.

   Unlike bed time below, the stored HHMMs are folded with no validity
   check. The asymmetry is deliberate and is about blast radius, not
   about one input being more trustworthy: a corrupt quiet window folds
   to out-of-range minutes and lands somewhere arbitrary — 9999/8888
   reads as always-quiet, 9999/9999 as never-quiet — but the whole
   effect is status pixels being dark or lit at the wrong hour, which is
   cosmetic and self-corrects on the next HA edit. A corrupt bed time
   locks the *screen* at noon with buttons dead, so it gets a fallback.
   Do not "make these consistent" without that trade in hand. */
bool config_cache_quiet_active(time_t now);

/* Bed time as local minutes since midnight, or -1 when bed time is off.
   The fallback rule is asymmetric and load-bearing: a *stored* 0 means
   the owner disabled the feature and stays disabled, while any other
   unusable value (below bedtime.c's evening floor, or malformed HHMM)
   falls back to the compile-time default. Treating the two alike either
   day-locks a device on a bad edit or silently drops the lock on a
   device that wanted one. The fallback is computed, never written back:
   the stored value must survive so HA can correct it. */
int config_cache_bedtime_minutes(void);

/* Drop every wake-scoped config cache — both of the above plus
   schedule.c's. Called once after a network window applies HA edits so
   the rest of the wake sees the new values; deliberately does no I/O of
   its own, leaving the reload to whoever asks next. */
void config_cache_invalidate(void);

#ifdef __cplusplus
}
#endif
