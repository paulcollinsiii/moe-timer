#include <unity.h>

/* Pull in mock implementations (single-TU compilation) */
#include "mock_hal_nvs.c"
/* Pull in source under test */
#include "../../src/nvs_config.c"

void setUp(void) {
    mock_nvs_reset();
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
