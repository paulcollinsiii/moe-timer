#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "chores.h"   /* CHORE_MAX — one chore row per ack button */
#include "neopixel.h" /* NEOPIXEL_COUNT — the strip both painters here drive */
#include "timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- who owns the four status pixels ------------------------------------

   There are NEOPIXEL_COUNT == 4 status pixels and FIVE claimants on them,
   so the allocation is MODE-DEPENDENT rather than fixed. Two claim a
   single pixel each and are what the mode table below is about; three
   take the whole strip, and two of THOSE cut across every mode:

     Timer modes   pixel 0   timer state — NP_STATE_PIXEL, below.
                   pixel 3   WiFi/NTP window — net_window.c's own
                             NP_WIFI_PIXEL, under
                             CONFIG_MAGTAG_SYNC_LED_FEEDBACK (=y today).
                   The two are at opposite ends of the strip, which is
                   what lets both be read at a glance. Pixels 1-2 are
                   unclaimed BY THESE TWO — which is not the same as
                   dark; see the strip-wide claimants below.

     Chore mode    ALL FOUR — chores_led_show(), below: three chore rows
                   plus the gate. Three chores plus one gate is exactly
                   the strip, so nothing is left over and the chore
                   display DISPLACES both single-pixel owners above for
                   as long as the checklist is the screen. That
                   displacement is intended — on the checklist the pixels
                   are the ack feedback — but its WiFi half still needs a
                   mechanism; see the caller contract on chores_led_show().

   STRIP-WIDE, and NOT mode-scoped — either can overwrite a chore row or
   the timer pixel without going through either owner above:

     neopixel_status_binary4()  writes all four pixels (neopixel.c). Its
                                sole caller is the <=15 s countdown in
                                wake_flow.c — which is a TIMER mode, so
                                pixels 1-2 do NOT simply stay dark there.

     neopixel_alert_pulse_*()   owns EVERY pixel while the expiry alarm
                                runs (alerts.c, via set_all_scaled in
                                neopixel.c), and _end() clears all four
                                and drops the power gate (neopixel.h), so
                                whatever was painted before an alarm is
                                gone after it.

   The alarm one is a live collision for the chore screen, not a
   theoretical one: an expiry alarm firing while the checklist is up
   wipes the strip, and nothing repaints it. It belongs to T8/M2-HW2 the
   same way the net_window collision does — recorded here so it is not
   missed merely because the contract below spells out only net_window. */
#define NP_STATE_PIXEL 0

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} status_led_rgb_t;

/* Traffic-light state feedback while the slow e-ink refresh runs:
   RUNNING = green, PAUSED = amber, EXPIRED = red, BREAK = cyan,
   IDLE = white. Split out pure so the triples are host-testable — they
   are read off the device by eye, so they are part of the contract. */
status_led_rgb_t status_led_for_state(timer_state_t state);

/* Drives NP_STATE_PIXEL from the live timer state. Status class — quiet
   hours are handled inside neopixel.c. Must not be called while the chore
   strip is up: it would repaint the gate's pixel with a timer colour. */
void status_led_show_timer_state(void);

/* ---- the chore checklist's strip (design §2.5) --------------------------

   The whole strip at once, so the caller cannot paint half of it. Index i
   is PHYSICAL pixel i — the chore-to-pixel mapping has already been
   applied — which is what keeps that mapping inside a pure function that
   a host test can assert without a board in front of it. */
typedef struct {
    status_led_rgb_t px[NEOPIXEL_COUNT];
} chores_led_t;

/* What the four pixels should show for a given chore state.

   `mask`     today's ack bits, bit i = chore i (chores.h). Bits at or
              above `n` are meaningless and are ignored here, as they are
              everywhere else.
   `n`        how many chores are configured, 0..CHORE_MAX. Values above
              CHORE_MAX are clamped: it arrives from NVS.
   `released` the day's LATCHED release flag, and a third argument rather
              than a spare mask bit because it is not a function of the
              mask in either direction. All three chores acked but not yet
              latched is a real tick (display.h), and un-acking a chore
              after release does NOT revoke it — so "every row green, gate
              red" and "a row red, gate green" are both reachable, and
              neither can be derived from the other.

   Red = outstanding, green = done, for the chore rows and for the gate
   alike: red while the withheld time is locked, green once it is granted.
   The triples are the timer table's own green and red — one device, one
   pair of eyes, one vocabulary.

   A row that is not a configured chore is DARK, not red: with two chores
   the third ack button is not a chore button at all (button_actions.h
   refuses it), and a red pixel under it would name an outstanding chore
   that does not exist. With n == 0 the whole strip is dark, the gate
   included — nothing is withheld, so neither colour would be true (C1).

   Pure: no clock, no NVS, no globals, no GPIO. T8 needs that, because the
   first frame it paints is the PRE-press state, which by then disagrees
   with the live one.

   SIGNATURE DRIFT: design §2.5 still writes this as chores_led_for(mask,
   n), two arguments. The DOC is stale, not the code — `released` is a
   latch and provably not derivable from the mask (see above), so the
   two-argument form cannot express the feature. Do not "correct" the
   code to match the doc; amending the doc is the orchestrator's call. */
chores_led_t chores_led_for(uint8_t mask, uint8_t n, bool released);

/* Paint chores_led_for()'s four pixels over neopixel.c. Status class, so
   quiet hours and brightness are handled inside that module.

   CALLER CONTRACT — three things this function cannot enforce:

   1. BUTTON WAKES ONLY (design §2.5, "Power discipline"). An unattended
      tick or NTP wake that happens to paint the chore screen must leave
      the pixels dark, or chore mode lights four LEDs every 55 s for
      nobody. This function paints whenever it is called; the wake cause
      is the caller's to check.

   2. NOT ALONGSIDE status_led_show_timer_state(). Both write pixel 0.
      The chore strip's gate lands there, so a timer paint in the same
      wake turns the gate into a timer colour, or vice versa.

   3. A NETWORK WINDOW WILL CORRUPT THIS — M2-T8 OWNS THE FIX.
      With CONFIG_MAGTAG_SYNC_LED_FEEDBACK=y (the default, and the current
      sdkconfig) net_window.c writes pixel 3 from its own task at window
      open, at the NTP result, and again at net_window_join(), where it
      sets that pixel to 0,0,0. Pixel 3 is a chore row under the mapping
      in status_led.c, and the corrupted colours are not merely wrong,
      they are CONVINCING: net_window.c's NTP-success triple is (0,20,0),
      the same three bytes as this module's CHORES_LED_DONE. A sync that
      lands while the checklist is up therefore paints a chore row a
      PERFECT green — a silent false ack, a row reading as done that
      nobody did, with nothing on the screen to contradict it. That is
      the worst of the three and the one to size the risk by. The
      failure triple (30,0,0) is likewise a near-match for the (25,0,0)
      red. The window-open blue (0,0,20) and the join's dark are the mild
      cases: dark reads as "not a configured chore", which is wrong but
      at least reads as anomalous.

      The DIRECTION is settled and is not T8's to re-litigate: in chore
      mode the chore display owns the whole strip, so T8 must SUPPRESS OR
      RELOCATE net_window's pixel feedback for chore-mode wakes, rather
      than weaken the chore paint around it. Timer modes keep their sync
      pixel — there it is how the user knows the device is awake and
      working; on the checklist that is obvious without it.

      The MECHANISM is T8's to pick, because T8 owns the call site that
      can test the choice. Candidates, none of them decided here: gate
      net_window.c's writes on a chore-mode flag; repaint the strip after
      net_window_join() returns; hold the window until the checklist is
      torn down; or move the sync feedback to a pixel the checklist does
      not own — noting that in chore mode there is no such pixel, all
      four are claimed, so that option means giving up a chore row.

   NOTED HAZARDS — carried, deliberately not solved here. Both are policy
   or sizing questions for T8/design, and T7 takes no position on either:

   * QUIET HOURS REMOVE THE ONLY ACK FEEDBACK. Status class means
     neopixel.c drops every post while the quiet callback returns true
     (status_muted()), so all four posts below become no-ops. Design §2.5
     made these pixels the ONLY ack feedback — it deliberately stopped
     making the panel the feedback channel — so during quiet hours an ack
     produces NO feedback at all until the ~1.9 s panel partial catches
     up. Whether that is acceptable, or whether a chore ack should be
     HIGHPRI class like the alarm is, has not been decided.

   * THE LED QUEUE CAN SWALLOW A PIXEL. The queue is 8 deep and this
     function bursts four posts with a zero timeout; a queue already
     carrying other traffic drops the overflow with only an ESP_LOGW.
     That leaves exactly the half-painted strip the write-every-pixel
     rule below exists to prevent. Not observed — but the depth was
     sized for one alert sequence, not for a four-post burst behind it. */
void chores_led_show(uint8_t mask, uint8_t n, bool released);

#ifdef __cplusplus
}
#endif
