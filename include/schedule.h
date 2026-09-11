#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    DAY_WEEKDAY = 0,
    DAY_WEEKEND,
    DAY_HOLIDAY,
    DAY_SUMMER, /* school summer break; precedence: holiday > weekend > summer > weekday */
} day_type_t;

#ifdef __cplusplus
extern "C" {
#endif

day_type_t schedule_get_day_type(time_t now);
uint32_t schedule_get_allocation_sec(day_type_t day_type);
/* The chore gate's free slice of the same day's allocation, in seconds —
   the part handed over with no strings attached, with the REMAINDER
   withheld until every configured chore is acked (design 3.3). The day
   type is resolved exactly as the allocation lookup above resolves it,
   from the sibling `chore_free_*` minute keys, and converted to seconds
   at the same point, so callers never see minutes from either.

   Two readings of the return that are one character apart:
     chore_free == allocation -> nothing withheld: the per-day-type off
       switch, and no extra key is needed to express it;
     chore_free  > allocation -> a config error, rejected where config is
       validated. What comes back here is CLAMPED to the allocation, so
       `allocation - chore_free` can never go negative. That clamp is a
       belt and not the rule - see the comment on the implementation
       before treating this as the place the rule is enforced.

   Defaults to 0 on any device that has never configured it, which is
   every device in the field: fully gated, and inert anyway while no chore
   is configured (design row C1, see chores_withheld_sec()). */
uint32_t schedule_get_chore_free_sec(day_type_t day_type);
/* Drop the wake-scoped NVS cache (holiday blob, school dates, allocations
   and their chore-free slices).
   Call after anything that edits schedule config mid-wake — in practice the
   post-network-window apply. Reads reload lazily. */
void schedule_cache_invalidate(void);
bool schedule_is_holiday(const char *date_str, const char *blob, size_t blob_len);
/* True when date_str (YYYY-MM-DD) falls outside the school year defined by
   NVS_DEFAULT_SCHOOL_START/END in nvs_defaults.h. */
bool schedule_is_summer(const char *date_str);

#ifdef __cplusplus
}
#endif
