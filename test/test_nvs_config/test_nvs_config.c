#include <stdint.h>
#include <string.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../src/nvs_config.c"
#include "mock_hal_nvs.c"

void setUp(void) {
    mock_nvs_reset(); /* start each test with blank NVS */
}

void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* nvs_config_init_defaults                                            */
/* ------------------------------------------------------------------ */

void test_init_defaults_writes_weekday_min(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, val);
}

void test_init_defaults_writes_weekend_min(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekend_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKEND_MIN, val);
}

void test_init_defaults_writes_holiday_min(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holiday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_HOLIDAY_MIN, val);
}

void test_init_defaults_writes_holiday_blob(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    char buf[2048];
    size_t len = sizeof(buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holidays(buf, &len));
    /* Blob must contain at least one YYYY-MM-DD date */
    TEST_ASSERT_TRUE(len >= 10);
    /* New Year's Day must be present */
    TEST_ASSERT_NOT_NULL(strstr(buf, "2026-01-01"));
}

void test_init_defaults_is_idempotent(void) {
    /* Call twice — second call must not overwrite user-modified values */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    /* Modify a value */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekday_min(45));
    /* Call defaults again */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    /* Value must NOT revert to default */
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(45, val);
}

/* ------------------------------------------------------------------ */
/* u16 round-trips                                                      */
/* ------------------------------------------------------------------ */

void test_weekday_min_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekday_min(45));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(45, val);
}

void test_weekend_min_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekend_min(90));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekend_min(&val));
    TEST_ASSERT_EQUAL_UINT16(90, val);
}

void test_holiday_min_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_holiday_min(180));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holiday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(180, val);
}

/* ------------------------------------------------------------------ */
/* String round-trips                                                   */
/* ------------------------------------------------------------------ */

void test_wifi_ssid_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_ssid("MyHomeNetwork"));
    char buf[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_ssid(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("MyHomeNetwork", buf);
}

void test_wifi_pass_round_trip(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_pass("hunter2"));
    char buf[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_pass(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("hunter2", buf);
}

void test_wifi_ssid_max_length(void) {
    /* SSID max is 32 bytes per 802.11 */
    char long_ssid[33];
    memset(long_ssid, 'A', 32);
    long_ssid[32] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_ssid(long_ssid));
    char buf[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_ssid(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(long_ssid, buf);
}

/* ------------------------------------------------------------------ */
/* Blob round-trip                                                      */
/* ------------------------------------------------------------------ */

void test_holidays_blob_round_trip(void) {
    const char *holidays = "2026-01-01\n2026-07-04\n2026-12-25\n";
    size_t write_len = strlen(holidays);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_holidays(holidays, write_len));

    char buf[256] = {0};
    size_t read_len = sizeof(buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holidays(buf, &read_len));
    TEST_ASSERT_EQUAL_UINT32(write_len, read_len);
    TEST_ASSERT_EQUAL_MEMORY(holidays, buf, write_len);
}

/* ------------------------------------------------------------------ */
/* Missing key fallback                                                  */
/* ------------------------------------------------------------------ */

void test_get_weekday_min_missing_returns_default(void) {
    uint16_t val = 0;
    nvs_config_get_weekday_min(&val); /* no write beforehand */
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, val);
}

/* ------------------------------------------------------------------ */
/* Runner                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_defaults_writes_weekday_min);
    RUN_TEST(test_init_defaults_writes_weekend_min);
    RUN_TEST(test_init_defaults_writes_holiday_min);
    RUN_TEST(test_init_defaults_writes_holiday_blob);
    RUN_TEST(test_init_defaults_is_idempotent);
    RUN_TEST(test_weekday_min_round_trip);
    RUN_TEST(test_weekend_min_round_trip);
    RUN_TEST(test_holiday_min_round_trip);
    RUN_TEST(test_wifi_ssid_round_trip);
    RUN_TEST(test_wifi_pass_round_trip);
    RUN_TEST(test_wifi_ssid_max_length);
    RUN_TEST(test_holidays_blob_round_trip);
    RUN_TEST(test_get_weekday_min_missing_returns_default);
    return UNITY_END();
}
