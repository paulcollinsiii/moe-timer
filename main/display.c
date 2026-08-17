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

/* "The panel is showing a full-screen takeover." Set by display_ota(),
   consumed by the next display_update().

   Zeroing s_partial_count is NOT enough on its own: the next
   display_update() increments it to 1, and 1 < FULL_REFRESH_EVERY_N, so
   the paint that lands on top of a 28 pt full-panel headline is a
   PARTIAL — the worst case for ghosting. lock_gate_promote_render()
   solves exactly this for the lock screens, one layer up; this is the
   same idea expressed where the takeover is painted, so no caller has to
   remember.

   WHAT THIS FLAG ACTUALLY COVERS: the repaint that follows a takeover
   WITHIN THE SAME WAKE. That is ota_flow's failure-path repaint (the
   update screen goes up, the download fails, wake_flow repaints the
   normal layout) and the equivalent after a lock screen. RTC_DATA_ATTR
   so it also survives DEEP SLEEP, which is a real case: the awake
   failsafe can sleep the device with the update screen still on the
   glass, and the next wake's first paint has to be full.

   WHAT IT DOES NOT COVER, corrected from an earlier claim here that it
   did: esp_restart(). RTC_DATA_ATTR does not survive a software reset on
   the ESP32-S2. The bootloader loads the .rtc.data segment on every reset
   EXCEPT a deep-sleep wake (esp_image_format.c: `load_rtc_memory =
   esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP`), so after
   the OTA reboot this flag comes back as whatever the new image's
   initialiser says — false.

   The post-OTA first paint is full anyway, and it is worth knowing by
   WHICH mechanism, because it is not this one. ssd1680.c's
   s_prev_frame_valid is RTC_DATA_ATTR too and is wiped by the same
   reboot, so ssd1680_resolve_refresh_mode() promotes the requested
   PARTIAL to FULL and logs "partial promoted to full: no valid previous
   frame this power cycle". The driver guard is doing the work. Nobody
   should "simplify it away" on the strength of the flag above, and no
   later task needs to add code for the post-reboot case — it is already
   correct, for this reason rather than for the one previously written
   down here. */
static RTC_DATA_ATTR bool s_takeover_on_panel;

/* Bring-up knobs: if the image is rotated 180 deg or mirrored on hardware,
   flip these (see docs/hardware_smoke_test.md step 2). */
#define ROT_FLIP_X 0
#define ROT_FLIP_Y 1

static uint32_t tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Landscape row bands holding per-wake content. Partial refreshes drive
   them inverse->true — a localized flash — so the content does not
   accumulate ghosting between the every-5th-wake full refreshes. Extents
   are the exact widget geometry from display_screens.c (align y + font
   line_height / bar height); byte alignment then widens each edge outward
   by up to 7 rows — that slop is the packed-framebuffer format, not
   margin. Bands must not share a framebuffer byte (y/8): a shared byte
   would be inverted twice and cancel out.

   NOTE what this table does NOT constrain. After byte alignment the four
   bands are 0..2, 3..6, 7..10 and 11..15 of a 16-byte row — contiguous,
   and jointly every byte of every row. So no widget can land outside a
   band, and "does this new label need a band?" is never the question for
   a widget on the main screen; the answer is always no. The rule the
   comment above states is a constraint on the BAND EXTENTS themselves
   (keep them from sharing a byte with each other), which is why adding a
   fifth band or moving a row boundary is the change that needs care. A
   screen that only ever full-refreshes needs no entry either, for the
   different reason that this table is read only on the partial path. */
static const struct {
    int y0, y1; /* inclusive landscape rows */
} CLEAN_BANDS[] = {
    {3, 17},   /* header: date/time + last sync (12 pt at y=3) */
    {26, 49},  /* progress bar (y=26, h=24) + Charge Me!!! badge */
    {58, 87},  /* remaining time (28 pt at y=58) + battery % (12 pt at y=66) */
    {95, 125}, /* mode/state row (12 pt, bottom -18) + button row (bottom -2);
                  one band — their byte ranges would otherwise overlap */
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
       promote to full anyway). The intermediate pass doesn't re-arm the
       refresh-rate guard (both passes are one render), so pass 2 starts
       the moment BUSY releases — no fixed inter-pass delay. */
    if (s_pending_mode == SSD1680_REFRESH_PARTIAL && ssd1680_partial_diff_ready() && s_prev_fb_valid) {
        memcpy(s_panel_clean, s_panel_fb, sizeof(s_panel_clean));
        if (invert_clean_bands(s_panel_clean) > 0 && ssd1680_write_framebuffer(s_panel_clean) == ESP_OK) {
            ssd1680_refresh_intermediate(SSD1680_REFRESH_PARTIAL);
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
    /* A takeover screen is still on the glass — from earlier in this
       wake, or from before a deep sleep. NOT from before a reboot: the
       flag does not survive esp_restart (see its declaration), and the
       driver's own previous-frame guard is what covers that case. Promote
       this paint to a full refresh and clear the flag. */
    if (s_takeover_on_panel) {
        s_takeover_on_panel = false;
        s_partial_count = 0;
        render(SSD1680_REFRESH_FULL);
        return;
    }
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
    s_takeover_on_panel = false; /* this paint is already the full one */
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

void display_bedtime(void) {
    if (!s_initialized)
        display_init();
    display_screens_build_bedtime();
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL);
}

/* Painted between the OTA check window and the download window, with the
   radio down: net_window.c documents that a panel refresh coinciding with
   a WiFi TX burst browns out the rail. Full refresh only, so it needs no
   CLEAN_BANDS entry (that table is read only on the partial path). */
void display_ota(const char *from_version, const char *to_version) {
    if (!s_initialized)
        display_init();
    display_screens_build_ota(from_version, to_version);
    /* Whatever paints next — the failure-path repaint, or the first paint
       of the new firmware after the reboot — must be full, not a partial
       over a full-panel 28 pt headline. See s_takeover_on_panel. */
    s_takeover_on_panel = true;
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL);
}
