#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All times are minutes since local midnight. A window with
   start == end is disabled; start > end wraps midnight. */
bool quiet_hours_active(int now_min, int start_min, int end_min);

/* Kconfig stores HHMM (e.g. 2230); convert to minutes since midnight. */
int quiet_hhmm_to_minutes(int hhmm);

#ifdef __cplusplus
}
#endif
