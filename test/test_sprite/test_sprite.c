#include <stdint.h>
#include <string.h>
#include <unity.h>

/* Single-TU compilation, same shape as test_qr_render.c. */
#include "../../main/sprite.c"
#include "../../main/sprite_data.c"

void setUp(void) {}
void tearDown(void) {}

#define MAXPX 512

/* Paint a sprite into a w*h byte-per-pixel bitmap (1 = ink) the way the
   screen does, run by run, and count the runs. Every yielded run is also
   checked to be non-empty and inside its row: that is the contract the
   paint callback relies on to never draw outside the sprite. */
static int decode(const sprite_t *s, uint8_t *px) {
    memset(px, 0, (size_t)s->w * s->h);
    sprite_iter_t it;
    sprite_iter_init(&it, s);
    uint16_t x, y, len;
    int runs = 0;
    while (sprite_iter_next(&it, &x, &y, &len)) {
        TEST_ASSERT_TRUE(len >= 1);
        TEST_ASSERT_TRUE(y < s->h);
        TEST_ASSERT_TRUE((uint32_t)x + len <= s->w);
        for (uint16_t k = 0; k < len; k++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, px[(size_t)y * s->w + x + k], "pixel painted twice");
            px[(size_t)y * s->w + x + k] = 1;
        }
        runs++;
    }
    return runs;
}

static void expect_rows(const sprite_t *s, const char *const *rows) {
    uint8_t px[MAXPX];
    decode(s, px);
    for (uint16_t y = 0; y < s->h; y++) {
        char got[64];
        for (uint16_t x = 0; x < s->w; x++)
            got[x] = px[y * s->w + x] ? '#' : '.';
        got[s->w] = 0;
        TEST_ASSERT_EQUAL_STRING_MESSAGE(rows[y], got, "row differs");
    }
}

void test_a_simple_image_decodes_to_the_bitmap(void) {
    /* 4x2: ". # # ." / "# . . #" -> bg1 ink2 bg1 ink1 bg2 ink1 */
    static const uint8_t rle[] = {1, 2, 1, 1, 2, 1};
    const sprite_t s = {4, 2, rle, sizeof rle};
    const char *rows[] = {".##.", "#..#"};
    expect_rows(&s, rows);
}

void test_a_run_wraps_across_rows_and_is_split_at_the_row_end(void) {
    /* 3x3, bg 2, ink 5 (row 0 cols 2, all of row 1, row 2 col 0), bg 2 */
    static const uint8_t rle[] = {2, 5, 2};
    const sprite_t s = {3, 3, rle, sizeof rle};
    const char *rows[] = {"..#", "###", "#.."};
    expect_rows(&s, rows);
    uint8_t px[MAXPX];
    /* three pieces, one per row touched */
    TEST_ASSERT_EQUAL_INT(3, decode(&s, px));
}

void test_a_255_0_continuation_keeps_the_same_colour(void) {
    /* 300x1: bg 10, then ink 280 written as 255,0,25, then bg 10 */
    static const uint8_t rle[] = {10, 255, 0, 25, 10};
    /* width 300 does not fit the 64-char row helper, so check by count */
    const sprite_t s = {300, 1, rle, sizeof rle};
    uint8_t px[MAXPX];
    decode(&s, px);
    for (int x = 0; x < 300; x++)
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((x >= 10 && x < 290) ? 1 : 0, px[x], "continuation lost the colour");
}

void test_a_continued_background_stays_background(void) {
    /* 300x1: bg 295 written 255,0,40, then ink 5 -> ink only at the end */
    static const uint8_t rle[] = {255, 0, 40, 5};
    const sprite_t s = {300, 1, rle, sizeof rle};
    uint8_t px[MAXPX];
    decode(&s, px);
    for (int x = 0; x < 300; x++)
        TEST_ASSERT_EQUAL_UINT8((x >= 295) ? 1 : 0, px[x]);
}

void test_all_background_yields_no_runs(void) {
    static const uint8_t rle[] = {12};
    const sprite_t s = {4, 3, rle, sizeof rle};
    uint8_t px[MAXPX];
    TEST_ASSERT_EQUAL_INT(0, decode(&s, px));
}

void test_all_ink_rows_and_all_background_rows(void) {
    /* 3x3: bg row, ink row, bg row  ->  bg3 ink3 bg3 */
    static const uint8_t rle[] = {3, 3, 3};
    const sprite_t s = {3, 3, rle, sizeof rle};
    const char *rows[] = {"...", "###", "..."};
    expect_rows(&s, rows);
}

void test_an_image_that_starts_with_ink_has_a_zero_length_background(void) {
    static const uint8_t rle[] = {0, 2, 2};
    const sprite_t s = {4, 1, rle, sizeof rle};
    const char *rows[] = {"##.."};
    expect_rows(&s, rows);
}

void test_a_run_ending_exactly_at_a_row_end_does_not_bleed_into_the_next(void) {
    /* 3x2: ink 3 at row 0, then bg 3 */
    static const uint8_t rle[] = {0, 3, 3};
    const sprite_t s = {3, 2, rle, sizeof rle};
    const char *rows[] = {"###", "..."};
    expect_rows(&s, rows);
    /* and a run that starts exactly at a row start */
    static const uint8_t rle2[] = {3, 3};
    const sprite_t s2 = {3, 2, rle2, sizeof rle2};
    const char *rows2[] = {"...", "###"};
    expect_rows(&s2, rows2);
}

void test_an_overrunning_run_is_clamped_to_the_image(void) {
    /* 3x2 = 6 px, but the ink run claims 200: paint stops at the end */
    static const uint8_t rle[] = {4, 200};
    const sprite_t s = {3, 2, rle, sizeof rle};
    const char *rows[] = {"...", ".##"};
    expect_rows(&s, rows);
}

void test_a_truncated_stream_stops_and_leaves_the_rest_background(void) {
    /* 4x2 whose stream ends after the first ink run */
    static const uint8_t rle[] = {1, 2};
    const sprite_t s = {4, 2, rle, sizeof rle};
    const char *rows[] = {".##.", "...."};
    expect_rows(&s, rows);
}

void test_an_empty_stream_and_an_empty_image_yield_nothing(void) {
    uint8_t px[MAXPX];
    static const uint8_t one[] = {5};
    const sprite_t none = {4, 2, one, 0};
    TEST_ASSERT_EQUAL_INT(0, decode(&none, px));
    const sprite_t zero_w = {0, 5, one, 1};
    sprite_iter_t it;
    uint16_t x, y, len;
    sprite_iter_init(&it, &zero_w);
    TEST_ASSERT_FALSE(sprite_iter_next(&it, &x, &y, &len));
    const sprite_t zero_h = {5, 0, one, 1};
    sprite_iter_init(&it, &zero_h);
    TEST_ASSERT_FALSE(sprite_iter_next(&it, &x, &y, &len));
}

void test_the_iterator_never_reads_past_rle_len(void) {
    /* The stream's buffer is followed by a poison byte the iterator must
       not consume: the visible length says the second run is absent. */
    static const uint8_t buf[] = {1, 2, 99};
    const sprite_t s = {8, 1, buf, 2};
    uint8_t px[MAXPX];
    decode(&s, px);
    TEST_ASSERT_EQUAL_UINT8(0, px[0]);
    TEST_ASSERT_EQUAL_UINT8(1, px[1]);
    TEST_ASSERT_EQUAL_UINT8(1, px[2]);
    for (int x = 3; x < 8; x++)
        TEST_ASSERT_EQUAL_UINT8(0, px[x]);
}

void test_a_finished_iterator_keeps_returning_false(void) {
    static const uint8_t rle[] = {0, 1};
    const sprite_t s = {1, 1, rle, sizeof rle};
    sprite_iter_t it;
    uint16_t x, y, len;
    sprite_iter_init(&it, &s);
    TEST_ASSERT_TRUE(sprite_iter_next(&it, &x, &y, &len));
    TEST_ASSERT_FALSE(sprite_iter_next(&it, &x, &y, &len));
    TEST_ASSERT_FALSE(sprite_iter_next(&it, &x, &y, &len));
}

/* The committed sprite data (generated from the TEMPLATES in tools/sprites,
   never from a private sprites.local/). The C suite cannot read the PNGs;
   the exact round-trip against them is test_gen_sprites, which is also the
   single source of the ink counts below (test_gen_sprites.py, the
   SpriteChecks subclasses' INK). What this pins is that the committed data
   is the sprite the screen was laid out for, and that the decoder accepts
   it whole. A template edit fails in test_gen_sprites first and names every
   place to update (here and in test_display_render.c). */
static void assert_generated_sprite(const sprite_t *s, uint16_t w, uint16_t h, uint32_t want_ink) {
    TEST_ASSERT_EQUAL_UINT16(w, s->w);
    TEST_ASSERT_EQUAL_UINT16(h, s->h);
    sprite_iter_t it;
    sprite_iter_init(&it, s);
    uint16_t x, y, len;
    uint32_t ink = 0;
    while (sprite_iter_next(&it, &x, &y, &len)) {
        TEST_ASSERT_TRUE(y < h);
        TEST_ASSERT_TRUE((uint32_t)x + len <= w);
        ink += len;
    }
    TEST_ASSERT_EQUAL_UINT32(want_ink, ink);
    /* the stream is consumed exactly: every byte used, none missing */
    TEST_ASSERT_EQUAL_UINT16(s->rle_len, it.i);
}

/* The charge slot: 105x122 with 3034 ink pixels, the ink count of
   tools/sprites/charge.png (the screen's copy is in test_display_render.c). */
void test_the_generated_charge_art_decodes_to_the_png_dimensions_and_ink_count(void) {
    assert_generated_sprite(&SPRITE_CHARGE, 105, 122, 3034);
}

/* The time's-up slot: 178x124 with 5048 ink pixels, the ink count of
   tools/sprites/timesup.png (the screen's copy is in test_display_render.c).
   Its stream opens with 255,0 pairs (a long blank top), so it takes the
   continuation path the other slots do not; the synthetic tests above cover
   that path too. */
void test_the_generated_timesup_art_decodes_to_the_png_dimensions_and_ink_count(void) {
    assert_generated_sprite(&SPRITE_TIMESUP, 178, 124, 5048);
}

/* The break slot: 46x52 with 1105 ink pixels, the ink count of
   tools/sprites/break.png (the screen's copy is in test_display_render.c).
   It is drawn at 2x, but the scale lives in the screen's paint, not in the
   data: the stream is the 1x art. The rle_len is pinned so a re-encode that
   changes the stream is a visible diff; test_gen_sprites.py (TestBreak.RLE_LEN)
   pins the same 205 from the PNG, so the two cannot drift apart. */
void test_the_generated_break_art_decodes_to_the_png_dimensions_and_ink_count(void) {
    assert_generated_sprite(&SPRITE_BREAK, 46, 52, 1105);
    TEST_ASSERT_EQUAL_UINT16(205, SPRITE_BREAK.rle_len);
}

/* The bed-time slot: 29x23 with 222 ink pixels, the ink count of
   tools/sprites/bedtime.png (the screen's copy is in test_display_render.c).
   Drawn at 4x, white on black; the stream is the 1x art. rle_len is pinned
   like the break slot's (60, also pinned by TestBedtime.RLE_LEN in
   test_gen_sprites.py). */
void test_the_generated_bedtime_art_decodes_to_the_png_dimensions_and_ink_count(void) {
    assert_generated_sprite(&SPRITE_BEDTIME, 29, 23, 222);
    TEST_ASSERT_EQUAL_UINT16(60, SPRITE_BEDTIME.rle_len);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_simple_image_decodes_to_the_bitmap);
    RUN_TEST(test_a_run_wraps_across_rows_and_is_split_at_the_row_end);
    RUN_TEST(test_a_255_0_continuation_keeps_the_same_colour);
    RUN_TEST(test_a_continued_background_stays_background);
    RUN_TEST(test_all_background_yields_no_runs);
    RUN_TEST(test_all_ink_rows_and_all_background_rows);
    RUN_TEST(test_an_image_that_starts_with_ink_has_a_zero_length_background);
    RUN_TEST(test_a_run_ending_exactly_at_a_row_end_does_not_bleed_into_the_next);
    RUN_TEST(test_an_overrunning_run_is_clamped_to_the_image);
    RUN_TEST(test_a_truncated_stream_stops_and_leaves_the_rest_background);
    RUN_TEST(test_an_empty_stream_and_an_empty_image_yield_nothing);
    RUN_TEST(test_the_iterator_never_reads_past_rle_len);
    RUN_TEST(test_a_finished_iterator_keeps_returning_false);
    RUN_TEST(test_the_generated_charge_art_decodes_to_the_png_dimensions_and_ink_count);
    RUN_TEST(test_the_generated_timesup_art_decodes_to_the_png_dimensions_and_ink_count);
    RUN_TEST(test_the_generated_break_art_decodes_to_the_png_dimensions_and_ink_count);
    RUN_TEST(test_the_generated_bedtime_art_decodes_to_the_png_dimensions_and_ink_count);
    return UNITY_END();
}
