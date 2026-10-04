#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "qrcodegen.h"

/* Single-TU compilation, same shape as test_setup_trigger.c. */
#include "../../main/qr_render.c"

void setUp(void) {}
void tearDown(void) {}

/* ---- qr_render_encode: version/size, measured against the real payloads
   this project actually sends (see main/qr_render.c's own arithmetic
   comment for the version-7 ceiling these numbers are measured against) --- */

void test_a_short_payload_encodes_at_the_smallest_version(void) {
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode("hello", &size));
    TEST_ASSERT_EQUAL_INT(21, size); /* version 1 */
}

void test_the_tasks_example_payload_encodes(void) {
    const char *example =
        "{\"ver\":\"v1\",\"name\":\"MagTag-a1b2c3\",\"username\":\"magtag\",\"pop\":\"ABCDEFGHJK\","
        "\"password\":\"ABCDEFGHJK\",\"transport\":\"softap\",\"security\":2}";
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode(example, &size));
    TEST_ASSERT_EQUAL_INT(41, size); /* version 6 */
}

/* The worst-case skeleton from setup_session.h's own comment: a 31-byte
   SSID (SETUP_SESSION_AP_SSID_MAX - 1) and the 10-char AP password used
   twice (pop and password), 150 bytes total. This is the payload
   QR_RENDER_MAX_VERSION is sized against. */
void test_the_worst_case_150_byte_payload_still_fits_version_7(void) {
    const char *worst =
        "{\"ver\":\"v1\",\"name\":\"1234567890123456789012345678901\",\"username\":\"magtag\","
        "\"pop\":\"ABCDEFGHJK\",\"password\":\"ABCDEFGHJK\",\"transport\":\"softap\","
        "\"security\":2}";
    TEST_ASSERT_EQUAL_UINT(150, strlen(worst));
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode(worst, &size));
    TEST_ASSERT_EQUAL_INT(QR_RENDER_MAX_MODULES, size); /* version 7, 45x45 */
}

/* Every QR's finder pattern puts a black module at each of these three
   corners of the outer ring, whatever the payload — a cheap sanity check
   that something was actually drawn, independent of any one golden
   (the plan's own worry: "a golden of a blank QR would pass"). */
static void assert_finder_corners_are_black(int size) {
    TEST_ASSERT_TRUE_MESSAGE(qr_render_module(0, 0), "top-left finder corner is white");
    TEST_ASSERT_TRUE_MESSAGE(qr_render_module(1, 0), "top-left finder ring is white");
    TEST_ASSERT_TRUE_MESSAGE(qr_render_module(0, size - 1), "bottom-left finder corner is white");
}

void test_finder_pattern_corners_are_black(void) {
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode("hello", &size));
    assert_finder_corners_are_black(size);

    TEST_ASSERT_TRUE(
        qr_render_encode("{\"ver\":\"v1\",\"name\":\"MagTag-a1b2c3\",\"username\":\"magtag\",\"pop\":\"ABCDEFGHJK\","
                         "\"password\":\"ABCDEFGHJK\",\"transport\":\"softap\",\"security\":2}",
                         &size));
    assert_finder_corners_are_black(size);
}

/* ---- failure: a payload too long for version 7 at ECC LOW ------------- */

void test_a_too_long_payload_fails_cleanly(void) {
    /* 155 lowercase bytes: forces byte mode (no alphanumeric shortcut) and
       is one byte past version 7 ECC LOW's measured 154-byte ceiling. */
    char buf[156];
    for (int i = 0; i < 155; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[155] = '\0';

    int size = 99;
    TEST_ASSERT_FALSE(qr_render_encode(buf, &size));
    TEST_ASSERT_EQUAL_INT(0, size);
}

void test_a_failed_encode_leaves_every_module_white(void) {
    char buf[156];
    for (int i = 0; i < 155; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[155] = '\0';
    TEST_ASSERT_FALSE(qr_render_encode(buf, &(int){0}));

    /* Every coordinate, not just one: a stale buffer from a previous
       success must not leak through a failed call. */
    for (int y = 0; y < QR_RENDER_MAX_MODULES; y++)
        for (int x = 0; x < QR_RENDER_MAX_MODULES; x++)
            TEST_ASSERT_FALSE(qr_render_module(x, y));
}

/* A failure must not corrupt whatever the PREVIOUS successful encode drew
   — qr_render_module() is specified to answer false only until the NEXT
   successful encode, and a caller that checks encode()'s return value
   before drawing should see this never matters in practice; this pins the
   buffer-level behaviour anyway, since nothing else does. */
void test_a_failed_encode_does_not_corrupt_a_later_success(void) {
    char buf[156];
    for (int i = 0; i < 155; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[155] = '\0';
    int size = -1;
    TEST_ASSERT_FALSE(qr_render_encode(buf, &size));
    TEST_ASSERT_TRUE(qr_render_encode("hello", &size));
    TEST_ASSERT_EQUAL_INT(21, size);
    assert_finder_corners_are_black(size);
}

/* ---- a second encode fully replaces the first (static-state sanity) --- */

void test_a_second_encode_replaces_the_first(void) {
    int size_a = -1;
    TEST_ASSERT_TRUE(qr_render_encode("hello", &size_a));

    int size_b = -1;
    TEST_ASSERT_TRUE(
        qr_render_encode("{\"ver\":\"v1\",\"name\":\"1234567890123456789012345678901\",\"username\":\"magtag\","
                         "\"pop\":\"ABCDEFGHJK\",\"password\":\"ABCDEFGHJK\",\"transport\":\"softap\",\"security\":2}",
                         &size_b));
    TEST_ASSERT_TRUE(size_b != size_a);

    int size_c = -1;
    TEST_ASSERT_TRUE(qr_render_encode("hello", &size_c));
    TEST_ASSERT_EQUAL_INT(size_a, size_c);
}

/* ---- the strong test: every module matches a fresh, independent encode
   of the same payload (the plan's requirement: a golden of a blank QR
   would pass a byte comparison, so this checks the content, not just that
   something non-blank exists). Mirrors qr_render_encode()'s own call
   exactly (ECC LOW, versions 1..QR_RENDER_MAX_VERSION, mask AUTO, no
   boost) with INDEPENDENT buffers, so a bug that only shows up through
   qr_render.c's own static state cannot hide from it. */
void test_every_module_matches_an_independent_qrcodegen_encode(void) {
    const char *payload =
        "{\"ver\":\"v1\",\"name\":\"MagTag-a1b2c3\",\"username\":\"magtag\","
        "\"pop\":\"ABCDEFGHJK\",\"password\":\"ABCDEFGHJK\",\"transport\":\"softap\","
        "\"security\":2}";

    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode(payload, &size));

    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_RENDER_MAX_VERSION)];
    uint8_t ref[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_RENDER_MAX_VERSION)];
    TEST_ASSERT_TRUE(qrcodegen_encodeText(payload, tmp, ref, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                          QR_RENDER_MAX_VERSION, qrcodegen_Mask_AUTO, false));
    TEST_ASSERT_EQUAL_INT(qrcodegen_getSize(ref), size);

    int mismatches = 0;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qr_render_module(x, y) != qrcodegen_getModule(ref, x, y))
                mismatches++;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatches, "qr_render_module disagrees with a fresh qrcodegen encode");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_short_payload_encodes_at_the_smallest_version);
    RUN_TEST(test_the_tasks_example_payload_encodes);
    RUN_TEST(test_the_worst_case_150_byte_payload_still_fits_version_7);
    RUN_TEST(test_finder_pattern_corners_are_black);
    RUN_TEST(test_a_too_long_payload_fails_cleanly);
    RUN_TEST(test_a_failed_encode_leaves_every_module_white);
    RUN_TEST(test_a_failed_encode_does_not_corrupt_a_later_success);
    RUN_TEST(test_a_second_encode_replaces_the_first);
    RUN_TEST(test_every_module_matches_an_independent_qrcodegen_encode);
    return UNITY_END();
}
