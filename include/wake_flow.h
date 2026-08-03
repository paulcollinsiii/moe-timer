#pragma once
#include <stdbool.h>
#include <time.h>

#include "buttons.h" /* button_id_t */
#include "timer.h"   /* timer_state_t */
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

/* ---- seams implemented by main.c ---------------------------------------- */

/* Render seam, implemented by main.c: tick the timer and full-refresh the
   panel with the result. It stays there because the state it paints needs
   a battery ADC read and the compile-time ParentTesting flag, neither of
   which has a home here yet. Deliberately not wake_flow_-prefixed —
   wake_flow does not implement it — which is the same shape as
   enter_deep_sleep(), declared by lock_gate.h and owned by main.c. */
void paint_current_state_full(void);

/* Post-action render seam, also implemented by main.c and also
   deliberately unprefixed: drain a break end, tick, and paint the result
   under the render policy, with no network work. The break tail above
   reaches it after a dispatched press. It stays in main.c for now because
   it paints through make_state() (battery ADC + the ParentTesting flag);
   it moves here with the rest of the post-action tail. */
void render_action_result(button_id_t btn, timer_state_t before, time_t now, bool selection_changed);

#ifdef __cplusplus
}
#endif
