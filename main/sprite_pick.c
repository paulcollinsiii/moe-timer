/* Per-episode choice of lock-screen art; see sprite_pick.h. */
#include "sprite_pick.h"

#define MAX_COUNT 255u /* the variant index is a uint8_t */

uint8_t sprite_pick(sprite_pick_t *st, sprite_lock_t lock, unsigned count, sprite_rand_fn rnd) {
    if (!st)
        return 0;
    if ((unsigned)lock == SPRITE_LOCK_NONE || (unsigned)lock >= SPRITE_LOCK_COUNT_) {
        sprite_pick_leave(st);
        return 0;
    }
    if (count > MAX_COUNT)
        count = MAX_COUNT;
    /* the episode in progress, and its pick still names a variant that exists */
    if (st->lock == (uint8_t)lock && st->variant < (count ? count : 1))
        return st->variant;
    st->lock = (uint8_t)lock;
    st->variant = (count > 1 && rnd) ? (uint8_t)(rnd() % count) : 0;
    return st->variant;
}

void sprite_pick_leave(sprite_pick_t *st) {
    if (!st)
        return;
    st->lock = SPRITE_LOCK_NONE;
    st->variant = 0;
}

const sprite_t *sprite_variant(const sprite_t *const *table, unsigned count, unsigned idx) {
    if (!table || count == 0)
        return NULL;
    return table[idx < count ? idx : 0];
}
