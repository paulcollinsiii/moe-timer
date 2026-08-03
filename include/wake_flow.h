#pragma once
#include <stdbool.h>
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

/* Render seam, implemented by main.c: tick the timer and full-refresh the
   panel with the result. It stays there because the state it paints needs
   a battery ADC read and the compile-time ParentTesting flag, neither of
   which has a home here yet. Deliberately not wake_flow_-prefixed —
   wake_flow does not implement it — which is the same shape as
   enter_deep_sleep(), declared by lock_gate.h and owned by main.c. */
void paint_current_state_full(void);

#ifdef __cplusplus
}
#endif
