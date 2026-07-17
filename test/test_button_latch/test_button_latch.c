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

void test_pick_empty_mask_returns_none(void) {
    TEST_ASSERT_EQUAL_INT(-1, button_latch_pick(0, 0x0F));
}

void test_pick_single_button_returns_it(void) {
    TEST_ASSERT_EQUAL_INT(1, button_latch_pick(1u << 1, 0x0F));
}

void test_pick_priority_a_over_all(void) {
    TEST_ASSERT_EQUAL_INT(0, button_latch_pick(0x0F, 0x0F));
}

void test_pick_priority_c_over_b_and_d(void) {
    TEST_ASSERT_EQUAL_INT(2, button_latch_pick((1u << 1) | (1u << 2) | (1u << 3), 0x0F));
}

void test_pick_priority_b_over_d(void) {
    TEST_ASSERT_EQUAL_INT(1, button_latch_pick((1u << 1) | (1u << 3), 0x0F));
}

void test_pick_respects_allowed_mask(void) {
    /* A latched but not allowed: fall through to the best allowed */
    TEST_ASSERT_EQUAL_INT(2, button_latch_pick((1u << 0) | (1u << 2), 1u << 2));
    /* Only disallowed buttons latched: none */
    TEST_ASSERT_EQUAL_INT(-1, button_latch_pick(1u << 3, (1u << 0) | (1u << 1) | (1u << 2)));
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
    RUN_TEST(test_pick_empty_mask_returns_none);
    RUN_TEST(test_pick_single_button_returns_it);
    RUN_TEST(test_pick_priority_a_over_all);
    RUN_TEST(test_pick_priority_c_over_b_and_d);
    RUN_TEST(test_pick_priority_b_over_d);
    RUN_TEST(test_pick_respects_allowed_mask);
    return UNITY_END();
}
