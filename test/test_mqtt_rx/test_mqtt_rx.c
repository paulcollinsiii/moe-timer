#include <string.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/mqtt_rx.c"

/* Fixed identity for every test: topic lengths mirror mqtt_ha_window's
   setup for device id "magtag-a1b2c3". */
#define CONFIG_TOPIC "magtag/magtag-a1b2c3/config"
#define CMD_TOPIC "magtag/magtag-a1b2c3/cmd"
#define SET_PREFIX "magtag/magtag-a1b2c3/set/"

static char s_config[128];
static char s_cmd[64];
static mqtt_set_kv_t s_sets[4];
static mqtt_rx_t s_rx;

void setUp(void) {
    memset(s_config, 0, sizeof(s_config));
    memset(s_cmd, 0, sizeof(s_cmd));
    memset(s_sets, 0, sizeof(s_sets));
    s_rx = (mqtt_rx_t){
        .config_buf = s_config,
        .config_cap = sizeof(s_config),
        .config_topic_len = (int)strlen(CONFIG_TOPIC),
        .cmd_buf = s_cmd,
        .cmd_cap = sizeof(s_cmd),
        .cmd_topic_len = (int)strlen(CMD_TOPIC),
        .sets = s_sets,
        .sets_cap = 4,
        .set_prefix_len = (int)strlen(SET_PREFIX),
    };
}

void tearDown(void) {}

static mqtt_rx_result_t feed(const char *topic, const char *data, int total, int offset, int data_len) {
    return mqtt_rx_on_data(&s_rx, topic, (int)strlen(topic), data, data_len, total, offset);
}

static mqtt_rx_result_t feed_whole(const char *topic, const char *data) {
    return feed(topic, data, (int)strlen(data), 0, (int)strlen(data));
}

void test_config_single_chunk(void) {
    TEST_ASSERT_EQUAL(MQTT_RX_OK, feed_whole(CONFIG_TOPIC, "{\"ver\":1}"));
    TEST_ASSERT_TRUE(s_rx.config_done);
    TEST_ASSERT_EQUAL_STRING("{\"ver\":1}", s_config);
}

void test_config_reassembles_chunks_by_offset(void) {
    const char *doc = "{\"ver\":42,\"weekday_min\":60}";
    int total = (int)strlen(doc);
    TEST_ASSERT_EQUAL(MQTT_RX_OK, feed(CONFIG_TOPIC, doc, total, 0, 10));
    TEST_ASSERT_FALSE(s_rx.config_done);
    TEST_ASSERT_EQUAL(MQTT_RX_OK, feed(CONFIG_TOPIC, doc + 10, total, 10, total - 10));
    TEST_ASSERT_TRUE(s_rx.config_done);
    TEST_ASSERT_EQUAL_STRING(doc, s_config);
}

void test_cmd_routes_separately(void) {
    TEST_ASSERT_EQUAL(MQTT_RX_OK, feed_whole(CMD_TOPIC, "{\"id\":\"x\"}"));
    TEST_ASSERT_TRUE(s_rx.cmd_done);
    TEST_ASSERT_FALSE(s_rx.config_done);
    TEST_ASSERT_EQUAL_STRING("{\"id\":\"x\"}", s_cmd);
}

void test_set_routes_by_content_not_length(void) {
    /* set/tz has the SAME topic length as .../config — the historical
       misroute hazard. It must land in the set buffer, not the config. */
    TEST_ASSERT_EQUAL((int)strlen(SET_PREFIX "tz"), (int)strlen(CONFIG_TOPIC));
    TEST_ASSERT_EQUAL(MQTT_RX_OK, feed_whole(SET_PREFIX "tz", "UTC0"));
    TEST_ASSERT_EQUAL_INT(1, s_rx.set_count);
    TEST_ASSERT_EQUAL_STRING("tz", s_sets[0].key);
    TEST_ASSERT_EQUAL_STRING("UTC0", s_sets[0].value);
    TEST_ASSERT_FALSE(s_rx.config_done);
}

void test_sets_accumulate(void) {
    feed_whole(SET_PREFIX "weekday_min", "45");
    feed_whole(SET_PREFIX "locate", "ON");
    TEST_ASSERT_EQUAL_INT(2, s_rx.set_count);
    TEST_ASSERT_EQUAL_STRING("weekday_min", s_sets[0].key);
    TEST_ASSERT_EQUAL_STRING("ON", s_sets[1].value);
}

void test_sets_full_drops(void) {
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(MQTT_RX_OK, feed_whole(SET_PREFIX "weekday_min", "45"));
    }
    TEST_ASSERT_EQUAL(MQTT_RX_SETS_FULL, feed_whole(SET_PREFIX "weekend_min", "90"));
    TEST_ASSERT_EQUAL_INT(4, s_rx.set_count);
}

void test_set_key_too_long_drops(void) {
    TEST_ASSERT_EQUAL(MQTT_RX_SET_TOO_LONG, feed_whole(SET_PREFIX "this_key_is_way_over_capacity", "1"));
    TEST_ASSERT_EQUAL_INT(0, s_rx.set_count);
}

void test_empty_retained_clear_ignored(void) {
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, feed(CONFIG_TOPIC, "", 0, 0, 0));
    TEST_ASSERT_FALSE(s_rx.config_done);
}

void test_oversize_config_ignored(void) {
    char big[256];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, feed(CONFIG_TOPIC, big, (int)sizeof(big), 0, (int)sizeof(big)));
    TEST_ASSERT_FALSE(s_rx.config_done);
}

void test_unknown_topic_ignored(void) {
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, feed_whole("magtag/other-device/statX", "{}"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_config_single_chunk);
    RUN_TEST(test_config_reassembles_chunks_by_offset);
    RUN_TEST(test_cmd_routes_separately);
    RUN_TEST(test_set_routes_by_content_not_length);
    RUN_TEST(test_sets_accumulate);
    RUN_TEST(test_sets_full_drops);
    RUN_TEST(test_set_key_too_long_drops);
    RUN_TEST(test_empty_retained_clear_ignored);
    RUN_TEST(test_oversize_config_ignored);
    RUN_TEST(test_unknown_topic_ignored);
    return UNITY_END();
}
