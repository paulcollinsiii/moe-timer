#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "buttons.h"    /* button_id_t */
#include "stats_json.h" /* stats_snapshot_t */
#include "timer.h"      /* timer_state_t */
#ifndef NATIVE
#include "esp_system.h" /* esp_reset_reason_t */
#endif

/* The wake orchestration, lifted out of main.c so the edges that
   historically carried the bugs carry tests instead. Layer 2 (stateful
   orchestration): it owns the wake-scoped state and calls the drivers
   directly, while the decisions it makes stay pure and elsewhere
   (wake_policy.c for the chime grace and the refresh choice). */

#ifdef __cplusplus
extern "C" {
#endif

/* Boot forensics: the USB CDC console drops output around sleep/reset
   transitions, so a crash's evidence must ride channels that survive —
   the stat payload (HA "Last reset" sensor) and a late-wake log line.
   Anything but DEEPSLEEP on a wake means the previous wake died.

   Takes the reason as a parameter rather than calling esp_reset_reason()
   itself, which is what makes the string table host-testable. Never
   returns NULL: the result goes straight into a log format and a JSON
   payload. */
const char *wake_flow_reset_reason_str(esp_reset_reason_t reason);

/* The panic-loop breaker, called from app_main before the boot prints.
   The S2 ROM USB console can panic when a host port-open races those
   prints; each panic reboots, re-enumerates USB and re-races, freezing
   the device for as long as a monitor keeps reconnecting. After a panic
   this stays quiet briefly so the host's open completes against silence.

   Reads the reset reason itself rather than taking it, so app_main is
   left with a call and no branch — WHICH reason earns the pause, and how
   long it lasts, are the decision and they are tested here. Every other
   reason returns immediately: a delay on every wake would put two
   seconds of CPU on the battery for nothing. */
void wake_flow_boot_quiet_after_panic(void);

/* ---- the break-end edge ------------------------------------------------ */

/* The single owner of the break-end edge: surfaces an elapsed Screen
   Break and decides — in one place — whether it chimes and snaps the
   selection back to whatever the break interrupted.

   Ticks THEN drains. Both halves are idempotent (the tick does nothing
   unless slot 0 is BREAK and its wall end has passed; the drain returns
   false unless something is latched), so this is safe to call anywhere
   and needs nothing to have run before it. Draining alone would not do:
   the latch only exists once something has ticked, so a caller that
   drained without ticking would miss a break that elapsed while the
   device was busy — including the awake watch's own wait loop, which
   exits on wall time without ticking anything.

   Because the edge is a LATCH rather than a return value, a tick that
   happens elsewhere still cannot lose it. Returns true when THIS call
   drained an edge. */
bool wake_flow_break_end(void);

/* The same drain, with the full repaint a drained edge owes the panel.
   Used where the break end must be VISIBLE as soon as it is heard: the
   break watch's tail, and the guaranteed drain both wake handlers reach
   after all timer work and before sleep — the one point that makes
   "every tick is safe" true rather than a promise each new call site has
   to keep. Draining before painting is the ordering, not an accident: the
   snap back changes which slot the paint will read. Returns true when
   this call drained an edge (and therefore painted). */
bool wake_flow_break_end_repaint(void);

/* The safety net enter_deep_sleep() reaches on its way down: takes a
   break-end edge that nothing drained and reports it. Both wake handlers
   run the guaranteed drain above before sleeping, so a true return here
   means a NEW code path reached sleep without one — that is what the
   warning is for, and it is the only reason this exists.

   A raw take, not the owner above: the panel is already done by the time
   this runs, so a tick, a chime or a snap back would be side effects
   nobody can see, and this also runs from the awake failsafe's esp_timer
   context where audio is not safe. Lives here rather than in main.c so
   the break-end latch keeps exactly one owner. */
bool wake_flow_report_undrained_break_end(void);

/* Did a background Screen Break end on this wake? Sticky for the whole
   wake, because every render after the end must be a full refresh: the
   inverted BREAK chip has gone, and a chiming end has also changed which
   timer's layout is showing, so a partial diff would ghost the panel.
   Feeds wake_policy_render()'s break_ended argument at every render site.
   A plain static behind this: the next wake is a fresh boot. */
bool wake_flow_break_ended_this_wake(void);

/* ---- the button guard matrix ------------------------------------------- */

/* Apply one button action (A/B/C — D is wake-only). Shared by the EXT1
   wake handler, the tick-wake latch drain and the break tail, so all
   three honour the same state guards.

   The signature is the contract, and every part of it encodes a shipped
   defect — do not "clean it up":

   `before` is by VALUE because it is the state that was PAINTED, not the
   state that is live. wake_policy_render()'s break-screen boundary check
   is the only thing standing between a swap during a break and a ghosted
   panel, and it can only see that boundary if `before` still names the
   layout on the glass. The guards below therefore key on this parameter,
   never on timer_get_state().

   `now` is by POINTER because a start/resume can span a network window
   (seconds) and the caller must render against the clock the action
   actually left behind, corrected and shifted, not the one it walked in
   with.

   `selection_changed` is an OUT-PARAM rather than a rewrite of `before`
   because both facts have to survive: landing on an already-EXPIRED slot
   must not re-fire its alert (which is what this reports), while `before`
   remains the only record of which layout was painted. Overwriting
   `before` to signal the swap made those renders partial, which ghosted
   the panel. Always written — false on every arm, including refusals and
   the buttons this function ignores.

   allow_net_window gates the NTP window on a start/resume: a wake that
   already ran a window skips the redundant second one (clock corrected,
   buffered HA effects already applied). Returns true when the press
   changed timer state (the caller must render). */
bool wake_flow_dispatch_button_action(button_id_t btn, time_t *now, timer_state_t before, bool allow_net_window,
                                      bool *selection_changed);

/* Awake pause poll. Buttons are only dispatched on EXT1 wake — while the
   firmware is awake a press would vanish — so the long awake waits poll
   this instead: a Button A press while RUNNING pauses immediately, the
   one action that must not be lost. The GPIO ISR latches the edge the
   moment it lands (even inside an e-ink flush or an NTP sync) and this
   consumes the latch, so no press is lost to a blind spot.

   MASKED take: only the A bit is consumed. Latched B/C presses stay in
   the latch for the tick-wake drain — a poll during the grid wait must
   not eat them. Returns true when it paused. */
bool wake_flow_poll_pause_button(void);

/* Latched Button A during the window join-wait: the screen has already
   painted and the device looks done, so a dropped press reads as broken.
   Mirrors the wake handler — RUNNING pauses, IDLE starts, PAUSED resumes,
   BREAK/EXPIRED stay wake-press-only. The LED acks instantly; the repaint
   rides the post-join changed-state re-render, because the panel must
   stay quiet while the MQTT tail is transmitting (brownout, see the
   snapshot rendezvous). The clock was already synced this wake, so a
   start here needs no expiry shift. Same masked take as the pause poll.
   Returns true when the state map did something. */
bool wake_flow_poll_button_a_action(void);

/* Button poll for the BREAK tail. The break watch owns the CPU for the
   whole tail, and a break no longer than SLEEP_PLAN_WATCH_SEC has no
   other phase — the tail IS the break. Without this poll every press made
   during it is latched by the ISR and then thrown away at deep sleep,
   which silently disables the one thing a break is for: walking over to a
   break-eligible timer and starting it (C to select, A to start). Symptom
   on-device: "I couldn't move to another timer in the final minute of the
   screen break."

   Same mask, dispatch and guards as the tick handler's latch drain, so
   the break's own refusals (A on slot 0, a non-eligible slot) still
   apply. allow_net_window is false: the window for this wake has already
   been joined by the time the watch runs, and a second one here would
   paint over the tail. Returns true when the press changed what the panel
   shows — the caller stops watching, having already repainted. */
bool wake_flow_poll_break_buttons(void);

/* ---- the break gate, the expiry alert and the day rollover -------------- */

/* Eye-rest break gate, run wherever the exposure balance can have crossed
   the interval. Returns true when a break was STARTED, which the caller
   must read as "the break screen is painted and the alarm has run — go
   straight to sleep"; false means nothing happened at all.

   Takes `now` rather than reading the clock, because the call sites do
   not agree on which instant they mean and must not: two of them
   deliberately re-read the wall clock first (an expiry alert holds the
   CPU for ~15 s before returning), while the post-press one passes the
   clock the dispatched action left behind.

   Two ways to return false — and one way not to return at all:
     - a configured interval of 0 disables eye-rest breaks entirely;
     - the balance has simply not reached the interval yet;
     - a break that would still be running at bed time never starts. The
       device goes to bed early and AUDIBLY instead, through
       lock_gate_bedtime_engage(), which DOES NOT RETURN — the one
       alerting path that begins before its own threshold is reached.
   Persists BREAK before the alarm, same rationale as the EXPIRED
   at-transition save below: a power cut during the ~15 s alarm must not
   restore a snapshot taken before the break existed. */
bool wake_flow_maybe_start_break(time_t now);

/* Full expiry sequence: big TIME'S UP screen, beeps + red pulse, then
   back to the main layout (empty bar, TIME'S UP state in the corner) once
   the alert is dismissed or times out — the big screen would only last
   until the next tick redraw anyway.

   RETURNS, unlike the bed-time engage above, and the tick handler depends
   on that: re-checking the break gate after this is what yields
   expiry-then-break within one wake.

   Exposed rather than wrapped in a main.c thunk because it is
   ADDRESS-TAKEN: net_apply's on_active_expired_alert hook points straight
   at it, so a network window that expires the active timer runs this same
   sequence. */
void wake_flow_fire_expiry_alert(void);

/* Day rollover, run first in both wake handlers. A no-op unless the
   stored date differs from today's.

   `now` is by POINTER because the rollover opens its own network window
   and the caller must carry on against the CORRECTED clock, not the one
   it walked in with — every later decision in the wake (the bed-time
   gate, the grid wait, this wake's tick) keys off that value.

   Yesterday's numbers are queued for HA before anything is reset. A
   same-day NVS snapshot then beats the reset: power cycling must never
   refund the day's allocation, so only a genuine date change gets a fresh
   one. */
void wake_flow_handle_day_rollover(time_t *now);

/* ---- the awake watches -------------------------------------------------- */

/* Absorb the wake residue so the render lands on the state's grid:
   RUNNING/BREAK on the countdown's round minute (the display truly reads
   1:11:00), clock-only states on the wall :00. HOW LONG to wait is the
   pure policy's decision (wake_policy_grid_wait_sec, tested there); what
   is here is the burning of it — a stretch that can run for max_wait_sec
   seconds, polled ten times a second so a Button A press inside it is not
   lost. Aborts early on a pause press: the caller then renders PAUSED,
   off-grid but honest.

   KNOWN DEFECT, DELIBERATELY PRESERVED: the poll consumes the A press
   BEFORE it checks the state, so a press made while the timer is not
   RUNNING is eaten here and nothing later in the wake can act on it — up
   to max_wait_sec seconds in which Button A does nothing at all. The
   reasoning and the eventual fix are recorded on
   wake_flow_poll_pause_button(); the current behaviour is pinned by
   test_row6_a_press_while_not_running_is_eaten_KNOWN_BUG. */
void wake_flow_wait_for_render_grid(int max_wait_sec);

/* BREAK tail: stay awake through the last seconds of a Screen Break so
   its end (chime + repaint, and the snap back to whatever it interrupted)
   lands within a tick of wall time. Keyed on slot 0, so it covers a break
   running behind another selected timer just as well as the break screen
   itself.

   Three ways to decline the job, all of them at the top: no break is
   running; an extra is RUNNING, which suppresses the end entirely (no
   chime, no snap — nothing to wait for); or the end is further out than
   SLEEP_PLAN_WATCH_SEC, which is the planner's to schedule, not this
   function's to sit through.

   Polls wake_flow_poll_break_buttons() at 250 ms throughout — a press it
   accepts has already repainted, so the watch stops there and the planner
   re-schedules the end (or the press started an eligible extra and the
   end is suppressed, exactly as the top-of-watch guard would have
   decided). Otherwise the wait ends on wall time and the edge is drained
   and repainted through wake_flow_break_end_repaint(). */
void wake_flow_watch_break_end(void);

/* RUNNING tail: own the final minute — the countdown partials at the
   quarter-minute marks, the last 15 s as a binary count on the pixels,
   the pause poll, the eye-rest break check, and the expiry alert at zero.

   Declines the same way the break tail does: an expiry already passed
   belongs to the alert path, and one further out than
   SLEEP_PLAN_WATCH_SEC belongs to the planner.

   Three things it does that are each a fixed field report rather than
   housekeeping: presses latched BEFORE the watch are discarded at entry
   (a resume with <70 s left flows straight in here, and its own release
   bounce would re-pause instantly); a stale clock is sharpened by a
   network window first, but only when the sync would not itself blow past
   the expiry; and the break balance is re-checked on EVERY poll, because
   a short allocation can put break-due inside this watch after the
   per-wake check has already passed. */
void wake_flow_watch_final_minute(void);

/* ---- the wake handlers -------------------------------------------------- */

/* The two wake entry points. app_main picks between them on the hardware
   wake cause and does nothing else with the wake; everything from the day
   rollover to the deep-sleep call is here.

   Neither RETURNS: both end in enter_deep_sleep(), and so do several
   early exits along the way (a lock gate engaging, a break starting, a
   button held through the previous sleep). Every statement after a call
   to either of them is dead on device.

   The tick handler covers RTC-timer wakes AND cold boot; the button
   handler covers EXT1. They are two functions rather than one with a
   flag because they differ in more than a branch: the tick handler owns
   the sync cadence, the render-grid alignment and the routine repaint,
   while the button handler owns the wake-press decode, the
   held-through-sleep guard and the immediate LED ack. What they do share
   — the post-action tail and the pre-sleep event watch — they share by
   calling the same statics, not by being the same function. */
void wake_flow_handle_timer_tick(void);
void wake_flow_handle_button_wake(void);

/* The wake-cause decode, and the only one of the three app_main calls
   here that picks anything. app_main reads the causes — that read is an
   ESP-IDF call on a boot-scoped hardware register and stays there — and
   hands the value straight over, so the choice between the two handlers
   above is made where it can be tested.

   `causes` is the BITMASK esp_sleep_get_wakeup_causes() returns, not a
   single cause: EXT1 arrives alongside the timer bit when a press
   coincides with the RTC alarm, and a cold boot reports no bits at all.
   Both of those are why this is a mask test with the tick handler on the
   else arm, and both have a case in the suite.

   DOES NOT RETURN — see the handlers above. */
void wake_flow_handle_wake(uint32_t causes);

/* Record what was still held at sleep entry, for the continuation guard
   at the top of the button handler. EXT1 ANY_LOW is level-triggered, so a
   button still held when enter_deep_sleep()'s release-wait times out
   (3 s) re-wakes the chip instantly and would re-fire its action; an
   immediate re-wake by one of the buttons recorded here is a continuation
   to ignore, not a new press.

   Called by enter_deep_sleep() (main.c, which owns the sleep contract)
   rather than by this module, because the read has to happen at a precise
   point in that sequence: while the pads are still digital. It is
   buttons_configure_wakeup_if() at the end of enter_deep_sleep that hands
   them to the RTC mux, and gpio_get_level is unreliable afterwards. Both
   halves of the guard — this write and the read that consults it — live
   here so the RTC-memory pair has exactly one owner. */
void wake_flow_note_sleep_entry(void);

/* Collect and hand the HA stats snapshot to the open network window; a
   no-op when no window is open. Released only AFTER the wake's paint has
   finished: the network task blocks on this rendezvous before opening the
   MQTT session, which is what keeps panel refresh current and radio TX
   bursts from coinciding (the combination browned out the rail on
   device).

   Exposed rather than kept static because it is ADDRESS-TAKEN: net_apply's
   post_stats hook points straight at it, so a window opened anywhere runs
   the same collection. Same reason as wake_flow_fire_expiry_alert above. */
void wake_flow_post_stats_snapshot(void);

/* Tick the timer and full-refresh the panel with the result: the normal
   screen, restored. A failed OTA download owes the panel this, because the
   last thing it painted was the update screen — so app_main hands this
   function to ota_flow_ops_t's `repaint` seam.

   Exposed for exactly the reason the two entry points above are, and no
   further: it is ADDRESS-TAKEN by a composition-root ops table, so it
   needs external linkage. It is NOT a re-export of the wake_flow static
   named in the section below — that one keeps its own name and stays
   private; this is a public entry point that happens to share its body,
   the same relationship wake_flow_break_end_repaint already has with it.

   Runs on the OTA task (ota_task.c) with the main task blocked in the
   join, so the tick and the panel flush inside it are serialised against
   everything else in this module rather than concurrent with it. */
void wake_flow_repaint_current_state(void);

/* ---- nothing here is implemented by main.c any more ---------------------- */

/* Four seams used to be declared here and implemented in main.c:
   paint_break_started(), paint_current_state_full(), make_display_state()
   and stats_collect(). All four are wake_flow.c's own statics now, and
   this header declares none of them.

   The first went during the residency audit, because the battery ADC
   read underneath it admitted a device CALL in the composition root but
   not the ORDERING inside it (the LED is lit BETWEEN the state assembly
   and the flush, holding the panel blue for the whole multi-second
   refresh), and an ordering is a decision.

   The next two went when the audit's own review took the next step: "the
   ADC read has no host answer" is not one of the four reasons the rule
   lists, and it is refuted by lock_gate.c, which reads the same ADC and
   is host-tested.

   stats_collect() went last, and it is the one that shows what the escape
   hatch cost. Its residency claim was esp_app_get_description() and
   esp_reset_reason() — bare CALLS, not handles, so reason 3 as written
   never admitted it. The shape it left behind is the tell: main.c
   implemented a seam that main.c itself never called, so the only edge
   into it came from wake_flow.c, and wake_flow.c had to reach back up
   through this header to get it. Deleting the declaration deletes that
   cycle. It did NOT move to app_state.c, which takes batt_mv and light_mv
   as INPUTS and reads no ADC; pushing the reads down would have made a
   suite that needs no device stubs grow two.

   Nothing outside wake_flow.c ever called any of the four, so nothing
   here has to declare them; what they do is pinned by test_wake_flow
   instead of by a comment. */

#ifdef __cplusplus
}
#endif
