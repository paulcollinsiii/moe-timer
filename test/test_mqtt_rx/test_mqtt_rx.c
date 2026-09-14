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
        .config_topic = CONFIG_TOPIC,
        .cmd_buf = s_cmd,
        .cmd_cap = sizeof(s_cmd),
        .cmd_topic = CMD_TOPIC,
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

/* Over-capacity on OUR OWN topic is a refusal, not an ignore. The two used
   to share MQTT_RX_IGNORED, which is why a retained oversized document was
   re-delivered and re-dropped on every reconnect with nothing logged and no
   ack published — permanent config paralysis with zero signal. The refusal
   also has to be readable from the window flow (which runs on another
   task), hence the context flags. */
void test_oversize_config_refused_and_recorded(void) {
    char big[256];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL(MQTT_RX_CONFIG_TOO_LONG, feed(CONFIG_TOPIC, big, (int)sizeof(big), 0, (int)sizeof(big)));
    TEST_ASSERT_FALSE(s_rx.config_done);
    TEST_ASSERT_TRUE(s_rx.config_too_long);
    TEST_ASSERT_EQUAL_INT((int)sizeof(big), s_rx.config_too_long_len);
}

void test_oversize_cmd_refused_and_recorded(void) {
    char big[256];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL(MQTT_RX_CMD_TOO_LONG, feed(CMD_TOPIC, big, (int)sizeof(big), 0, (int)sizeof(big)));
    TEST_ASSERT_FALSE(s_rx.cmd_done);
    TEST_ASSERT_TRUE(s_rx.cmd_too_long);
    TEST_ASSERT_EQUAL_INT((int)sizeof(big), s_rx.cmd_too_long_len);
}

/* The exact boundary of the `total_len < cap` gate, both sides, so a
   one-character document can never be refused by an off-by-one and the
   NUL at config_buf[total_len] always has a byte to land in. */
void test_config_at_the_capacity_boundary(void) {
    char big[sizeof(s_config)];
    memset(big, 'y', sizeof(big));
    /* cap - 1 bytes: the longest document that fits with its terminator. */
    TEST_ASSERT_EQUAL(MQTT_RX_OK, feed(CONFIG_TOPIC, big, (int)sizeof(s_config) - 1, 0, (int)sizeof(s_config) - 1));
    TEST_ASSERT_TRUE(s_rx.config_done);
    TEST_ASSERT_EQUAL_INT((int)sizeof(s_config) - 1, (int)strlen(s_config));

    setUp();
    /* Exactly cap: one byte too many, because the terminator needs one. */
    TEST_ASSERT_EQUAL(MQTT_RX_CONFIG_TOO_LONG,
                      feed(CONFIG_TOPIC, big, (int)sizeof(s_config), 0, (int)sizeof(s_config)));
    TEST_ASSERT_FALSE(s_rx.config_done);
}

/* A foreign topic that happens to be the same LENGTH as ours — a sibling
   device on the same broker — is not ours, at either size. Both halves
   matter and neither is redundant:
     - a document that FITS was previously ACCEPTED as our config and
       applied to NVS, because the match was `topic_len ==`;
     - a document that does not fit must stay an ignore rather than become
       a refusal, because a refusal now publishes a retained config_ack,
       and accusing an operator of a document they never sent would be
       worse than the silence being fixed here. */
void test_foreign_topic_of_equal_length_is_ignored_not_refused(void) {
    char foreign[sizeof(CONFIG_TOPIC)];
    memcpy(foreign, CONFIG_TOPIC, sizeof(CONFIG_TOPIC));
    foreign[7] = 'Z'; /* same length, a different device id */
    TEST_ASSERT_EQUAL_INT((int)strlen(CONFIG_TOPIC), (int)strlen(foreign));
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, feed_whole(foreign, "{\"ver\":\"999\"}"));
    TEST_ASSERT_FALSE(s_rx.config_done);
    TEST_ASSERT_EQUAL_INT(0, s_config[0]); /* not a byte was copied */

    char big[sizeof(s_config) + 8];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, feed(foreign, big, (int)sizeof(big), 0, (int)sizeof(big)));
    TEST_ASSERT_FALSE(s_rx.config_too_long);
    TEST_ASSERT_FALSE(s_rx.config_done);
}

/* The reachable half of the same hazard, and the reason length matching had
   to go rather than merely be commented: the set/ branch only claims chunk
   zero, so the TAIL of a chunked set/tz fell through to the config branch —
   and magtag/<id>/set/tz is exactly as long as magtag/<id>/config. */
void test_chunked_set_tail_is_not_routed_to_config(void) {
    TEST_ASSERT_EQUAL_INT((int)strlen(SET_PREFIX "tz"), (int)strlen(CONFIG_TOPIC));
    char value[40];
    memset(value, 'v', sizeof(value));
    /* Deliberately sized to FIT config_cap: the old length match copied
       this tail into config_buf at its offset and, since it completes the
       payload, set config_done — a set value applied as a config document.
       An over-cap payload would not have shown it, because the old
       capacity test would have dropped it for the wrong reason. */
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, feed(SET_PREFIX "tz", value, (int)sizeof(value), 20, 20));
    TEST_ASSERT_FALSE(s_rx.config_done);
    TEST_ASSERT_FALSE(s_rx.config_too_long);
    TEST_ASSERT_EQUAL_INT(0, s_config[20]); /* nothing landed in the buffer */
    TEST_ASSERT_EQUAL_INT(0, s_rx.set_count);
}

/* Local bound hardening. Correct MQTT framing makes every one of these
   unreachable (total_len is the remaining-length header), so the point is
   not to handle a case that happens — it is to make the memcpy safe by
   inspection instead of safe by trusting esp-mqtt. */
void test_chunk_past_total_len_is_rejected(void) {
    const char *doc = "{\"ver\":1}";
    TEST_ASSERT_EQUAL(MQTT_RX_BAD_CHUNK, feed(CONFIG_TOPIC, doc, 8, 4, 8)); /* 4 + 8 > 8 */
    TEST_ASSERT_FALSE(s_rx.config_done);
    TEST_ASSERT_EQUAL(MQTT_RX_BAD_CHUNK, feed(CMD_TOPIC, doc, 8, 4, 8));
    TEST_ASSERT_FALSE(s_rx.cmd_done);
}

void test_negative_offset_or_len_is_rejected(void) {
    const char *doc = "{\"ver\":1}";
    TEST_ASSERT_EQUAL(MQTT_RX_BAD_CHUNK, feed(CONFIG_TOPIC, doc, 8, -1, 4));
    TEST_ASSERT_EQUAL(MQTT_RX_BAD_CHUNK, feed(CONFIG_TOPIC, doc, 8, 0, -1));
    TEST_ASSERT_FALSE(s_rx.config_done);
    /* The set branch reaches memcpy with data_len cast to size_t, so a
       negative one is the same hazard wearing different clothes: the
       `data_len >= sizeof(value)` capacity test passes it straight through. */
    TEST_ASSERT_EQUAL(MQTT_RX_BAD_CHUNK, feed(SET_PREFIX "tz", doc, 8, 0, -1));
    TEST_ASSERT_EQUAL_INT(0, s_rx.set_count);
}

void test_null_payload_is_ignored(void) {
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, mqtt_rx_on_data(&s_rx, CONFIG_TOPIC, (int)strlen(CONFIG_TOPIC), NULL, 0, 8, 0));
    TEST_ASSERT_FALSE(s_rx.config_done);
}

/* An unconfigured topic slot must not match a zero-length topic or crash. */
void test_unconfigured_topic_never_matches(void) {
    s_rx.config_topic = NULL;
    s_rx.cmd_topic = NULL;
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, feed_whole(CONFIG_TOPIC, "{\"ver\":1}"));
    TEST_ASSERT_FALSE(s_rx.config_done);
}

/* esp-mqtt fills in `topic` only on the FIRST data event of a fragmented
   message unless CONFIG_MQTT_TOPIC_PRESENT_ALL_DATA_EVENTS is set — it is
   not, and it depends on MQTT_USE_CUSTOM_CONFIG, which is off too. Every
   later fragment therefore arrives exactly like this, with no topic at all,
   and must be ignored: there is nothing to route it by, and assuming "it
   will be the config, it usually is" would reopen the misroute the content
   match just closed.

   The consequence is why mqtt_ha.c sets the client's buffer.size instead of
   defaulting it: a document big enough to fragment loses its tail HERE, and
   nothing downstream can tell the difference between that and a document
   nobody published. Pinned so the constraint is executable rather than
   folklore. */
void test_a_continuation_fragment_with_no_topic_is_ignored(void) {
    const char *doc = "{\"ver\":1,\"tz\":\"UTC0\"}";
    int total = (int)strlen(doc);
    TEST_ASSERT_EQUAL(MQTT_RX_OK, feed(CONFIG_TOPIC, doc, total, 0, 10));
    TEST_ASSERT_FALSE(s_rx.config_done);
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, mqtt_rx_on_data(&s_rx, NULL, 0, doc + 10, total - 10, total, 10));
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
    RUN_TEST(test_oversize_config_refused_and_recorded);
    RUN_TEST(test_oversize_cmd_refused_and_recorded);
    RUN_TEST(test_config_at_the_capacity_boundary);
    RUN_TEST(test_foreign_topic_of_equal_length_is_ignored_not_refused);
    RUN_TEST(test_chunked_set_tail_is_not_routed_to_config);
    RUN_TEST(test_chunk_past_total_len_is_rejected);
    RUN_TEST(test_negative_offset_or_len_is_rejected);
    RUN_TEST(test_null_payload_is_ignored);
    RUN_TEST(test_unconfigured_topic_never_matches);
    RUN_TEST(test_a_continuation_fragment_with_no_topic_is_ignored);
    RUN_TEST(test_unknown_topic_ignored);
    return UNITY_END();
}
