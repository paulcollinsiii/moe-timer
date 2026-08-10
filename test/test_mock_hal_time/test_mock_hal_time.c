#include <unity.h>

/* The host clock mock is itself under test here. It is shared harness —
   every suite that fakes time links it, and the awake poll loops arriving
   in wake_flow.c will depend on hal_delay_ms() advancing the clock — so a
   silent break in it would show up as a hang or a bogus pass somewhere
   else entirely. */
#include "mock_hal_time.c"

/* Arbitrary pinned second, distinct from MOCK_TIME_DEFAULT. */
#define T_PINNED 1800000000

void setUp(void) {
    mock_time_reset();
}

void tearDown(void) {}

/* ---- the clock ---------------------------------------------------------- */

void test_reset_restores_the_default_epoch(void) {
    mock_time_set(T_PINNED);
    mock_time_reset();
    TEST_ASSERT_EQUAL_INT64(MOCK_TIME_DEFAULT, (int64_t)hal_time_now());
}

void test_set_pins_the_clock(void) {
    mock_time_set(T_PINNED);
    TEST_ASSERT_EQUAL_INT64(T_PINNED, (int64_t)hal_time_now());
}

/* ---- the non-sleeping delay --------------------------------------------- */

void test_delay_advances_the_mock_clock(void) {
    time_t t0 = hal_time_now();
    hal_delay_ms(2000);
    TEST_ASSERT_EQUAL_INT64((int64_t)t0 + 2, (int64_t)hal_time_now());
}

void test_sub_second_delays_accumulate(void) {
    /* The break watch polls at 250 ms. Four polls are one second, and
       truncating the remainder on each call would freeze the clock — a
       loop waiting on the wall clock would then never exit. */
    time_t t0 = hal_time_now();
    for (int i = 0; i < 4; i++) {
        hal_delay_ms(250);
    }
    TEST_ASSERT_EQUAL_INT64((int64_t)t0 + 1, (int64_t)hal_time_now());
}

void test_partial_carry_does_not_advance_the_clock_early(void) {
    time_t t0 = hal_time_now();
    hal_delay_ms(999);
    TEST_ASSERT_EQUAL_INT64((int64_t)t0, (int64_t)hal_time_now());
    hal_delay_ms(1);
    TEST_ASSERT_EQUAL_INT64((int64_t)t0 + 1, (int64_t)hal_time_now());
}

void test_delay_total_is_recorded(void) {
    hal_delay_ms(100);
    hal_delay_ms(250);
    TEST_ASSERT_EQUAL_UINT32(350, mock_delay_total_ms());
}

void test_reset_clears_the_delay_total(void) {
    hal_delay_ms(350);
    mock_time_reset();
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

/* ---- pinning the clock is a clean slate --------------------------------- */

void test_set_drops_the_sub_second_carry(void) {
    /* Without this, a suite that pinned an exact second after some
       incidental polling would see the very next short delay tip the
       clock a second early — an off-by-one that would surface as a
       mysterious extra tick in whichever test ran next. */
    hal_delay_ms(750);
    mock_time_set(T_PINNED);
    hal_delay_ms(250); /* 250 ms alone is under a second */
    TEST_ASSERT_EQUAL_INT64(T_PINNED, (int64_t)hal_time_now());
}

void test_set_clears_the_delay_total(void) {
    hal_delay_ms(500);
    mock_time_set(T_PINNED);
    TEST_ASSERT_EQUAL_UINT32(0, mock_delay_total_ms());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reset_restores_the_default_epoch);
    RUN_TEST(test_set_pins_the_clock);
    RUN_TEST(test_delay_advances_the_mock_clock);
    RUN_TEST(test_sub_second_delays_accumulate);
    RUN_TEST(test_partial_carry_does_not_advance_the_clock_early);
    RUN_TEST(test_delay_total_is_recorded);
    RUN_TEST(test_reset_clears_the_delay_total);
    RUN_TEST(test_set_drops_the_sub_second_carry);
    RUN_TEST(test_set_clears_the_delay_total);
    return UNITY_END();
}
