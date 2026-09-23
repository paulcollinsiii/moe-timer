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
                   are the ack feedback. Both halves have a mechanism now
                   (M2-T8): the timer pixel via wake_flow.c's single
                   status-paint wrapper, the WiFi pixel via
                   net_window_claim_leds(). Items 2 and 3 of the caller
                   contract on chores_led_show() carry both.
                   ONLY ON A BUTTON WAKE, though: row C17 keeps the strip
                   dark on an unattended wake that happens to paint the
                   chore screen, and there both single-pixel owners keep
                   their pixels exactly as they do in any other mode.

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

   The alarm one was a live collision for the chore screen, not a
   theoretical one, and M2-T8 closed it: an expiry alarm firing while the
   checklist is up still wipes the strip, and wake_flow_fire_expiry_alert()
   now repaints it afterwards. See item 4 of the caller contract below for
   how it is reached and why the BREAK alarm deliberately gets no such
   repair.

   The binary4 countdown has NO such repair and is the one strip-wide
   claimant M2-T8 left alone. It is entered only from wake_flow's
   final-minute watch, which runs only while timer_get_state() ==
   TIMER_RUNNING — and a RUNNING timer is never the checklist's screen,
   because display_screen_for() answers MAIN for one whatever the mode byte
   says (display_layout.c: `timer_state != TIMER_RUNNING`).

   THAT ONE LINE, IN THAT ONE FILE, IS THE WHOLE GUARANTEE. An earlier
   version of this note argued it from four facts instead — "no press
   reaches a chore-mode RUNNING today: B's start is rebound, C no longer
   swaps, the join poll is guarded, and HA cannot start a timer" — and that
   argument was FALSE as well as fragile. All four are about how a timer
   STARTS; none of them covers a timer that was ALREADY RUNNING when chore
   mode was entered, which needs no press at all. Chore mode reaches
   RUNNING freely: the mode byte and the timer state are independent, and
   button_actions.c gates an ack with no timer-state term. What it cannot
   do is be the SCREEN while a timer runs. Prefer the single-line
   guarantee; a reachability argument dressed up as one is worse than
   either, because it reads as settled.

   If that line moves, the countdown will paint over the checklist and
   nothing here will notice — it does not go through wake_flow's
   status-paint wrapper, because it is not a status paint. */
#define NP_STATE_PIXEL 0

/* How long a PRE-PRESS colour is held before the pixel that acknowledges
   the press changes. The acknowledgement is the TRANSITION, not the
   destination colour, so there has to be a frame of the old state for the
   new one to be a change from — repaint with no hold and a user sees one
   colour appear, which is indistinguishable from a device that was
   already showing it.

   ONE constant for two call sites, which is the whole reason it is here
   rather than a literal at either: Button B's start/resume (wake_flow.c)
   and the chore ack (design §2.5) are the same gesture — hold the old
   colour, then change it — read by the same person on the same device.
   Two figures for that would be an inconsistency nobody could interpret,
   and nothing but a shared constant and a test across both paths stops
   them drifting.

   400 IS DESIGN §2.5's PROSE ESTIMATE, ADOPTED BECAUSE A BOARD RULED 250
   OUT — WHICH IS NOT THE SAME AS A BOARD CHOOSING 400, and that gap is
   the first thing to know about this number. The 2026-09-22 session
   produced one fact and it is a LOWER BOUND: 250 is too short. Nothing
   has been measured above it. 400 is unvalidated UPWARD — 300 may do,
   600 may be better — and the only reason it rather than some other
   figure above 250 is that §2.5 had already guessed ~400 before any of
   this. Design §8 Q-C records it as a first correction wanting another
   pass. Treat it as provisional; do not cite it as swept.

   This line carried 250 first, on an argument worth keeping because the
   measurement is exactly what overturned it. That argument ran: 250 was
   the only figure anybody had watched work (it had been in the tree since
   Button B's start path was written) while ~400 was an estimate in prose;
   §2.5's own thesis is that the pixels are the FAST channel covering the
   panel's ~1.9 s partial, so 150 ms more of deliberate nothing in front
   of the ack spends the very latency the section exists to remove; and
   250 ms is already this device's poll quantum, so the hold costs at most
   one extra pass of a loop that was going to run anyway.

   Every clause of that is still true, and it still loses, because it
   weighed the wrong half. 250 had been watched on a board as a HOLD, in
   front of one press; nobody had watched it as the COALESCING WINDOW,
   which is the other job this same figure does and the one a child is
   inside when they reach for the second chore. On the glass, 2026-09-22,
   the window was the binding half: 250 ms was not long enough to get the
   next chore pressed before the refresh began, so a child ticking two
   boxes got two ~1.9 s redraws instead of one. 150 ms of extra latency in
   front of the first ack buys that back, and a redraw is an order of
   magnitude more of the same latency.

   Which is the lesson the two-jobs-one-figure shape was always carrying:
   the number is only "measured" for whichever job the measurement was
   watching. If a board ever wants them apart, this is the line to split.

   IT IS A MENUCONFIG KNOB, because that is how a board says so:
   CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS, so a sweep is a rebuild rather
   than a patch. The host build has no sdkconfig.h, so it takes the
   literal below — the same #ifdef shape nvs_defaults.h uses for every
   other Kconfig-backed constant, and for the same reason: the tests must
   not need a generated header to compile.

   THE FALLBACK AND THE KCONFIG DEFAULT MUST AGREE, and what holds that is
   scripts/check-ack-hold-default.py, a pre-commit gate that reads the
   `default` out of the Kconfig block and the literal out of the #else
   below and fails when they differ. It is named here rather than left as
   an exhortation because an earlier version of this comment asserted the
   MUST as though a mechanism carried it, and review found the assertion
   was doing all the work AND that its stated consequence was unreachable:
   the ONLY host suite that expands STATUS_LED_ACK_HOLD_MS is
   test_wake_flow, which is built with CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS
   defined (=170) and so takes the #ifdef arm. Every other suite compiles
   the #else and never mentions the macro. Mutating the literal below to
   250 or to 0 therefore changed no host binary and failed no test.

   So the reason to keep the two in step is NOT "the host suites would
   time a different device" — today none of them would notice. It is that
   the next suite to measure this figure without an EXTRA_DEFS override
   would silently be timing the fallback while the firmware timed the
   Kconfig default, and it would pass. The gate makes the divergence
   impossible to commit instead of waiting for that suite to exist.

   test_wake_flow is deliberately built at NEITHER value
   (test/CMakeLists.txt overrides it to 170) so that no case there can
   quietly encode today's figure as a literal; the gate does not look at
   that override and must not, since disagreeing with both is its point.

   THE HOLD IS ALSO THE COALESCING WINDOW (wake_flow.c's ack drain): after
   an ack the panel work is held open for this long, polling for the next
   press, so two or three boxes ticked in one go land in ONE refresh with
   every flip immediate. That is deliberately one figure and not two, but
   the two halves pull in OPPOSITE directions — a longer window buys
   clicking time, a longer hold spends the latency §2.5 exists to remove —
   so a board that wants them apart splits this line, and the Kconfig help
   says so. */
#ifdef CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS
#define STATUS_LED_ACK_HOLD_MS CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS
#else
#define STATUS_LED_ACK_HOLD_MS 400
#endif

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
   strip is up: NP_STATE_PIXEL is pixel 0, which is chore slot 2's row
   (button D's) under status_led.c's mapping, so it would repaint a chore
   row with a timer colour. */
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

   Pure: no clock, no NVS, no globals, no GPIO. The ack sequencing needs
   that, because the first frame painted on a button wake is the PRE-press
   state, which by then disagrees with the live one.

   SIGNATURE DRIFT: design §2.5 still writes this as chores_led_for(mask,
   n), two arguments. The DOC is stale, not the code — `released` is a
   latch and provably not derivable from the mask (see above), so the
   two-argument form cannot express the feature. Do not "correct" the
   code to match the doc; amending the doc is the orchestrator's call. */
chores_led_t chores_led_for(uint8_t mask, uint8_t n, bool released);

/* Paint chores_led_for()'s four pixels over neopixel.c. Status class, so
   quiet hours and brightness are handled inside that module.

   CALLER CONTRACT — three things this function cannot enforce. The sole
   caller today is wake_flow.c, which honours all three; each entry names
   where, so a second caller knows what it is taking on:

   1. BUTTON WAKES ONLY (design §2.5, "Power discipline"). An unattended
      tick or NTP wake that happens to paint the chore screen must leave
      the pixels dark, or chore mode lights four LEDs every 55 s for
      nobody. This function paints whenever it is called; the wake cause
      is the caller's to check.
      HONOURED BY wake_flow.c's s_chore_strip_lit, which is set in exactly
      one place — the EXT1 button-wake decode — and never on a tick wake,
      including one that drains a latched ack out of a previous wake's
      tail (test_t8_a_latched_ack_on_a_tick_wake_leaves_the_strip_dark).

   2. NOT ALONGSIDE status_led_show_timer_state(). Both write pixel 0.
      CHORE SLOT 2 — button D's row — lands there, so a timer paint in
      the same wake turns that row into a timer colour, or vice versa.
      (Pixel 0 held the GATE until M2-HW2 inverted the strip on
      2026-09-22. The collision is unchanged by that: the mapping is a
      permutation of the whole strip, so pixel 0 always belongs to some
      slot. Only which slot it is moved.)
      HONOURED BY wake_flow_show_status_leds(), which is the ONLY caller
      of status_led_show_timer_state() in that file — an invariant a reader
      can check by grep, and which scripts/check-status-led-wrapper.py
      checks by grep on every commit, because "a reader can check it" is
      not the same as anything checking it: ten of the twelve call sites
      that wrapper replaced turned out to be unpinned by any test — and
      which repaints the checklist instead on any wake that claimed the
      strip
      (test_t8_a_chore_mode_wake_never_paints_a_timer_colour_over_a_chore_row).

   3. A NETWORK WINDOW WILL CORRUPT THIS unless the caller stands the
      sync pixel down first. With CONFIG_MAGTAG_SYNC_LED_FEEDBACK=y (the
      default, and the current sdkconfig) net_window.c writes pixel 3 at
      window open, at the NTP result, and again at net_window_join(),
      where it sets that pixel to 0,0,0. Pixel 3 is THE GATE under the
      mapping in status_led.c — it was chore slot 2's row until M2-HW2
      inverted the strip on 2026-09-22, and the inversion moved the gate
      onto the sync pixel rather than off it, so this item got worse and
      not better. The corrupted colours are not merely wrong, they are
      CONVINCING: net_window.c's NTP-success triple is (0,20,0), the same
      three bytes as this module's CHORES_LED_DONE, and on the GATE that
      green does not mean "one row is done", it means THE DAY IS RELEASED
      — the withheld time is granted. A sync landing while the checklist
      is up therefore paints a PERFECT green release that nobody earned,
      with nothing on the screen to contradict it. That is the worst of
      the four and the one to size the risk by, and it is a strictly
      bigger lie than the single false row this said before the strip was
      inverted. The failure triple (30,0,0) is likewise a near-match for
      the (25,0,0) red, which on the gate reads as "still locked" — false
      whenever the day HAS been released, though at least the cautious
      direction. The window-open blue (0,0,20) and the join's dark are
      the mild cases: blue is not in this module's vocabulary at all, and
      a dark gate reads as "nothing is withheld" (C1's n == 0 strip),
      which is wrong but at least reads as anomalous.
      HONOURED BY net_window_claim_leds(), which suppresses all four
      writes at the source for the rest of the wake. M2-T8 chose
      suppression over a repaint after the join because the false green
      is written by net_window_wait_ntp() and the caller then spends the
      whole ~1.9 s panel partial before it could repaint — exactly the
      stretch the user is looking at the pixels. The reachable route is
      the DAY ROLLOVER, which opens a window from inside the button
      handler's own prologue, so the claim is made above it
      (test_t8_the_checklist_claims_the_sync_pixel_before_the_rollover_opens_a_window).
      Timer modes keep their sync pixel: there it is how the user knows
      the device is awake and working, and nothing else claims the strip.

   AND ONE THE CALLER MUST REPAIR RATHER THAN PREVENT:

   4. AN ALARM WIPES THIS AND DOES NOT PUT IT BACK.
      neopixel_alert_pulse_*() owns every pixel while it runs and its
      _end() clears all four and drops the power gate (neopixel.h), so
      whatever was painted before an alarm is gone after it. It is
      reachable on a chore-mode wake: a config edit arriving in the
      window can move the active slot to EXPIRED under the press, and
      wake_policy_render answers EXPIRY_ALERT for any transition INTO
      expired, not only from RUNNING. The expiry alert then RETURNS and
      the wake carries on, so the checklist would sit on the glass with a
      dead strip under it until sleep.
      REPAIRED BY wake_flow_fire_expiry_alert(), which repaints the strip
      after the alarm when the strip is its to repaint
      (test_t8_an_expiry_alarm_repaints_the_strip_it_wiped). The BREAK
      alarm deliberately does not: that path goes straight to sleep, and
      relighting the strip for the length of an MQTT join would be a
      battery cost with nobody left to read it.

   NOTED HAZARDS. The first was a policy question and now has an answer;
   the second is carried, and is sized here so the next person does not
   have to re-derive it:

   * QUIET HOURS USED TO REMOVE THE ONLY ACK FEEDBACK — SETTLED, and the
     answer is HIGHPRI. Status class means neopixel.c drops every post
     while the quiet callback returns true (status_muted()). Design §2.5
     made these pixels the ONLY ack feedback — it deliberately stopped
     making the panel the feedback channel — so during quiet hours an ack
     produced no feedback at ALL: no pixel, no panel for ~1.9 s, no sound.
     The user chose to let acks through the mute, so this function posts
     HIGHPRI class (status_led.c says it at the loop). Quiet hours are for
     a sleeping house; a checklist ack is a deliberate press by somebody
     awake and standing at the device, which is not what the mute was
     written for. It bypasses the MUTE and not the DIMMER — HIGHPRI still
     takes the status brightness percent (neopixel.c) — and nothing else
     moved class: the timer state pixel and net_window's sync pixel are
     genuinely ambient and still go dark at night.

   * THE LED QUEUE CAN SWALLOW A PIXEL. The queue is 8 deep and this
     function bursts four posts with a zero timeout; a queue already
     carrying other traffic drops the overflow with only an ESP_LOGW.

     BURSTS PAST THE DEPTH ARE REACHABLE TODAY, which is the correction an
     earlier version of this note needed: it credited
     STATUS_LED_ACK_HOLD_MS with separating the frames and concluded that
     only "a FUTURE caller that paints twice with nothing in between" could
     reach eight. A SECOND ack pays no hold at all (§2.5 clause 4), so that
     caller already existed on the break tail — the dispatch's flip and the
     poll's own status paint are adjacent — and wake_flow.c's coalescing
     drain can now apply two or three latched acks in one pass, which is 8
     or 12 posts with nothing between them. The LED task's priority (5,
     above the main task) does not save it either: the task preempts on the
     first post, but flush_pixels() then blocks in
     rmt_tx_wait_all_done(portMAX_DELAY), and the main task can issue the
     rest of the burst inside one ~200 us flush.

     WHAT ACTUALLY KEEPS THE OUTCOME CORRECT is the write-every-pixel rule
     below, plus one ordering fact: every frame carries ALL FOUR pixels
     computed from the LIVE state, so a dropped post is never a MISSING
     update — only a stale one — and the next frame of the wake repairs it
     wholesale. Every ack burst in wake_flow.c is followed, within
     milliseconds and before any panel work, by that file's own status
     paint (an ADC read and two NVS reads later), which repaints the whole
     strip from the state the acks left. THAT is the property to preserve:
     an ack burst must never be the LAST strip paint of a wake. */
void chores_led_show(uint8_t mask, uint8_t n, bool released);

#ifdef __cplusplus
}
#endif
