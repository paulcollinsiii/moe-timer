#pragma once
#include <stdbool.h>
#include <time.h>

#include "sleep_plan.h"  /* wake_sleep_mode_t */
#include "wake_policy.h" /* wake_render_t */

/* The three screen locks — battery charge, bed time and config error —
   and the sleep mode they imply. All three are gates: they run early in a
   wake and either return (the wake carries on) or paint a lock screen
   once and end the wake there. All three keep their "am I locked" bit in
   RTC memory, because the whole point is to survive the long sleeps they
   schedule.

   Everything here is an edge, which is why it earns a module of its own:
   engaging must stop the timer before the panel changes, engaging must
   happen exactly once (later wakes leave the e-ink alone), releasing must
   tell the next render that the panel is still showing a lock screen, and
   a locked re-wake must do nothing but its one network window — the only
   remote fix path left while the buttons are dark.

   Layer 2 (stateful orchestration): the gates own the lock state and call
   the drivers directly. The policies they consult stay pure and
   elsewhere — battery_policy.c for the lock band and its hysteresis,
   bedtime.c for the window and the alert rule, sleep_plan.c for the
   precedence between the two locks. */

#ifdef __cplusplus
extern "C" {
#endif

/* The sleep seam, and the reason these gates are testable at all.
   Implemented by main.c: deep sleep is an ESP-IDF contract (wake sources,
   RTC pad holds, the held-button release wait) with no module home. The
   host suite defines its own, so a gate that ends the wake is an
   observable outcome instead of a call that never comes back.

   DOES NOT RETURN. Every statement after a call to it is dead on device;
   the gates below are written so that is always the function's tail. */
void enter_deep_sleep(wake_sleep_mode_t mode);

/* Which sleep policy the device is currently under. Every sleep site asks
   through here rather than inspecting the lock flags, so the precedence
   between them (host-tested in sleep_plan.c) has exactly one reader.
   Sampling at the call site — not inside enter_deep_sleep, past its 15 s
   net_window_join — also shrinks the awake-failsafe race to the few ms
   between the gate entry and the flag write, and a mis-sampled wake
   self-corrects on the next one. */
wake_sleep_mode_t lock_gate_sleep_mode(void);

/* For the HA stat snapshot. The bed-time flag has no such reader: it is
   not published, and the sleep mode above is the only other question
   anyone asks of it. */
bool lock_gate_charge_locked(void);

/* For the WAKE MASK, and for nothing else so far. buttons.c narrows the
   EXT1 mask to Button D alone while this is true (buttons_policy.h), and
   short-circuits the three gates it would otherwise ask — two of which go
   to flash — off the same answer. Not published to HA: a device that is
   config-locked has just told the parent so on the panel, which is a
   louder channel than a stat nobody has a card for. */
bool lock_gate_config_locked(void);

/* A release leaves the panel showing Charge Me!, Bed Time or Config
   Error, which a partial refresh cannot clear, so the wake that observes
   the release owes the panel a full one. Only PARTIAL is promoted: FULL is already
   what we want, and an expiry alert owns the display for itself. Levels
   are wrong here and edges are right — the flags are set by the release
   and live only for that wake, so a device that merely happens to be
   unlocked does not repaint on every tick.

   Asked by the TICK handler only. The button handler's render never goes
   through here; it pays the same debt off lock_gate_check_bedtime()'s
   true return instead (s_lock_screen_on_glass, wake_flow.c). */
wake_render_t lock_gate_promote_render(wake_render_t wr);

/* Battery gate. Runs before any wake work — a battery that cannot afford
   an e-ink refresh cannot afford to find that out later. Returns normally
   when operation may continue; in the lock band it paints Charge Me! once
   (pausing a RUNNING timer first, so the allocation does not burn while
   the device is unusable) and then sleeps, so this call does not return. */
void lock_gate_check_charge(void);

/* Lock onto the Bed Time screen and sleep — does not return. The timer is
   paused, never expired: day rollover resets the slots overnight, so
   expiring would only skew the daily-summary stats. Separate from the
   gate below because the break planner reaches it directly — a break that
   would still be running at bed time is skipped in favour of going to bed
   early, audibly. */
void lock_gate_bedtime_engage(time_t now, bool alert);

/* Bed-time gate, modeled on the battery one: called from both wake
   handlers right after day rollover (rollover-first ordering is what
   clears the lock on the new day). May not return.

   A CLOCK THAT WAS NEVER SET GETS NO BED-TIME DECISION (BUG-11): when
   `now` fails time_util_clock_plausible() the bed-time half neither
   engages nor releases and leaves its flag as it stood. The config-error
   half still runs. On a power-on reset the day-rollover window normally
   syncs NTP before this gate runs; the skip fires only when that attempt
   failed, and then on every wake until NTP works. So a device that lost
   power and has no WiFi does not enter bed time until it syncs.

   AND THE CONFIG-ERROR GATE, WHICH RUNS HERE TOO (design 5.3). The name
   is now narrower than the function, which is a cost paid deliberately
   and is worth reading before "tidying" it into two entry points:

     - This is the only place in the firmware where a gate can ask what
       TODAY's day type is. The config-error lock engages for today's pair
       and stays dormant over the other three (rows C11/C12), and today is
       only settled once wake_flow_handle_day_rollover() has run. Both
       wake handlers call this immediately after it, with `now` in hand.
       The battery gate — the other existing entry point — runs in
       app_main before any of that and takes no instant at all.
     - main.c takes ZERO additions on this plan, and the file list this
       task owns does not include wake_flow.c either, so a second call
       site was never available. Folding it in here is not a shortcut
       around that constraint; it is the position the ordering already
       required.
     - The shapes are the same to the line: paint once, then sleep in long
       intervals that still run a network window, because for both locks
       an edit arriving in that window is the only remote fix path and it
       must not wait another interval.

   ORDER IS BED TIME, THEN CONFIG ERROR, and it is not interchangeable.
   Bed time's engage does not return, so a broken pair found at 21:00 gets
   no screen that night — correctly, since nobody is editing config then
   and the panel is already saying "not in service". The morning the
   bed-time lock lets go, the config gate picks the panel back up in the
   same wake.

   RETURNS TRUE WHEN THIS CALL RELEASED A LOCK — bed time or config error,
   on any path: the pre-window check (fixed between wakes, a new day) or
   the post-window re-check (an edit or a clock step in the window) for
   either lock, and for the config-error lock alone an engage and a
   release inside the same call (a bed-time engage never returns). False
   when there was nothing to release, and false on an unset-clock skip.
   When it returns at all, both locks are off, with one exception: an
   unset-clock skip leaves a bed-time flag that was already up standing.
   Nothing can raise that flag on an unset clock: the power-on reset that
   unsets the clock also zeroes it (RTC), and the break planner's
   bed-time crossing (wake_flow_maybe_start_break) is guarded by the same
   plausibility check. What the bool adds is that one of them was ON a
   moment ago, so the panel is still holding its screen and every press
   made until that screen is repainted BELONGS TO THE LOCK. The wake
   handlers act on that
   (wake_flow.c, s_lock_screen_on_glass): the press that woke the device
   is consumed rather than also run as ✓3 or a sync, B is dropped by the
   join poll, the latch is emptied of every press made before the repaint,
   and the repaint is a full one. "Fix it in HA, then press D" is then
   exactly true — D reaches the window early and does nothing else. */
bool lock_gate_check_bedtime(time_t now);

#ifdef __cplusplus
}
#endif
