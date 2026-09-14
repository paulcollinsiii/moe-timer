#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The chore checklist's model: acks, the list hash, the withheld split and
   the release edge.

   Layer 1 (pure). Nothing in here reads a clock, NVS, a GPIO or a global —
   every function is a total function over its arguments, which is what
   lets the whole feature's decision table be host-tested without a device.
   The state lives one layer up (M1-T3 owns the NVS record, M1-T4 the wake
   flow); this module only says what that state means.

   The shape of the feature, for a reader arriving here first: a day's
   allocation is split in two. `chore_free` seconds are handed over with no
   strings attached; the remainder is WITHHELD until every configured chore
   has been acked, at which point the day is `released` and the rest is
   granted. Release is a LATCHED day flag, not a recomputation — see
   chores_release_due() and chores_reconcile() for why that matters. */

#ifdef __cplusplus
extern "C" {
#endif

/* One chore per ack button, one screenful, one NeoPixel. Three is not an
   arbitrary cap: it is what the hardware has, so there is no refill logic,
   no scrolling and no paging anywhere in the feature, and the ack set fits
   in three bits of a single uint8_t. */
#define CHORE_MAX 3

/* Usable BYTES in a chore name, NOT counting the terminator.
   CHORE_NAME_BUF is the buffer size — declare storage as
   `char names[CHORE_MAX][CHORE_NAME_BUF]` and a name may fill all 20
   bytes with the NUL still inside the buffer.

   This is a STORAGE cap and only that. It sizes the NVS record's name
   rows and the chore field of the config payload, and it is the length
   chores_list_hash() caps its strnlen() at. Nothing else is bounded by
   it.

   Bytes, not characters, and the difference is reachable: names arrive
   from an MQTT config payload as UTF-8, so "Räum dein Zimmer" is 16
   characters in 17 bytes and an emoji costs four. DISPLAY_VERSION_MAX's
   comment (include/display.h:235-238) documents the identical
   distinction.

   And it does NOT bound rendered width — it must never be read as if it
   did. This project already wrote that trap down, measured, in that same
   comment (include/display.h:244-247): a character budget cannot bound
   rendered width on its own, because glyph advances differ. The same
   measurement for this font and this row, taken from
   lv_font_montserrat_16's own glyph table: 'W' advances exactly 18.00 px,
   so 20 of them render 360 px — 64 px past a bare 296 px panel, and only
   14 fit once a 32 px tick column is subtracted. So the display layer has
   to carry its own GEOMETRIC cap on top of this one. That is M2's job; no
   constant here can do it. */
#define CHORE_NAME_MAX 20
#define CHORE_NAME_BUF (CHORE_NAME_MAX + 1)

/* The two pieces of per-day chore state that persist, together because
   they are always read and written as a pair and because the interesting
   rule (C10) is about what happens to ONE of them and not the other.

   `acked` bits are POSITIONAL — bit i is chore i in list order — which is
   cheap but only honest as long as the list has not been edited under
   them; that is the whole job of the stored list hash. Bits at or above
   the configured count are meaningless and every function here ignores
   them rather than trusting them. */
typedef struct {
    uint8_t acked; /* bit i (i < n) set = chore i acknowledged today */
    bool released; /* latched: the day's withheld remainder has been granted */
} chore_ack_t;

/* True when idx names a configured chore. The bounds-safe reject the
   button handler needs: with fewer than CHORE_MAX chores configured the
   spare ack buttons are not chore buttons at all, and an index from a
   corrupt record must not reach a shift. */
bool chores_index_valid(uint8_t idx, uint8_t n);

/* Test one ack bit. False for any index that is not a configured chore,
   so a stale high bit left over from a longer list never reads as an ack. */
bool chores_is_acked(uint8_t mask, uint8_t idx, uint8_t n);

/* Toggle one ack bit, returning the new mask; the input is returned
   unchanged for an index that is not a configured chore. Toggle rather
   than set, because a mis-press has to be undoable with the same button —
   there is no other input on the device to undo it with. */
uint8_t chores_toggle_ack(uint8_t mask, uint8_t idx, uint8_t n);

/* How many configured chores are still un-acked. Counts only bits below
   n, so shrinking the list cannot leave the day permanently short. */
uint8_t chores_outstanding(uint8_t mask, uint8_t n);

/* True when at least one chore is configured and none are outstanding.
   The empty list is deliberately NOT "all acked": with no chores there is
   nothing to release and the feature is inert (C1). */
bool chores_all_acked(uint8_t mask, uint8_t n);

/* A stable hash over the chore names, stored beside the acks so that a
   list edit can be detected. Order-sensitive, sensitive to any change
   within a name, sensitive to the count, and byte-oriented so the value
   does not depend on the host's endianness. n above CHORE_MAX is clamped:
   only CHORE_MAX rows exist to read.

   PRECONDITION on `names`: it must point at min(n, CHORE_MAX) readable
   rows of CHORE_NAME_BUF bytes each. NULL is safe with n == 0 and ONLY
   with n == 0, where no row is touched; NULL with n > 0 is undefined
   behaviour and is not checked for — the caller builds the array itself,
   and a NULL check here would trade a loud crash for a plausible-looking
   hash written into NVS.

   A row need not be NUL-terminated. Each is read with
   strnlen(row, CHORE_NAME_MAX), so an unterminated one stops at the cap
   instead of running off the array — which is the case that matters,
   since these rows come straight back from NVS.

   16 bits collide by construction, and that is a considered choice rather
   than an oversight. Be accurate about the scale, though: the worst case
   is not "one chore counts as done that was not". A colliding edit leaves
   the WHOLE stored mask standing against the new list, so if that mask
   happens to be all-acked for the new count, the gate opens with nothing
   acked at all. The conclusion survives anyway, because the failure is
   bounded in two directions: it lasts one day, on a list the parent just
   edited, and it is over-permissive only — a collision keeps stale acks,
   and stale acks can never LOCK a day that should have opened. Nothing
   here is a security boundary — the device is the only writer and the
   child cannot choose the names — so the hash does not need to be
   cryptographic, and two bytes in the NVS record is the price that buys
   the check at all. */
uint16_t chores_list_hash(const char names[][CHORE_NAME_BUF], uint8_t n);

/* The withheld part of today's allocation, in seconds, derived at the
   point of use rather than stored (design §5.2), and that formula
   verbatim:

       withheld = released ? 0 : max(0, allocation_for_day - chore_free_for_day)

   `mask` is not a term in it. All configured chores acked with `released`
   still false STILL returns the remainder — that is not an oversight, it
   is the whole mechanism: that number IS the amount the release owes.

   The one guard that is not in the formula is n == 0, and it stays. Design
   row C1 makes a device with no chores configured entirely inert, which is
   the default and the state every device in the field is in today; without
   the guard such a device, with the default chore_free of 0, would withhold
   its whole day forever, since no chore exists that could ever be acked to
   release it. Do not "simplify" the guard away for not matching the
   formula.

   CALLER CONTRACT — the release is ONE-SHOT, and `released` is the only
   thing that makes it so:
     - read the amount owed from this function WHILE `released` is still
       false; that value is exactly what the release should grant;
     - fire the grant;
     - latch `released` immediately afterwards, and do not leave a wake
       boundary between the two.
   Both natural calling shapes then total exactly one allocation. Size a
   one-shot release from this function and it grants alloc − free, once.
   Or carry (alloc − withheld) as a live cap and add the remainder on the
   release edge: the cap is `free` before the edge and `alloc` after it,
   and the edge contributes the difference once. Either way: alloc, once.
   Skip the latch and the second shape grants the remainder on every wake.

   N2, for whoever adds the UI (M2): a 0 return collapses three distinct
   situations — no chores configured, the day already released, and the
   gate off for this day type (chore_free >= allocation, which includes an
   allocation of 0) — so `withheld == 0` cannot answer "is the gate armed
   today?". A predicate for that belongs here and is deliberately not added
   until something consumes it. Note what is NOT in that list: all chores
   acked before the latch returns the remainder, not 0.

   Seconds in, seconds out — schedule_get_allocation_sec() and its chore
   sibling both speak seconds, so no unit conversion happens here.

   Saturating: free_sec > alloc_sec yields 0, never an unsigned wrap. That
   pairing is a config error and is caught where the config is validated;
   this function's job is only to refuse to turn it into a ~136-year
   withholding. */
uint32_t chores_withheld_sec(uint32_t alloc_sec, uint32_t free_sec, uint8_t mask, uint8_t n, bool released);

/* The release EDGE: true exactly when there is at least one configured
   chore, all of them are acked, and the day has not already been
   released. The caller fires timer_release_gated() on this and latches
   `released`, which is what makes the release happen once.

   Read the name with care: it says "the gate released" and it MEANS "all
   chores are acked and the day is not latched yet". alloc_sec and free_sec
   are not arguments, so on a day where the gate is off anyway
   (chore_free >= allocation, the §3.3 per-day-type off switch) this still
   goes true on the last tick, and the caller latches `released` and fires
   a zero-second release. Harmless — the amount comes from
   chores_withheld_sec(), which returns 0 there — but it is why a true here
   must not be read as "seconds were granted".

   ORDERING: this and chores_all_acked() answer for whatever mask they are
   handed, and neither can check that chores_reconcile() has already run
   against the current list hash — they see a mask, not a hash. See
   chores_reconcile() for the contract.

   Latched, not recomputed, is the whole point. Un-ticking a chore after
   the release re-arms this predicate only if `released` was never set —
   with it set the day stays released no matter what the mask does (C8),
   and there is nothing to be gained by toggling, so there is nothing to
   game. Before the release, un-ticking re-arms normally (C9). */
bool chores_release_due(uint8_t mask, uint8_t n, bool released);

/* Reconcile stored acks against the chore list as it is NOW (C10).
   Positional ack bits stop meaning anything the moment the list is
   edited, so a hash mismatch clears them — at three chores, re-acking
   costs three presses.

   ORDERING, which nothing in this module enforces: a wake must call this
   ONCE, on load, before chores_all_acked(), chores_release_due() or
   chores_withheld_sec(). None of them check, and none of them can — they
   take a mask, not a hash. Every function here ignoring mask bits at or
   above n is defence in depth for a caller that skipped this step, and it
   is worth having, but it only narrows a stale mask; it cannot make one
   mean anything against a list that has changed under it.

   `released` is preserved unconditionally, on BOTH sides of the compare.
   That is the rule worth stating out loud: an edit must never re-lock a
   day that has already released. Clearing it would hand a parent a
   retroactive claw-back they did not ask for and would contradict C8,
   which says a released day stays released. */
chore_ack_t chores_reconcile(chore_ack_t stored, uint16_t stored_hash, uint16_t current_hash);

#ifdef __cplusplus
}
#endif
