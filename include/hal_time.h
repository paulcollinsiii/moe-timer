#pragma once
#include <time.h>

/* Returns current Unix timestamp. Wraps time(NULL) in production;
   injectable via mock_time_set() in native tests. */
time_t hal_time_now(void);
