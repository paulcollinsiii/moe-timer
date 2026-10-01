# Stream 3a — Display Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `display.cpp` — LovyanGFX SSD1680 driver wired to MagTag GPIO, layout renderer for all four display states, and partial/full refresh strategy. The layout math helper functions (`display_bar_fill_px`, `display_format_remaining`) are tested on native; the full render path is validated by `pio run` (compile gate), not by flashing.

**Architecture:** `display.cpp` is the sole C++ translation unit. It instantiates a `LGFX` class configured for the MagTag SPI/GPIO pinout and delegates all drawing to LovyanGFX. The public C API (declared in `display.h` with `extern "C"`) is the only interface other modules use. Layout math functions are separated so they can be tested on native without any LovyanGFX dependency.

**Tech Stack:** C++17, LovyanGFX 1.1.x, SSD1680 e-paper panel, PlatformIO ESP-IDF, Unity (native layout tests)

**Prerequisite:** Stream 1 (`feature/test-harness`) merged to `integration`. LovyanGFX added to `platformio.ini` (done in Stream 0).

---

## Files

| Action | Path |
|---|---|
| Modify | `src/display.cpp` |
| Modify | `test/test_schedule/test_schedule.c` — add layout math tests (separate file below) |
| Create | `test/test_display/test_display.c` |

Note: `include/display.h` already has `display_bar_fill_px` and `display_format_remaining` declared (done in Stream 0).

---

## Task 1: Create branch

- [ ] **Step 1: Branch from integration**

```bash
git fetch origin
git checkout integration
git checkout -b feature/display
```

---

## Task 2: Verify MagTag SPI pin assignments

**This must be done before writing any LovyanGFX config.**

- [ ] **Step 1: Fetch the Adafruit MagTag schematic**

The MagTag 2.9" schematic is available at:
`https://learn.adafruit.com/adafruit-magtag/downloads`

Look for the ESP32-S2 SPI lines connected to the e-paper display. Confirm the following pin assignments match the schematic (these are the expected values from the CLAUDE.md hardware reference — verify each one):

| Signal | Expected GPIO | Confirmed? |
|--------|--------------|------------|
| EPD_MOSI (SPI DIN) | 35 | |
| EPD_SCK (SPI CLK) | 36 | |
| EPD_CS | 8 | |
| EPD_DC | 7 | |
| EPD_RST | 6 | |
| EPD_BUSY | 5 | |

If any pin differs from the schematic, use the schematic values and note the discrepancy in a code comment.

- [ ] **Step 2: Verify with a second source**

Cross-check with the Adafruit CircuitPython MagTag board definition:
`https://github.com/adafruit/circuitpython/blob/main/ports/espressif/boards/adafruit_magtag_2.9_grayscale/`

Look at `board.c` or the pin definitions file for `EPD_*` constants.

Record confirmed values before proceeding.

---

## Task 3: Write native layout math tests

**Files:** Create `test/test_display/test_display.c`

These tests run on native and validate the pure-math helpers in `display.cpp`. No LovyanGFX dependency.

- [ ] **Step 1: Create the test file**

```c
#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Pull in only the two testable math functions.
   display.cpp is C++; we test the C implementations here directly. */
extern uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec);
extern void     display_format_remaining(char *buf, size_t len, int32_t remaining_sec);

/* Provide stub implementations for this TU (avoids linking LovyanGFX) */
uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec)
{
    if (allocation_sec == 0 || remaining_sec <= 0) return 0;
    if ((uint32_t)remaining_sec >= allocation_sec) return 280;
    return (uint16_t)((uint32_t)remaining_sec * 280u / allocation_sec);
}

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec)
{
    if (remaining_sec <= 0) { snprintf(buf, len, "0 min"); return; }
    if (remaining_sec < 300) {
        snprintf(buf, len, "%ld min %ld sec",
                 (long)(remaining_sec / 60), (long)(remaining_sec % 60));
    } else {
        snprintf(buf, len, "%ld min", (long)(remaining_sec / 60));
    }
}

void setUp(void)  {}
void tearDown(void) {}

/* ---- display_bar_fill_px ---- */

void test_bar_full_when_remaining_equals_allocation(void)
{
    TEST_ASSERT_EQUAL_UINT16(280, display_bar_fill_px(3600, 3600));
}

void test_bar_full_when_remaining_exceeds_allocation(void)
{
    TEST_ASSERT_EQUAL_UINT16(280, display_bar_fill_px(4000, 3600));
}

void test_bar_zero_when_expired(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, display_bar_fill_px(0, 3600));
    TEST_ASSERT_EQUAL_UINT16(0, display_bar_fill_px(-1, 3600));
}

void test_bar_zero_when_allocation_zero(void)
{
    TEST_ASSERT_EQUAL_UINT16(0, display_bar_fill_px(60, 0));
}

void test_bar_half_fill(void)
{
    /* 1800 / 3600 = 50% → 140 px */
    TEST_ASSERT_EQUAL_UINT16(140, display_bar_fill_px(1800, 3600));
}

void test_bar_quarter_fill(void)
{
    /* 900 / 3600 = 25% → 70 px */
    TEST_ASSERT_EQUAL_UINT16(70, display_bar_fill_px(900, 3600));
}

void test_bar_just_expired(void)
{
    /* 1 second past expiry */
    TEST_ASSERT_EQUAL_UINT16(0, display_bar_fill_px(-1, 3600));
}

/* ---- display_format_remaining ---- */

void test_format_shows_minutes_only_above_5_min(void)
{
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 600); /* 10 min */
    TEST_ASSERT_EQUAL_STRING("10 min", buf);
}

void test_format_shows_minutes_only_at_exactly_5_min(void)
{
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 300); /* 5 min */
    TEST_ASSERT_EQUAL_STRING("5 min", buf);
}

void test_format_shows_minutes_and_seconds_below_5_min(void)
{
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 299); /* 4 min 59 sec */
    TEST_ASSERT_EQUAL_STRING("4 min 59 sec", buf);
}

void test_format_shows_zero_when_expired(void)
{
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_STRING("0 min", buf);
    display_format_remaining(buf, sizeof(buf), -1);
    TEST_ASSERT_EQUAL_STRING("0 min", buf);
}

void test_format_1_min_30_sec(void)
{
    char buf[64];
    display_format_remaining(buf, sizeof(buf), 90);
    TEST_ASSERT_EQUAL_STRING("1 min 30 sec", buf);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bar_full_when_remaining_equals_allocation);
    RUN_TEST(test_bar_full_when_remaining_exceeds_allocation);
    RUN_TEST(test_bar_zero_when_expired);
    RUN_TEST(test_bar_zero_when_allocation_zero);
    RUN_TEST(test_bar_half_fill);
    RUN_TEST(test_bar_quarter_fill);
    RUN_TEST(test_bar_just_expired);
    RUN_TEST(test_format_shows_minutes_only_above_5_min);
    RUN_TEST(test_format_shows_minutes_only_at_exactly_5_min);
    RUN_TEST(test_format_shows_minutes_and_seconds_below_5_min);
    RUN_TEST(test_format_shows_zero_when_expired);
    RUN_TEST(test_format_1_min_30_sec);
    return UNITY_END();
}
```

- [ ] **Step 2: Run native display tests**

```bash
pio test -e native -f test_display
```

Expected: all 12 tests PASS immediately (the test file provides its own stub implementations — no LovyanGFX needed).

- [ ] **Step 3: Commit**

```bash
git add test/test_display/test_display.c
git commit -m "test(display): add native layout math tests for bar fill and time format"
```

---

## Task 4: Implement LovyanGFX configuration

**Files:** Modify `src/display.cpp`

- [ ] **Step 1: Replace display.cpp with full LovyanGFX implementation**

Use the GPIO values confirmed in Task 2. The values below use the expected MagTag pinout — replace if the schematic differs.

```cpp
#include "display.h"
#include <LovyanGFX.hpp>
#include <string.h>
#include <stdio.h>

/* ---- LovyanGFX panel configuration ---- */

class LGFX_MagTag : public lgfx::LGFX_Device {
    lgfx::Panel_SSD1680 _panel;
    lgfx::Bus_SPI       _bus;

public:
    LGFX_MagTag(void)
    {
        /* SPI bus */
        {
            auto cfg        = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 4000000;   /* 4 MHz — conservative for e-paper */
            cfg.pin_sclk    = 36;        /* EPD_SCK  — verify against schematic */
            cfg.pin_mosi    = 35;        /* EPD_MOSI — verify against schematic */
            cfg.pin_miso    = -1;        /* e-paper is write-only */
            cfg.pin_dc      = 7;         /* EPD_DC   — verify against schematic */
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        /* Panel */
        {
            auto cfg        = _panel.config();
            cfg.pin_cs      = 8;         /* EPD_CS   — verify against schematic */
            cfg.pin_rst     = 6;         /* EPD_RST  — verify against schematic */
            cfg.pin_busy    = 5;         /* EPD_BUSY — verify against schematic */
            /* Native orientation: 128 wide × 296 tall (portrait).
               Rotation 1 = landscape 296 wide × 128 tall. */
            cfg.panel_width  = 128;
            cfg.panel_height = 296;
            _panel.config(cfg);
        }

        setPanel(&_panel);
    }
};

static LGFX_MagTag s_display;
static bool        s_initialized = false;
static uint8_t     s_partial_refresh_count = 0;

/* ---- Init ---- */

void display_init(void)
{
    s_display.init();
    s_display.setRotation(1);           /* landscape: 296 × 128 */
    s_display.setColorDepth(1);         /* 1-bit black/white */
    s_display.fillScreen(TFT_WHITE);
    s_display.display();                /* full refresh to clear ghosting */
    s_initialized = true;
    s_partial_refresh_count = 0;
}

/* ---- Layout helpers ---- */

uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec)
{
    if (allocation_sec == 0 || remaining_sec <= 0) return 0;
    if ((uint32_t)remaining_sec >= allocation_sec) return 280;
    return (uint16_t)((uint32_t)remaining_sec * 280u / allocation_sec);
}

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec)
{
    if (remaining_sec <= 0) { snprintf(buf, len, "0 min"); return; }
    if (remaining_sec < 300) {
        snprintf(buf, len, "%ld min %ld sec",
                 (long)(remaining_sec / 60), (long)(remaining_sec % 60));
    } else {
        snprintf(buf, len, "%ld min", (long)(remaining_sec / 60));
    }
}

static const char *day_type_str(day_type_t dt)
{
    switch (dt) {
    case DAY_WEEKEND: return "Weekend";
    case DAY_HOLIDAY: return "Holiday";
    default:          return "Weekday";
    }
}

static const char *state_str(timer_state_t ts)
{
    switch (ts) {
    case TIMER_RUNNING: return "RUNNING";
    case TIMER_PAUSED:  return "PAUSED";
    case TIMER_EXPIRED: return "TIME'S UP";
    default:            return "IDLE";
    }
}

/* ---- Render ---- */

static void render(const display_state_t *state)
{
    s_display.fillScreen(TFT_WHITE);
    s_display.setTextColor(TFT_BLACK);

    /* Row 0–18: date + time + last sync */
    {
        char date_buf[32], time_buf[16], sync_buf[24];
        struct tm tm_local;
        time_t wt = state->wall_time;
        localtime_r(&wt, &tm_local);
        strftime(date_buf, sizeof(date_buf), "%a %b %d", &tm_local);
        strftime(time_buf, sizeof(time_buf), "%I:%M %p", &tm_local);

        time_t st = state->last_sync_time;
        struct tm tm_sync;
        localtime_r(&st, &tm_sync);
        strftime(sync_buf + 12, sizeof(sync_buf) - 12, "%I:%M", &tm_sync);
        memcpy(sync_buf, "Last sync: ", 11);
        sync_buf[11] = ' ';

        s_display.setFont(&fonts::Font2);
        s_display.setCursor(0, 0);
        s_display.printf("%s  %s  %s", date_buf, time_buf, sync_buf);
    }

    /* Row 26–50: progress bar (280 px wide, 24 px tall) */
    {
        uint16_t fill = display_bar_fill_px(state->remaining_sec,
                                            state->allocation_sec);
        s_display.drawRect(0, 26, 282, 24, TFT_BLACK);   /* outer border (2 px) */
        s_display.drawRect(1, 27, 280, 22, TFT_BLACK);
        if (fill > 0) {
            s_display.fillRect(2, 28, fill, 20, TFT_BLACK);
        }
    }

    /* Row 58–78: remaining time, centred */
    {
        char rem_buf[32];
        display_format_remaining(rem_buf, sizeof(rem_buf), state->remaining_sec);
        s_display.setFont(&fonts::Font4);
        int16_t text_w = s_display.textWidth(rem_buf);
        s_display.setCursor((296 - text_w) / 2, 58);
        s_display.print(rem_buf);
    }

    /* Row 88–108: day-type + allocation (left), state (right) */
    {
        s_display.setFont(&fonts::Font2);
        uint16_t alloc_min = state->allocation_sec / 60;
        s_display.setCursor(0, 88);
        s_display.printf("%s · %u min", day_type_str(state->day_type), alloc_min);

        const char *st = state_str(state->timer_state);
        int16_t st_w = s_display.textWidth(st);
        s_display.setCursor(296 - st_w, 88);
        s_display.print(st);
    }
}

void display_update(const display_state_t *state)
{
    if (!s_initialized) display_init();
    render(state);
    s_partial_refresh_count++;
    if (s_partial_refresh_count >= 5) {
        s_display.display();       /* full refresh every 5th partial */
        s_partial_refresh_count = 0;
    } else {
        s_display.display(true);   /* partial refresh: true = partial */
    }
}

void display_full_refresh(const display_state_t *state)
{
    if (!s_initialized) display_init();
    render(state);
    s_display.display();           /* always full */
    s_partial_refresh_count = 0;
}

void display_timesup(void)
{
    if (!s_initialized) display_init();
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
```

---

## Task 5: Compile gate — verify pio run succeeds

- [ ] **Step 1: Run the magtag build**

```bash
pio run
```

Expected: `[SUCCESS]`.

**Troubleshooting:**

- `Panel_SSD1680` not found: check LovyanGFX version in `platformio.ini`. v1.1.x includes SSD1680 support. Run `pio pkg update` to get the latest.
- `SPI2_HOST` not defined: add `#include "driver/spi_common.h"` or `#include "hal/spi_types.h"`.
- `localtime_r` undefined: it's POSIX, available in ESP-IDF. Ensure `#include <time.h>` is present.
- `fonts::Font2` not found: these are LovyanGFX built-in fonts. Include `<lgfx/v1/fonts/lgfx_fonts.h>` if needed, or use `&fonts::FreeSans9pt7b` as an alternative.
- `display()` vs `display(true)`: verify the LovyanGFX SSD1680 panel's partial refresh API. In LovyanGFX 1.1.x, `display(true)` triggers a partial update on supported panels. If the API differs, check the LovyanGFX changelog and adjust.
- C API linkage: if linker complains about `display_init` with C++ mangling, verify `extern "C"` block in `display.h` wraps all declarations.

Do NOT proceed until `pio run` succeeds.

- [ ] **Step 2: Also run native display tests to confirm they still pass**

```bash
pio test -e native -f test_display
```

Expected: all 12 tests PASS.

- [ ] **Step 3: Commit**

```bash
git add src/display.cpp
git commit -m "feat(display): implement LovyanGFX SSD1680 driver and layout renderer"
```

---

## Task 6: Code review

- [ ] **Step 1: Run adversarial code review**

Use `superpowers:requesting-code-review` skill. Focus areas:
- SPI pin values: are they exactly what the schematic says, or still "expected"? If unverified, the review must block until verified.
- `display(true)` for partial refresh: does the SSD1680 panel in LovyanGFX actually support partial updates at this API level? Check if `setPartialMode()` or similar must be called first.
- `s_initialized` flag: thread safety is not a concern (single-threaded), but what happens if `display_update()` is called before `display_init()`? (It calls `display_init()` — correct.)
- Font references (`fonts::Font2`, `fonts::Font4`, `fonts::Font7`): are these available in LovyanGFX without additional font header includes?
- `strftime` format `%I:%M %p`: ESP-IDF's `newlib` strftime may not support `%p` (AM/PM) with all locales. Test if needed.
- `display_bar_fill_px`: integer overflow risk if `remaining_sec * 280` overflows uint32? Max remaining = 7200 s; 7200 * 280 = 2,016,000 — well within uint32 range. ✓
- No `malloc`/`free` — stack-only. ✓

Fix any issues before merging.

---

## Task 7: Merge to integration

- [ ] **Step 1: Push and merge**

```bash
git push -u origin feature/display
git checkout integration
git merge --no-ff feature/display -m "feat(display): LovyanGFX SSD1680 driver with layout renderer"
git push origin integration
```
