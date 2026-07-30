/* NVS mirror of the timer's RTC state, extracted from main.c so the two
 * rules guarding it are host-tested rather than reviewed.
 *
 * RTC memory is where the timer actually lives; NVS is only the copy that
 * survives what RTC memory does not — a panic, an EN reset, a power cycle.
 * The copy exists for one specific failure: after such a reset the state
 * comes back zeroed, which reads as a brand-new day, and the rollover
 * hands the whole screen allocation back. A kid who is out of time would
 * get a fresh day by pulling the battery.
 */
#pragma once
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirror the live timer state into NVS, writing only when the bytes
   differ from what is already stored.

   The diff is against flash, deliberately, and not against a copy of the
   last write held in RAM. Two things depend on that: a write that failed
   (worn or full page) is retried on the next save, and a blob that
   drifted underneath us is corrected. A RAM cache of "what we believe we
   wrote" would silently lose both.

   The guard is not an optimisation to trade away. enter_deep_sleep()
   saves on every wake and the snapshot fields are stable across routine
   RUNNING ticks, so writing unconditionally would burn a flash page per
   wake, all day, for byte-identical content. It rests in turn on
   timer_make_snapshot() zeroing the struct before it fills the fields —
   timer_snapshot_t has interior and trailing padding, and without that
   memset two snapshots of one unchanged state would differ in the
   padding alone, so the guard would never hold and the value would still
   look correct. */
void timer_persist_save(void);

/* Restore today's timer state from NVS, returning whether it did.
   Callers read false as "nothing usable stored — reset the day".

   Refused while RTC state is intact, and the test for that is `last_date`
   being non-empty, NOT "the stored date differs from today". The two
   disagree in exactly one arrangement: RTC survived across midnight
   holding yesterday while NVS holds a snapshot dated today. RTC wins
   there, and should — that is a genuine date change, which is precisely
   when the day is supposed to be refunded.

   Refused too for a snapshot that is not from today, in either
   direction. Yesterday's would carry spent allocation into a new day
   (the mirror image of refunding it); a future-dated one means the clock
   has not been corrected yet. The latter is why app_main calls this
   twice — once at boot, which is enough after a panic because the RTC
   clock survives, and again after the rollover's NTP sync, which covers
   a power-on where the clock is invalid until corrected.

   Reads flash at most once and never writes it: this runs on the boot
   path ahead of the display, so a write here would add flash latency to
   every cold boot and would make the boot path capable of destroying the
   very blob it is reading. timer_defs_install() must have run first —
   the restore ends in timer_ensure_active_slot_enabled(), which reads the
   defs table to decide whether the restored selection still exists, and
   with no table every extra slot reads as disabled and the selection is
   dragged to Screen. */
bool timer_persist_try_restore(time_t now);

#ifdef __cplusplus
}
#endif
