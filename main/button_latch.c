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

int button_latch_pick(uint8_t mask, uint8_t allowed_mask) {
    /* A (0) first — start/pause is the time-sensitive action; then C (2)
       select, B (1) reset, D (3) sync. */
    static const int priority[BUTTON_LATCH_COUNT] = {0, 2, 1, 3};
    mask &= allowed_mask;
    for (int i = 0; i < BUTTON_LATCH_COUNT; i++) {
        if (mask & (1u << priority[i]))
            return priority[i];
    }
    return -1;
}
