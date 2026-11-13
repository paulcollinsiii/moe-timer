#pragma once
#include <stddef.h>
#include <stdint.h>

#include "sprite.h"

/* Which variant of a lock screen's art to show, chosen once per lock
   EPISODE. Pure -- no ESP-IDF, no LVGL, no RTC -- so it host-tests directly;
   display.c owns the one sprite_pick_t (in RTC memory, so the choice
   survives deep sleep) and the random source.

   An episode is the stretch during which the same lock screen stays up. The
   pick is drawn when the screen changes into that lock and kept until it
   changes away, so every repaint within the episode shows the same art and
   the art never flickers between wakes.

   The state is validated against what the caller says NOW, never trusted:
   RTC data is zeroed by esp_restart (an OTA reboot) and can hold another
   image's bytes after an OTA failsafe sleep, and a firmware may ship fewer
   variants than the one that made the pick. A zeroed state is simply "no
   episode" (the next call draws), and a stored variant at or above the
   current count is redrawn, so the result is always below the count. */

typedef enum {
    SPRITE_LOCK_NONE = 0, /* not a lock screen: ends the episode. MUST be 0 (a zeroed state is "none") */
    SPRITE_LOCK_BREAK,
    SPRITE_LOCK_TIMESUP,
    SPRITE_LOCK_CHARGE,
    SPRITE_LOCK_BEDTIME,
    SPRITE_LOCK_COUNT_ /* one past the last: anything at or above is "none" */
} sprite_lock_t;

typedef struct {
    uint8_t lock;    /* sprite_lock_t of the episode in progress; 0 = none */
    uint8_t variant; /* the pick for that episode; meaningful only while lock != 0 */
} sprite_pick_t;

/* The random source: esp_random() on the device, a stub in the tests. */
typedef uint32_t (*sprite_rand_fn)(void);

#ifdef __cplusplus
extern "C" {
#endif

/* The variant index (0 .. count-1) for the lock screen being painted now.
   count is how many variants its slot has (SPRITE_<SLOT>_VARIANT_COUNT).
   A count of 0 or 1 gives 0 without calling `rnd`; a count above 255 is
   treated as 255 (the index is a byte). A NULL `rnd` with several variants
   gives 0. A NULL `st` gives 0 and remembers nothing. */
uint8_t sprite_pick(sprite_pick_t *st, sprite_lock_t lock, unsigned count, sprite_rand_fn rnd);

/* The screen is no longer a lock screen: the next lock starts a new
   episode. NULL is ignored. */
void sprite_pick_leave(sprite_pick_t *st);

/* table[idx] when idx < count, else table[0] (variant 0 always exists).
   NULL when there is no table or count is 0. */
const sprite_t *sprite_variant(const sprite_t *const *table, unsigned count, unsigned idx);

#ifdef __cplusplus
}
#endif
