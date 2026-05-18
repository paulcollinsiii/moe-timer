#include <unity.h>
/* Pull in mock implementations (single-TU compilation) */
#include "mock_hal_nvs.c"
#include "mock_hal_time.c"
/* Pull in source under test */
#include "../../src/schedule.c"

void setUp(void) {
    mock_nvs_reset();
    mock_time_set(1767225600); /* 2026-01-01 00:00:00 UTC */
}

void tearDown(void) {}

void test_harness_scaffold(void) {
    TEST_ASSERT_TRUE(1); /* harness check — always passes */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_harness_scaffold);
    return UNITY_END();
}
