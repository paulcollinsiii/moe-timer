#include "status_led.h"

#include "chores.h"
#include "neopixel.h"
#include "timer.h"

status_led_rgb_t status_led_for_state(timer_state_t state) {
    switch (state) {
        case TIMER_RUNNING:
            return (status_led_rgb_t){0, 20, 0};
        case TIMER_PAUSED:
            return (status_led_rgb_t){25, 15, 0};
        case TIMER_EXPIRED:
            return (status_led_rgb_t){25, 0, 0};
        case TIMER_BREAK:
            return (status_led_rgb_t){0, 10, 25}; /* blue-cyan */
        default:
            return (status_led_rgb_t){10, 10, 10};
    }
}

void status_led_show_timer_state(void) {
    status_led_rgb_t rgb = status_led_for_state(timer_get_state());
    neopixel_status_pixel(NP_STATE_PIXEL, rgb.r, rgb.g, rgb.b);
}

/* ---- the chore checklist's strip (design §2.5) -------------------------- */

/* Three chore rows plus the gate is exactly the strip. If either constant
   ever moved, the mapping below would stop being a permutation and a
   pixel would be left holding whatever painted it last. */
_Static_assert(CHORE_MAX + 1 == NEOPIXEL_COUNT, "chore mode needs one pixel per chore plus one for the gate");

/* THE chore-to-pixel mapping, and the only one in the tree. Slot i below
   CHORE_MAX is chore i's physical pixel; slot CHORES_LED_GATE_SLOT is the
   gate's. Everything else works in slots and reads this table, so
   inverting the strip is this one initialiser.

   M2-HW2 (design §8, Q-B) SETTLED THIS LINE ON A BOARD, 2026-09-22, and
   the answer was the one the strip-is-reversed branch predicted: THE
   PIXELS RUN OPPOSITE THE BUTTONS. Pixel 3 sits over button A and pixel 0
   over button D, so the old {1, 2, 3, 0} — written from the guess that
   pixel i sits over button i — lit the wrong row for every chore.

   What the table says now, and it is one rule and not four exceptions:
   the pixel over button b is 3 - b. The ack buttons B/C/D are chores
   0/1/2 (BUTTON_CHORE_IDX_B/C/D), so they are buttons 1/2/3 and take
   pixels 2/1/0; the gate slot is button A, button 0, and takes pixel 3.
   Hence {2, 1, 0, 3} — still a permutation of the strip, which
   test_the_mapping_covers_every_pixel_exactly_once holds, and still the
   only mapping in the tree: everything else works in slots and reads this
   table, so a future strip is again this one initialiser.

   Why nothing caught it before a board did: the only support for the old
   reading anywhere in the tree was a comment on neopixel_status_binary4()
   (neopixel.h: "pixel 0 (over button A) = bit3"), and its sole caller —
   the last-15-seconds countdown — would look merely mirrored if it were
   wrong, so nothing ever held it true. That parenthetical named the wrong
   button and has been corrected.

   THE COUNTDOWN ITSELF WAS NEVER WRONG, and this is worth stating plainly
   because the two defects look identical from the source. Both run off
   the same reversed strip, but only one of them was a bug: a checklist
   lighting the row above the wrong BUTTON is wrong by construction,
   whereas a binary number is only ever a convention about which end the
   MSB goes. Pixel 0 being over button D puts the MSB on the RIGHT, the
   owner read it on the board on 2026-09-22 and called it correct, and
   neopixel.h now records that as verified. So there is nothing deferred
   here and nothing for a later reader to tidy: correcting the comment
   above WAS the whole of that fix. Flipping the countdown to match this
   table would break working behaviour. */
#define CHORES_LED_GATE_SLOT CHORE_MAX
static const uint8_t k_chore_pixel[CHORE_MAX + 1] = {2, 1, 0, 3};

/* The timer table's own green and red, reused rather than re-picked: the
   same person reads both on the same device, and a second almost-green
   would only ever be a mistake. Keeping them literal keeps them where the
   other triples are — read off the device by eye, so part of the
   contract. */
#define CHORES_LED_DONE ((status_led_rgb_t){0, 20, 0}) /* == TIMER_RUNNING  */
#define CHORES_LED_TODO ((status_led_rgb_t){25, 0, 0}) /* == TIMER_EXPIRED  */

chores_led_t chores_led_for(uint8_t mask, uint8_t n, bool released) {
    chores_led_t out = {0}; /* dark is the default: a pixel must be claimed to light */

    if (n > CHORE_MAX)
        n = CHORE_MAX; /* from NVS; only CHORE_MAX rows and CHORE_MAX pixels exist */
    if (n == 0)
        return out; /* C1: no list, so nothing is withheld and the gate has nothing true to say */

    for (uint8_t i = 0; i < n; i++) {
        /* THE LOOP BOUND is what keeps a mask left over from a longer
           list off the screen: rows i >= n are never visited, so they
           keep the dark default above. chores_is_acked() cannot help
           with that here — with n already clamped, i < n means it is
           never handed an out-of-range index and its guard never runs.
           It is called rather than shifting inline as defence in depth,
           so that a future edit loosening this bound still cannot light
           a row that is not on the screen, and so that "which bits
           count" keeps exactly one definition (chores.h). */
        out.px[k_chore_pixel[i]] = chores_is_acked(mask, i, n) ? CHORES_LED_DONE : CHORES_LED_TODO;
    }
    /* The LATCH, not the mask. Release is a day flag that un-acking does
       not revoke, and it is set one tick after the last ack lands. */
    out.px[k_chore_pixel[CHORES_LED_GATE_SLOT]] = released ? CHORES_LED_DONE : CHORES_LED_TODO;
    return out;
}

void chores_led_show(uint8_t mask, uint8_t n, bool released) {
    chores_led_t t = chores_led_for(mask, n, released);
    /* Every pixel, including the dark ones: this takes over a strip with
       four other claimants (NP_STATE_PIXEL above, net_window.c's pixel 3,
       and the two strip-wide ones listed in status_led.h — the countdown's
       binary4 and the alert pulse), any of which may have left colours
       standing, so a skipped pixel is a stale one.
       Of those four, three can no longer repaint over this AFTER it
       returns — the timer pixel and the sync pixel are both stood down for
       the wake (status_led.h's caller contract, items 2 and 3), and the
       countdown cannot paint a screen the checklist is on because
       display_screen_for() answers MAIN for a RUNNING timer, whatever the
       mode byte says (display_layout.c: `timer_state != TIMER_RUNNING`).
       THAT ONE LINE IS THE WHOLE GUARANTEE, and it is stated that way
       because the version of this comment that said "the countdown needs a
       RUNNING timer chore mode does not reach" was FALSE: chore mode
       reaches RUNNING freely — the mode byte and the timer state are
       independent, and button_actions.c gates an ack without consulting
       the state at all. What chore mode cannot do is be the SCREEN while a
       timer runs, which is a different claim, in one file, checkable.
       The alert pulse still can repaint over this, which is why item 4
       makes repainting after an alarm the caller's job rather than
       something this function could defend against.

       HIGHPRI CLASS, AND IT IS THE ONLY HIGHPRI PAINT IN THE TREE. Design
       §2.5 deliberately stopped making the panel the ack channel, so these
       four pixels are the ONLY feedback a press gets until the panel
       paints — the hold, a quiet window and a partial later: ~2.4 s from
       the press at the defaults, plus the boot, the partial's ~0.8 s being
       an estimate (M2-T15). Status class drops every post while the quiet-hours
       callback is true, which made a night-time ack produce nothing at
       all: no pixel, no panel, no sound. Quiet hours exist for a sleeping
       house; a checklist ack is a deliberate press by somebody awake and
       standing at the device, which is not what the mute was written for.
       Nothing else moves class — status_led_show_timer_state() above and
       net_window.c's sync pixel are both still STATUS, because both of
       those really are ambient and really should go dark at night.
       NP_MSG_PIXEL_HI still applies the brightness scale (neopixel.c), so
       this bypasses the MUTE and not the user's dimmer. */
    for (int p = 0; p < NEOPIXEL_COUNT; p++) {
        neopixel_highpri_pixel(p, t.px[p].r, t.px[p].g, t.px[p].b);
    }
}
