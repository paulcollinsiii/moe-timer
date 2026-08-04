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

/* Called at the end of every hal_delay_ms(), after the clock has moved.

   This is how a suite models something that happens WHILE a poll loop is
   waiting — a GPIO ISR latching a button press being the case that
   matters — without hanging it off a call the code under test makes. That
   distinction is the whole point: an event armed inside a stub the loop
   happens to call can only ever arrive on code paths that still call it,
   so a change which stops calling it makes the event silently never
   arrive, and a test asserting "the press was discarded" passes just as
   happily when no press was ever delivered. Time passing is not something
   the code under test gets a vote on, so this is where such an event
   belongs.

   NULL (the default) is a no-op, and neither mock_time_set() nor
   mock_time_reset() clears it — a suite that installs one does so once,
   from setUp(). */
void mock_delay_set_hook(void (*fn)(void));
