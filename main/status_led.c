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

   M2-HW2 (design §8, Q-B) OWNS THIS LINE. The values below encode an
   assumption nobody has checked on a board: that pixel i sits over button
   i. Under it the ack buttons B/C/D — BUTTON_CHORE_IDX_B/C/D are chores
   0/1/2 — take pixels 1/2/3, and the gate takes pixel 0, over button A,
   the button that switches into chore mode in the first place.

   The only support for that reading anywhere in the tree is a comment on
   neopixel_status_binary4() ("pixel 0 (over button A) = bit3"), and its
   sole caller — the last-15-seconds countdown — would look merely
   mirrored if it were wrong, so nothing has ever held it true. If the
   strip runs opposite the buttons, this becomes {2, 1, 0, 3}; change
   test_the_mapping_is_button_order_until_hardware_says_otherwise with it
   and every other test in that suite keeps passing unchanged. */
#define CHORES_LED_GATE_SLOT CHORE_MAX
static const uint8_t k_chore_pixel[CHORE_MAX + 1] = {1, 2, 3, 0};

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
       standing, so a skipped pixel is a stale one. See the caller contract
       in status_led.h — especially the network window and the expiry
       alarm, either of which can still repaint over this after it
       returns. */
    for (int p = 0; p < NEOPIXEL_COUNT; p++) {
        neopixel_status_pixel(p, t.px[p].r, t.px[p].g, t.px[p].b);
    }
}
