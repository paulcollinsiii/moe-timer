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
#include <stdint.h>
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

   ALSO RESTORES TODAY'S CHORE ACKS, on the success path and only there.
   Succeeding is by definition "RTC memory was lost and the day came back
   from flash", which is the one arrangement where coming back with the
   gate re-armed does damage: the restored allocation already carries
   whatever the release granted, so a second release grants the withheld
   remainder twice. The failure path deliberately does not restore —
   there the caller resets the day to a fresh full allocation and the
   gate is SUPPOSED to be armed, and one release then hands back exactly
   what was withheld. See timer_persist_restore_chore_acks() below for
   the record's own rules; the list hash it needs is read here.

   Never writes flash, and reads it at most three times: the snapshot,
   and then on the success path the chore names and the ack record. This
   runs on the boot path ahead of the display, so a write here would add
   flash latency to every cold boot and would make the boot path capable
   of destroying the very blobs it is reading. Two of the three reads are
   skipped on every ordinary deep-sleep wake, because this returns at its
   first guard. timer_defs_install() must have run first —
   the restore ends in timer_ensure_active_slot_enabled(), which reads the
   defs table to decide whether the restored selection still exists, and
   with no table every extra slot reads as disabled: the selection is
   dragged to Screen and every RUNNING extra is retired as an orphan
   (BUG-7; a PAUSED one keeps its state). */
bool timer_persist_try_restore(time_t now);

/* Put today's chore acks back into RTC from the "chore_ack" NVS record —
   design row C14: a power cycle, a panic or an OTA reboot must never cost
   the kid their chores.

   `now` is the device's best current wall time — the same value handed to
   timer_persist_try_restore() — and the ISO date the record is matched
   against is derived from it in here, with the same localtime_r() +
   date_fmt_iso() pair timer_record_date() uses. A PARAMETER, exactly as
   try_restore's is: nothing in here reads a clock, and nothing in here
   reads a date out of RTC behind the caller's back. `current_hash` is
   chores_list_hash() of the chore list as it is NOW, handed straight to
   the loader so the C10 list-edit rule is applied there rather than
   restated here.

   IT TAKES A time_t AND NOT A DATE STRING, and that is a bug fix rather
   than a matter of taste. timer_current_date() is the only date string a
   caller has to hand, and it is the WRONG one at the one moment that
   matters: on a rollover wake RTC memory is intact and still holds
   YESTERDAY, so a string-taking version fed from it loaded yesterday's
   record and wrote yesterday's acks AND yesterday's `released` latch into
   RTC as today's. The new day would then start unlocked, and a `released`
   latch brought forward lets C8's withheld remainder be granted twice.
   (On the esp_restart path the same argument is "" instead, which was
   merely refused, and deferred the restore by a wake.) Deriving the date
   from `now` makes both unrepresentable: the day this asks flash about is
   the day the caller believes it is.

   Returns true only when acks were actually written into RTC. A false
   return means "no usable record" — nothing stored yet, or a blob whose
   length or version byte could not be believed — and on every one of those
   paths g_rtc_state is untouched and no flash is written, so a device
   that has never had a chore configured keeps its live copy instead of
   being cleared. Repeating the call is idempotent: the second one lands
   what the first one did. Idempotent is NOT the same as harmless, and the
   distinction is the whole of the note below — it is the FIRST call that
   can lose state.

   What must never follow a false return is a chore_store_save_ack() built
   on the RTC's zeros: that is the one sequence that destroys a good
   record (chore_store.h, WHERE `today` MUST COME FROM). A false return
   means DEFER, not "nothing acked today" — the record in flash is
   untouched, so a later wake gets the real answer at no cost.

   WHY IT EXISTS, and it is the one thing the RTC copy cannot do:
   RTC_DATA_ATTR survives deep sleep only. esp_restart() reloads .rtc.data
   from the image as zeros, so on the wake after an OTA reboot today's
   acks exist in exactly one place — flash — and this is what fetches
   them.

   WHERE IT IS CALLED, and it is NOT a boot block in main.c. Its home is
   the tail of timer_persist_try_restore(), above — which already IS the
   "RTC was lost, rebuild the day from flash" path, already derives and
   refuses a stale date, and is already reached on both of the wakes that
   need this (app_main's boot restore, and the rollover's second attempt
   once NTP has corrected the clock). Wiring it there costs main.c
   nothing, keeps §5.1's one authority, and makes the two requirements
   below hold by construction rather than by a comment:

   (1) AFTER timer_rtc_state_guard(). The guard memsets the whole of
   g_rtc_state when it rejects an image, so acks restored ahead of it are
   thrown away. try_restore is only reached past its own last_date guard,
   and on the esp_restart path it is the rtc_state_guard two calls
   earlier in app_main that emptied last_date. (2) BEFORE anything that
   paints the checklist or gates on an ack, which is what it is for; the
   boot restore precedes the first paint.

   It remains public, and is called directly by the suite, because it is
   where every rule below lives and because nothing in it depends on
   try_restore having run: try_restore writes only the snapshot's own
   fields (it does not memset g_rtc_state) and reads only last_date,
   which this function neither reads nor writes.

   THE ONE ASYMMETRY, stated rather than buried. Flash is the AUTHORITY,
   so wherever the record and the live RTC copy disagree the record wins —
   and that is true of BOTH shapes of record, not only of a date mismatch:
     - stamped with the day this was asked about, it replaces the RTC copy
       with its own acks and its own `released`, so anything that reached
       RTC and never reached flash is gone;
     - stamped with another day, it makes chore_store_load_ack() return
       ESP_OK with a CLEARED record, and those zeros are written just the
       same — over `chore_released` as well as over the acks.
   Three ways to arrive at a disagreement, and only the first is benign:
     - a genuine day rollover, where clearing is CORRECT and is row C13: a
       new day starts locked, whatever yesterday achieved;
     - a toggle whose flash write failed while its RTC write succeeded —
       an ack that was never durable, and that the next reboot would have
       lost anyway;
     - a clock correction that lands `now` and the stored stamp on
       opposite sides of midnight: NTP fixing a fast or slow RTC, or a
       timezone change. No flash failure anywhere, and acks plus a
       release earned under the old reading are dropped. (A DST shift
       reaches it only in a zone whose transition is at midnight.)
   That is the deliberate trade: one authority that can be stale beats two
   that diverge silently. It is also why REPEATING the call is the cheap
   half — the first call is where a divergence dies.

   `mode` is NOT restored and has no NVS row — design §5.1 persists the
   ack record and nothing else, so a restart legitimately comes back
   painting Timers. C16 is about the device SLEEPING, which RTC already
   covers. */
bool timer_persist_restore_chore_acks(time_t now, uint16_t current_hash);

#ifdef __cplusplus
}
#endif
