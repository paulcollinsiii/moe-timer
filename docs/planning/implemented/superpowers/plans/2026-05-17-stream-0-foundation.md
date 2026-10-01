# Stream 0 — Foundation & Housekeeping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix housekeeping issues (.gitignore, pre-commit, CLAUDE.md), add LovyanGFX dependency, scaffold all source stubs, and verify the project compiles clean against the ESP-IDF toolchain.

**Architecture:** All changes are configuration and scaffolding — no logic. The compile gate (`pio run`) proves stub function signatures are correct before any stream writes real code. This stream creates the `integration` branch that all subsequent streams target.

**Tech Stack:** PlatformIO + ESP-IDF, LovyanGFX, pre-commit, clang-format

---

## Files

| Action | Path |
|---|---|
| Modify | `.gitignore` |
| Modify | `.pre-commit-config.yaml` |
| Modify | `.claude/CLAUDE.md` |
| Modify | `docs/ProductOverview.md` |
| Modify | `platformio.ini` |
| Modify | `src/CMakeLists.txt` |
| Create | `include/timer.h` |
| Create | `include/schedule.h` |
| Create | `include/nvs_config.h` |
| Create | `include/nvs_defaults.h` |
| Create | `include/display.h` |
| Create | `include/ntp.h` |
| Create | `include/buttons.h` |
| Create | `include/audio.h` |
| Create | `include/neopixel.h` |
| Create | `include/hal_time.h` |
| Create | `include/hal_nvs.h` |
| Create | `src/main.c` |
| Create | `src/timer.c` |
| Create | `src/schedule.c` |
| Create | `src/nvs_config.c` |
| Create | `src/display.cpp` |
| Create | `src/ntp.c` |
| Create | `src/buttons.c` |
| Create | `src/audio.c` |
| Create | `src/neopixel.c` |
| Create | `src/hal_time.c` |
| Create | `src/hal_nvs.c` |
| Delete | `components/ssd1680/.gitkeep` (entire directory) |
| Delete | `src/.gitkeep` |
| Delete | `include/.gitkeep` |

---

## Task 1: Create branch and integration branch

**Files:** none

- [ ] **Step 1: Create the integration branch from main**

```bash
git checkout main
git checkout -b integration
git push -u origin integration
```

- [ ] **Step 2: Create the feature branch**

```bash
git checkout main
git checkout -b feature/foundation
```

---

## Task 2: Fix .gitignore

**Files:** Modify `.gitignore`

- [ ] **Step 1: Add build/ to .gitignore**

Open `.gitignore`. Current content:
```
.claude/
.pio/
.vscode/
sdkconfig
```

Replace with:
```
.claude/
.pio/
.vscode/
sdkconfig
build/
```

- [ ] **Step 2: Commit**

```bash
git add .gitignore
git commit -m "chore: add build/ to .gitignore"
```

Expected: commit succeeds, pre-commit hooks pass.

---

## Task 3: Fix pre-commit clang-format hook

**Files:** Modify `.pre-commit-config.yaml`

- [ ] **Step 1: Add exclude pattern to clang-format hook**

In `.pre-commit-config.yaml`, find the `clang-format` hook entry:
```yaml
      - id: clang-format
        name: clang-format (C/H files)
        language: system
        entry: clang-format --dry-run --Werror
        files: \.(c|h)$
```

Add `exclude` so it matches the cppcheck hook:
```yaml
      - id: clang-format
        name: clang-format (C/H files)
        language: system
        entry: clang-format --dry-run --Werror
        files: \.(c|h)$
        exclude: ^(build/|\.pio/)
```

- [ ] **Step 2: Commit**

```bash
git add .pre-commit-config.yaml
git commit -m "chore: exclude build/ and .pio/ from clang-format hook"
```

---

## Task 4: Trim CLAUDE.md

**Files:** Modify `.claude/CLAUDE.md`

Note: `.claude/` is gitignored — this change is local only and does not commit.

- [ ] **Step 1: Replace CLAUDE.md with trimmed version**

Replace the full content of `.claude/CLAUDE.md` with:

```markdown
# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See `docs/developer_setup.md` for build commands, project layout, and container setup.

## Process Rules

- Planning must always use the superpowers skill (`superpowers:writing-plans`)
- TDD is required — write unit tests before implementing modules
- All development on feature branches via git worktrees; never commit directly to `main`

## Pre-Commit Hooks

Run once after cloning (inside the devcontainer):

```bash
pre-commit install
pre-commit install --hook-type commit-msg
```

Both commands are required — the second activates the commit-msg stage for the Conventional Commits validator.

**Commit message format** (enforced by hook):
```
<type>[(<scope>)][!]: <description>
```
Valid types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `perf`, `style`, `ci`, `build`, `revert`

**C formatting**: `.c`/`.h` files must pass `clang-format --dry-run --Werror`. To auto-fix:
```bash
clang-format -i <file.c>
```

## Hardware Target

| Attribute | Value |
|-----------|-------|
| Board | Adafruit MagTag 2.9" (2025) |
| PlatformIO ID | `adafruit_magtag29_esp32s2` |
| MCU | ESP32-S2 @ 240 MHz |
| Display | 2.9" grayscale e-ink 296×128 px, SSD1680 controller |
| Buttons | 4× tactile — GPIO 15/12/14/11 (active-LOW, internal pull-up) |
| NeoPixels | 4× RGB on GPIO 1 (power gate: GPIO 21 LOW = on) |
| Speaker | Amplifier shutdown pin GPIO 16 (HIGH = on) |
| No dedicated RTC chip | ESP32-S2 RTC timer used for deep-sleep wakeup |

## Module Structure (`src/`)

| File | Responsibility |
|------|---------------|
| `main.c` | `app_main`: read `esp_sleep_get_wakeup_cause()`, dispatch to button or timer-tick handler |
| `timer.c/h` | State machine (IDLE/RUNNING/PAUSED/EXPIRED); expiry time calc; RTC memory persistence |
| `display.cpp/h` | LovyanGFX SSD1680 driver; layout rendering; partial vs full refresh |
| `ntp.c/h` | WiFi init/deinit; SNTP sync via esp_sntp |
| `nvs_config.c/h` | Typed NVS accessors; first-boot defaults |
| `schedule.c/h` | Daily time-block lookup for screen-time allocation |
| `buttons.c/h` | Wake reason decode; GPIO wakeup config; 10 ms software debounce |
| `audio.c/h` | LEDC PWM tone generation; beep pattern sequencer |
| `neopixel.c/h` | RMT-based NeoPixel driver; alert pulse pattern |
| `hal_time.c/h` | HAL shim: wraps time() for testability |
| `hal_nvs.c/h` | HAL shim: wraps NVS API for testability |
| `nvs_defaults.h` | Compile-time defaults (holiday list, allocations, WiFi placeholder) |

## RTC Persistent State

Declared with `RTC_DATA_ATTR` so values survive deep sleep:

```c
typedef struct {
    timer_state_t state;
    int64_t       expiry_wall_time;   // Unix timestamp; 0 if unset. Set ONLY at IDLE→RUNNING and PAUSED→RUNNING.
    int32_t       remaining_at_pause; // seconds saved on PAUSE
    int32_t       allocation_sec;
    char          last_date[11];      // "YYYY-MM-DD"
    int64_t       next_ntp_sync;      // timestamp of next required sync
    uint8_t       partial_refresh_count;
} rtc_state_t;
```

## Key Implementation Constraints

- **Display library**: LovyanGFX (ESP-IDF native). `display.cpp` is the sole C++ translation unit. `display.h` is a C-compatible header (`extern "C"` guards).
- **SNTP**: use `esp_sntp` with `CONFIG_SNTP_TIME_SYNC_METHOD_IMMED`. `ntp_sync()` NEVER modifies `expiry_wall_time` — it corrects `time(NULL)` and the subtraction self-corrects.
- **Deep sleep wakeup**: `esp_sleep_enable_timer_wakeup(55 * 1000000ULL)` + `esp_sleep_enable_gpio_wakeup()` for all 4 buttons.
- **WiFi lifecycle**: init → connect → sync → disconnect → deinit on every NTP session.
- **NeoPixel power gate**: GPIO 21 must be HIGH (off) on every boot/wake before any other peripheral code runs.

## TDD Priority Modules

Write tests first for (use `pio test -e native`):
- `schedule.c` — day-type logic
- `timer.c` — state machine transitions and expiry math
- `nvs_config.c` — serialisation round-trips
```

---

## Task 5: Update ProductOverview.md

**Files:** Modify `docs/ProductOverview.md`

- [ ] **Step 1: Update Out-of-Scope section**

Find this line in the Out-of-Scope section:
```
- Custom font rendering beyond a small bitmap font header
```
Delete it. LovyanGFX provides font rendering; this is no longer a constraint.

- [ ] **Step 2: Add library note to Implementation Notes**

In the Implementation Notes section, replace:
```
3. **SSD1680 driver**: adapt an existing ESP-IDF-compatible driver (e.g. Waveshare ESP32 e-paper examples) to MagTag GPIO assignments — do not write from scratch.
```
with:
```
3. **Display library**: LovyanGFX (ESP-IDF native). `display.cpp` is the single C++ translation unit; all other modules are C. `display.h` exposes a C-compatible API with `extern "C"` guards. LovyanGFX handles SSD1680 init, partial/full refresh, and font rendering.
```

- [ ] **Step 3: Commit**

```bash
git add docs/ProductOverview.md
git commit -m "docs: update ProductOverview for LovyanGFX display library"
```

---

## Task 6: Update platformio.ini

**Files:** Modify `platformio.ini`

- [ ] **Step 1: Add LovyanGFX dependency**

Replace the full content of `platformio.ini` with:

```ini
[platformio]
default_envs = magtag

[env:magtag]
platform = espressif32
board = adafruit_magtag29_esp32s2
framework = espidf
monitor_speed = 115200
lib_deps =
    lovyan03/LovyanGFX @ ^1.1.16
```

- [ ] **Step 2: Commit**

```bash
git add platformio.ini
git commit -m "build: add LovyanGFX display library dependency"
```

---

## Task 7: Update src/CMakeLists.txt

**Files:** Modify `src/CMakeLists.txt`

- [ ] **Step 1: Replace the auto-generated CMakeLists with an explicit one**

Replace the full content of `src/CMakeLists.txt` with:

```cmake
idf_component_register(
    SRCS
        "main.c"
        "timer.c"
        "display.cpp"
        "ntp.c"
        "nvs_config.c"
        "schedule.c"
        "buttons.c"
        "audio.c"
        "neopixel.c"
        "hal_time.c"
        "hal_nvs.c"
    INCLUDE_DIRS
        "."
        "../include"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_timer
        driver
        log
        esp-tls
        json
)
```

Note: `esp_sntp` is part of `lwip` component in ESP-IDF v5; if the build fails with "component not found", replace `esp_sntp` reference with `lwip`. The `esp_sntp` header is at `esp_sntp.h` and is pulled in via `lwip`.

- [ ] **Step 2: Commit**

```bash
git add src/CMakeLists.txt
git commit -m "build: explicit CMakeLists with all source files and include dirs"
```

---

## Task 8: Create HAL headers

**Files:** Create `include/hal_time.h`, `include/hal_nvs.h`

These are thin shim headers. Source files call these functions; the implementation differs between magtag (`src/hal_*.c`) and native tests (`test/mocks/mock_hal_*.c`).

- [ ] **Step 1: Create include/hal_time.h**

```c
#pragma once
#include <time.h>

/* Returns current Unix timestamp. Wraps time(NULL) in production;
   injectable via mock_time_set() in native tests. */
time_t hal_time_now(void);
```

- [ ] **Step 2: Create include/hal_nvs.h**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
/* esp_err_t: from esp_err.h in magtag build, from esp_compat.h in native. */

esp_err_t hal_nvs_read_u16(const char *key, uint16_t *out);
esp_err_t hal_nvs_write_u16(const char *key, uint16_t val);
esp_err_t hal_nvs_read_str(const char *key, char *buf, size_t len);
esp_err_t hal_nvs_write_str(const char *key, const char *val);
esp_err_t hal_nvs_read_blob(const char *key, void *buf, size_t *len);
esp_err_t hal_nvs_write_blob(const char *key, const void *buf, size_t len);
```

- [ ] **Step 3: Commit**

```bash
git add include/hal_time.h include/hal_nvs.h
git commit -m "feat(hal): add thin HAL shim headers for time and NVS"
```

---

## Task 9: Create public API headers

**Files:** Create all headers in `include/`

- [ ] **Step 1: Create include/timer.h**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

typedef enum {
    TIMER_IDLE = 0,
    TIMER_RUNNING,
    TIMER_PAUSED,
    TIMER_EXPIRED,
} timer_state_t;

#ifndef NATIVE
#include "esp_attr.h"
#endif

typedef struct {
    timer_state_t state;
    int64_t       expiry_wall_time;    /* Unix ts; 0 if unset */
    int32_t       remaining_at_pause;  /* seconds saved on PAUSE */
    int32_t       allocation_sec;
    char          last_date[11];       /* "YYYY-MM-DD\0" */
    int64_t       next_ntp_sync;
    uint8_t       partial_refresh_count;
} rtc_state_t;

#ifndef NATIVE
extern rtc_state_t RTC_DATA_ATTR g_rtc_state;
#else
extern rtc_state_t g_rtc_state;
#endif

timer_state_t timer_get_state(void);
void          timer_start(time_t now, int32_t allocation_sec);
void          timer_pause(time_t now);
void          timer_resume(time_t now);
int32_t       timer_tick(time_t now);   /* returns remaining seconds; negative = expired */
void          timer_reset(void);
bool          timer_is_new_day(time_t now);
void          timer_record_date(time_t now);
bool          timer_needs_ntp_sync(time_t now);
void          timer_record_ntp_sync(time_t now);
```

- [ ] **Step 2: Create include/schedule.h**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stddef.h>

typedef enum {
    DAY_WEEKDAY = 0,
    DAY_WEEKEND,
    DAY_HOLIDAY,
} day_type_t;

day_type_t schedule_get_day_type(time_t now);
uint32_t   schedule_get_allocation_sec(day_type_t day_type);
bool       schedule_is_holiday(const char *date_str, const char *blob, size_t blob_len);
```

- [ ] **Step 3: Create include/nvs_config.h**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
/* esp_err_t: from esp_err.h in magtag build, from esp_compat.h in native. */

esp_err_t nvs_config_init_defaults(void);

esp_err_t nvs_config_get_weekday_min(uint16_t *out);
esp_err_t nvs_config_set_weekday_min(uint16_t val);
esp_err_t nvs_config_get_weekend_min(uint16_t *out);
esp_err_t nvs_config_set_weekend_min(uint16_t val);
esp_err_t nvs_config_get_holiday_min(uint16_t *out);
esp_err_t nvs_config_set_holiday_min(uint16_t val);

esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_ssid(const char *ssid);
esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len);
esp_err_t nvs_config_set_wifi_pass(const char *pass);

esp_err_t nvs_config_get_holidays(char *buf, size_t *len);
esp_err_t nvs_config_set_holidays(const char *blob, size_t len);

esp_err_t nvs_config_get_display_contrast(uint8_t *out);
esp_err_t nvs_config_set_display_contrast(uint8_t val);
```

- [ ] **Step 4: Create include/nvs_defaults.h**

```c
#pragma once

#define NVS_DEFAULT_WEEKDAY_MIN  60
#define NVS_DEFAULT_WEEKEND_MIN  120
#define NVS_DEFAULT_HOLIDAY_MIN  120
#define NVS_DEFAULT_WIFI_SSID    ""
#define NVS_DEFAULT_WIFI_PASS    ""
#define NVS_DEFAULT_CONTRAST     1

/* US Federal Holidays 2026 — newline-separated YYYY-MM-DD */
#define NVS_DEFAULT_HOLIDAYS \
    "2026-01-01\n" \
    "2026-01-19\n" \
    "2026-02-16\n" \
    "2026-05-25\n" \
    "2026-06-19\n" \
    "2026-07-03\n" \
    "2026-07-04\n" \
    "2026-09-07\n" \
    "2026-10-12\n" \
    "2026-11-11\n" \
    "2026-11-26\n" \
    "2026-12-25\n"
```

- [ ] **Step 5: Create include/display.h**

```c
#pragma once
#include <stdint.h>
#include <time.h>
#include "timer.h"
#include "schedule.h"

typedef struct {
    int32_t       remaining_sec;
    uint32_t      allocation_sec;
    timer_state_t timer_state;
    day_type_t    day_type;
    time_t        wall_time;
    time_t        last_sync_time;
    uint8_t       contrast_level;  /* 0, 1, or 2 */
} display_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void display_init(void);
void display_update(const display_state_t *state);        /* partial refresh */
void display_full_refresh(const display_state_t *state);  /* full refresh */
void display_timesup(void);                               /* TIME'S UP full-screen layout */

/* Layout math helpers — pure C, testable on native */
uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec);
void     display_format_remaining(char *buf, size_t len, int32_t remaining_sec);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 6: Create include/ntp.h**

```c
#pragma once
/* esp_err_t: from esp_err.h in magtag build. */

/* Performs a full WiFi+SNTP sync cycle.
   On ESP_OK, time(NULL) reflects corrected UTC.
   Does NOT read or write expiry_wall_time. */
esp_err_t ntp_sync(void);
```

- [ ] **Step 7: Create include/buttons.h**

```c
#pragma once
#include <stdbool.h>

typedef enum {
    BTN_A = 0,  /* GPIO 15 — Start/Pause */
    BTN_B,      /* GPIO 12 — Reset */
    BTN_C,      /* GPIO 14 — Cycle contrast */
    BTN_D,      /* GPIO 11 — Force NTP sync */
    BTN_NONE,
} button_id_t;

void        buttons_init(void);
void        buttons_configure_wakeup(void);
button_id_t buttons_get_wakeup_button(void);
bool        buttons_is_pressed(button_id_t btn);
```

- [ ] **Step 8: Create include/audio.h**

```c
#pragma once

void audio_init(void);
void audio_beep_sequence(void);  /* 3 beeps × 5 cycles = 15 s alert */
void audio_stop(void);
```

- [ ] **Step 9: Create include/neopixel.h**

```c
#pragma once

/* Must be called on every boot/wake before any other peripheral code.
   Ensures GPIO 21 (power gate) is HIGH (off). */
void neopixel_init(void);
void neopixel_alert_start(void);  /* GPIO 21 LOW, slow red pulse */
void neopixel_stop(void);         /* stops RMT, GPIO 21 HIGH; safe if already off */
```

- [ ] **Step 10: Commit all headers**

```bash
git add include/
git commit -m "feat: add all public API headers and HAL shim declarations"
```

---

## Task 10: Create source stubs

**Files:** Create all stubs in `src/`

Each stub compiles to an empty-but-valid translation unit. Function bodies return 0 or do nothing.

- [ ] **Step 1: Create src/main.c**

```c
#include "esp_sleep.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "MagTag Screen Timer boot");
    /* stub: full implementation in Stream 4 */
}
```

- [ ] **Step 2: Create src/timer.c**

```c
#include "timer.h"
#include "hal_time.h"
#include <string.h>
#include <time.h>

#ifndef NATIVE
rtc_state_t RTC_DATA_ATTR g_rtc_state;
#else
rtc_state_t g_rtc_state;
#endif

timer_state_t timer_get_state(void)          { return g_rtc_state.state; }
void          timer_start(time_t now, int32_t allocation_sec) { (void)now; (void)allocation_sec; }
void          timer_pause(time_t now)         { (void)now; }
void          timer_resume(time_t now)        { (void)now; }
int32_t       timer_tick(time_t now)          { (void)now; return 0; }
void          timer_reset(void)               { memset(&g_rtc_state, 0, sizeof(g_rtc_state)); }
bool          timer_is_new_day(time_t now)    { (void)now; return false; }
void          timer_record_date(time_t now)   { (void)now; }
bool          timer_needs_ntp_sync(time_t now){ (void)now; return false; }
void          timer_record_ntp_sync(time_t now){ (void)now; }
```

- [ ] **Step 3: Create src/schedule.c**

```c
#include "schedule.h"
#include "hal_nvs.h"
#include "hal_time.h"
#include <string.h>
#include <time.h>

day_type_t schedule_get_day_type(time_t now)                              { (void)now; return DAY_WEEKDAY; }
uint32_t   schedule_get_allocation_sec(day_type_t day_type)               { (void)day_type; return 3600; }
bool       schedule_is_holiday(const char *d, const char *b, size_t len)  { (void)d; (void)b; (void)len; return false; }
```

- [ ] **Step 4: Create src/nvs_config.c**

```c
#include "nvs_config.h"
#include "hal_nvs.h"
#include "nvs_defaults.h"

esp_err_t nvs_config_init_defaults(void)                           { return 0; }
esp_err_t nvs_config_get_weekday_min(uint16_t *out)                { (void)out; return 0; }
esp_err_t nvs_config_set_weekday_min(uint16_t val)                 { (void)val; return 0; }
esp_err_t nvs_config_get_weekend_min(uint16_t *out)                { (void)out; return 0; }
esp_err_t nvs_config_set_weekend_min(uint16_t val)                 { (void)val; return 0; }
esp_err_t nvs_config_get_holiday_min(uint16_t *out)                { (void)out; return 0; }
esp_err_t nvs_config_set_holiday_min(uint16_t val)                 { (void)val; return 0; }
esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len)          { (void)buf; (void)len; return 0; }
esp_err_t nvs_config_set_wifi_ssid(const char *s)                  { (void)s; return 0; }
esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len)          { (void)buf; (void)len; return 0; }
esp_err_t nvs_config_set_wifi_pass(const char *s)                  { (void)s; return 0; }
esp_err_t nvs_config_get_holidays(char *buf, size_t *len)          { (void)buf; (void)len; return 0; }
esp_err_t nvs_config_set_holidays(const char *b, size_t l)         { (void)b; (void)l; return 0; }
esp_err_t nvs_config_get_display_contrast(uint8_t *out)            { (void)out; return 0; }
esp_err_t nvs_config_set_display_contrast(uint8_t val)             { (void)val; return 0; }
```

- [ ] **Step 5: Create src/display.cpp**

```cpp
#include "display.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

void display_init(void) {}
void display_update(const display_state_t *state) { (void)state; }
void display_full_refresh(const display_state_t *state) { (void)state; }
void display_timesup(void) {}

uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec)
{
    if (allocation_sec == 0 || remaining_sec <= 0) return 0;
    if ((uint32_t)remaining_sec >= allocation_sec) return 280;
    return (uint16_t)((uint32_t)remaining_sec * 280 / allocation_sec);
}

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec)
{
    if (remaining_sec <= 0) {
        snprintf(buf, len, "0 min");
        return;
    }
    if (remaining_sec < 300) {
        snprintf(buf, len, "%ld min %ld sec",
                 (long)(remaining_sec / 60), (long)(remaining_sec % 60));
    } else {
        snprintf(buf, len, "%ld min", (long)(remaining_sec / 60));
    }
}
```

- [ ] **Step 6: Create src/ntp.c**

```c
#include "ntp.h"

esp_err_t ntp_sync(void) { return 0; /* stub */ }
```

- [ ] **Step 7: Create src/buttons.c**

```c
#include "buttons.h"
#include "esp_sleep.h"

void        buttons_init(void)                    {}
void        buttons_configure_wakeup(void)        {}
button_id_t buttons_get_wakeup_button(void)       { return BTN_NONE; }
bool        buttons_is_pressed(button_id_t btn)   { (void)btn; return false; }
```

- [ ] **Step 8: Create src/audio.c**

```c
#include "audio.h"

void audio_init(void)           {}
void audio_beep_sequence(void)  {}
void audio_stop(void)           {}
```

- [ ] **Step 9: Create src/neopixel.c**

```c
#include "neopixel.h"
#include "driver/gpio.h"

#define NEOPIXEL_POWER_GPIO 21

void neopixel_init(void)
{
    gpio_set_direction(NEOPIXEL_POWER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1); /* HIGH = power gate OFF */
}

void neopixel_alert_start(void) {}
void neopixel_stop(void)
{
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1); /* safe even if already off */
}
```

- [ ] **Step 10: Create src/hal_time.c**

```c
#include "hal_time.h"
#include <time.h>

time_t hal_time_now(void)
{
    return time(NULL);
}
```

- [ ] **Step 11: Create src/hal_nvs.c**

```c
#include "hal_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define NVS_NAMESPACE "timer_cfg"
static const char *TAG = "hal_nvs";

static esp_err_t open_nvs(nvs_handle_t *out_handle)
{
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, out_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t hal_nvs_read_u16(const char *key, uint16_t *out)
{
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_u16(h, key, out);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_write_u16(const char *key, uint16_t val)
{
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u16(h, key, val);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_read_str(const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_write_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_str(h, key, val);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_read_blob(const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK) return ret;
    ret = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return ret;
}

esp_err_t hal_nvs_write_blob(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t ret = open_nvs(&h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_blob(h, key, buf, len);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}
```

- [ ] **Step 12: Commit all stubs**

```bash
git add src/
git commit -m "feat: scaffold all source stubs with empty implementations"
```

---

## Task 11: Remove ssd1680 component directory

**Files:** Delete `components/ssd1680/`

- [ ] **Step 1: Remove the directory**

```bash
git rm -r components/ssd1680/
```

- [ ] **Step 2: Commit**

```bash
git commit -m "chore: remove ssd1680 component directory (replaced by LovyanGFX)"
```

---

## Task 12: Compile gate — verify pio run succeeds

- [ ] **Step 1: Run the build**

```bash
pio run
```

Expected output ends with:
```
================================= [SUCCESS] Took XX.XX seconds =================================
```

If the build fails:
- "component not found" errors: add missing component to `REQUIRES` in `src/CMakeLists.txt`
- "undefined reference": check that the stub file for that function exists in `src/`
- C++ compile errors in `display.cpp`: ensure `display.h` has correct `extern "C"` guards
- LovyanGFX not found: run `pio pkg install` first

Fix all errors before proceeding.

- [ ] **Step 2: Run clang-format to verify stub formatting**

```bash
clang-format --dry-run --Werror src/*.c src/*.h include/*.h 2>&1 | head -20
```

Expected: no output (all files formatted correctly). If violations appear:
```bash
clang-format -i src/*.c include/*.h
git add -u
git commit -m "style: apply clang-format to stubs"
```

---

## Task 13: Code review

- [ ] **Step 1: Run adversarial code review**

Use `superpowers:requesting-code-review` skill. Focus areas:
- All `esp_err_t` returns checked (stubs are OK to ignore for now, but note any design issues)
- `neopixel_init()` correctly sets GPIO 21 HIGH
- No ESP-IDF includes leaking into headers that will be used in native tests
- `extern "C"` guards correct in `display.h`
- `RTC_DATA_ATTR` gating is correct

Fix any issues found before merging.

---

## Task 14: Merge to integration

- [ ] **Step 1: Push and merge to integration**

```bash
git push -u origin feature/foundation
git checkout integration
git merge --no-ff feature/foundation -m "feat: foundation — stubs, deps, housekeeping"
git push origin integration
```
