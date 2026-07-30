#pragma once
#include <stdint.h>

#include "hal_time.h"

/* Default epoch: 2026-01-01 00:00:00 UTC. */
#define MOCK_TIME_DEFAULT 1767225600

/* Set the time returned by hal_time_now() for the current test. Also
   clears the delay accounting, so pinning the clock is a clean slate. */
void mock_time_set(time_t t);

/* Back to the default epoch, with the delay accounting cleared. Suites
   that exercise hal_delay_ms() call this from setUp(), since the clock
   and the accumulator both carry across tests in a single-TU suite. */
void mock_time_reset(void);

/* Total ms passed to hal_delay_ms() since the last reset — lets a suite
   assert how long a poll loop believed it waited. */
uint32_t mock_delay_total_ms(void);
