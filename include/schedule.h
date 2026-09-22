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
/* The same pair as it is STORED: MINUTES, and the free slice UNCLAMPED.
   Both outputs are always written, so neither pointer may be NULL.

   For the config-error gate (design 5.3) and for nothing else. Every
   ordinary consumer wants the accessors above, whose clamp is what keeps
   `allocation - chore_free` from underflowing; this one exists because
   that clamp makes the fault INVISIBLE. `chore_free > allocation` is the
   condition the gate has to see, and after the clamp there is no pair
   left that fails it — a gate built on the seconds accessors would find
   every device healthy for ever and the blocking screen would never
   paint.

   MINUTES, NOT SECONDS, and the unit is load-bearing rather than
   incidental: the predicate that judges the pair
   (config_is_valid_chore_free_min) takes two uint16_t, so a seconds value
   truncates silently and can invert the answer. config_validate.h works
   the arithmetic out. Reading the pair here keeps the whole question in
   the unit config is edited and stored in, and nothing converts.

   Same wake-scoped cache as the accessors above, deliberately: an HA edit
   that lands mid-wake becomes visible to the gate at exactly the moment
   schedule_cache_invalidate() runs, and a wake where nothing is wrong
   costs the gate no flash read the allocation lookup was not making
   anyway. */
void schedule_get_chore_free_pair_min(day_type_t day_type, uint16_t *free_min, uint16_t *alloc_min);
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
