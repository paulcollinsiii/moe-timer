/* Pure LiPo state-of-charge estimation — no ESP dependencies; host-tested
   via ctest. The ADC/hardware side lives in battery.c. */
#include "battery.h"

/* Piecewise-linear LiPo OCV approximation. Resting-voltage based; good to
   ~10% which is plenty for a fridge-display gauge. */
static const struct {
    int mv;
    int pct;
} CURVE[] = {
    {3300, 0}, {3500, 15}, {3650, 35}, {3800, 55}, {3950, 80}, {4150, 100},
};
#define CURVE_LEN ((int)(sizeof(CURVE) / sizeof(CURVE[0])))

int battery_percent_from_mv(int mv) {
    if (mv <= CURVE[0].mv)
        return 0;
    if (mv >= CURVE[CURVE_LEN - 1].mv)
        return 100; /* USB power reads above true full — clamp */
    for (int i = 1; i < CURVE_LEN; i++) {
        if (mv < CURVE[i].mv) {
            int span_mv = CURVE[i].mv - CURVE[i - 1].mv;
            int span_pct = CURVE[i].pct - CURVE[i - 1].pct;
            return CURVE[i - 1].pct + (mv - CURVE[i - 1].mv) * span_pct / span_mv;
        }
    }
    return 100;
}
