#pragma once
#include <stdint.h>

/* ISR-fed press latch: GPIO negative-edge interrupts record presses that
   land while the firmware is awake (e-ink flush, NTP sync, between poll
   samples); the awake checkpoints consume them instead of sampling pin
   levels. Pure logic — no ESP dependencies; the firmware wrapper in
   buttons.c provides the ISR and the critical sections (record runs in
   ISR context, take in task context). Plain RAM: unconsumed presses
   evaporate at deep sleep and can never fire on a later wake. */

/* Ignore edges this close to the last ACCEPTED edge of the same button:
   contact chatter on the PRESS, which is all this window is for.

   IT IS NOT, ON ITS OWN, ENOUGH TO REJECT RELEASE BOUNCE, and the version of
   this comment that claimed it was is what M2-T12 came back to fix. Release
   bounce happens at press + HOLD DURATION, and a hold is as long as the
   person holding it: 100-300 ms for a deliberate press, longer for a child
   leaning on a pad. So it falls OUTSIDE any fixed window measured from the
   press edge, no matter how wide, and the edge was accepted as a second
   press. Harmless while the only consumer was the EXT1 tail's bare take,
   which discarded it; a spurious extra chore toggle from M2-T8 onward, when
   the coalescing drain above that take started reading every latched bit as
   a press. What actually rejects it is the RELEASE GATE below — the window
   only has to cover chatter, which is milliseconds. */
#define BUTTON_LATCH_DEBOUNCE_US 50000

/* And ignore edges this close to the moment the button was observed back UP.
   A SEPARATE AND MUCH SHORTER WINDOW, for a different job: a level sample
   can land in a HIGH phase of a bounce train and report "up" while the
   contact is still ringing, and this rejects the falling edge that follows
   it a millisecond or two later. Nothing else can — the press-edge window
   above is a whole hold duration in the past by then.

   IT WAS BUTTON_LATCH_DEBOUNCE_US, and reusing the chatter window for this
   is what brought M2-T12 back a second time. The gate reopens at a POLL, so
   that poll became the anchor, and a genuine press within 50 ms of it was
   dropped ENTIRELY — no toggle, no pixel, not even a reset of the ack
   coalescer's idle window, because the next poll then read the pad as held
   and closed the gate again for the rest of that press. With R the release,
   P the first poll at or after it and g the RELEASE-TO-PRESS gap, a press
   was lost whenever g < (P - R) + 50 ms: every gap up to 50 ms always, and
   one up to 100 ms with probability (100 - g)/50 at the coalescer's
   cadence. A double tap correcting a mis-press lands squarely in that band,
   which is the one gesture this task exists to deliver (design 2.5 clause
   5). Note the measure is RELEASE-to-press and not press-to-press: the hold
   in between is the person's, so a 250 ms double tap can carry a 50 ms gap.

   10 ms BECAUSE THAT IS THE FIGURE THIS BOARD ALREADY TRUSTS FOR THESE
   PADS, not because it is a round number: buttons.c waits exactly it
   (DEBOUNCE_US there) before reading pad levels that it then blames a whole
   wake on, so "these contacts have stopped ringing" is already worth 10 ms
   in this tree and nothing anywhere says it is worth more. The two are one
   physical fact and should move together — a board that needs one widened
   needs the other.

   AND NOT A KCONFIG KNOB, unlike CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS (the
   coalescer's idle window, split off STATUS_LED_ACK_HOLD_MS by M2-T15),
   which is the comparison worth drawing because from a distance they look
   alike. How long a child needs to get the next press in is a question
   about people and belongs to a board; how long a dome contact rings is a property of
   the contact. Nor is shorter the safe direction: at zero this re-admits
   the release bounce the gate exists to reject, which is the phantom chore
   toggle of field finding 3. */
#define BUTTON_LATCH_RELEASE_SETTLE_US 10000

#ifdef __cplusplus
extern "C" {
#endif

void button_latch_reset(void);
/* Record a falling edge on button `btn` (0..3) at time t_us; out-of-range
   buttons, edges inside either debounce window, and edges on a button that
   has not been observed RELEASED since its last accepted press, are ignored.

   THE RELEASE GATE is that last term, and it is what makes an accepted edge
   mean "a press" rather than "an edge". One physical press produces a
   falling edge when it goes down and, on a bouncing contact, more falling
   edges when it comes back up; nothing in the edge itself tells the two
   apart. So a press is only counted once the button has been SEEN back up
   (button_latch_note_levels), which is a fact no bounce can forge. */
void button_latch_record(int btn, int64_t t_us);
/* Feed the latch a LEVEL sample: bit n set = button n is held down now,
   matching buttons_scan_held(). Call it from task context alongside every
   take, and once at reset before the ISRs are armed. That first sample is
   what stops the WAKE button's own release bounce from reading as a press
   nobody made — usually by recording it as HELD, since an EXT1 wake means
   the pad was down when the chip woke. Usually and not always: a quick tap
   can be over before boot reaches the sample, in which case the pad reads
   UP and it is the settle anchor on that observation, not the held seed,
   that rejects whatever is left of the bounce train.

   A button seen UP arms the release gate for one press and anchors
   BUTTON_LATCH_RELEASE_SETTLE_US on the observation. A button seen DOWN
   disarms it. Only the FIRST such observation anchors — the down-to-up
   transition, or the seeding sample of a button that was already up —
   because refreshing the anchor on every sample would push the window
   forward under a press the user is about to make and reject it.

   WHAT THE GATE COSTS, stated as the loss it is, because the sentence that
   stood here ("a sample that lands mid-bounce costs at most one poll of
   latency and never manufactures a press") was right in its second clause
   and wrong in its first, which is the half a reader budgets against. A
   press arriving before the sample that observes the release is REJECTED,
   not delayed: nothing latches it and nothing replays it, and it cannot be
   otherwise, because as an EDGE it is identical to the release bounce the
   gate is there to reject. So the gate's price is up to one lost press per
   sampling interval, and the interval is the caller's. That is why the ack
   coalescer polls at BUTTON_LATCH_DEBOUNCE_US instead of at its own idle
   window (wake_flow.c), and it is the real reason the cadence matters — not
   the bitmask claim below, which holds at any cadence.

   THE ONE WINDOW ON THE PRESS PATH WITH NO SAMPLE IN IT, recorded here
   because it is where that loss is reachable rather than theoretical: the
   pre-press ack hold (STATUS_LED_ACK_HOLD_MS, wake_flow_apply_chore_ack)
   runs between the boot seeding sample and the coalescer's first take, and
   a second press of the WAKE button inside it is lost whenever that seeding
   sample still saw the pad DOWN. Usually it did not — boot outlasts a
   100-300 ms press, so the pad reads up, the gate is already open and the
   press lands — but a hold that outlasts boot loses it. Closing it needs a
   sample INSIDE the hold, and a sample with no take beside it breaks the
   bitmask below (two accepted edges between takes collapse to one bit), so
   it is a per-button counter's job and not a comment's.

   WHY A SAMPLE AND NOT AN INTERRUPT: reading the level in the ISR would
   mean either GPIO_INTR_ANYEDGE plus a level read from a flash-resident
   driver call inside an IRAM handler — the exact shape of the M2 sleep
   panic — or inferring the level from edge parity, which desynchronises for
   the rest of the wake on one bounce train longer than the debounce. A
   task-context sample cannot be wrong about the level it read, and the
   resolution it costs is bounded by how often the caller polls.

   WHY THE LATCH CAN STAY A BITMASK rather than becoming a per-button
   counter: two accepted edges on one button need a release OBSERVATION
   between them, and on every path here an observation happens only as part
   of a take (buttons.c samples inside each one). So no take can find two
   presses of one button waiting in it, at ANY cadence, and a count that
   cannot exceed one is machinery no case could fail.

   NOT "a caller that samples at least as often as the debounce window can
   never find two waiting", which is what stood here and was a property of
   exactly one of the four take sites. Read from the source: 50 ms at the
   ack coalescer, 100 ms at the render-grid wait, 250 ms at both event
   watches, and a whole panel refresh at the take after it — ESTIMATED at
   ~0.8 s for a ghost-cleaned partial and ~3 s for a full, not measured on
   a board. On the slow three the bitmask holds
   one press not because a second could not arrive but because the GATE
   REJECTED it — which is the cost above, not a guarantee, and reading it as
   one is how a counter gets argued away on evidence that does not exist.
   The cadence bounds what the gate costs; the take-shaped sampling is what
   makes the bitmask sound. */
void button_latch_note_levels(uint8_t held_mask, int64_t t_us);
/* Return the latched press bitmask (bit n = button n) and clear it. */
uint8_t button_latch_take(void);
/* Take only the buttons in `mask`, leaving other latched presses for a
   later consumer — an awake poll interested in one button must not eat
   presses a later checkpoint (e.g. the tick-wake drain) will act on. */
uint8_t button_latch_take_masked(uint8_t mask);
/* Pick the single button to act on from a taken mask, restricted to
   allowed_mask; priority B > C > D > A. Returns -1 when none allowed.

   The order is "the time-sensitive action wins", not "the lowest index
   wins", and A being LAST is the load-bearing part — button_latch.c says
   why, and both of wake_flow.c's latch drains admit A to their allowed
   masks only because of it. This line said "A > C > B > D" until
   2026-09-16, which was ACCURATE when it was written — the table was
   {0, 2, 1, 3} back when A was start/pause — and went stale at 04b4752
   (M0-T4), which rewrote the table to {1, 2, 3, 0} without it. Nothing
   caught that for six days because the two latch drains in wake_flow.c
   carry their own copy of the order and both state it correctly, so the
   only reader who could have been misled is a future one. Callers reason
   about this order — do not change it without reading both of them. */
int button_latch_pick(uint8_t mask, uint8_t allowed_mask);

#ifdef __cplusplus
}
#endif
