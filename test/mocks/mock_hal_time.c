#include "mock_hal_time.h"

static time_t s_mock_time = MOCK_TIME_DEFAULT;
static uint32_t s_delay_total_ms;
static uint32_t s_delay_carry_ms; /* sub-second remainder, see hal_delay_ms */

void mock_time_set(time_t t) {
    s_mock_time = t;
    /* Pinning the clock also drops the delay accounting: a test that sets
       an exact second must not inherit 750 ms of someone else's polling,
       and the seven existing suites that call this from setUp() must not
       see a total carried in from the previous case either. */
    s_delay_total_ms = 0;
    s_delay_carry_ms = 0;
}

void mock_time_reset(void) {
    s_mock_time = MOCK_TIME_DEFAULT;
    s_delay_total_ms = 0;
    s_delay_carry_ms = 0;
}

uint32_t mock_delay_total_ms(void) {
    return s_delay_total_ms;
}

time_t hal_time_now(void) {
    return s_mock_time;
}

/* Never sleeps — the suite must run the awake poll loops instantly. But
   the delay still has to be observable as time passing, or a loop that
   waits for the wall clock to move never terminates. hal_time_now() has
   only second resolution, so carry the sub-second remainder forward:
   four 250 ms polls advance the clock by exactly one second, where
   truncating each call would freeze it. */
void hal_delay_ms(uint32_t ms) {
    s_delay_total_ms += ms;
    s_delay_carry_ms += ms;
    s_mock_time += (time_t)(s_delay_carry_ms / 1000u);
    s_delay_carry_ms %= 1000u;
}
