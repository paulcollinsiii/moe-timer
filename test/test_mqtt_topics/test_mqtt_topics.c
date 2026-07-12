#include <string.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/mqtt_topics.c"

void setUp(void) {}
void tearDown(void) {}

/* Characterization: these exact shapes are what HA and the retained
   broker state already contain — they must never drift. */

void test_device_topic_shape(void) {
    char buf[96];
    int n = mqtt_topic(buf, sizeof(buf), "magtag-a1b2c3", "stat");
    TEST_ASSERT_EQUAL_STRING("magtag/magtag-a1b2c3/stat", buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

void test_device_topic_nested_suffix(void) {
    char buf[96];
    mqtt_topic(buf, sizeof(buf), "magtag-a1b2c3", "set/screen_bonus");
    TEST_ASSERT_EQUAL_STRING("magtag/magtag-a1b2c3/set/screen_bonus", buf);
}

void test_discovery_topic_shape(void) {
    char buf[128];
    int n = mqtt_disc_topic(buf, sizeof(buf), "number", "magtag-a1b2c3", "screen_bonus");
    TEST_ASSERT_EQUAL_STRING("homeassistant/number/magtag-a1b2c3_screen_bonus/config", buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

void test_truncation_reports_full_length(void) {
    char buf[10];
    int n = mqtt_topic(buf, sizeof(buf), "magtag-a1b2c3", "stat");
    TEST_ASSERT_TRUE(n >= (int)sizeof(buf)); /* snprintf semantics for the callers' guards */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_device_topic_shape);
    RUN_TEST(test_device_topic_nested_suffix);
    RUN_TEST(test_discovery_topic_shape);
    RUN_TEST(test_truncation_reports_full_length);
    return UNITY_END();
}
