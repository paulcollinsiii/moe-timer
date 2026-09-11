/* The chore checklist's model: acks, the list hash, the withheld split and
   the release edge. What each rule guarantees is in chores.h; what lives
   here is the arithmetic.

   Layer 1, and deliberately friendless: no clock, no NVS, no GPIO, no
   globals. Every function below is a total function over its arguments,
   including the arguments no sane caller would pass - a count above
   CHORE_MAX, an index from a corrupt record, a chore_free larger than the
   allocation. That is not defensiveness for its own sake: the inputs
   arrive from an NVS blob and an MQTT config payload, so "no sane caller"
   is not a property this module can rely on. */
#include "chores.h"

#include <string.h>

/* Every entry point funnels its count through here, so a corrupt or
   over-large count is narrowed once instead of being re-checked (or
   forgotten) at each use. CHORE_MAX rows are all that exist to read;
   clamping is the only answer that stays inside the array. */
static uint8_t chore_count_clamp(uint8_t n) {
    return (n > CHORE_MAX) ? CHORE_MAX : n;
}

bool chores_index_valid(uint8_t idx, uint8_t n) {
    return idx < chore_count_clamp(n);
}

/* The bit for a chore index, bounded by construction. Every caller below
   already rejects an out-of-range idx, so the AND is not what makes the
   result correct -- it is what makes the SHIFT well defined on every path,
   including the ones only a static analyser walks. cppcheck cannot prove
   the guard holds across the call to chores_index_valid and reports the
   shift as undefined behaviour for a corrupt idx; masking to the ack
   mask's own width (uint8_t, so bit indices are 0-7 by the type's
   definition) makes that unrepresentable rather than merely unreachable. */
static uint8_t chore_bit(uint8_t idx) {
    return (uint8_t)(1u << (idx & 7u));
}

bool chores_is_acked(uint8_t mask, uint8_t idx, uint8_t n) {
    if (!chores_index_valid(idx, n)) {
        return false; /* not a configured chore: also the shift guard below */
    }
    return (mask & chore_bit(idx)) != 0u;
}

uint8_t chores_toggle_ack(uint8_t mask, uint8_t idx, uint8_t n) {
    if (!chores_index_valid(idx, n)) {
        return mask; /* the spare ack buttons, and any index from a bad record */
    }
    return (uint8_t)(mask ^ chore_bit(idx));
}

uint8_t chores_outstanding(uint8_t mask, uint8_t n) {
    const uint8_t cnt = chore_count_clamp(n);
    uint8_t out = 0;
    /* Counts CLEAR bits below cnt rather than set bits anywhere: a mask
       carrying bit 2 from a three-chore list that has since become a
       two-chore list must not make the shorter list look over-acked. */
    for (uint8_t i = 0; i < cnt; i++) {
        if ((mask & (uint8_t)(1u << i)) == 0u) {
            out++;
        }
    }
    return out;
}

bool chores_all_acked(uint8_t mask, uint8_t n) {
    const uint8_t cnt = chore_count_clamp(n);
    /* cnt > 0 first: with no chores there is nothing to have finished,
       and reporting the empty list as "all done" would arm a release the
       feature is supposed to be inert for (C1). */
    return cnt > 0u && chores_outstanding(mask, cnt) == 0u;
}

/* FNV-1a, 32-bit, folded to 16. A sum or XOR checksum would be cheaper
   and would not work: both are order-insensitive, so swapping two chore
   names - the single most likely edit after a rename - leaves the value
   unchanged, and the stale positional ack bits would survive exactly the
   edit that invalidated them. FNV's multiply mixes each byte into every
   subsequent one, so position is part of the value.
   Two more details carry weight:
     - The count is hashed first, so shortening the list changes the value
       even when the remaining names are untouched.
     - Each name is followed by a 0x00 separator, so {"ab","c"} and
       {"a","bc"} differ. Without it the names concatenate and a rename
       that only moves a boundary would go unnoticed.
   Byte-at-a-time by construction: nothing here reinterprets a multi-byte
   object, so the value is the same on the device and on the host suite
   that pins it. */
#define CHORE_FNV1A32_OFFSET 2166136261u
#define CHORE_FNV1A32_PRIME 16777619u

static uint32_t chore_fnv1a_byte(uint32_t h, uint8_t b) {
    return (h ^ (uint32_t)b) * CHORE_FNV1A32_PRIME; /* unsigned: wraps by definition, not UB */
}

uint16_t chores_list_hash(const char names[][CHORE_NAME_BUF], uint8_t n) {
    const uint8_t cnt = chore_count_clamp(n);
    uint32_t h = chore_fnv1a_byte(CHORE_FNV1A32_OFFSET, cnt);
    for (uint8_t i = 0; i < cnt; i++) {
        /* strnlen, not strlen: the caller's buffers are fixed-size and
           come back from NVS, so an unterminated row must stop at the
           budget rather than run off the array. The cap is
           CHORE_NAME_MAX (20) because the 21st byte of a row is only
           ever the terminator, so a name filling all 20 usable bytes has
           nothing left to read past them.
           Both halves are pinned by the suite, because neither is
           reachable from an ordinary fixture: a row littered past its
           NUL does not exercise this at all, since strlen stops at that
           NUL too. test_hash_stops_at_the_cap_on_an_unterminated_row
           hashes a heap row with no terminator anywhere - strlen trips
           ASan there - and pins the value, so widening the cap to
           CHORE_NAME_BUF changes a golden instead of passing quietly. */
        const size_t len = strnlen(names[i], CHORE_NAME_MAX);
        for (size_t j = 0; j < len; j++) {
            /* The (uint8_t) is redundant TODAY - chore_fnv1a_byte takes
               a uint8_t, so the implicit conversion narrows identically -
               and it is written out because this value is a PERSISTED
               field compared across firmware versions. char is signed on
               Xtensa; the day that helper takes anything wider, a bare
               names[i][j] sign-extends every byte above 0x7F and every
               device with a non-ASCII chore name loses a day's acks,
               silently and once. Naming the width here survives that
               edit. What pins the byte values themselves is the golden
               over a UTF-8 name carrying a high byte
               (test_hash_reads_bytes_above_0x7f_unchanged). */
            h = chore_fnv1a_byte(h, (uint8_t)names[i][j]);
        }
        /* The separator, which is what makes {"ab","c"} and {"a","bc"}
           differ. It is NOT what keeps the tail past a NUL out of the
           value - that is the strnlen above, which ends the loop at the
           terminator; this byte is appended after it, not instead. */
        h = chore_fnv1a_byte(h, 0u);
    }
    /* The standard FNV xor-fold. Truncating to the low 16 bits instead
       would throw away the half of the avalanche the multiply just
       built. */
    return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));
}

/* Design 5.2, and that formula verbatim:
       withheld = released ? 0 : max(0, allocation_for_day - chore_free_for_day)
   The ack mask is NOT a term in it. All three chores ticked with
   `released` still false still withholds the remainder, deliberately:
   that number IS the amount the release owes, so short-circuiting it to
   0 the instant the last tick lands is precisely what would make the
   release grant nothing. `mask` survives in the signature only so that
   callers need not special-case it - see the contract in chores.h. */
uint32_t chores_withheld_sec(uint32_t alloc_sec, uint32_t free_sec, uint8_t mask, uint8_t n, bool released) {
    (void)mask; /* not a term in the formula; see the note above */
    if (chore_count_clamp(n) == 0u) {
        /* C1, and the one guard that is not in the formula. It is here on
           purpose: with no chores configured the feature is entirely
           inert, which is the state every device in the field is in
           today. Delete this and a device with no chores and the default
           chore_free of 0 withholds its whole day forever - there is no
           chore left to ack that could ever release it. Do not
           "simplify" it away for not matching the formula. */
        return 0;
    }
    if (released) {
        return 0; /* C8: latched for the day; a later un-tick claws nothing back */
    }
    if (free_sec >= alloc_sec) {
        /* free == alloc is the ordinary "gate off for this day type"
           configuration (design 3.3). free > alloc is a config error
           caught where the config is validated; unsigned subtraction
           would turn it into a ~136-year withholding, so it saturates. */
        return 0;
    }
    return alloc_sec - free_sec;
}

bool chores_release_due(uint8_t mask, uint8_t n, bool released) {
    /* The edge, not the level: the caller fires the release on a true
       here and latches `released`, which is what stops it firing twice.
       Un-ticking before that latch re-arms this normally (C9).
       The name overpromises, and chores.h spells out why: alloc_sec and
       free_sec are not arguments here, so this means "all acked, not
       latched yet" rather than "seconds were granted". The amount always
       comes from chores_withheld_sec(). */
    return !released && chores_all_acked(mask, n);
}

chore_ack_t chores_reconcile(chore_ack_t stored, uint16_t stored_hash, uint16_t current_hash) {
    chore_ack_t out = {
        /* Positional bits mean nothing against a list that has changed
           under them, so a mismatch clears them outright rather than
           trying to guess which name moved where. */
        .acked = (stored_hash == current_hash) ? stored.acked : 0u,
        /* Unconditional on BOTH sides of that compare, and written that
           way rather than left to a struct copy so it reads as the rule
           it is: an edit must never re-lock a day that already released
           (C10, and C8's "stays released"). */
        .released = stored.released,
    };
    return out;
}
