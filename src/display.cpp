#include "display.h"

#include <stdio.h>
#include <string.h>

#include <LovyanGFX.hpp>

/* ---- LovyanGFX panel configuration ---- */

/* LovyanGFX 1.2.x does not include Panel_SSD1680. Panel_GDEW0154D67 is used
   as a structural stand-in for the SPI e-paper interface; replace with a
   dedicated SSD1680 panel driver once one is available. */
class LGFX_MagTag : public lgfx::LGFX_Device {
    lgfx::Panel_GDEW0154D67 _panel;
    lgfx::Bus_SPI _bus;

   public:
    LGFX_MagTag(void) {
        /* SPI bus */
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 4000000;
            cfg.pin_sclk = 36;
            cfg.pin_mosi = 35;
            cfg.pin_miso = -1;
            cfg.pin_dc = 7;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        /* Panel */
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 8;
            cfg.pin_rst = 6;
            cfg.pin_busy = 5;
            cfg.panel_width = 128;
            cfg.panel_height = 296;
            _panel.config(cfg);
        }

        setPanel(&_panel);
    }
};

static LGFX_MagTag s_display;
static bool s_initialized = false;
static uint8_t s_partial_refresh_count = 0;

/* ---- Init ---- */

void display_init(void) {
    s_display.init();
    s_display.setRotation(1);
    s_display.setColorDepth(1);
    s_display.fillScreen(TFT_WHITE);
    s_display.display();
    s_initialized = true;
    s_partial_refresh_count = 0;
}

/* ---- Layout helpers ---- */

uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec) {
    if (allocation_sec == 0 || remaining_sec <= 0)
        return 0;
    if ((uint32_t)remaining_sec >= allocation_sec)
        return 280;
    return (uint16_t)((uint32_t)remaining_sec * 280u / allocation_sec);
}

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec) {
    if (remaining_sec <= 0) {
        snprintf(buf, len, "0 min");
        return;
    }
    if (remaining_sec < 300) {
        snprintf(buf, len, "%ld min %ld sec", (long)(remaining_sec / 60), (long)(remaining_sec % 60));
    } else {
        snprintf(buf, len, "%ld min", (long)(remaining_sec / 60));
    }
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

static const char *state_str(timer_state_t ts) {
    switch (ts) {
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

/* ---- Render ---- */

static void render(const display_state_t *state) {
    s_display.fillScreen(TFT_WHITE);
    s_display.setTextColor(TFT_BLACK);

    /* Row 0-18: date + time + last sync */
    {
        char date_buf[32], time_buf[16], sync_buf[32];
        struct tm tm_local;
        time_t wt = state->wall_time;
        localtime_r(&wt, &tm_local);
        strftime(date_buf, sizeof(date_buf), "%a %b %d", &tm_local);
        strftime(time_buf, sizeof(time_buf), "%H:%M", &tm_local);

        time_t st = state->last_sync_time;
        struct tm tm_sync;
        localtime_r(&st, &tm_sync);
        char sync_time[8];
        strftime(sync_time, sizeof(sync_time), "%H:%M", &tm_sync);
        snprintf(sync_buf, sizeof(sync_buf), "sync %s", sync_time);

        s_display.setFont(&fonts::Font2);
        s_display.setCursor(0, 0);
        s_display.printf("%s  %s  %s", date_buf, time_buf, sync_buf);
    }

    /* Row 26-50: progress bar */
    {
        uint16_t fill = display_bar_fill_px(state->remaining_sec, state->allocation_sec);
        s_display.drawRect(0, 26, 282, 24, TFT_BLACK);
        s_display.drawRect(1, 27, 280, 22, TFT_BLACK);
        if (fill > 0) {
            s_display.fillRect(2, 28, fill, 20, TFT_BLACK);
        }
    }

    /* Row 58-78: remaining time, centred */
    {
        char rem_buf[32];
        display_format_remaining(rem_buf, sizeof(rem_buf), state->remaining_sec);
        s_display.setFont(&fonts::Font4);
        int16_t text_w = s_display.textWidth(rem_buf);
        s_display.setCursor((296 - text_w) / 2, 58);
        s_display.print(rem_buf);
    }

    /* Row 88-108: day-type + allocation (left), state (right) */
    {
        s_display.setFont(&fonts::Font2);
        uint16_t alloc_min = state->allocation_sec / 60;
        s_display.setCursor(0, 88);
        s_display.printf("%s - %u min", day_type_str(state->day_type), alloc_min);

        const char *st = state_str(state->timer_state);
        int16_t st_w = s_display.textWidth(st);
        s_display.setCursor(296 - st_w, 88);
        s_display.print(st);
    }
}

void display_update(const display_state_t *state) {
    if (!s_initialized)
        display_init();
    render(state);
    s_partial_refresh_count++;
    if (s_partial_refresh_count >= 5) {
        s_display.display();
        s_partial_refresh_count = 0;
    } else {
        s_display.display();
    }
}

void display_full_refresh(const display_state_t *state) {
    if (!s_initialized)
        display_init();
    render(state);
    s_display.display();
    s_partial_refresh_count = 0;
}

void display_timesup(void) {
    if (!s_initialized)
        display_init();
    s_display.fillScreen(TFT_WHITE);
    s_display.setTextColor(TFT_BLACK);
    s_display.setFont(&fonts::Font7);
    const char *msg = "TIME'S UP";
    int16_t w = s_display.textWidth(msg);
    s_display.setCursor((296 - w) / 2, 40);
    s_display.print(msg);
    s_display.display();
    s_partial_refresh_count = 0;
}
