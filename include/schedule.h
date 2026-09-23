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

/* How many day types there are. schedule.c asserts it against the enum's
   last member, so a fifth day type cannot leave it behind. */
#define SCHEDULE_DAY_TYPES 4

/* The display name of a day type, as Home Assistant sees it. ONE table,
   inline here rather than in schedule.c, because its two readers are the
   stat payload's `day_type` field (app_state.c) and the config warning
   that names broken day types (stats_json.c), and stats_json.c is a pure
   builder that must not link the NVS-backed schedule module. The warning
   saying "Summer" while the day-type sensor says something else would be
   two names for one thing. Out-of-range values read as the weekday, the
   same fallback day_type_index() in schedule.c applies. */
static inline const char *schedule_day_type_name(day_type_t dt) {
    switch (dt) {
        case DAY_WEEKEND:
            return "Weekend";
        case DAY_HOLIDAY:
            return "Holiday";
        case DAY_SUMMER:
            return "Summer";
        case DAY_WEEKDAY:
        default:
            return "Weekday";
    }
}

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
/* Every day type whose stored chore_free pair is broken, as a bitmask:
   bit (1u << d) set when day type d's pair fails
   config_is_valid_chore_free_min().

   For the stat payload's config warning (M2-D6), which exists because
   the two places that already judge a pair each see only part of the
   picture: the config_ack names a broken pair once, in a retained message
   the NEXT document overwrites, and the blocking gate reads only TODAY's
   day type. This names all four, on every stat publish, until the pair is
   fixed.

   NOT a second definition of "broken". The pair is the one
   schedule_get_chore_free_pair_min() returns — RAW, unclamped minutes,
   which is what the config-error lock reads (lock_gate.c) and what the
   seconds accessors' clamp would hide — and the judgement is the one
   predicate both the lock and config_apply.c's check_chore_free_pairs()
   call. So a bit here is set exactly when the lock would engage on a day
   of that type. Bits at or above SCHEDULE_DAY_TYPES are never set. Same
   wake-scoped cache as the pair reader, so call it from the main task
   only, like every other schedule_* reader. */
uint8_t schedule_chore_free_broken_mask(void);
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
