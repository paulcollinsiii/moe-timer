/* The generated 1 bpp UI fonts against what the screens draw.
   Three claims, for every font under main/fonts/:
     1. each font is 1 bpp and carries every code point of
        include/ui_font_ranges.h (and nothing a 28/48 size was subset away);
     2. its stamped lv_font_conv command line names exactly those ranges, so a
        range edit without a regeneration fails here;
     3. the ranges cover what display_screens.c draws: the literal text of
        every 28 and 48 label, the symbols used at 12 and 16, and a pinned
        count of the labels whose text is not a literal (the digit labels),
        so a new dynamic string at a subset size cannot slip in. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "lvgl.h"
#include "ui_font_ranges.h"
#include "ui_fonts.h"

typedef struct {
    const char *set;
    int px;
    const lv_font_t *font;
} font_entry_t;

#define ENTRY(px) {"term", px, UI_FONT_##px},
static const font_entry_t FONT_TABLE[] = {ENTRY(12) ENTRY(16) ENTRY(18) ENTRY(28) ENTRY(48)};
#define FONT_COUNT (sizeof(FONT_TABLE) / sizeof(FONT_TABLE[0]))

static const char *range_of(int px) {
    switch (px) {
        case 12:
            return UI_RANGE_12;
        case 16:
            return UI_RANGE_16;
        case 18:
            return UI_RANGE_18;
        case 28:
            return UI_RANGE_28;
        default:
            return UI_RANGE_48;
    }
}

static const char *syms_of(int px) {
    switch (px) {
        case 12:
            return UI_SYMS_12;
        case 16:
            return UI_SYMS_16;
        case 18:
            return UI_SYMS_18;
        case 28:
            return UI_SYMS_28;
        default:
            return UI_SYMS_48;
    }
}

/* Calls fn(codepoint, ctx) for every code point of an lv_font_conv range
   list: "0x20-0x7E,0xB0". */
typedef void (*cp_fn)(uint32_t cp, void *ctx);
static void each_cp(const char *list, cp_fn fn, void *ctx) {
    const char *p = list;
    while (*p != '\0') {
        char *end;
        unsigned long lo = strtoul(p, &end, 0);
        unsigned long hi = lo;
        if (*end == '-')
            hi = strtoul(end + 1, &end, 0);
        for (unsigned long c = lo; c <= hi; c++)
            fn((uint32_t)c, ctx);
        p = (*end == ',') ? end + 1 : end;
    }
}

static bool in_list(const char *list, uint32_t cp);
static void in_list_cb(uint32_t c, void *ctx) {
    uint32_t *probe = ctx; /* probe[0] = wanted, probe[1] = found */
    if (c == probe[0])
        probe[1] = 1;
}
static bool in_list(const char *list, uint32_t cp) {
    uint32_t probe[2] = {cp, 0};
    each_cp(list, in_list_cb, probe);
    return probe[1] != 0;
}

typedef struct {
    const font_entry_t *e;
    int missing;
} have_ctx_t;
static void have_cb(uint32_t cp, void *ctx) {
    have_ctx_t *h = ctx;
    lv_font_glyph_dsc_t dsc;
    if (!lv_font_get_glyph_dsc(h->e->font, &dsc, cp, 0)) {
        printf("%s %d px: no glyph for U+%04X\n", h->e->set, h->e->px, (unsigned)cp);
        h->missing++;
    }
}

void test_every_font_is_1bpp_and_carries_its_ranges(void) {
    for (size_t i = 0; i < FONT_COUNT; i++) {
        const font_entry_t *e = &FONT_TABLE[i];
        const lv_font_fmt_txt_dsc_t *dsc = e->font->dsc;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dsc->bpp, e->set);
        TEST_ASSERT_NULL_MESSAGE(e->font->fallback, "a fallback would drag a 4 bpp font back in");
        have_ctx_t h = {e, 0};
        each_cp(range_of(e->px), have_cb, &h);
        each_cp(syms_of(e->px), have_cb, &h);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, h.missing, e->set);
    }
}

/* A label with no explicit font, the theme and the style defaults resolve to
   LVGL's default font; it must be ours, not a stock Montserrat. */
void test_lvgl_default_font_is_the_12_px_ui_font(void) {
    TEST_ASSERT_EQUAL_PTR(UI_FONT_12, lv_font_get_default());
    TEST_ASSERT_EQUAL_PTR(UI_FONT_12, LV_FONT_DEFAULT);
}

void test_the_subset_sizes_carry_nothing_else(void) {
    static const uint32_t ABSENT[] = {'a', 'z', 'Z', '@', 0xF00C, 0xF240};
    for (size_t i = 0; i < FONT_COUNT; i++) {
        const font_entry_t *e = &FONT_TABLE[i];
        if (e->px != 48)
            continue;
        for (size_t k = 0; k < sizeof(ABSENT) / sizeof(ABSENT[0]); k++) {
            lv_font_glyph_dsc_t dsc;
            TEST_ASSERT_FALSE_MESSAGE(lv_font_get_glyph_dsc(e->font, &dsc, ABSENT[k], 0), e->set);
        }
    }
}

/* The Opts line lv_font_conv stamps into each file records the command
   that made it. */
static void read_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
}

void test_each_generated_file_was_made_from_the_current_ranges(void) {
    static char text[1 << 20];
    for (size_t i = 0; i < FONT_COUNT; i++) {
        const font_entry_t *e = &FONT_TABLE[i];
        char path[512];
        snprintf(path, sizeof(path), "%s/main/fonts/ui_font_%s_%d.c", SRC_DIR, e->set, e->px);
        read_file(path, text, sizeof(text));
        const char *opts = strstr(text, " * Opts:");
        TEST_ASSERT_NOT_NULL_MESSAGE(opts, path);
        char line[2048];
        size_t n = strcspn(opts, "\n");
        TEST_ASSERT_TRUE(n < sizeof(line));
        memcpy(line, opts, n);
        line[n] = '\0';

        char want[1024];
        snprintf(want, sizeof(want), "--bpp 1 --size %d ", e->px);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line, want), path);
        snprintf(want, sizeof(want), " -r %s ", range_of(e->px));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line, want), path);
        if (syms_of(e->px)[0] != '\0') {
            snprintf(want, sizeof(want), " -r %s ", syms_of(e->px));
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line, want), path);
        } else {
            TEST_ASSERT_NULL_MESSAGE(strstr(line, "FontAwesome"), path);
        }
        snprintf(want, sizeof(want), "--lv-font-name ui_font_%s_%d ", e->set, e->px);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line, want), path);
    }
}

/* ---- what the screens draw ------------------------------------------- */

void test_the_text_sizes_cover_printable_ascii(void) {
    static const int TEXT_SIZES[] = {12, 16, 18};
    for (size_t s = 0; s < 3; s++)
        for (uint32_t c = 0x20; c <= 0x7E; c++)
            TEST_ASSERT_TRUE_MESSAGE(in_list(range_of(TEXT_SIZES[s]), c), "user text needs all printable ASCII");
}

/* The symbols display_screens.c draws, by name. A name found in the source
   but missing here fails the scan below, so the table cannot go stale. */
typedef struct {
    const char *name;
    const char *utf8;
    bool at12, at16;
} sym_t;
static const sym_t SYMS[] = {
    {"LV_SYMBOL_BATTERY_EMPTY", LV_SYMBOL_BATTERY_EMPTY, true, false},
    {"LV_SYMBOL_BATTERY_1", LV_SYMBOL_BATTERY_1, true, false},
    {"LV_SYMBOL_BATTERY_2", LV_SYMBOL_BATTERY_2, true, false},
    {"LV_SYMBOL_BATTERY_3", LV_SYMBOL_BATTERY_3, true, false},
    {"LV_SYMBOL_BATTERY_FULL", LV_SYMBOL_BATTERY_FULL, true, false},
    {"LV_SYMBOL_PLAY", LV_SYMBOL_PLAY, true, false},
    {"LV_SYMBOL_PAUSE", LV_SYMBOL_PAUSE, true, false},
    {"LV_SYMBOL_RIGHT", LV_SYMBOL_RIGHT, true, true},
    {"LV_SYMBOL_REFRESH", LV_SYMBOL_REFRESH, true, true},
    {"LV_SYMBOL_OK", LV_SYMBOL_OK, true, true},
};

static uint32_t decode3(const char *s) {
    return ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
}

void test_the_symbol_set_covers_every_symbol_the_screens_use(void) {
    for (size_t i = 0; i < sizeof(SYMS) / sizeof(SYMS[0]); i++) {
        uint32_t cp = decode3(SYMS[i].utf8);
        TEST_ASSERT_EQUAL_MESSAGE(SYMS[i].at12, in_list(UI_SYMS_12, cp), SYMS[i].name);
        TEST_ASSERT_EQUAL_MESSAGE(SYMS[i].at16, in_list(UI_SYMS_16, cp), SYMS[i].name);
        TEST_ASSERT_FALSE_MESSAGE(in_list(UI_SYMS_18, cp) || in_list(UI_SYMS_28, cp) || in_list(UI_SYMS_48, cp),
                                  SYMS[i].name);
    }
}

static char s_src[1 << 20];

void test_every_symbol_in_display_screens_is_in_the_table(void) {
    read_file(SRC_DIR "/main/display_screens.c", s_src, sizeof(s_src));
    const char *p = s_src;
    while ((p = strstr(p, "LV_SYMBOL_")) != NULL) {
        size_t n = strspn(p, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
        bool known = false;
        for (size_t i = 0; i < sizeof(SYMS) / sizeof(SYMS[0]); i++)
            known |= strlen(SYMS[i].name) == n && strncmp(SYMS[i].name, p, n) == 0;
        if (!known)
            printf("unlisted symbol: %.*s\n", (int)n, p);
        TEST_ASSERT_TRUE_MESSAGE(known, "a symbol display_screens.c draws is not in SYMS (and UI_SYMS_*)");
        p += n;
    }
}

/* The label call before each UI_FONT_<px> use in display_screens.c: the
   literal when the text is one, else a count. */
static void scan_labels(int px, int *dynamic) {
    char tag[32];
    snprintf(tag, sizeof(tag), "UI_FONT_%d,", px);
    const char *p = s_src;
    *dynamic = 0;
    while ((p = strstr(p, tag)) != NULL) {
        const char *q = p;
        while (q > s_src && (q[-1] == ' ' || q[-1] == ',' || q[-1] == '\n'))
            q--;
        if (q > s_src && q[-1] == '"') {
            const char *close = q - 1;
            const char *open = close - 1;
            while (open > s_src && !(*open == '"' && open[-1] != '\\'))
                open--;
            for (const char *c = open + 1; c < close; c++) {
                if (*c == '\\') {
                    c++; /* an escape: \n is a line break, drawn as nothing */
                    continue;
                }
                if (!in_list(range_of(px), (uint32_t)(unsigned char)*c))
                    printf("%d px label \"%.*s\": '%c' not in UI_RANGE_%d\n", px, (int)(close - open - 1), open + 1, *c,
                           px);
                TEST_ASSERT_TRUE_MESSAGE(in_list(range_of(px), (uint32_t)(unsigned char)*c),
                                         "a 28/48 label draws a character outside its range");
            }
        } else {
            (*dynamic)++;
        }
        p += strlen(tag);
    }
}

void test_the_28_and_48_labels_stay_inside_their_ranges(void) {
    read_file(SRC_DIR "/main/display_screens.c", s_src, sizeof(s_src));
    int dyn28, dyn48;
    scan_labels(28, &dyn28);
    scan_labels(48, &dyn48);
    /* One non-literal label each: the remaining-time digits at 28 and the
       break countdown at 48, both "0-9" and ':' from snprintf. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dyn28, "a 28 px label with computed text: audit its characters");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, dyn48, "a 48 px label with computed text: audit its characters");
    for (uint32_t c = '0'; c <= '9'; c++) {
        TEST_ASSERT_TRUE(in_list(UI_RANGE_28, c));
        TEST_ASSERT_TRUE(in_list(UI_RANGE_48, c));
    }
    TEST_ASSERT_TRUE(in_list(UI_RANGE_28, ':'));
    TEST_ASSERT_TRUE(in_list(UI_RANGE_48, ':'));
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    lv_init();
    UNITY_BEGIN();
    RUN_TEST(test_every_font_is_1bpp_and_carries_its_ranges);
    RUN_TEST(test_lvgl_default_font_is_the_12_px_ui_font);
    RUN_TEST(test_the_subset_sizes_carry_nothing_else);
    RUN_TEST(test_each_generated_file_was_made_from_the_current_ranges);
    RUN_TEST(test_the_text_sizes_cover_printable_ascii);
    RUN_TEST(test_the_symbol_set_covers_every_symbol_the_screens_use);
    RUN_TEST(test_every_symbol_in_display_screens_is_in_the_table);
    RUN_TEST(test_the_28_and_48_labels_stay_inside_their_ranges);
    return UNITY_END();
}
