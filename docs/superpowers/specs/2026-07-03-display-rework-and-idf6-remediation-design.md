# Display Rework & ESP-IDF 6 Remediation — Design

**Date:** 2026-07-03
**Status:** Approved (user, 2026-07-03)
**Supersedes:** display sections of `2026-05-17-magtag-workstream-design.md` and `plans/2026-05-17-stream-3a-display.md`

## Problem

The codebase generated in the 2026-05-17 streams is unvalidated on hardware and has three structural defects:

1. **The display driver cannot work.** LovyanGFX has no SSD1680 panel driver (verified against upstream `src/lgfx/v1/panel/` on 2026-07-03). `src/display.cpp` uses `Panel_GDEW0154D67` — a 1.54" 200×200 UC8151-family panel — as an admitted stand-in for the MagTag's 2.9" 296×128 SSD1680. No drop-in replacement exists: the ESP Component Registry has no SSD1680 component, `espressif/esp_lcd_ssd1681` targets the wrong controller variant and size, and the closest GitHub candidate (`aivoprykk/esp_lcd_panel_ssd1680`, Apache-2.0) lacks a 2.9" 296×128 panel model and is unverified on IDF 6.
2. **The build system is split-brain.** The repo carries both a `platformio.ini` (the only place LovyanGFX is declared) and a pure ESP-IDF `CMakeLists.txt`; the committed sdkconfig targets ESP-IDF 6.0.1, which PlatformIO's `espressif32` platform does not ship. Recent IDF-6.0.1 fix commits were built outside this devcontainer, which has only PlatformIO installed.
3. **A deep-sleep wakeup spec bug.** ProductOverview.md prescribes `esp_sleep_enable_gpio_wakeup()`, which on ESP32-S2 is the *light-sleep* API. Buttons would never wake the device from deep sleep.

Additionally, `build/` artifacts are tracked in git.

## Decisions (made interactively with the user, 2026-07-03)

| Decision | Choice |
|---|---|
| Toolchain | Pure ESP-IDF 6.x with `idf.py`; PlatformIO removed entirely (native tests move to host CMake + ctest) |
| Panel driver | Custom `components/ssd1680` (own code; reference Adafruit_EPD, GxEPD2, CircuitPython SSD1680, aivoprykk) |
| Graphics/text | LVGL 9 managed component, I1 (1-bit) render format |
| Button C | Unbound in v1 (e-ink has no contrast; feature dropped) |
| Hardware access | Limited/later — front-load compile + native-test work; prepare a smoke-test checklist |

## Architecture

```
src/display.c  ──►  LVGL 9 (managed component)  ──►  components/ssd1680 (ours)
  layout, refresh      I1 1-bit render buffer,         SPI cmds, init sequence,
  strategy             Montserrat fonts, flush cb      full/partial LUT refresh,
                                                       BUSY wait + timeout, panel sleep
```

### components/ssd1680 (new, pure C, ~400 lines)

Public API (`ssd1680.h`):

- `ssd1680_init(const ssd1680_config_t *cfg)` — SPI bus + panel init. Config carries pins (SCK 36, MOSI 35, CS 8, DC 7, RST 6, BUSY 5), resolution 296×128.
- `ssd1680_write_framebuffer(const uint8_t *buf)` — write 1-bit buffer to controller RAM.
- `ssd1680_refresh(ssd1680_refresh_mode_t mode)` — `SSD1680_REFRESH_FULL` (cmd 0x22 = 0xF7) or `SSD1680_REFRESH_PARTIAL` (partial LUT).
- `ssd1680_sleep(void)` — panel deep-sleep mode 1 (cmd 0x10 = 0x01, RAM-retaining).
- All BUSY-pin waits have a timeout (order of 5 s); on timeout: log error, return `ESP_ERR_TIMEOUT`, never block forever. Rationale: if the BUSY line never clears (damaged panel, loose flex cable, SPI miswire), the wake handler must still finish and re-enter deep sleep so the timer keeps timing — the alternative is a frozen device that drains the battery until someone power-cycles it.

Behavioural rules:

- Panel gets a hardware reset on every wake (mandatory after panel deep sleep).
- Panel is put to sleep after **every** refresh — power draw and panel longevity. Driver uses SSD1680 deep-sleep **mode 1** (0x10 = 0x01, RAM-retaining) so the controller keeps the previous frame for partial-refresh waveforms; the MagTag's panel rail is always powered.
- **Panel-protection is enforced in the driver, independent of caller policy:** (1) refreshes are serialized on BUSY completion — a new refresh cannot start while one is in progress; (2) a minimum interval between refresh commands (default 1 s, configurable) is enforced against a last-refresh timestamp kept in RTC memory, so the floor holds across deep-sleep wakes and protects the panel from a runaway wake loop. The floor is small enough that rapid legitimate button presses stay responsive (BUSY serialization already spaces refreshes by the 0.4–3 s the panel physically takes).
- Init, LUT, and partial-refresh sequences derived from Adafruit_EPD / GxEPD2 / CircuitPython drivers that run this exact MagTag panel.

### LVGL integration (in src/display.c)

- `lvgl/lvgl ^9.x` via `src/idf_component.yml` (managed component).
- One `lv_display` with `LV_COLOR_FORMAT_I1`, full-frame draw buffer (296×128÷8 + palette ≈ 4.8 KB, plain SRAM).
- Flush callback repacks the I1 buffer into SSD1680 RAM layout (portrait byte order) and calls the driver.
- **Refresh-mode policy lives in display.c** (partial refresh on 55 s ticks; full refresh on state transitions and every 5th partial, matching ProductOverview §7) — it is UX. **Panel protection lives in the driver** (see behavioural rules above) — policy bugs cannot damage the panel.
- Fonts: built-in Montserrat via Kconfig (`LV_FONT_MONTSERRAT_12/16/28/48` — final sizes chosen during implementation to fit the spec'd layout rows).
- Per-wake sequence: boot → `lv_init` → build widget tree from `display_state_t` → render once (`lv_refr_now`) → flush → `ssd1680_refresh` → `ssd1680_sleep` → deep sleep. LVGL state is rebuilt from scratch each wake; nothing LVGL survives deep sleep.
- **Partial refresh works despite the per-wake rebuild.** Two unrelated meanings of "partial": the MCU always re-renders the *full frame* into its own buffer (4.8 KB, trivial); "partial refresh" refers to the *panel waveform*, which diffs the new frame against the previous frame held in the **controller's own RAM** — preserved across wakes by RAM-retaining deep-sleep mode 1. Fallback if hardware bring-up shows RAM retention is unreliable: write both RAM planes (0x24/0x26) every wake; costs transient ghosting that the every-5th full refresh already cleans up.

### src/display.c (replaces display.cpp)

- LVGL is C, so the sole C++ translation unit disappears; **the project becomes pure C**. Remove `build_unflags = -fuse-cxa-atexit` leftovers and C++-specific build config.
- Public API unchanged except `contrast_level` is removed from `display_state_t`.
- `display_bar_fill_px()` and `display_format_remaining()` remain pure C, unchanged semantics, keeping their existing native test suite.
- Screen layout per ProductOverview §7, visually unchanged.

## Module audit (ESP-IDF 6.0.1)

| Module | Action |
|---|---|
| `buttons.c` | **Fix wakeup:** `esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW)` on GPIOs 11/12/14/15 (all RTC-capable on S2). RTC-domain pull-ups configured and held through deep sleep (buttons are active-low). Identify wake button via `esp_sleep_get_ext1_wakeup_status()`. |
| `ntp.c` | Verify esp_wifi / esp_netif / esp_sntp calls compile on 6.0.1 (SNTP kconfig symbol already renamed in `23164c4`). |
| `neopixel.c` | Verify new-style RMT driver (`esp_driver_rmt`) usage compiles. |
| `audio.c` | Verify LEDC API compiles. |
| `timer.c`, `schedule.c` | No change (platform-independent, TDD'd). |
| `nvs_config.c` | Remove display-contrast accessor/key. |
| `main.c` | Keep stream-4 dispatch structure; Button C → no-op; NeoPixel power gate (GPIO 21 HIGH) remains first peripheral action. |

## Build system & repo hygiene

- ESP-IDF 6.0.1 **baked into the devcontainer image** (Dockerfile `RUN` layer clones IDF and installs the ESP32-S2 toolchain), so container rebuilds start with a working environment; `.devcontainer` and `docs/developer_setup.md` updated. Firmware builds are `idf.py build|flash|monitor`. (User-requested change from the earlier volume-based approach, 2026-07-03.)
- **PlatformIO removed entirely** (`platformio.ini` deleted, PIO dropped from the devcontainer). It was only acting as the native test runner; see Testing below for the replacement. Project dependencies become just gcc + cmake (already in the container) + ESP-IDF.
- `build/` removed from git tracking; `.gitignore` added covering `build/`, `managed_components/`, and the host-test build dir.

## Error handling

- **Display BUSY timeout:** log + return error + continue to sleep (see driver rules above).
- **WiFi/NTP failure:** unchanged from ProductOverview — "no sync — check WiFi" message blocks timer start; failed periodic syncs retry next wake; day-rollover reset is fail-open.
- **NVS init failure at boot:** unchanged — `ESP_ERROR_CHECK` (unrecoverable).

## Testing & validation

1. **Native (TDD):** existing suites (`test_timer`, `test_schedule`, `test_nvs_config`, `test_display`) migrate from `pio test -e native` to a small host-side CMake project (`test/CMakeLists.txt`) with **vendored Unity** (three MIT-licensed files) — one executable per suite, run via `ctest`. The test sources and mocks are already plain C and carry over unchanged; only the runner changes. New or changed pure logic gets tests first (per CLAUDE.md).
2. **Compile gate:** `idf.py build` completes clean. This is the acceptance bar for all driver/LVGL work before hardware is available.
3. **Hardware smoke test (deferred to a scheduled session):** `docs/hardware_smoke_test.md` — ordered checklist: flash; first-boot IDLE screen; Button A start (NTP-gated); 55 s partial refresh cadence; full refresh on 5th wake (no ghosting); pause/resume; forced expiry alert (beeps + red pulse, button dismissal); Button D forced sync; day-rollover reallocation; wake-from-deep-sleep via every button.

## Deltas from ProductOverview.md

| ProductOverview said | This design says | Why |
|---|---|---|
| LovyanGFX, `display.cpp` C++ TU | LVGL 9 + custom `components/ssd1680`, pure C `display.c` | LovyanGFX has no SSD1680 driver |
| Button C cycles contrast (3 levels) | Button C unbound in v1 | E-ink has no contrast/brightness |
| `contrast_level` in display state / NVS | Removed | Same |
| `esp_sleep_enable_gpio_wakeup()` | EXT1 `ANY_LOW` wakeup on GPIOs 11/12/14/15 | GPIO wakeup is light-sleep-only on ESP32-S2 |
| PlatformIO recommended; IDF 5.x alternative | ESP-IDF 6.0.1 + `idf.py`; PlatformIO removed, host tests via CMake/ctest + vendored Unity | Codebase already migrated to 6.0.1; PIO espidf integration unmaintained; one less dependency |
| (silent on panel power) | Panel deep-sleep cmd after every refresh; hw reset each wake | Power + panel health |
| NTP every 10 min while RUNNING | Unchanged | Device predominantly USB-powered |

Everything else — timer state machine, absolute-expiry math, RTC memory layout, schedule/NVS design, 55 s wake cadence, display layout, alert behaviour — is unchanged from ProductOverview.md.

## Out of scope

Unchanged from ProductOverview.md §Out of Scope (OTA, companion app, profiles, SD, BLE provisioning, button-adjustable allocations). Additionally out of scope here: battery-life optimization of the NTP cadence (revisit if battery operation becomes primary).
