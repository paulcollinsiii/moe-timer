#include <stdint.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/button_latch.c"

/* Arbitrary microsecond base; the latch only compares differences. */
#define T0_US ((int64_t)1000000000)

void setUp(void) {
    button_latch_reset();
}

void tearDown(void) {}

void test_take_returns_zero_when_nothing_latched(void) {
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_record_then_take_returns_bit_and_clears(void) {
    button_latch_record(0, T0_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take());
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take()); /* consumed */
}

void test_multiple_buttons_combine_into_one_mask(void) {
    button_latch_record(0, T0_US);
    button_latch_record(3, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8((1u << 0) | (1u << 3), button_latch_take());
}

void test_edges_within_debounce_window_are_ignored(void) {
    /* Contact chatter on the PRESS: only the first edge of a burst latches,
       and after a take chatter still inside the window must not re-latch.
       Chatter is what this window is for and all it is for — bounce on the
       RELEASE is a hold duration away and is the release gate's job. */
    button_latch_record(1, T0_US);
    button_latch_record(1, T0_US + BUTTON_LATCH_DEBOUNCE_US / 2);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
    button_latch_record(1, T0_US + BUTTON_LATCH_DEBOUNCE_US - 1);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* A SECOND PRESS NEEDS BOTH: past the debounce window AND a release seen
   in between. The debounce half is what this case was before M2-T12, and on
   its own it accepted the release bounce of the first press as a second one
   — the bounce lands a whole hold duration after the press edge, so it
   always clears the window. `+ 2 *` rather than `+`: the sample sits AT
   T0 + one window, and the release observation is its own anchor
   (BUTTON_LATCH_RELEASE_SETTLE_US), so an edge at the same instant would be
   rejected by that even though it clears this one. */
void test_edge_beyond_debounce_window_and_past_a_release_is_accepted(void) {
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(1, T0_US + 2 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

/* THE RELEASE GATE, and the field defect it closes (M2-T12 finding 3). One
   physical press is a falling edge going down and, on a bouncing contact,
   another coming back up — and the second is separated from the first by
   the HOLD, which is as long as the person holding it. So no window
   measured from the press edge can reject it, and while the coalescing
   drain read every latched bit as a press it toggled a chore row back:
   "toggling 3 flips 2 back to green".
   Ten times the debounce window here to make the point that widening the
   window is not the fix. */
void test_an_edge_with_no_release_observed_is_not_a_second_press(void) {
    button_latch_record(1, T0_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
    button_latch_record(1, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* The gate is armed by the OBSERVATION and not by the take: consuming a
   press says nothing about whether the finger came off. */
void test_taking_a_press_does_not_arm_the_release_gate(void) {
    button_latch_record(0, T0_US);
    button_latch_take();
    button_latch_take(); /* however many times */
    button_latch_record(0, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* A sample taken while the button is STILL DOWN observes no release, which
   is the case a level poll hits when the press outlasts the poll interval —
   the common one, since a poll is milliseconds and a press is not. */
void test_a_sample_with_the_button_still_held_does_not_arm_the_gate(void) {
    button_latch_record(2, T0_US);
    button_latch_take();
    button_latch_note_levels(1u << 2, T0_US + BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(2, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* THE SECOND ANCHOR. A level sample can land in a HIGH phase of a bounce
   train and report "up" while the contact is still chattering; the falling
   edge a millisecond later would then be a press with the gate armed. The
   release observation is therefore a debounce anchor in its own right —
   which the press edge cannot be, being a hold duration in the past.
   ONE MICROSECOND, so this is the case that fails if the settle window is
   ever narrowed to nothing; the case below it is the one that fails if it is
   ever widened back to the chatter window. Between them they bracket
   BUTTON_LATCH_RELEASE_SETTLE_US from both sides, which is the only way a
   figure whose two failure modes are opposite can be pinned at all. */
void test_an_edge_right_after_the_release_sample_is_still_bounce(void) {
    button_latch_record(3, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(3, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US + 1);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* THE OTHER SIDE OF THAT BRACKET, and the case M2-T12 came back a second
   time for: a GENUINE press landing past the settle window but still inside
   the chatter window of a release observation.
   The gate reopens at a POLL, so the poll that admits the press is also the
   anchor it is measured against. While that anchor was a whole
   BUTTON_LATCH_DEBOUNCE_US wide, a press within 50 ms of the poll was
   rejected here as bounce — and then LOST outright, because the next poll
   read the pad as held and closed the gate again for the rest of the press.
   No toggle, no pixel, and not even a reset of the coalescer's idle window.
   With R the release, P the first poll at or after it and g the
   RELEASE-to-press gap, every g below (P - R) + 50 ms went that way: all of
   0-50 ms and much of 50-100 ms. A double tap correcting a mis-press lives
   in that band, which is design §2.5 clause 5 and the field report this
   whole task exists for. */
void test_a_press_past_the_settle_window_is_accepted_inside_the_chatter_window(void) {
    /* 30 ms, WRITTEN AS A DURATION rather than derived from the two windows,
       so that widening the settle anchor back to the chatter window fails the
       BEHAVIOUR below and not merely an arithmetic guard. A guard tripping
       says "these constants no longer differ"; the assertion says "the press
       was eaten", and it is the second sentence a future reader needs. */
    const int64_t probe_gap_us = 30000;
    const int64_t release = T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US;
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_note_levels(0, release);
    button_latch_record(1, release + probe_gap_us);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u << 1, button_latch_take(),
                                    "a real press soon after the poll that reopened the gate was rejected as "
                                    "bounce, which is the lost correction press of M2-T12");
    /* AFTER the assertion, because these only say whether the assertion
       above was worth making. The press has to sit strictly between the two
       windows or it proves nothing: too early and the gate is right to
       reject it, too late and it clears an anchor of any width. */
    TEST_ASSERT_TRUE_MESSAGE(probe_gap_us > BUTTON_LATCH_RELEASE_SETTLE_US,
                             "the probe gap no longer clears the settle anchor");
    TEST_ASSERT_TRUE_MESSAGE(probe_gap_us < BUTTON_LATCH_DEBOUNCE_US,
                             "the probe gap no longer lands inside the chatter window, so this case passes "
                             "without distinguishing the two anchors");
}

/* THE SEEDING SAMPLE ANCHORS TOO, for a button it finds already UP.
   button_latch_reset() seeds the gate open with NO anchor, because it cannot
   know what the pads read; until M2-T12's fix pass nothing anchored such a
   button at all, so its very next falling edge cleared both windows. That
   pad is not always an idle one: a wake press tapped and released during
   boot can be sampled in a HIGH phase of its own release bounce train, and
   the falling edge a millisecond later was then a phantom press of the
   button that caused the wake — the same phantom the gate exists to stop,
   arrived at through the seed instead of through the sample. */
void test_the_seeding_sample_anchors_a_button_it_finds_already_up(void) {
    button_latch_note_levels(0, T0_US); /* buttons_watch_begin()'s sample: every pad up */
    button_latch_record(2, T0_US + 1);  /* the bounce train's next falling edge */
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
    /* And a real press once the contact has settled is still a press: the
       anchor is a window, not a lock. */
    button_latch_record(2, T0_US + BUTTON_LATCH_RELEASE_SETTLE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

/* The gate is per button, like the debounce window: a sample that sees B up
   must not arm C, which is still down. */
void test_the_release_gate_is_per_button(void) {
    button_latch_record(1, T0_US);
    button_latch_record(2, T0_US);
    button_latch_take();
    button_latch_note_levels(1u << 2, T0_US + BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(1, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(2, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

/* THE WAKE BUTTON, which is why buttons.c samples the pads before it arms
   the ISRs. It is already down at boot so it fires no falling edge going
   in; seeded as HELD, its release bounce is bounce. Seeded by a reset
   alone — every button "released" — that same bounce is a phantom press of
   the very button that caused the wake, which on a chore ack toggled the
   row straight back and looked like the press had done nothing. */
void test_a_button_seeded_as_held_rejects_its_release_bounce(void) {
    button_latch_note_levels(1u << 1, T0_US); /* buttons_watch_begin()'s sample */
    button_latch_record(1, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* And the other half of that seed: a button that was NOT held at boot is
   free to be pressed without anybody having sampled it first. Without this
   the seed could be "reject everything until a sample arrives" and the case
   above would still pass. */
void test_a_button_seeded_as_up_accepts_its_first_press(void) {
    button_latch_note_levels(1u << 1, T0_US); /* only B was held */
    button_latch_record(2, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

/* A repeated sample must not keep pushing the anchor forward, or a press
   made just after the latest poll is rejected as bounce off it — which
   would be every press on a device that polls, i.e. the whole coalescing
   loop. Only the transition down-to-up anchors. */
void test_repeated_release_samples_do_not_push_the_anchor_forward(void) {
    button_latch_record(0, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + BUTTON_LATCH_DEBOUNCE_US);
    /* Nine more polls, the last of them right before the press. */
    for (int i = 2; i <= 10; i++) {
        button_latch_note_levels(0, T0_US + (int64_t)i * BUTTON_LATCH_DEBOUNCE_US);
    }
    button_latch_record(0, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US + 1);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take());
}

/* Out-of-range bits in the held mask are the caller's business, not a
   crash: the mask is four bits wide and a wider one must be ignored above
   the fourth, which ASan would catch as a write past s_released_seen. */
void test_note_levels_ignores_bits_above_the_four_buttons(void) {
    button_latch_note_levels(0xF0, T0_US);
    button_latch_record(0, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take());
}

void test_debounce_windows_are_per_button(void) {
    button_latch_record(0, T0_US);
    /* A different button inside button 0's window still latches */
    button_latch_record(2, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8((1u << 0) | (1u << 2), button_latch_take());
}

void test_debounce_window_anchors_on_accepted_edge_only(void) {
    /* A rejected edge must not extend the window: T0 accepted, T0+40ms
       rejected, T0+60ms is beyond T0+50ms and must be accepted even
       though it is within 50ms of the REJECTED edge.
       The release sample sits at T0+5ms so the gate is armed for all of
       this and the only thing under test is the press-edge anchor — early
       enough that its own anchor (BUTTON_LATCH_RELEASE_SETTLE_US, 10 ms, so
       expired by T0+15ms) cannot be what rejects or admits either edge. */
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + BUTTON_LATCH_DEBOUNCE_US / 10);
    button_latch_record(1, T0_US + (BUTTON_LATCH_DEBOUNCE_US * 4) / 5);
    button_latch_record(1, T0_US + (BUTTON_LATCH_DEBOUNCE_US * 6) / 5);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

void test_out_of_range_button_ignored(void) {
    button_latch_record(-1, T0_US);
    button_latch_record(4, T0_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_reset_clears_mask_and_debounce_history(void) {
    button_latch_record(0, T0_US);
    button_latch_reset();
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
    /* History cleared: an edge inside the old window latches again */
    button_latch_record(0, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take());
}

void test_take_masked_returns_only_requested_bits(void) {
    button_latch_record(0, T0_US);
    button_latch_record(2, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take_masked(1u << 0));
}

void test_take_masked_leaves_other_bits_latched(void) {
    /* The C1 field case: A-poll during the grid wait must not eat a
       latched C press meant for the tick-wake drain. */
    button_latch_record(0, T0_US);
    button_latch_record(2, T0_US + 1000);
    button_latch_take_masked(1u << 0);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

void test_take_masked_consumed_bit_stays_consumed(void) {
    button_latch_record(0, T0_US);
    button_latch_take_masked(1u << 0);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take_masked(1u << 0));
}

void test_take_masked_empty_mask_is_noop(void) {
    button_latch_record(1, T0_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take_masked(0));
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

void test_take_masked_requested_but_unlatched_returns_zero(void) {
    button_latch_record(3, T0_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take_masked(1u << 0));
    TEST_ASSERT_EQUAL_UINT8(1u << 3, button_latch_take());
}

void test_take_masked_full_mask_equals_take(void) {
    button_latch_record(0, T0_US);
    button_latch_record(3, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8((1u << 0) | (1u << 3), button_latch_take_masked(0x0F));
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_pick_empty_mask_returns_none(void) {
    TEST_ASSERT_EQUAL_INT(-1, button_latch_pick(0, 0x0F));
}

void test_pick_single_button_returns_it(void) {
    TEST_ASSERT_EQUAL_INT(1, button_latch_pick(1u << 1, 0x0F));
}

/* B (1) is the start/pause/resume button and therefore the time-sensitive
   one, so it outranks everything — including A, whose binding (the
   Timers/Chores mode toggle) only chooses which screen is painted and so
   loses nothing by waiting for the next press. */
void test_pick_priority_b_over_all(void) {
    TEST_ASSERT_EQUAL_INT(1, button_latch_pick(0x0F, 0x0F));
}

/* THE swallowing case, and the reason the order had to move with the
   layout. A is index 0, so an order that still led with it would let a
   press of the mode toggle beat a genuine start/pause press: pick returns
   A, the dispatch changes which screen is painted, and the B press is
   gone — taken out of the latch by the same unmasked take and discarded
   at sleep. The user presses start, nothing happens, and there is no
   feedback to tell them why. This mattered more once A gained a binding,
   not less: both drains admit A to their allowed masks now (M2-T3), so
   this priority is the only thing left standing between the two presses. */
void test_pick_priority_b_over_a(void) {
    TEST_ASSERT_EQUAL_INT(1, button_latch_pick((1u << 0) | (1u << 1), 0x0F));
}

void test_pick_priority_c_over_d_and_a(void) {
    TEST_ASSERT_EQUAL_INT(2, button_latch_pick((1u << 0) | (1u << 2) | (1u << 3), 0x0F));
}

void test_pick_priority_d_over_a(void) {
    TEST_ASSERT_EQUAL_INT(3, button_latch_pick((1u << 0) | (1u << 3), 0x0F));
}

/* A is last, not absent: it is still a valid pick when it is the only
   thing latched. The callers that must never act on it drop it from the
   ALLOWED mask instead (wake_flow's two drains), which is the case
   below. */
void test_pick_returns_a_when_it_is_the_only_button_latched(void) {
    TEST_ASSERT_EQUAL_INT(0, button_latch_pick(1u << 0, 0x0F));
}

void test_pick_respects_allowed_mask(void) {
    /* A latched but not allowed: fall through to the best allowed */
    TEST_ASSERT_EQUAL_INT(2, button_latch_pick((1u << 0) | (1u << 2), 1u << 2));
    /* Only disallowed buttons latched: none */
    TEST_ASSERT_EQUAL_INT(-1, button_latch_pick(1u << 3, (1u << 0) | (1u << 1) | (1u << 2)));
    /* The wake_flow drains' allowed set, with A dropped: a latched A
       alone picks nothing at all, so the drain never runs. */
    TEST_ASSERT_EQUAL_INT(-1, button_latch_pick(1u << 0, (1u << 1) | (1u << 2)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_take_returns_zero_when_nothing_latched);
    RUN_TEST(test_record_then_take_returns_bit_and_clears);
    RUN_TEST(test_multiple_buttons_combine_into_one_mask);
    RUN_TEST(test_edges_within_debounce_window_are_ignored);
    RUN_TEST(test_edge_beyond_debounce_window_and_past_a_release_is_accepted);
    RUN_TEST(test_an_edge_with_no_release_observed_is_not_a_second_press);
    RUN_TEST(test_taking_a_press_does_not_arm_the_release_gate);
    RUN_TEST(test_a_sample_with_the_button_still_held_does_not_arm_the_gate);
    RUN_TEST(test_an_edge_right_after_the_release_sample_is_still_bounce);
    RUN_TEST(test_a_press_past_the_settle_window_is_accepted_inside_the_chatter_window);
    RUN_TEST(test_the_seeding_sample_anchors_a_button_it_finds_already_up);
    RUN_TEST(test_the_release_gate_is_per_button);
    RUN_TEST(test_a_button_seeded_as_held_rejects_its_release_bounce);
    RUN_TEST(test_a_button_seeded_as_up_accepts_its_first_press);
    RUN_TEST(test_repeated_release_samples_do_not_push_the_anchor_forward);
    RUN_TEST(test_note_levels_ignores_bits_above_the_four_buttons);
    RUN_TEST(test_debounce_windows_are_per_button);
    RUN_TEST(test_debounce_window_anchors_on_accepted_edge_only);
    RUN_TEST(test_out_of_range_button_ignored);
    RUN_TEST(test_reset_clears_mask_and_debounce_history);
    RUN_TEST(test_take_masked_returns_only_requested_bits);
    RUN_TEST(test_take_masked_leaves_other_bits_latched);
    RUN_TEST(test_take_masked_consumed_bit_stays_consumed);
    RUN_TEST(test_take_masked_empty_mask_is_noop);
    RUN_TEST(test_take_masked_requested_but_unlatched_returns_zero);
    RUN_TEST(test_take_masked_full_mask_equals_take);
    RUN_TEST(test_pick_empty_mask_returns_none);
    RUN_TEST(test_pick_single_button_returns_it);
    RUN_TEST(test_pick_priority_b_over_all);
    RUN_TEST(test_pick_priority_b_over_a);
    RUN_TEST(test_pick_priority_c_over_d_and_a);
    RUN_TEST(test_pick_priority_d_over_a);
    RUN_TEST(test_pick_returns_a_when_it_is_the_only_button_latched);
    RUN_TEST(test_pick_respects_allowed_mask);
    return UNITY_END();
}
