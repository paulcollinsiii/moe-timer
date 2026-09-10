#include <unity.h>

/* Single-TU compilation of the pure wake-source policy (no ESP-IDF headers) */
#include "../../main/buttons_policy.c"

void setUp(void) {}
void tearDown(void) {}

#define M(btn) ((uint8_t)(1u << (btn)))

/* NB: designated initializer — a field added to buttons_policy_in_t
   defaults to 0 here instead of failing to compile, which would quietly
   narrow the exhaustive sweeps below to that field's zero value. Any new
   field needs a parameter here too. */
static uint8_t wake_mask_for(bool enable, bool swap_allowed) {
    buttons_policy_in_t in = {
        .enable = enable,
        .swap_allowed = swap_allowed,
    };
    return buttons_policy_wake_mask(&in);
}

/* ---- the arm/don't-arm decision --------------------------------------- */

/* A locked sleep arms nothing, whatever the timer gates say. */
void test_disabled_arms_nothing(void) {
    TEST_ASSERT_EQUAL_UINT8(0, wake_mask_for(false, true));
    TEST_ASSERT_EQUAL_UINT8(0, wake_mask_for(false, false));
}

/* The invariant the driver's early return leans on: an enabled sleep can
   never produce an empty mask, so mask == 0 means "locked", never "no
   button qualified". If this ever fails, the driver would silently stop
   arming wake sources on a normal sleep. */
void test_enabled_is_never_an_empty_mask(void) {
    for (int swap = 0; swap <= 1; swap++) {
        TEST_ASSERT_NOT_EQUAL_UINT8(0, wake_mask_for(true, swap));
    }
}

/* ---- which buttons qualify -------------------------------------------- */

/* A is unbound this milestone, so it never wakes — not even on the most
   permissive sleep there is. A press that could only do nothing must not
   cost a wake, a panel refresh and the battery behind it. Swept over the
   locked sleep too: A must be absent for the "nothing qualified" reason as
   well as the "arm nothing" one. */
void test_a_never_wakes(void) {
    for (int enable = 0; enable <= 1; enable++) {
        for (int swap = 0; swap <= 1; swap++) {
            TEST_ASSERT_FALSE(wake_mask_for(enable, swap) & M(BTN_A));
        }
    }
}

/* B (start/pause/resume/reload) and D (refresh) are unconditional wake
   sources — they are what makes the never-empty invariant above hold. */
void test_b_and_d_always_wake(void) {
    for (int swap = 0; swap <= 1; swap++) {
        uint8_t m = wake_mask_for(true, swap);
        TEST_ASSERT_TRUE(m & M(BTN_B));
        TEST_ASSERT_TRUE(m & M(BTN_D));
    }
}

/* C is the next-timer button: no wake when a swap would be refused. */
void test_c_follows_swap_allowed(void) {
    TEST_ASSERT_TRUE(wake_mask_for(true, true) & M(BTN_C));
    TEST_ASSERT_FALSE(wake_mask_for(true, false) & M(BTN_C));
}

/* The whole mask, not just the bit under test: the single gate moves C and
   nothing else, and never resurrects A. */
void test_mask_is_exactly_the_qualifying_buttons(void) {
    TEST_ASSERT_EQUAL_UINT8(M(BTN_B) | M(BTN_C) | M(BTN_D), wake_mask_for(true, true));
    TEST_ASSERT_EQUAL_UINT8(M(BTN_B) | M(BTN_D), wake_mask_for(true, false));
}

/* Bit n must be button n — the driver indexes BTN_GPIOS with the same n it
   tests the bit for, so a transposition arms the wrong pads. Literal
   expectations on purpose: unlike test_mask_is_exactly_the_qualifying_buttons
   above, which composes M(BTN_x) and so follows the enum wherever it goes,
   these values are only correct for one assignment of buttons to bit
   positions. Coverage is recorded rather than assumed here, because it is
   no longer total: driving swap_allowed apart separates A (never armed)
   and C (the only gated button) from B and D, so an A<->D transposition
   surfaces as 0x03 and B<->C as 0x0c. B<->D does NOT — both are
   unconditional, so the mask cannot encode any difference between them.
   That gap opened when B became unconditional (the old reload_allowed gate
   had caught it) and is inherent to the policy rather than a defect here;
   it leaves the driver seam named in buttons.c — that the pad loop indexes
   BTN_GPIOS with the same bit the policy set — resting on review alone for
   that one permutation. */
void test_bit_positions_match_the_button_ids(void) {
    TEST_ASSERT_EQUAL_UINT8(0x0a, wake_mask_for(true, false)); /* B|D */
    TEST_ASSERT_EQUAL_UINT8(0x0e, wake_mask_for(true, true));  /* B|C|D */
    TEST_ASSERT_EQUAL_UINT8(0x00, wake_mask_for(false, true)); /* locked: nothing */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_arms_nothing);
    RUN_TEST(test_enabled_is_never_an_empty_mask);
    RUN_TEST(test_a_never_wakes);
    RUN_TEST(test_b_and_d_always_wake);
    RUN_TEST(test_c_follows_swap_allowed);
    RUN_TEST(test_mask_is_exactly_the_qualifying_buttons);
    RUN_TEST(test_bit_positions_match_the_button_ids);
    return UNITY_END();
}
