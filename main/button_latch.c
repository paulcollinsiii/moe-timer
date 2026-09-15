#include "button_latch.h"

#include <string.h>

#define BUTTON_LATCH_COUNT 4

static volatile uint8_t s_mask;
/* Last ACCEPTED edge per button — rejected chatter must not extend the
   window, or a long bounce train would mask a genuine second press. */
static int64_t s_last_edge_us[BUTTON_LATCH_COUNT];

void button_latch_reset(void) {
    s_mask = 0;
    memset(s_last_edge_us, 0, sizeof(s_last_edge_us));
}

void button_latch_record(int btn, int64_t t_us) {
    if (btn < 0 || btn >= BUTTON_LATCH_COUNT)
        return;
    if (s_last_edge_us[btn] != 0 && t_us - s_last_edge_us[btn] < BUTTON_LATCH_DEBOUNCE_US)
        return;
    s_last_edge_us[btn] = t_us;
    s_mask |= (uint8_t)(1u << btn);
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
