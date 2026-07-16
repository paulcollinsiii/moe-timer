#include <string.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/wav_header.c"

void setUp(void) {}
void tearDown(void) {}

/* Build a canonical RIFF/WAVE header into buf; returns total length.
   Layout: RIFF | WAVE | fmt (16-byte PCM block) | data. */
static size_t make_wav(uint8_t *buf, uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits,
                       uint32_t data_len) {
    uint32_t byte_rate = rate * channels * bits / 8;
    uint16_t block_align = (uint16_t)(channels * bits / 8);
    uint8_t *p = buf;
    memcpy(p, "RIFF", 4);
    uint32_t riff_len = 4 + 24 + 8 + data_len;
    memcpy(p + 4, &riff_len, 4);
    memcpy(p + 8, "WAVE", 4);
    p += 12;
    memcpy(p, "fmt ", 4);
    uint32_t fmt_len = 16;
    memcpy(p + 4, &fmt_len, 4);
    memcpy(p + 8, &format, 2);
    memcpy(p + 10, &channels, 2);
    memcpy(p + 12, &rate, 4);
    memcpy(p + 16, &byte_rate, 4);
    memcpy(p + 20, &block_align, 2);
    memcpy(p + 22, &bits, 2);
    p += 8 + 16;
    memcpy(p, "data", 4);
    memcpy(p + 4, &data_len, 4);
    p += 8;
    return (size_t)(p - buf);
}

static uint8_t s_buf[512];

void test_accepts_valid_16bit_mono(void) {
    size_t hdr = make_wav(s_buf, 1, 1, 16000, 16, 64000);
    wav_info_t info;
    TEST_ASSERT_TRUE(wav_header_parse(s_buf, sizeof(s_buf), &info));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)hdr, info.data_offset);
    TEST_ASSERT_EQUAL_UINT32(64000, info.data_len);
    TEST_ASSERT_EQUAL_UINT32(16000, info.sample_rate);
}

void test_accepts_rate_bounds(void) {
    wav_info_t info;
    make_wav(s_buf, 1, 1, 8000, 16, 100);
    TEST_ASSERT_TRUE(wav_header_parse(s_buf, sizeof(s_buf), &info));
    make_wav(s_buf, 1, 1, 22050, 16, 100);
    TEST_ASSERT_TRUE(wav_header_parse(s_buf, sizeof(s_buf), &info));
}

void test_rejects_wrong_formats(void) {
    wav_info_t info;
    make_wav(s_buf, 3, 1, 16000, 16, 100); /* IEEE float */
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
    make_wav(s_buf, 1, 2, 16000, 16, 100); /* stereo */
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
    make_wav(s_buf, 1, 1, 16000, 8, 100); /* 8-bit */
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
    make_wav(s_buf, 1, 1, 16000, 24, 100); /* 24-bit */
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
    make_wav(s_buf, 1, 1, 44100, 16, 100); /* too fast */
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
    make_wav(s_buf, 1, 1, 7999, 16, 100); /* too slow */
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
}

void test_rejects_empty_data(void) {
    wav_info_t info;
    make_wav(s_buf, 1, 1, 16000, 16, 0);
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
}

void test_rejects_wrong_magics(void) {
    wav_info_t info;
    make_wav(s_buf, 1, 1, 16000, 16, 100);
    s_buf[0] = 'X'; /* not RIFF */
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
    make_wav(s_buf, 1, 1, 16000, 16, 100);
    s_buf[8] = 'X'; /* not WAVE */
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
}

void test_rejects_truncated_buffers(void) {
    wav_info_t info;
    size_t hdr = make_wav(s_buf, 1, 1, 16000, 16, 100);
    for (size_t len = 0; len < hdr; len++) {
        TEST_ASSERT_FALSE_MESSAGE(wav_header_parse(s_buf, len, &info), "accepted a truncated header");
    }
    /* Erased-flash prefix (all 0xFF) must not parse. */
    memset(s_buf, 0xFF, sizeof(s_buf));
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
}

void test_rejects_missing_data_chunk(void) {
    wav_info_t info;
    size_t hdr = make_wav(s_buf, 1, 1, 16000, 16, 100);
    memcpy(s_buf + hdr - 8, "LIST", 4); /* rename the data chunk */
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
}

void test_rejects_absurd_chunk_size(void) {
    wav_info_t info;
    make_wav(s_buf, 1, 1, 16000, 16, 100);
    uint32_t huge = 0xFFFFFFF0u; /* fmt chunk size overflows the walk */
    memcpy(s_buf + 16, &huge, 4);
    TEST_ASSERT_FALSE(wav_header_parse(s_buf, sizeof(s_buf), &info));
}

void test_skips_leading_extra_chunk(void) {
    /* LIST chunk between WAVE and fmt: walker must skip it. */
    uint8_t tmp[512];
    size_t hdr = make_wav(tmp, 1, 1, 16000, 16, 4242);
    memcpy(s_buf, tmp, 12);
    memcpy(s_buf + 12, "LIST", 4);
    uint32_t list_len = 9; /* odd on purpose: chunks are word-padded */
    memcpy(s_buf + 16, &list_len, 4);
    memset(s_buf + 20, 0, 9 + 1);
    memcpy(s_buf + 12 + 8 + 9 + 1, tmp + 12, hdr - 12); /* +pad byte */
    wav_info_t info;
    TEST_ASSERT_TRUE(wav_header_parse(s_buf, sizeof(s_buf), &info));
    TEST_ASSERT_EQUAL_UINT32(4242, info.data_len);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_accepts_valid_16bit_mono);
    RUN_TEST(test_accepts_rate_bounds);
    RUN_TEST(test_rejects_wrong_formats);
    RUN_TEST(test_rejects_empty_data);
    RUN_TEST(test_rejects_wrong_magics);
    RUN_TEST(test_rejects_truncated_buffers);
    RUN_TEST(test_rejects_missing_data_chunk);
    RUN_TEST(test_rejects_absurd_chunk_size);
    RUN_TEST(test_skips_leading_extra_chunk);
    return UNITY_END();
}
