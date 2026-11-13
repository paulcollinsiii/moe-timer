/* Per-episode choice of lock-screen art; see sprite_pick.h. */
#include "sprite_pick.h"

#define MAX_COUNT 255u /* a variant index is stored in a byte, as index + 1 */

static bool is_lock(sprite_lock_t lock) {
    return (unsigned)lock != SPRITE_LOCK_NONE && (unsigned)lock < SPRITE_LOCK_COUNT_;
}

uint8_t sprite_pick(sprite_pick_t *st, sprite_lock_t lock, unsigned count, sprite_rand_fn rnd) {
    if (!st || !is_lock(lock))
        return 0;
    if (count > MAX_COUNT)
        count = MAX_COUNT;
    const unsigned limit = count ? count : 1;
    const unsigned held = st->held[lock];
    /* this lock's episode is open, and its pick still names a variant that exists */
    if (held != 0 && held - 1 < limit)
        return (uint8_t)(held - 1);
    const uint8_t variant = (count > 1 && rnd) ? (uint8_t)(rnd() % count) : 0;
    st->held[lock] = (uint8_t)(variant + 1);
    return variant;
}

void sprite_pick_end(sprite_pick_t *st, sprite_lock_t lock) {
    if (!st || !is_lock(lock))
        return;
    st->held[lock] = 0;
}

const sprite_t *sprite_variant(const sprite_t *const *table, unsigned count, unsigned idx) {
    if (!table || count == 0)
        return NULL;
    return table[idx < count ? idx : 0];
}
