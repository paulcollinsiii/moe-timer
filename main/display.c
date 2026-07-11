#include "display.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display_screens.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "ssd1680.h"

static const char *TAG = "display";

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
static bool s_panel_slept; /* panel in deep sleep — must re-init before next flush */
static ssd1680_refresh_mode_t s_pending_mode = SSD1680_REFRESH_FULL;
/* Partial/full cadence counter (policy, distinct from the driver's
   protection guard). Display-owned RTC state — the cadence survives deep
   sleep without living in the timer module's rtc_state_t. */
static RTC_DATA_ATTR uint8_t s_partial_count;

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

    /* Every flush ends in panel deep sleep (mode 1, registers lost). A
       second render in the same awake period — final-minute TIME'S UP,
       post-alert main screen — must wake and re-init the panel first, or
       it silently writes to a sleeping controller. */
    if (s_panel_slept) {
        esp_err_t ret = ssd1680_init(&PINS);
        if (ret != ESP_OK) {
            /* BUSY can straggle coming out of panel deep sleep — retry
               once rather than silently dropping the frame (a dropped
               frame leaves e.g. the big TIME'S UP screen stuck). */
            vTaskDelay(pdMS_TO_TICKS(100));
            ret = ssd1680_init(&PINS);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "panel re-init failed - dropping frame");
            lv_display_flush_ready(disp);
            return;
        }
        s_panel_slept = false;
    }

    /* Two renders in one wake (countdown step → pause/break/alert) can
       land inside the driver's minimum refresh interval, and the guard
       silently DROPS the frame — the panel would keep the stale screen
       (field-observed with the mid-watch break). Absorb the remainder
       here so every accepted flush actually reaches glass. */
    int32_t guard_wait = ssd1680_refresh_wait();
    if (guard_wait > 0) {
        vTaskDelay(pdMS_TO_TICKS(guard_wait * 1000 + 100)); /* +margin: guard is second-granular */
    }

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
    s_panel_slept = true;
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

static void render(ssd1680_refresh_mode_t mode) {
    s_pending_mode = mode;
    lv_refr_now(s_disp); /* renders + calls flush_cb synchronously */
}

static void build_for_state(const display_state_t *st) {
    if (st->timer_state == TIMER_BREAK) {
        display_screens_build_break(st);
    } else {
        display_screens_build_main(st);
    }
}

void display_update(const display_state_t *st) {
    if (!s_initialized)
        display_init();
    build_for_state(st);
    /* Policy: full refresh every Nth partial (anti-ghosting). The counter
       lives in RTC memory so the cadence survives deep sleep. */
    s_partial_count++;
    if (s_partial_count >= FULL_REFRESH_EVERY_N) {
        s_partial_count = 0;
        render(SSD1680_REFRESH_FULL);
    } else {
        render(SSD1680_REFRESH_PARTIAL);
    }
}

void display_full_refresh(const display_state_t *st) {
    if (!s_initialized)
        display_init();
    build_for_state(st);
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL);
}

void display_timesup(void) {
    if (!s_initialized)
        display_init();
    display_screens_build_timesup();
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL);
}

void display_charge_me(void) {
    if (!s_initialized)
        display_init();
    display_screens_build_charge_me();
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL);
}

void display_sync_failed(void) {
    if (!s_initialized)
        display_init();
    display_screens_build_sync_failed();
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL);
}
