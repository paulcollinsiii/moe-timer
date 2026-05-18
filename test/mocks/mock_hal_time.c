#include "mock_hal_time.h"

static time_t s_mock_time = 1767225600; /* 2026-01-01 00:00:00 UTC */

void mock_time_set(time_t t) {
    s_mock_time = t;
}

time_t hal_time_now(void) {
    return s_mock_time;
}
