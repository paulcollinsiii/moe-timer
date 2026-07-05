#include "display.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ssd1680.h"

static const char *TAG = "display";

#define DISP_HOR 296
#define DISP_VER 128
#define FULL_REFRESH_EVERY_N 5

/* MagTag EPD pinout (Adafruit schematic) */
static const ssd1680_pins_t PINS = {
    .pin_sclk = 36, .pin_mosi = 35, .pin_cs = 8, .pin_dc = 7, .pin_rst = 6, .pin_busy = 5};

/* LVGL I1 draw buffer: 8-byte palette header + 1 bit per pixel */
static uint8_t s_lvbuf[8 + DISP_HOR * DISP_VER / 8];
static uint8_t s_panel_fb[SSD1680_FB_SIZE];
static uint8_t s_panel_clean[SSD1680_FB_SIZE]; /* inverse pass of the double partial */
/* Previous displayed frame, kept across deep sleep so the cleaning pass can
   be limited to the characters that actually changed. Zeroed (invalid) on
   power-on reset, like the panel's own previous-frame RAM. */
static RTC_DATA_ATTR uint8_t s_prev_fb[SSD1680_FB_SIZE];
static RTC_DATA_ATTR bool s_prev_fb_valid;
static lv_display_t *s_disp;
static bool s_initialized;
static ssd1680_refresh_mode_t s_pending_mode = SSD1680_REFRESH_FULL;

/* Bring-up knobs: if the image is rotated 180 deg or mirrored on hardware,
   flip these (see docs/hardware_smoke_test.md step 2). */
#define ROT_FLIP_X 0
#define ROT_FLIP_Y 1

static uint32_t tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Landscape row bands holding per-wake text (header date/time/sync,
   remaining-time text). Partial refreshes drive them inverse->true — a
   localized flash — so the text does not accumulate ghosting between the
   every-5th-wake full refreshes. Byte-aligned outward, so bands may clean
   up to 7 extra rows on each edge. */
static const struct {
    int y0, y1;
} CLEAN_BANDS[] = {
    {0, 22},  /* header row */
    {54, 96}, /* remaining-time text */
};

/* Invert band bytes only where fb differs from the previous frame; returns
   the number of dirty portrait rows (0 = nothing in the bands changed). */
static int invert_clean_bands(uint8_t *fb) {
    int dirty = 0;
    for (size_t i = 0; i < sizeof(CLEAN_BANDS) / sizeof(CLEAN_BANDS[0]); i++) {
        /* Landscape row y maps to panel x bit px (see flush_cb transpose) */
        int p0 = ROT_FLIP_X ? (DISP_VER - 1 - CLEAN_BANDS[i].y1) : CLEAN_BANDS[i].y0;
        int p1 = ROT_FLIP_X ? (DISP_VER - 1 - CLEAN_BANDS[i].y0) : CLEAN_BANDS[i].y1;
        dirty += display_fb_invert_dirty_rows(fb, s_prev_fb, SSD1680_HEIGHT, SSD1680_WIDTH / 8, p0 / 8, p1 / 8);
    }
    return dirty;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)area;                      /* RENDER_MODE_FULL: always the whole frame */
    const uint8_t *src = px_map + 8; /* skip I1 palette header */

    /* Transpose landscape 296x128 -> panel portrait 128x296. */
    memset(s_panel_fb, 0, sizeof(s_panel_fb));
    for (int y = 0; y < DISP_VER; y++) {
        for (int x = 0; x < DISP_HOR; x++) {
            int bit = (src[y * (DISP_HOR / 8) + x / 8] >> (7 - (x & 7))) & 1;
            /* LVGL I1: 1 = white. Panel fb convention: 1 = black. */
            if (!bit) {
                int px = ROT_FLIP_X ? (DISP_VER - 1 - y) : y;
                int py = ROT_FLIP_Y ? (DISP_HOR - 1 - x) : x;
                s_panel_fb[py * (SSD1680_WIDTH / 8) + px / 8] |= (uint8_t)(0x80 >> (px & 7));
            }
        }
    }

    /* Ghost-cleaning double partial: pass 1 inverts the changed characters
       inside the text bands, pass 2 restores the true frame, so those
       pixels are driven both ways. Skipped when nothing in the bands
       changed or when previous-frame state is invalid (driver would
       promote to full anyway). The 1.1 s delay satisfies the driver's 1 s
       refresh-rate guard. */
    if (s_pending_mode == SSD1680_REFRESH_PARTIAL && ssd1680_partial_diff_ready() && s_prev_fb_valid) {
        memcpy(s_panel_clean, s_panel_fb, sizeof(s_panel_clean));
        if (invert_clean_bands(s_panel_clean) > 0 && ssd1680_write_framebuffer(s_panel_clean) == ESP_OK &&
            ssd1680_refresh(SSD1680_REFRESH_PARTIAL) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1100));
        }
    }

    if (ssd1680_write_framebuffer(s_panel_fb) == ESP_OK) {
        if (ssd1680_refresh(s_pending_mode) == ESP_OK) { /* errors logged inside */
            memcpy(s_prev_fb, s_panel_fb, sizeof(s_prev_fb));
            s_prev_fb_valid = true;
        }
    }
    ssd1680_sleep();
    lv_display_flush_ready(disp);
}

void display_init(void) {
    if (ssd1680_init(&PINS) != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed - continuing headless");
    }
    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(DISP_HOR, DISP_VER);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(s_disp, s_lvbuf, NULL, sizeof(s_lvbuf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    s_initialized = true;
}

static const char *day_type_str(day_type_t dt) {
    switch (dt) {
        case DAY_WEEKEND:
            return "Weekend";
        case DAY_HOLIDAY:
            return "Holiday";
        default:
            return "Weekday";
    }
}

static const char *state_str(timer_state_t st) {
    switch (st) {
        case TIMER_RUNNING:
            return "RUNNING";
        case TIMER_PAUSED:
            return "PAUSED";
        case TIMER_EXPIRED:
            return "TIME'S UP";
        default:
            return "IDLE";
    }
}

static lv_obj_t *fresh_screen(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    return scr;
}

/* newlib's C locale renders strftime %p empty — format 12h time manually. */
static void format_time_12h(char *buf, size_t len, const struct tm *tm) {
    int h12 = tm->tm_hour % 12;
    if (h12 == 0)
        h12 = 12;
    snprintf(buf, len, "%d:%02d %s", h12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
}

static void style_bar(lv_obj_t *bar) {
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

static void build_screen(const display_state_t *st) {
    lv_obj_t *scr = fresh_screen();
    char buf[64];
    char time_buf[16];
    struct tm tm;

    /* Row 0-18: date + time (left), last sync (right) */
    localtime_r(&st->wall_time, &tm);
    format_time_12h(time_buf, sizeof(time_buf), &tm);
    char date_buf[24];
    strftime(date_buf, sizeof(date_buf), "%a %b %d", &tm);
    snprintf(buf, sizeof(buf), "%s  %s", date_buf, time_buf);
    lv_obj_t *hdr = lv_label_create(scr);
    lv_label_set_text(hdr, buf);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    /* At 12 pt the ':' hugs the preceding digit — open it up slightly */
    lv_obj_set_style_text_letter_space(hdr, 1, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 4, 3);

    if (st->last_sync_time > 0) {
        struct tm ts;
        localtime_r(&st->last_sync_time, &ts);
        format_time_12h(time_buf, sizeof(time_buf), &ts);
        snprintf(buf, sizeof(buf), "Last sync: %s", time_buf);
    } else {
        snprintf(buf, sizeof(buf), "Last sync: --:--");
    }
    lv_obj_t *sync = lv_label_create(scr);
    lv_label_set_text(sync, buf);
    lv_obj_set_style_text_font(sync, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(sync, 1, 0);
    lv_obj_align(sync, LV_ALIGN_TOP_RIGHT, -4, 3);

    /* Row 26-50: progress bar, 284x24 with 2 px border */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 284, 24);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 26);
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, display_bar_fill_px(st->remaining_sec, st->allocation_sec), LV_ANIM_OFF);
    style_bar(bar);

    /* Row 58-86: remaining time, centred */
    display_format_remaining(buf, sizeof(buf), st->remaining_sec);
    lv_obj_t *rem = lv_label_create(scr);
    lv_label_set_text(rem, buf);
    lv_obj_set_style_text_font(rem, &lv_font_montserrat_28, 0);
    lv_obj_align(rem, LV_ALIGN_TOP_MID, 0, 58);

    /* Bottom row: day-type + allocation (left), state (right) */
    snprintf(buf, sizeof(buf), "%s - %u min", day_type_str(st->day_type), (unsigned)(st->allocation_sec / 60));
    lv_obj_t *day = lv_label_create(scr);
    lv_label_set_text(day, buf);
    lv_obj_set_style_text_font(day, &lv_font_montserrat_12, 0);
    lv_obj_align(day, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    lv_obj_t *state = lv_label_create(scr);
    lv_label_set_text(state, state_str(st->timer_state));
    lv_obj_set_style_text_font(state, &lv_font_montserrat_12, 0);
    lv_obj_align(state, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
}

static void render(ssd1680_refresh_mode_t mode) {
    s_pending_mode = mode;
    lv_refr_now(s_disp); /* renders + calls flush_cb synchronously */
}

void display_update(const display_state_t *st) {
    if (!s_initialized)
        display_init();
    build_screen(st);
    /* Policy: full refresh every Nth partial (anti-ghosting). The counter
       lives in RTC memory so the cadence survives deep sleep. */
    g_rtc_state.partial_refresh_count++;
    if (g_rtc_state.partial_refresh_count >= FULL_REFRESH_EVERY_N) {
        g_rtc_state.partial_refresh_count = 0;
        render(SSD1680_REFRESH_FULL);
    } else {
        render(SSD1680_REFRESH_PARTIAL);
    }
}

void display_full_refresh(const display_state_t *st) {
    if (!s_initialized)
        display_init();
    build_screen(st);
    g_rtc_state.partial_refresh_count = 0;
    render(SSD1680_REFRESH_FULL);
}

void display_timesup(void) {
    if (!s_initialized)
        display_init();
    lv_obj_t *scr = fresh_screen();

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "TIME'S UP");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_48, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, -10);

    /* Empty bar underneath, per spec */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 284, 16);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    style_bar(bar);

    g_rtc_state.partial_refresh_count = 0;
    render(SSD1680_REFRESH_FULL);
}

void display_sync_failed(void) {
    if (!s_initialized)
        display_init();
    lv_obj_t *scr = fresh_screen();
    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "No sync - check WiFi");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
    g_rtc_state.partial_refresh_count = 0;
    render(SSD1680_REFRESH_FULL);
}
