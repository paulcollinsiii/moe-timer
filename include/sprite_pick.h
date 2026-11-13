#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sprite.h"

/* Which variant of a lock screen's art to show, chosen once per lock
   EPISODE. Pure -- no ESP-IDF, no LVGL, no RTC -- so it host-tests directly;
   display.c owns the one sprite_pick_t (in RTC memory, so the choice
   survives deep sleep) and the random source.

   An episode is the stretch during which one lock (time's up, charge, bed
   time, a screen break) stays in force. It starts LAZILY: the first
   sprite_pick() for that lock draws and stores the variant, and every later
   one returns it, so repaints, chores detours and network windows in
   between all show the same art. It ends only when the lock's owner calls
   sprite_pick_end(), on the edge that releases the lock (display.h lists
   them). The next paint of that lock then draws afresh.

   Each lock has its own slot, so an episode open for one lock is not
   disturbed by a paint or an end of another.

   The one exception to "the art holds for the episode" is an esp_restart
   (an OTA commit, a panic): RTC data is zeroed, so the episode has no pick
   and the next paint draws again.

   The state is validated against what the caller says NOW, never trusted:
   RTC data can hold another image's bytes after an OTA failsafe sleep, and a
   firmware may ship fewer variants than the one that made the pick. A
   zeroed state is simply "no episode" for every lock (the next call draws),
   and a stored variant at or above the current count is redrawn, so the
   result is always below the count. */

typedef enum {
    SPRITE_LOCK_NONE = 0, /* not a lock screen: ends the episode. MUST be 0 (a zeroed state is "none") */
    SPRITE_LOCK_BREAK,
    SPRITE_LOCK_TIMESUP,
    SPRITE_LOCK_CHARGE,
    SPRITE_LOCK_BEDTIME,
    SPRITE_LOCK_COUNT_ /* one past the last: anything at or above is "none" */
} sprite_lock_t;

typedef struct {
    /* Indexed by sprite_lock_t (entry 0, SPRITE_LOCK_NONE, is never used).
       0 = no episode open for that lock; otherwise the picked variant + 1,
       so variant 0 is still an open episode. */
    uint8_t held[SPRITE_LOCK_COUNT_];
} sprite_pick_t;

/* The random source: esp_random() on the device, a stub in the tests. */
typedef uint32_t (*sprite_rand_fn)(void);

#ifdef __cplusplus
extern "C" {
#endif

/* The variant index (0 .. count-1) for the lock screen being painted now:
   the open episode's pick for `lock`, or a fresh draw (which opens the
   episode) when there is none. count is how many variants its slot has
   (SPRITE_<SLOT>_VARIANT_COUNT). A count of 0 or 1 gives 0 without calling
   `rnd`; a count above 255 is treated as 255 (the index is a byte). A NULL
   `rnd` with several variants gives 0. A NULL `st`, SPRITE_LOCK_NONE or a
   value outside the enum gives 0 and remembers nothing. */
uint8_t sprite_pick(sprite_pick_t *st, sprite_lock_t lock, unsigned count, sprite_rand_fn rnd);

/* The lock's episode is over: its next paint draws afresh. Other locks'
   episodes are untouched. NULL, SPRITE_LOCK_NONE and out-of-range locks are
   ignored. */
void sprite_pick_end(sprite_pick_t *st, sprite_lock_t lock);

/* table[idx] when idx < count, else table[0] (variant 0 always exists).
   NULL when there is no table or count is 0. */
const sprite_t *sprite_variant(const sprite_t *const *table, unsigned count, unsigned idx);

#ifdef __cplusplus
}
#endif
