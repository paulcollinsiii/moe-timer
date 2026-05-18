#include <unity.h>

/* Pull in mock implementations (single-TU compilation) */
#include "mock_hal_time.c"
/* Pull in source under test */
#include "../../src/timer.c"

void setUp(void) {
    timer_reset();
    mock_time_set(1767225600);
}

void tearDown(void) {}

void test_harness_scaffold(void) {
    TEST_ASSERT_TRUE(1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_harness_scaffold);
    return UNITY_END();
}
