# MagTag Firmware — Sub-Agent Workstream Design

**Date**: 2026-05-17
**Status**: Approved

---

## Context

The MagTag Screen Timer is a greenfield ESP32-S2 firmware project (PlatformIO + ESP-IDF). All source files are currently empty stubs. The goal of this design is to decompose the full implementation into independent, parallelisable sub-agent workstreams that can be executed in git worktrees and merged to an integration branch before landing on `main`.

Hardware flashing is expensive and fragile as a validation loop. The design maximises host-testable coverage so that flashing only happens once, at integration.

Reference: [ProductOverview.md](../../ProductOverview.md)

---

## Key Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Display library | LovyanGFX (ESP-IDF native, C++ wrapper with C API) | Only e-ink library with native ESP-IDF support; handles SSD1680 partial/full refresh; built-in fonts |
| HAL for host tests | Thin shims — function pointers per TDD module | Minimal boilerplate; sufficient for the 3 TDD modules; avoids over-engineering |
| Hardware driver split | Display = own worktree; buttons+audio+neopixel = shared "peripherals" worktree | Display is ~5× more complex; peripherals are small independent drivers with no overlap |
| Branch strategy | All streams → `integration` → `main` | Hardware project: "compiles" ≠ "works"; integration branch is the hardware-validation gate |
| `expiry_wall_time` ownership | `timer.c` only | NTP module corrects `time(NULL)` via SNTP; `remaining = expiry_wall_time - time(NULL)` self-corrects. `expiry_wall_time` changes only at `IDLE→RUNNING` and `PAUSED→RUNNING` |
| Flashing policy | Deferred entirely to Stream 4 on `integration` | Minimises hardware iterations; earlier streams gate on `pio run` (compile) or `pio test -e native` |

---

## Code Review Policy (All Streams)

Every stream runs `superpowers:requesting-code-review` before merging to `integration`. Reviews are adversarial with explicit focus on:

- **Memory leaks**: `malloc`/`free` balance, RTC struct lifetimes, LovyanGFX object ownership
- **Crashes**: null dereference, stack overflows in ISR/RMT context, uninitialized RTC state on cold boot
- **Logic errors**: state machine invariants, expiry math, timestamp arithmetic (signed 64-bit overflow)
- **Undefined behaviour**: pointer aliasing, signed integer overflow, out-of-bounds array access
- **ESP-IDF API correctness**: every `esp_err_t` return value checked; no fire-and-forget calls to `esp_wifi_*`, `nvs_*`, `esp_sleep_*`; APIs used in the documented sequence (e.g. NVS open → read/write → close)
- **Power invariants**: no peripheral left enabled across a deep-sleep boundary — NeoPixel power gate (GPIO 21), speaker amplifier (GPIO 16), and WiFi must all be explicitly off before `esp_deep_sleep_start()`
- **Deep-sleep safety**: no FreeRTOS tasks, timers, or semaphores left running at sleep entry; no stack-local state expected to survive sleep
- **Library usage**: prefer ESP-IDF built-ins and established libraries over custom re-implementations; flag any hand-rolled protocol or driver that has an existing well-supported alternative
- **Interface clarity**: public API headers must be self-contained (no implementation leaking into `.h`), function names unambiguous, return types consistent
- **Test quality**: tests must assert meaningful behaviour, not trivially-true conditions; mock interactions must reflect real HAL contracts
- **Compile failure**: hard gate — any stream touching C/C++ source must `pio run` or `pio test` clean before review is considered

---

## Compile Gate Per Stream

| Stream | Gate command | Hard failure condition |
|---|---|---|
| 0 — Foundation | `pio run` | Stub signatures cause link errors |
| 1 — Test Harness | `pio test -e native` | Any test fails or fails to compile |
| 2a — schedule | `pio test -e native` | Any test fails |
| 2b — timer | `pio test -e native` | Any test fails |
| 2c — nvs_config | `pio test -e native` | Any test fails |
| 3a — display | `pio run` | Compile error against ESP-IDF headers |
| 3b — peripherals | `pio run` | Compile error against ESP-IDF headers |
| 3c — wifi-ntp | `pio run` | Compile error against ESP-IDF headers |
| 4 — integration | `pio run` + `pio test -e native` + hardware smoke | Any failure |

---

## Dependency Graph

```
Stream 0: Foundation
    │
    └─► Stream 1: Test Harness & HAL
            │
            ├─► Stream 2a: schedule.c  (TDD) ──────────────────────────────────┐
            ├─► Stream 2b: timer.c     (TDD) ──────────────────────────────────┤
            ├─► Stream 2c: nvs_config.c (TDD) ─────────────────────────────────┤
            │                                                                   │
            ├─► Stream 3a: display     (feature/display) ───────────────────────┤
            ├─► Stream 3b: peripherals (feature/peripherals) ───────────────────┤─► Stream 4: Integration
            └─► Stream 3c: wifi-ntp    (feature/wifi-ntp) ──────────────────────┘
```

Streams 2a/2b/2c and 3a/3b/3c are fully parallel once Stream 1 merges to `integration`.

---

## Stream 0 — Foundation & Housekeeping

**Branch**: `feature/foundation`
**Blocks**: everything

### Scope

**`.gitignore` fixes**
- Add `build/` (CMake artifact directory currently tracked by git)
- Verify `.pio/` is already present (it is)

**Pre-commit fixes**
- Add `exclude: ^(build/|\.pio/)` to the `clang-format` hook (cppcheck already has this; clang-format does not)

**CLAUDE.md trim**
- Remove: Build & Flash Commands section (duplicate of `developer_setup.md`)
- Remove: Project Layout section (duplicate of `developer_setup.md`)
- Remove: Dev Container section (covered in detail by `developer_setup.md`)
- Keep: Process Rules, Pre-Commit Hooks (brief), Hardware Target table, Module Structure table, RTC Persistent State, Key Implementation Constraints, TDD Priority Modules
- Add one-line pointer: "See `docs/developer_setup.md` for build commands, project layout, and container setup."

**ProductOverview.md updates**
- Out-of-Scope: remove "Custom font rendering beyond a small bitmap font header" (LovyanGFX provides fonts)
- Add to Implementation Notes: "Display library: LovyanGFX (ESP-IDF native). `display.cpp` is the single C++ translation unit; all other modules are C. `display.h` exposes a C-compatible API."

**PlatformIO config**
- Add to `platformio.ini`: `lib_deps = lovyan03/LovyanGFX` under `[env:magtag]`

**Source stubs**
Scaffold the following with correct include guards, empty function bodies (returning `ESP_OK` or `void`), and no implementation:
- `src/main.c`
- `src/timer.c` + `include/timer.h`
- `src/display.cpp` + `include/display.h` (C API header with `extern "C"` guards)
- `src/ntp.c` + `include/ntp.h`
- `src/nvs_config.c` + `include/nvs_config.h`
- `src/schedule.c` + `include/schedule.h`
- `src/buttons.c` + `include/buttons.h`
- `src/audio.c` + `include/audio.h`
- `src/neopixel.c` + `include/neopixel.h`
- `components/ssd1680/` — remove the directory entirely; LovyanGFX replaces the custom driver and is managed as a PlatformIO library dependency, not an ESP-IDF component
- `src/CMakeLists.txt` — register all source files with ESP-IDF build system

**Compile gate**: `pio run` succeeds with empty stubs.

---

## Stream 1 — Test Harness & HAL

**Branch**: `feature/test-harness`
**Depends on**: Stream 0
**Blocks**: Streams 2a, 2b, 2c

### Scope

**PlatformIO native environment**
Add to `platformio.ini`:
```ini
[env:native]
platform = native
test_framework = unity
build_flags = -DNATIVE
```

**Thin-shim HAL headers**

`include/hal_time.h`: wraps `time()` and wall-clock reads
```c
typedef struct {
    time_t (*get_time)(void);
} hal_time_t;
void hal_time_install(const hal_time_t *impl);
```

`include/hal_nvs.h`: wraps NVS read/write for typed keys
```c
typedef struct {
    esp_err_t (*read_u16)(const char *key, uint16_t *out);
    esp_err_t (*write_u16)(const char *key, uint16_t val);
    esp_err_t (*read_str)(const char *key, char *buf, size_t len);
    esp_err_t (*write_str)(const char *key, const char *val);
    esp_err_t (*read_blob)(const char *key, void *buf, size_t *len);
    esp_err_t (*write_blob)(const char *key, const void *buf, size_t len);
} hal_nvs_t;
void hal_nvs_install(const hal_nvs_t *impl);
```

**Real implementations** (compiled only for `magtag` target, gated with `#ifndef NATIVE`):
- `src/hal_time.c` — wraps `time(NULL)` and `gettimeofday`
- `src/hal_nvs.c` — wraps `nvs_get_u16`, `nvs_set_u16`, etc.

**Mock implementations** (compiled only for `native` target):
- `test/mocks/mock_hal_time.c` — injectable `time_t` value, settable per-test
- `test/mocks/mock_hal_nvs.c` — in-memory key-value store, inspectable per-test

**Test scaffolds** (trivial passing tests to confirm the harness works):
- `test/test_schedule/test_schedule.c` — one `TEST_ASSERT_TRUE(1)` test
- `test/test_timer/test_timer.c` — one `TEST_ASSERT_TRUE(1)` test
- `test/test_nvs_config/test_nvs_config.c` — one `TEST_ASSERT_TRUE(1)` test

**Compile gate**: `pio test -e native` runs all three scaffold tests green.

---

## Stream 2a — `schedule.c` Module (TDD)

**Branch**: `feature/schedule`
**Depends on**: Stream 1
**Parallel with**: 2b, 2c, 3a, 3b, 3c

### Scope

**Tests first** (`test/test_schedule/`):
- Weekday returns `DAY_WEEKDAY` and weekday allocation
- Saturday/Sunday returns `DAY_WEEKEND` and weekend allocation
- Date in holiday blob returns `DAY_HOLIDAY` and holiday allocation
- Date not in holiday blob does not return `DAY_HOLIDAY`
- Holiday blob with trailing newline / Windows line endings parses correctly
- Day rollover at midnight (23:59 → 00:00) returns correct day type
- Full-year holiday list: spot-check 3 dates hit, 3 dates miss

**Implementation** (`src/schedule.c`, `include/schedule.h`):
- `day_type_t schedule_get_day_type(time_t now)` — uses mocked `hal_time.h`; reads holiday blob and allocations via mocked `hal_nvs.h`
- `uint32_t schedule_get_allocation_sec(day_type_t)` — looks up NVS key for the day type
- `bool schedule_is_holiday(const char *date_str, const char *blob, size_t blob_len)` — pure string search, no HAL dependency; directly unit-testable

**Compile gate**: `pio test -e native` all schedule tests green.

---

## Stream 2b — `timer.c` Module (TDD)

**Branch**: `feature/timer`
**Depends on**: Stream 1
**Parallel with**: 2a, 2c, 3a, 3b, 3c

### Scope

**Tests first** (`test/test_timer/`):
- `IDLE → RUNNING`: sets `expiry_wall_time = now + allocation_sec`; does not modify it again on subsequent calls
- `RUNNING → PAUSED`: saves `remaining_at_pause`; clears `expiry_wall_time`
- `PAUSED → RUNNING`: sets `expiry_wall_time = now + remaining_at_pause` (re-NTP sync assumed done by caller)
- `RUNNING → EXPIRED`: `remaining ≤ 0` detected correctly
- `expiry_wall_time` is NOT modified when `timer_tick()` is called during RUNNING state
- `timer_tick()` returns correct `remaining` seconds at various offsets
- Cold-boot reset: state initialises to `IDLE`, all fields zeroed
- Pause/resume round-trip preserves remaining seconds within ±1 s

**Implementation** (`src/timer.c`, `include/timer.h`):
- `rtc_state_t` struct declared with `RTC_DATA_ATTR`; gated with `#ifndef NATIVE` for host build (native build uses a regular static variable)
- `timer_state_t timer_get_state(void)`
- `void timer_start(time_t now, uint32_t allocation_sec)` — IDLE→RUNNING
- `void timer_pause(time_t now)` — RUNNING→PAUSED
- `void timer_resume(time_t now)` — PAUSED→RUNNING
- `int32_t timer_tick(time_t now)` — returns remaining seconds; transitions to EXPIRED if ≤ 0
- `void timer_reset(void)` — cold-boot init

**Compile gate**: `pio test -e native` all timer tests green.

---

## Stream 2c — `nvs_config.c` Module (TDD)

**Branch**: `feature/nvs-config`
**Depends on**: Stream 1
**Parallel with**: 2a, 2b, 3a, 3b, 3c

### Scope

**Tests first** (`test/test_nvs_config/`):
- `weekday_min` round-trip: write 45, read back 45
- `weekend_min` round-trip
- `holiday_min` round-trip
- `wifi_ssid` string round-trip (max length)
- `wifi_pass` string round-trip
- Holiday blob round-trip: write multi-line date string, read back identical
- First-boot defaults: blank NVS → `nvs_config_init_defaults()` → all keys readable with correct defaults
- Missing key falls back to compile-time default, not crash

**Implementation** (`src/nvs_config.c`, `include/nvs_config.h`):
- Typed accessors for each NVS key using mocked `hal_nvs.h`
- `esp_err_t nvs_config_init_defaults(void)` — writes all defaults from `nvs_defaults.h` if keys absent
- `nvs_defaults.h` defines compile-time defaults: US federal holiday list for current year, `weekday_min = 60`, `weekend_min = 120`, `holiday_min = 120`, `wifi_ssid = ""`, `wifi_pass = ""`

**Compile gate**: `pio test -e native` all nvs_config tests green.

---

## Stream 3a — Display Module

**Branch**: `feature/display`
**Depends on**: Stream 1
**Parallel with**: 2a, 2b, 2c, 3b, 3c
**No hardware flashing** — validated by compile gate + native mock tests for layout math

### Scope

**LovyanGFX configuration** (`src/display.cpp`):
- Before writing any code: pull the Adafruit MagTag 2025 schematic and confirm SPI pin assignments (MOSI, CLK, CS, DC, RST, BUSY). CLAUDE.md has a partial list; treat it as unverified until cross-checked against the schematic.
- `LGFX_Config` struct for SSD1680 with confirmed MagTag SPI pins
- Partial refresh enabled; full refresh triggered by caller

**C API** (`include/display.h`, `extern "C"` guards):
```c
void display_init(void);
void display_update(const display_state_t *state);  // partial refresh
void display_full_refresh(const display_state_t *state);
void display_timesup(void);                          // full-screen TIME'S UP layout
```

Where `display_state_t` carries: `remaining_sec`, `allocation_sec`, `timer_state`, `day_type`, `wall_time`, `last_sync_time`.

**Layout rendering**:
- Row 0–18: date string (`Sat May 16`) + current time + `Last sync: HH:MM`
- Row 26–50: progress bar (280 px wide, 24 px tall, thick border, fill = remaining/allocation)
- Row 58–78: remaining time label (minutes + seconds if < 5 min, else minutes only), centred
- Row 88–108: day-type + allocation (bottom-left), state label (bottom-right)

**Native mock tests** (layout math only, no render):
- Progress bar fill width: 0%, 50%, 100%, just-expired
- Remaining label format switches at 5-minute boundary
- `display_state_t` correctly populated from timer + schedule values

**Compile gate**: `pio run` compiles against ESP-IDF and LovyanGFX headers without error. Native layout tests green.

---

## Stream 3b — Peripherals (Buttons, Audio, NeoPixel)

**Branch**: `feature/peripherals`
**Depends on**: Stream 1
**Parallel with**: 2a, 2b, 2c, 3a, 3c
**No hardware flashing**

### Scope

**`buttons.c/h`**:

Button role assignment (from ProductOverview Feature 6):

| ID | GPIO | Role |
|----|------|------|
| `BTN_A` | 15 | Start (IDLE/PAUSED → RUNNING, NTP sync first) / Pause (RUNNING → PAUSED) |
| `BTN_B` | 12 | Reset to IDLE with today's full allocation |
| `BTN_C` | 14 | Cycle display contrast / brightness (3 levels) |
| `BTN_D` | 11 | Force NTP re-sync + full display refresh |

All 4 buttons are active-LOW with internal pull-ups. During an EXPIRED alert, any button press dismisses the alert (calls `audio_stop()` + `neopixel_stop()`) before the button's primary action is processed.

Public API:
- `void buttons_init(void)` — configure GPIOs with INPUT + PULLUP, no action yet
- `void buttons_configure_wakeup(void)` — calls `esp_sleep_enable_gpio_wakeup()` for all 4 GPIOs; called immediately before `esp_deep_sleep_start()`
- `button_id_t buttons_get_wakeup_button(void)` — decodes `esp_sleep_get_wakeup_cause()` + reads GPIO levels to identify which button woke the device; returns `BTN_NONE` if wake cause was not GPIO
- `bool buttons_is_pressed(button_id_t)` — reads current GPIO level (LOW = pressed); used for dismiss-on-any-button logic during alert
- 10 ms software debounce via `esp_timer_get_time()` applied in `buttons_get_wakeup_button()`

`button_id_t` enum: `BTN_A`, `BTN_B`, `BTN_C`, `BTN_D`, `BTN_NONE`

**`audio.c/h`**:
- `audio_beep_sequence(void)` — enables GPIO 16 amplifier, plays 3 short beeps via LEDC PWM, repeats every 3 s for 5 cycles (15 s total)
- `audio_stop(void)` — immediate stop, called on any button press during alert
- Amplifier disabled between sequences

**`neopixel.c/h`**:
- GPIO 21 is HIGH (power gate OFF) by default. `neopixel_init()` must assert GPIO 21 HIGH immediately on every boot path — including cold boot and every wake from deep sleep — before any other peripheral code runs. NeoPixels must never be on unless an alert is actively firing.
- `neopixel_alert_start(void)` — sets GPIO 21 LOW (power gate on), drives RMT on GPIO 1 with slow red pulse
- `neopixel_stop(void)` — stops RMT, asserts GPIO 21 HIGH (power gate off); safe to call even if NeoPixels were already off

**Compile gate**: `pio run` compiles clean.

---

## Stream 3c — WiFi / NTP Module

**Branch**: `feature/wifi-ntp`
**Depends on**: Stream 1
**Parallel with**: 2a, 2b, 2c, 3a, 3b
**No hardware flashing**

### Scope

**Library policy**: use ESP-IDF built-in components throughout — `esp_wifi` for WiFi lifecycle, `esp_netif` for network interface init, `esp_sntp` for time sync. No custom socket-based NTP implementation. Same principle applies across all streams: prefer an established library or ESP-IDF component over hand-rolled code for any protocol or peripheral with existing support.

**`ntp.c/h`**:
- `esp_err_t ntp_sync(void)` — full lifecycle: `esp_netif_init()` → `esp_wifi` init/connect → `esp_sntp` sync (IMMED mode, `sntp_get_sync_status()` polling) → `esp_wifi` disconnect/deinit → `esp_netif` deinit
- Returns `ESP_OK` on successful sync, error code otherwise
- Caller reads corrected `time(NULL)` after `ntp_sync()` returns `ESP_OK`
- **`ntp_sync()` never reads or writes `expiry_wall_time`** — that field is owned by `timer.c`
- Compile-time POSIX TZ string `#define TZ_STRING "EST5EDT,M3.2.0,M11.1.0"` (configurable in `nvs_defaults.h`)
- No-WiFi error path: `ntp_sync()` returns `ESP_ERR_WIFI_NOT_CONNECT` or similar; caller (`main.c`) surfaces "check WiFi" state to display

**Compile gate**: `pio run` compiles clean.

---

## Stream 4 — Integration

**Branch**: `feature/integration`
**Depends on**: all streams merged to `integration`

### Scope

**`main.c`**:
```c
void app_main(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        handle_button_wake();
    } else {
        handle_timer_tick();
    }
}
```

**`handle_timer_tick()`** sequence:
1. Read `time(NULL)` from RTC
2. Check day rollover (`last_date` vs today) → if new day: `ntp_sync()`, `timer_reset()`, load new allocation from `schedule`
3. Check `next_ntp_sync` threshold (every 10 min while RUNNING) → `ntp_sync()` if due
4. `timer_tick(now)` → compute remaining
5. `display_update(&state)` or `display_full_refresh(&state)` per refresh counter
6. If `EXPIRED`: run `audio_beep_sequence()` + `neopixel_alert_start()`, skip deep sleep until dismissed or complete
7. `esp_deep_sleep_start()`

**`handle_button_wake()`** sequence:
1. Identify button via `buttons_get_wakeup_button()`
2. If state is `EXPIRED`: call `audio_stop()` + `neopixel_stop()` regardless of which button woke the device, then return to deep sleep
3. Dispatch on button ID:
   - `BTN_A`: if IDLE or PAUSED → `ntp_sync()` then `timer_start()` / `timer_resume()`; if RUNNING → `timer_pause()`
   - `BTN_B`: `timer_reset()`, reload today's allocation from `schedule`
   - `BTN_C`: cycle display contrast level (3 steps, persisted in NVS)
   - `BTN_D`: `ntp_sync()` then `display_full_refresh(&state)`
4. `display_full_refresh(&state)`
5. `esp_deep_sleep_start()`

**Deep sleep configuration** (called once before first sleep):
```c
esp_sleep_enable_timer_wakeup(55ULL * 1000000ULL);
buttons_configure_wakeup();
```

**Hardware smoke test** (on `integration` branch, one flash):
- Device boots, shows IDLE display, enters deep sleep
- Button press wakes, triggers full refresh, re-enters sleep
- Timer start: NTP sync, RUNNING display, 55-second wake cycle
- Expiry: TIME'S UP layout, beep sequence, NeoPixel pulse
- Dismiss: returns to EXPIRED + deep sleep

**Compile gate**: `pio run` + `pio test -e native` both clean before flashing.

---

## Branch Naming Summary

| Stream | Branch |
|---|---|
| 0 | `feature/foundation` |
| 1 | `feature/test-harness` |
| 2a | `feature/schedule` |
| 2b | `feature/timer` |
| 2c | `feature/nvs-config` |
| 3a | `feature/display` |
| 3b | `feature/peripherals` |
| 3c | `feature/wifi-ntp` |
| 4 | `feature/integration` |
| Staging | `integration` |
| Production | `main` |
