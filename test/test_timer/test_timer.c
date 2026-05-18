#include <unity.h>

#include "../../src/timer.c"
#include "mock_hal_time.c"

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
