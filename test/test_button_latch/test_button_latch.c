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
    /* Contact chatter: only the first edge of a burst latches. After a
       take, chatter still inside the window must not re-latch (this is
       what swallows release bounce right after a consumed press). */
    button_latch_record(1, T0_US);
    button_latch_record(1, T0_US + BUTTON_LATCH_DEBOUNCE_US / 2);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
    button_latch_record(1, T0_US + BUTTON_LATCH_DEBOUNCE_US - 1);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_edge_beyond_debounce_window_is_accepted(void) {
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_record(1, T0_US + BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
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
       though it is within 50ms of the REJECTED edge. */
    button_latch_record(1, T0_US);
    button_latch_take();
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
    RUN_TEST(test_edge_beyond_debounce_window_is_accepted);
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
