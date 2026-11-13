#include <stdint.h>
#include <string.h>
#include <unity.h>

/* Single-TU compilation, same shape as test_sprite.c. */
#include "../../main/sprite_pick.c"

/* The random source is a stub: it returns what the test queued and counts
   its calls, so "the random source is not called" is an assertion. */
static uint32_t s_next;
static int s_calls;

static uint32_t stub_rand(void) {
    s_calls++;
    return s_next;
}

void setUp(void) {
    s_next = 0;
    s_calls = 0;
}
void tearDown(void) {}

void test_a_count_of_one_always_gives_zero_and_never_calls_the_random_source(void) {
    sprite_pick_t st = {0};
    s_next = 99;
    for (int k = SPRITE_LOCK_BREAK; k <= SPRITE_LOCK_BEDTIME; k++) {
        TEST_ASSERT_EQUAL_UINT8(0, sprite_pick(&st, (sprite_lock_t)k, 1, stub_rand));
    }
    TEST_ASSERT_EQUAL_INT(0, s_calls);
}

void test_a_count_of_zero_is_treated_as_one(void) {
    sprite_pick_t st = {0};
    TEST_ASSERT_EQUAL_UINT8(0, sprite_pick(&st, SPRITE_LOCK_CHARGE, 0, stub_rand));
    TEST_ASSERT_EQUAL_INT(0, s_calls);
}

void test_a_new_episode_draws_random_modulo_the_count(void) {
    sprite_pick_t st = {0};
    s_next = 7;
    TEST_ASSERT_EQUAL_UINT8(1, sprite_pick(&st, SPRITE_LOCK_CHARGE, 3, stub_rand)); /* 7 % 3 */
    TEST_ASSERT_EQUAL_INT(1, s_calls);
}

void test_every_repaint_in_one_episode_returns_the_same_variant_without_drawing_again(void) {
    sprite_pick_t st = {0};
    s_next = 2;
    const uint8_t first = sprite_pick(&st, SPRITE_LOCK_TIMESUP, 4, stub_rand);
    TEST_ASSERT_EQUAL_UINT8(2, first);
    s_next = 3; /* a different draw would be visible */
    for (int i = 0; i < 20; i++)
        TEST_ASSERT_EQUAL_UINT8(first, sprite_pick(&st, SPRITE_LOCK_TIMESUP, 4, stub_rand));
    TEST_ASSERT_EQUAL_INT(1, s_calls);
}

void test_changing_to_another_lock_screen_starts_a_new_episode(void) {
    sprite_pick_t st = {0};
    s_next = 1;
    TEST_ASSERT_EQUAL_UINT8(1, sprite_pick(&st, SPRITE_LOCK_BREAK, 3, stub_rand));
    s_next = 2;
    TEST_ASSERT_EQUAL_UINT8(2, sprite_pick(&st, SPRITE_LOCK_TIMESUP, 3, stub_rand));
    TEST_ASSERT_EQUAL_INT(2, s_calls);
    /* and the new episode is the one now remembered */
    s_next = 0;
    TEST_ASSERT_EQUAL_UINT8(2, sprite_pick(&st, SPRITE_LOCK_TIMESUP, 3, stub_rand));
    TEST_ASSERT_EQUAL_INT(2, s_calls);
}

void test_leaving_ends_the_episode_so_the_next_one_draws_again(void) {
    sprite_pick_t st = {0};
    s_next = 1;
    TEST_ASSERT_EQUAL_UINT8(1, sprite_pick(&st, SPRITE_LOCK_CHARGE, 3, stub_rand));
    sprite_pick_leave(&st);
    s_next = 2;
    TEST_ASSERT_EQUAL_UINT8(2, sprite_pick(&st, SPRITE_LOCK_CHARGE, 3, stub_rand));
    TEST_ASSERT_EQUAL_INT(2, s_calls);
}

void test_leaving_when_nothing_is_picked_changes_nothing(void) {
    sprite_pick_t st = {0};
    sprite_pick_leave(&st);
    TEST_ASSERT_EQUAL_UINT8(0, st.lock);
    TEST_ASSERT_EQUAL_UINT8(0, st.variant);
}

void test_no_lock_returns_zero_clears_the_episode_and_does_not_draw(void) {
    sprite_pick_t st = {0};
    s_next = 1;
    sprite_pick(&st, SPRITE_LOCK_BREAK, 3, stub_rand);
    s_calls = 0;
    TEST_ASSERT_EQUAL_UINT8(0, sprite_pick(&st, SPRITE_LOCK_NONE, 3, stub_rand));
    TEST_ASSERT_EQUAL_INT(0, s_calls);
    TEST_ASSERT_EQUAL_UINT8(SPRITE_LOCK_NONE, st.lock);
}

void test_a_lock_value_outside_the_enum_is_treated_as_no_lock(void) {
    sprite_pick_t st = {0};
    TEST_ASSERT_EQUAL_UINT8(0, sprite_pick(&st, (sprite_lock_t)77, 3, stub_rand));
    TEST_ASSERT_EQUAL_INT(0, s_calls);
    TEST_ASSERT_EQUAL_UINT8(SPRITE_LOCK_NONE, st.lock);
}

void test_a_count_that_shrank_since_the_pick_is_never_indexed_out_of_range(void) {
    /* the RTC state outlived a firmware that had 5 variants; this one has 2 */
    sprite_pick_t st = {.lock = SPRITE_LOCK_BEDTIME, .variant = 4};
    s_next = 5;
    const uint8_t v = sprite_pick(&st, SPRITE_LOCK_BEDTIME, 2, stub_rand);
    TEST_ASSERT_EQUAL_UINT8(1, v); /* redrawn: 5 % 2 */
    TEST_ASSERT_EQUAL_INT(1, s_calls);
    TEST_ASSERT_EQUAL_UINT8(1, st.variant);
}

void test_a_count_that_shrank_to_one_gives_zero(void) {
    sprite_pick_t st = {.lock = SPRITE_LOCK_BEDTIME, .variant = 4};
    TEST_ASSERT_EQUAL_UINT8(0, sprite_pick(&st, SPRITE_LOCK_BEDTIME, 1, stub_rand));
    TEST_ASSERT_EQUAL_INT(0, s_calls);
    TEST_ASSERT_EQUAL_UINT8(0, st.variant);
}

void test_garbage_rtc_state_never_yields_an_out_of_range_variant(void) {
    /* the OTA failsafe case: a new image reading an old image's bytes */
    for (unsigned lock = 0; lock < 256; lock++) {
        for (unsigned var = 0; var < 256; var += 5) {
            sprite_pick_t st = {.lock = (uint8_t)lock, .variant = (uint8_t)var};
            s_next = 12345;
            const uint8_t v = sprite_pick(&st, SPRITE_LOCK_CHARGE, 3, stub_rand);
            TEST_ASSERT_TRUE(v < 3);
            TEST_ASSERT_TRUE(st.variant < 3);
        }
    }
}

void test_a_zeroed_state_after_esp_restart_simply_draws_again(void) {
    sprite_pick_t st;
    memset(&st, 0, sizeof st);
    s_next = 1;
    TEST_ASSERT_EQUAL_UINT8(1, sprite_pick(&st, SPRITE_LOCK_BREAK, 2, stub_rand));
    TEST_ASSERT_EQUAL_INT(1, s_calls);
}

void test_the_result_is_always_below_the_count_for_any_draw(void) {
    const uint32_t draws[] = {0, 1, 2, 3, 254, 255, 256, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu};
    for (unsigned count = 1; count <= 300; count += (count < 10 ? 1 : 37)) {
        for (unsigned i = 0; i < sizeof draws / sizeof draws[0]; i++) {
            sprite_pick_t st = {0};
            s_next = draws[i];
            const uint8_t v = sprite_pick(&st, SPRITE_LOCK_CHARGE, count, stub_rand);
            TEST_ASSERT_TRUE(v < (count > 255 ? 255 : count));
        }
    }
}

void test_a_count_over_255_is_clamped_so_the_variant_fits_a_byte(void) {
    sprite_pick_t st = {0};
    s_next = 1000;
    TEST_ASSERT_EQUAL_UINT8(1000 % 255, sprite_pick(&st, SPRITE_LOCK_CHARGE, 1000, stub_rand));
}

void test_a_missing_random_source_with_several_variants_gives_zero(void) {
    sprite_pick_t st = {0};
    TEST_ASSERT_EQUAL_UINT8(0, sprite_pick(&st, SPRITE_LOCK_CHARGE, 3, NULL));
}

void test_a_null_state_gives_zero_and_does_not_crash(void) {
    TEST_ASSERT_EQUAL_UINT8(0, sprite_pick(NULL, SPRITE_LOCK_CHARGE, 3, stub_rand));
    sprite_pick_leave(NULL);
}

void test_the_draw_is_roughly_uniform_over_many_episodes(void) {
    /* a deterministic LCG feeds the stub; this catches a pick stuck on one value */
    int seen[4] = {0};
    uint32_t x = 12345;
    for (int i = 0; i < 400; i++) {
        sprite_pick_t st = {0};
        x = x * 1664525u + 1013904223u;
        s_next = x >> 8;
        seen[sprite_pick(&st, SPRITE_LOCK_BREAK, 4, stub_rand)]++;
    }
    for (int v = 0; v < 4; v++)
        TEST_ASSERT_TRUE(seen[v] > 50);
}

void test_sprite_variant_returns_the_table_entry_in_range(void) {
    const sprite_t a = {1, 1, NULL, 0}, b = {2, 2, NULL, 0};
    const sprite_t *const table[] = {&a, &b};
    TEST_ASSERT_EQUAL_PTR(&a, sprite_variant(table, 2, 0));
    TEST_ASSERT_EQUAL_PTR(&b, sprite_variant(table, 2, 1));
}

void test_sprite_variant_falls_back_to_variant_zero_out_of_range(void) {
    const sprite_t a = {1, 1, NULL, 0}, b = {2, 2, NULL, 0};
    const sprite_t *const table[] = {&a, &b};
    TEST_ASSERT_EQUAL_PTR(&a, sprite_variant(table, 2, 2));
    TEST_ASSERT_EQUAL_PTR(&a, sprite_variant(table, 2, 255));
    TEST_ASSERT_EQUAL_PTR(&a, sprite_variant(table, 1, 1));
}

void test_sprite_variant_with_no_table_or_count_is_null(void) {
    const sprite_t a = {1, 1, NULL, 0};
    const sprite_t *const table[] = {&a};
    TEST_ASSERT_NULL(sprite_variant(NULL, 1, 0));
    TEST_ASSERT_NULL(sprite_variant(table, 0, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_count_of_one_always_gives_zero_and_never_calls_the_random_source);
    RUN_TEST(test_a_count_of_zero_is_treated_as_one);
    RUN_TEST(test_a_new_episode_draws_random_modulo_the_count);
    RUN_TEST(test_every_repaint_in_one_episode_returns_the_same_variant_without_drawing_again);
    RUN_TEST(test_changing_to_another_lock_screen_starts_a_new_episode);
    RUN_TEST(test_leaving_ends_the_episode_so_the_next_one_draws_again);
    RUN_TEST(test_leaving_when_nothing_is_picked_changes_nothing);
    RUN_TEST(test_no_lock_returns_zero_clears_the_episode_and_does_not_draw);
    RUN_TEST(test_a_lock_value_outside_the_enum_is_treated_as_no_lock);
    RUN_TEST(test_a_count_that_shrank_since_the_pick_is_never_indexed_out_of_range);
    RUN_TEST(test_a_count_that_shrank_to_one_gives_zero);
    RUN_TEST(test_garbage_rtc_state_never_yields_an_out_of_range_variant);
    RUN_TEST(test_a_zeroed_state_after_esp_restart_simply_draws_again);
    RUN_TEST(test_the_result_is_always_below_the_count_for_any_draw);
    RUN_TEST(test_a_count_over_255_is_clamped_so_the_variant_fits_a_byte);
    RUN_TEST(test_a_missing_random_source_with_several_variants_gives_zero);
    RUN_TEST(test_a_null_state_gives_zero_and_does_not_crash);
    RUN_TEST(test_the_draw_is_roughly_uniform_over_many_episodes);
    RUN_TEST(test_sprite_variant_returns_the_table_entry_in_range);
    RUN_TEST(test_sprite_variant_falls_back_to_variant_zero_out_of_range);
    RUN_TEST(test_sprite_variant_with_no_table_or_count_is_null);
    return UNITY_END();
}
