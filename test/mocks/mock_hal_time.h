#pragma once
#include "hal_time.h"

/* Set the time returned by hal_time_now() for the current test. */
void mock_time_set(time_t t);
