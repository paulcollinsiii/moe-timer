#pragma once
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns current Unix timestamp. Wraps time(NULL) in production;
   injectable via mock_time_set() in native tests. */
time_t hal_time_now(void);

/* Blocks the calling task. Wraps vTaskDelay() in production; the native
   mock does not sleep — it advances the injected clock instead, so the
   awake poll loops run instantly under test while still observing time
   pass (a loop waiting on the wall clock would otherwise never exit). */
void hal_delay_ms(uint32_t ms);

/* Free-running millisecond counter since boot, for gestures timed in
   milliseconds (the BOOT hold) where hal_time_now()'s whole seconds are too
   coarse. Wraps esp_timer_get_time() in production; the native mock counts
   exactly the milliseconds hal_delay_ms() has been asked to wait, so a poll
   loop sees time pass without sleeping. Wraps at 2^32 ms like any such
   counter — compare readings by subtraction. */
uint32_t hal_time_now_ms(void);

#ifdef __cplusplus
}
#endif
