#include "button_latch.h"

#include <stdbool.h>
#include <string.h>

#define BUTTON_LATCH_COUNT 4

static volatile uint8_t s_mask;
/* Last ACCEPTED edge per button — rejected chatter must not extend the
   window, or a long bounce train would mask a genuine second press. */
static int64_t s_last_edge_us[BUTTON_LATCH_COUNT];
/* THE RELEASE GATE (button_latch.h). Has this button been observed back UP
   since its last accepted press? A falling edge only becomes a press when
   it has, which is what tells a genuine second press apart from the
   release bounce of the first — the two are identical as edges, and no
   window measured from the press edge can separate them because the hold
   in between is as long as the person holding it.

   Seeded TRUE by reset, with NO anchor, so the first edge of a wake needs
   no prior sample to be accepted. Which pads are actually up is not
   something reset can know: buttons.c samples them immediately afterwards
   and before arming the ISRs, and that sample is what supplies both the
   truth and the anchor (note_levels below). */
static bool s_released_seen[BUTTON_LATCH_COUNT];
/* When that observation was made, and a second debounce anchor. A level
   sample can land in a HIGH phase of a bounce train and see "up" while the
   contact is still chattering; anchoring here rejects the falling edge that
   follows it a millisecond later, which the press-edge anchor above cannot
   (it is a whole hold duration away by then). Its window is
   BUTTON_LATCH_RELEASE_SETTLE_US and NOT the chatter window — see
   button_latch.h, where reusing the chatter window is written up as the
   defect that cost a real press for every one it saved. Zero means "no
   observation to anchor on", exactly as it does for s_last_edge_us. */
static int64_t s_released_us[BUTTON_LATCH_COUNT];

void button_latch_reset(void) {
    s_mask = 0;
    memset(s_last_edge_us, 0, sizeof(s_last_edge_us));
    memset(s_released_us, 0, sizeof(s_released_us));
    for (int i = 0; i < BUTTON_LATCH_COUNT; i++) {
        s_released_seen[i] = true;
    }
}

void button_latch_record(int btn, int64_t t_us) {
    if (btn < 0 || btn >= BUTTON_LATCH_COUNT)
        return;
    /* Still down as far as anybody has looked, so this edge is bounce on a
       press already counted — not a new one. */
    if (!s_released_seen[btn])
        return;
    if (s_last_edge_us[btn] != 0 && t_us - s_last_edge_us[btn] < BUTTON_LATCH_DEBOUNCE_US)
        return;
    if (s_released_us[btn] != 0 && t_us - s_released_us[btn] < BUTTON_LATCH_RELEASE_SETTLE_US)
        return;
    s_last_edge_us[btn] = t_us;
    s_released_seen[btn] = false;
    s_released_us[btn] = 0;
    s_mask |= (uint8_t)(1u << btn);
}

void button_latch_note_levels(uint8_t held_mask, int64_t t_us) {
    for (int i = 0; i < BUTTON_LATCH_COUNT; i++) {
        if (held_mask & (1u << i)) {
            /* Down again (or still down): whatever edges arrive next belong
               to THIS press, and the one after its release is bounce. */
            s_released_seen[i] = false;
            s_released_us[i] = 0;
            continue;
        }
        /* Anchor the observation, but only when this button has no anchor
           yet. Two samples qualify and no others: the down-to-up
           TRANSITION, and the first sample of a button reset seeded as
           released (s_released_seen true, s_released_us zero) — without the
           second term that button's next edge clears both windows, so a tap
           released just before the sample lands its bounce train's next
           falling edge as a phantom press.
           Refreshing the anchor on every sample instead would push the
           window forward under a press the user is about to make, and
           reject it. */
        if (!s_released_seen[i] || s_released_us[i] == 0) {
            s_released_seen[i] = true;
            s_released_us[i] = t_us;
        }
    }
}

uint8_t button_latch_take(void) {
    uint8_t taken = s_mask;
    s_mask = 0;
    return taken;
}

uint8_t button_latch_take_masked(uint8_t mask) {
    uint8_t taken = s_mask & mask;
    s_mask &= (uint8_t)~mask;
    return taken;
}

int button_latch_pick(uint8_t mask, uint8_t allowed_mask) {
    /* B (1) first — start/pause/resume is the time-sensitive action; then
       C (2) next timer, D (3) refresh, and A (0) LAST: the mode toggle
       only chooses which screen is painted, so it is the one press that
       loses nothing by yielding to a press that moves a timer.

       The rule is "the time-sensitive action wins", not "the lowest index
       wins": this list moved when the layout did (A used to be
       start/pause). Leaving A at the front once it lost start/pause made
       it SWALLOW a real press — pick returns the first candidate in this
       order, so A won, the dispatch's A arm did nothing (it had no
       binding then), and the B press was gone with it, taken by the same
       unmasked take and dropped at sleep. A has a binding again as of
       M2-T3, which is what let both drains admit it to their ALLOWED
       masks; keeping it last here is what makes that safe, so the two
       facts are load bearing together. */
    static const int priority[BUTTON_LATCH_COUNT] = {1, 2, 3, 0};
    mask &= allowed_mask;
    for (int i = 0; i < BUTTON_LATCH_COUNT; i++) {
        if (mask & (1u << priority[i]))
            return priority[i];
    }
    return -1;
}
