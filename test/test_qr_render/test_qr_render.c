#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "qrcodegen.h"

/* Single-TU compilation, same shape as test_setup_trigger.c. */
#include "../../main/qr_render.c"

void setUp(void) {}
void tearDown(void) {}

/* One byte past version 3's ECC LOW byte-mode capacity (53), the ceiling
   QR_RENDER_MAX_VERSION sets. Lowercase forces byte mode, so no
   alphanumeric shortcut can make it fit. */
#define TOO_LONG_LEN 54

/* ---- qr_render_encode: version/size, measured against the real payloads
   this project actually sends (see include/qr_render.h for the version-3
   ceiling these numbers are measured against) --- */

void test_a_short_payload_encodes_at_the_smallest_version(void) {
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode("hello", &size));
    TEST_ASSERT_EQUAL_INT(21, size); /* version 1 */
}

/* The payload this device really sends: a 13-byte SSID and a 10-byte
   password, 41 bytes in all. */
void test_the_real_device_payload_encodes_at_version_3(void) {
    const char *real = "WIFI:T:WPA;S:MagTag-a1b2c3;P:ABCDEFGHJK;;";
    TEST_ASSERT_EQUAL_UINT(41, strlen(real));
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode(real, &size));
    TEST_ASSERT_EQUAL_INT(29, size); /* version 3 */
}

/* The longest payload that still fits: 53 bytes, filling version 3 to its
   ECC LOW byte-mode capacity. */
void test_a_payload_at_the_ceiling_still_fits_the_largest_version(void) {
    char buf[54];
    for (int i = 0; i < 53; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[53] = '\0';
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode(buf, &size));
    TEST_ASSERT_EQUAL_INT(QR_RENDER_MAX_MODULES, size); /* version 3, 29x29 */
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

    TEST_ASSERT_TRUE(qr_render_encode("WIFI:T:WPA;S:MagTag-a1b2c3;P:ABCDEFGHJK;;", &size));
    assert_finder_corners_are_black(size);
}

/* ---- failure: a payload too long for the largest version at ECC LOW --- */

void test_a_too_long_payload_fails_cleanly(void) {
    char buf[TOO_LONG_LEN + 1];
    for (int i = 0; i < TOO_LONG_LEN; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[TOO_LONG_LEN] = '\0';

    int size = 99;
    TEST_ASSERT_FALSE(qr_render_encode(buf, &size));
    TEST_ASSERT_EQUAL_INT(0, size);
}

void test_a_failed_encode_leaves_every_module_white(void) {
    char buf[TOO_LONG_LEN + 1];
    for (int i = 0; i < TOO_LONG_LEN; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[TOO_LONG_LEN] = '\0';
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
    char buf[TOO_LONG_LEN + 1];
    for (int i = 0; i < TOO_LONG_LEN; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[TOO_LONG_LEN] = '\0';
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
    TEST_ASSERT_TRUE(qr_render_encode("WIFI:T:WPA;S:MagTag-a1b2c3;P:ABCDEFGHJK;;", &size_b));
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
    const char *payload = "WIFI:T:WPA;S:MagTag-a1b2c3;P:ABCDEFGHJK;;";

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

/* ---- qr_render_last_size: the draw callback's own source of truth ----- */

void test_last_size_matches_a_successful_encodes_size_out(void) {
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode("hello", &size));
    TEST_ASSERT_EQUAL_INT(size, qr_render_last_size());
}

void test_last_size_is_zero_before_any_encode_and_after_a_failure(void) {
    /* This test file's own process never called qr_render_encode() before
       this point in a fresh run, but ordering across Unity test functions
       in one binary is not guaranteed independent of global state, so
       this pins the POST-FAILURE case, which is the one the draw callback
       actually depends on, rather than relying on being run first. */
    char buf[TOO_LONG_LEN + 1];
    for (int i = 0; i < TOO_LONG_LEN; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[TOO_LONG_LEN] = '\0';
    TEST_ASSERT_FALSE(qr_render_encode(buf, &(int){0}));
    TEST_ASSERT_EQUAL_INT(0, qr_render_last_size());
}

/* ---- qr_render_release: the heap buffer's explicit lifetime ----------- */

/* display.c's setup wrapper calls this right after render() returns, so
   the draw callback must be safe to have already read everything it
   needed before this runs — proven here under ASan (test/CMakeLists.txt
   builds every host suite with -fsanitize=address): if qr_render_module()
   below touched the freed buffer instead of just answering false, this
   test would abort the whole binary, not merely fail an assertion. */
void test_release_blanks_every_module_without_touching_the_freed_buffer(void) {
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode("hello", &size));
    TEST_ASSERT_TRUE(size > 0);

    qr_render_release();

    TEST_ASSERT_EQUAL_INT(0, qr_render_last_size());
    for (int y = 0; y < QR_RENDER_MAX_MODULES; y++)
        for (int x = 0; x < QR_RENDER_MAX_MODULES; x++)
            TEST_ASSERT_FALSE(qr_render_module(x, y));
}

/* Release with nothing held, and release twice in a row: both are
   documented no-ops and must not crash (free(NULL) is well-defined, but
   this pins it as this module's own contract rather than relying on
   knowing that about free()). */
void test_release_is_safe_with_no_code_held_and_safe_to_call_twice(void) {
    qr_render_release();
    qr_render_release();
    TEST_ASSERT_EQUAL_INT(0, qr_render_last_size());
    TEST_ASSERT_FALSE(qr_render_module(0, 0));
}

/* A successful encode followed by a failed one must blank every module
   from the FIRST encode too — qr_render_encode() releases the previous
   buffer unconditionally, before it even attempts the new one, so a
   failure can never leave the previous success still readable. */
void test_a_failed_encode_after_a_success_blanks_every_module(void) {
    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode("hello", &size));
    TEST_ASSERT_TRUE(size > 0);

    char buf[TOO_LONG_LEN + 1];
    for (int i = 0; i < TOO_LONG_LEN; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[TOO_LONG_LEN] = '\0';
    TEST_ASSERT_FALSE(qr_render_encode(buf, &size));

    TEST_ASSERT_EQUAL_INT(0, qr_render_last_size());
    for (int y = 0; y < QR_RENDER_MAX_MODULES; y++)
        for (int x = 0; x < QR_RENDER_MAX_MODULES; x++)
            TEST_ASSERT_FALSE(qr_render_module(x, y));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_short_payload_encodes_at_the_smallest_version);
    RUN_TEST(test_the_real_device_payload_encodes_at_version_3);
    RUN_TEST(test_a_payload_at_the_ceiling_still_fits_the_largest_version);
    RUN_TEST(test_finder_pattern_corners_are_black);
    RUN_TEST(test_a_too_long_payload_fails_cleanly);
    RUN_TEST(test_a_failed_encode_leaves_every_module_white);
    RUN_TEST(test_a_failed_encode_does_not_corrupt_a_later_success);
    RUN_TEST(test_a_second_encode_replaces_the_first);
    RUN_TEST(test_every_module_matches_an_independent_qrcodegen_encode);
    RUN_TEST(test_last_size_matches_a_successful_encodes_size_out);
    RUN_TEST(test_last_size_is_zero_before_any_encode_and_after_a_failure);
    RUN_TEST(test_release_blanks_every_module_without_touching_the_freed_buffer);
    RUN_TEST(test_release_is_safe_with_no_code_held_and_safe_to_call_twice);
    RUN_TEST(test_a_failed_encode_after_a_success_blanks_every_module);
    return UNITY_END();
}
