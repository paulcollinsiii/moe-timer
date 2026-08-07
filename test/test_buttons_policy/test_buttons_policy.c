#include <unity.h>

/* Single-TU compilation of the pure wake-source policy (no ESP-IDF headers) */
#include "../../main/buttons_policy.c"

void setUp(void) {}
void tearDown(void) {}

#define M(btn) ((uint8_t)(1u << (btn)))
#define ALL (M(BTN_A) | M(BTN_B) | M(BTN_C) | M(BTN_D))

/* NB: designated initializer — a field added to buttons_policy_in_t
   defaults to 0 here instead of failing to compile, which would quietly
   narrow the exhaustive sweeps below to that field's zero value. Any new
   field needs a parameter here too. */
static uint8_t wake_mask_for(bool enable, bool swap_allowed, bool reload_allowed) {
    buttons_policy_in_t in = {
        .enable = enable,
        .swap_allowed = swap_allowed,
        .reload_allowed = reload_allowed,
    };
    return buttons_policy_wake_mask(&in);
}

/* ---- the arm/don't-arm decision --------------------------------------- */

/* A locked sleep arms nothing, whatever the timer gates say. */
void test_disabled_arms_nothing(void) {
    TEST_ASSERT_EQUAL_UINT8(0, wake_mask_for(false, true, true));
    TEST_ASSERT_EQUAL_UINT8(0, wake_mask_for(false, false, false));
    TEST_ASSERT_EQUAL_UINT8(0, wake_mask_for(false, true, false));
    TEST_ASSERT_EQUAL_UINT8(0, wake_mask_for(false, false, true));
}

/* The invariant the driver's early return leans on: an enabled sleep can
   never produce an empty mask, so mask == 0 means "locked", never "no
   button qualified". If this ever fails, the driver would silently stop
   arming wake sources on a normal sleep. */
void test_enabled_is_never_an_empty_mask(void) {
    for (int swap = 0; swap <= 1; swap++) {
        for (int reload = 0; reload <= 1; reload++) {
            TEST_ASSERT_NOT_EQUAL_UINT8(0, wake_mask_for(true, swap, reload));
        }
    }
}

/* ---- which buttons qualify -------------------------------------------- */

/* A and D are unconditional wake sources — they are what makes the
   invariant above hold. */
void test_a_and_d_always_wake(void) {
    for (int swap = 0; swap <= 1; swap++) {
        for (int reload = 0; reload <= 1; reload++) {
            uint8_t m = wake_mask_for(true, swap, reload);
            TEST_ASSERT_TRUE(m & M(BTN_A));
            TEST_ASSERT_TRUE(m & M(BTN_D));
        }
    }
}

/* C is the swap button: no wake when a swap would be refused. */
void test_c_follows_swap_allowed(void) {
    TEST_ASSERT_TRUE(wake_mask_for(true, true, false) & M(BTN_C));
    TEST_ASSERT_FALSE(wake_mask_for(true, false, false) & M(BTN_C));
}

/* B is the reset button: no wake when a reset would be refused. */
void test_b_follows_reload_allowed(void) {
    TEST_ASSERT_TRUE(wake_mask_for(true, false, true) & M(BTN_B));
    TEST_ASSERT_FALSE(wake_mask_for(true, false, false) & M(BTN_B));
}

/* The two gates are independent — neither may stand in for the other. */
void test_gates_do_not_cross(void) {
    TEST_ASSERT_EQUAL_UINT8(ALL, wake_mask_for(true, true, true));
    TEST_ASSERT_EQUAL_UINT8(M(BTN_A) | M(BTN_D), wake_mask_for(true, false, false));
    TEST_ASSERT_EQUAL_UINT8(M(BTN_A) | M(BTN_C) | M(BTN_D), wake_mask_for(true, true, false));
    TEST_ASSERT_EQUAL_UINT8(M(BTN_A) | M(BTN_B) | M(BTN_D), wake_mask_for(true, false, true));
}

/* Bit n must be button n — the driver indexes BTN_GPIOS with the same n it
   tests the bit for, so a transposition arms the wrong pads. Literal
   expectations on purpose: unlike test_gates_do_not_cross above, which
   composes M(BTN_x) and so follows the enum wherever it goes, these values
   are only correct for one assignment of buttons to bit positions. B and C
   are the gateable pair, so driving them apart is what exposes a swap. */
void test_bit_positions_match_the_button_ids(void) {
    TEST_ASSERT_EQUAL_UINT8(0x09, wake_mask_for(true, false, false)); /* A|D */
    TEST_ASSERT_EQUAL_UINT8(0x0d, wake_mask_for(true, true, false));  /* A|C|D */
    TEST_ASSERT_EQUAL_UINT8(0x0b, wake_mask_for(true, false, true));  /* A|B|D */
    TEST_ASSERT_EQUAL_UINT8(0x0f, wake_mask_for(true, true, true));   /* A|B|C|D */
    TEST_ASSERT_EQUAL_UINT8(0x00, wake_mask_for(false, true, true));  /* locked: nothing */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_arms_nothing);
    RUN_TEST(test_enabled_is_never_an_empty_mask);
    RUN_TEST(test_a_and_d_always_wake);
    RUN_TEST(test_c_follows_swap_allowed);
    RUN_TEST(test_b_follows_reload_allowed);
    RUN_TEST(test_gates_do_not_cross);
    RUN_TEST(test_bit_positions_match_the_button_ids);
    return UNITY_END();
}
