# Display Rework & ESP-IDF 6 Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the non-functional LovyanGFX display stack with a custom SSD1680 driver + LVGL 9, migrate the project to pure ESP-IDF 6.0.1 (`idf.py`), drop PlatformIO in favour of host CMake/ctest tests, fix the deep-sleep button wakeup, and implement the full `main.c` integration.

**Architecture:** `main.c` dispatches on wake cause to timer-tick or button handlers, renders via `display.c` (LVGL 9, I1 format) which flushes through a new `components/ssd1680` SPI driver, then re-enters deep sleep. Pure-logic modules stay host-tested via ctest; hardware behaviour is compile-gated now and smoke-tested later per `docs/hardware_smoke_test.md`.

**Tech Stack:** ESP-IDF 6.0.1, ESP32-S2, LVGL 9.x (managed component), Unity (vendored), CMake/ctest host tests.

**Spec:** `docs/superpowers/specs/2026-07-03-display-rework-and-idf6-remediation-design.md` (approved 2026-07-03).

---

## File Structure

| Action | Path | Responsibility |
|---|---|---|
| Delete | `platformio.ini`, `sdkconfig.magtag`, `src/display.cpp`, `lib/` | PlatformIO + LovyanGFX removal |
| Rename | `src/` → `main/` | idf.py requires a `main` component |
| Create | `.gitignore` | untrack build artifacts |
| Create | `test/unity/{unity.c,unity.h,unity_internals.h}` | vendored Unity v2.6.0 (MIT) |
| Create | `test/CMakeLists.txt` | host test runner (ctest) |
| Modify | `.devcontainer/Dockerfile`, `.devcontainer/devcontainer.json` | bake ESP-IDF 6.0.1 into image, drop PIO |
| Modify | `CMakeLists.txt` (root) | project name, standard IDF layout |
| Create | `components/ssd1680/{CMakeLists.txt,include/ssd1680.h,ssd1680.c,ssd1680_guard.c}` | panel driver + refresh guard |
| Create | `test/test_ssd1680/test_ssd1680.c` | guard-logic host tests |
| Create | `main/idf_component.yml` | LVGL 9 dependency |
| Modify | `sdkconfig.defaults` | LVGL color depth + fonts |
| Create | `main/display_layout.c` | pure layout math (host-tested) |
| Create | `main/display.c` | LVGL UI + refresh policy + flush |
| Modify | `include/display.h` | drop contrast; add `display_sync_failed()` |
| Modify | `main/nvs_config.c`, `include/nvs_config.h`, `include/nvs_defaults.h` | remove contrast accessors |
| Modify | `main/buttons.c`, `include/buttons.h` | EXT1 deep-sleep wakeup |
| Modify | `main/main.c` | full integration |
| Modify | `test/test_nvs_config/test_nvs_config.c`, `test/test_display/test_display.c` | contrast removal; test real layout code |
| Create | `docs/hardware_smoke_test.md` | deferred hardware checklist |
| Modify | `docs/developer_setup.md`, `.claude/CLAUDE.md` | new toolchain docs |

Working branch: `feature/display-rework`, from `integration`. Never commit to `main`.

**Shell convention:** every `idf.py` command below assumes `source $HOME/esp/esp-idf/export.sh` has been run in that shell first (it is not in `.bashrc` to keep shell startup fast).

---

### Task 1: Create branch in a worktree

- [ ] **Step 1: Use the superpowers:using-git-worktrees skill** to create a worktree for branch `feature/display-rework` based on `integration`.

Note: the repo working tree currently has unrelated uncommitted changes (`build/` noise, `src/nvs_config.c` NATIVE guard, `.clang-format` untracked, a plan-doc edit). Bring `src/nvs_config.c` and `.clang-format` into the new branch (they are wanted); leave `build/` noise behind (Task 2 untracks it).

- [ ] **Step 2: Verify**

```bash
git status && git branch --show-current
```
Expected: on `feature/display-rework`, clean or with only the intended carried-over changes.

- [ ] **Step 3: Commit carried-over hygiene files**

```bash
git add .clang-format src/nvs_config.c
git commit -m "chore: add clang-format config; guard nvs.h include for native builds"
```

---

### Task 2: Repo hygiene — untrack build artifacts, remove PlatformIO

**Files:**
- Create: `.gitignore`
- Delete (from tracking): `build/`
- Delete: `platformio.ini`, `sdkconfig.magtag`, `lib/`

- [ ] **Step 1: Create `.gitignore`**

```gitignore
# ESP-IDF
build/
managed_components/
dependencies.lock
sdkconfig.old

# Host tests
test/build/

# Editors
.vscode/
*.swp
```

(`sdkconfig` itself stays tracked — it is regenerated in Task 5 and committed so builds are reproducible.)

- [ ] **Step 2: Untrack build artifacts and delete PlatformIO files**

```bash
git rm -r --cached build
git rm platformio.ini sdkconfig.magtag
rmdir lib 2>/dev/null || true
```

- [ ] **Step 3: Verify**

```bash
git status --short | head -20
```
Expected: `build/*` shows as deleted-from-index (D), `.gitignore` new, no `build/` files listed as untracked.

- [ ] **Step 4: Commit**

```bash
git add .gitignore
git commit -m "chore(build): untrack build artifacts; remove PlatformIO config"
```

---

### Task 3: Host test harness — CMake + ctest + vendored Unity

**Files:**
- Create: `test/unity/unity.c`, `test/unity/unity.h`, `test/unity/unity_internals.h`
- Create: `test/CMakeLists.txt`

The four existing suites are single-TU programs (each `#include`s the source under test and mocks, and has its own `main()`), so they map 1:1 onto plain executables.

- [ ] **Step 1: Vendor Unity v2.6.0 (MIT)**

```bash
mkdir -p test/unity
for f in unity.c unity.h unity_internals.h; do
  curl -fsSL "https://raw.githubusercontent.com/ThrowTheSwitch/Unity/v2.6.0/src/$f" -o "test/unity/$f"
done
head -5 test/unity/unity.h   # sanity: shows Unity copyright header
```

- [ ] **Step 2: Create `test/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
project(magtag_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)

# Each suite is a single-TU test program: it #includes the source under
# test and its mocks directly, so no project sources are compiled here.
function(add_host_test name)
  add_executable(${name} ${name}/${name}.c)
  target_include_directories(${name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/mocks
      ${CMAKE_CURRENT_SOURCE_DIR}/../include)
  target_compile_definitions(${name} PRIVATE NATIVE)
  target_compile_options(${name} PRIVATE
      -include ${CMAKE_CURRENT_SOURCE_DIR}/mocks/esp_compat.h
      -Wall -Wextra)
  target_link_libraries(${name} unity)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_host_test(test_timer)
add_host_test(test_schedule)
add_host_test(test_nvs_config)
add_host_test(test_display)
```

- [ ] **Step 3: Build and run all suites**

```bash
cmake -S test -B test/build && cmake --build test/build && ctest --test-dir test/build --output-on-failure
```
Expected: 4/4 tests pass (`test_display` still tests its local stubs at this point — replaced with the real implementation in Task 8).

Troubleshooting: if a suite fails to compile on `#include "unity.h"` ordering or `setUp`/`tearDown` linkage, check that the executable's only TU is the suite file — do NOT add `unity.c` to the executable sources (it's in the `unity` library).

- [ ] **Step 4: Commit**

```bash
git add test/unity test/CMakeLists.txt
git commit -m "test: replace PlatformIO native runner with CMake/ctest + vendored Unity"
```

---

### Task 4: Remove display-contrast config (TDD)

**Files:**
- Modify: `test/test_nvs_config/test_nvs_config.c`, `main/nvs_config.c` (still `src/` at this point — rename happens in Task 5; use `src/` paths here), `include/nvs_config.h`, `include/nvs_defaults.h`

- [ ] **Step 1: Remove the two contrast tests**

In `test/test_nvs_config/test_nvs_config.c`: delete `test_display_contrast_round_trip` and `test_display_contrast_missing_returns_default` function bodies (lines ~135–150) and their two `RUN_TEST(...)` lines.

- [ ] **Step 2: Run tests — expect compile failure is NOT acceptable; they should still pass**

```bash
cmake --build test/build && ctest --test-dir test/build -R test_nvs_config --output-on-failure
```
Expected: PASS (the accessors still exist; tests just no longer reference them).

- [ ] **Step 3: Remove the accessors and default**

- `src/nvs_config.c`: delete `nvs_config_get_display_contrast` / `nvs_config_set_display_contrast` (lines ~98–109) and the `init_u16_if_missing("disp_contrast", NVS_DEFAULT_CONTRAST)` block in `nvs_config_init_defaults` (lines ~128–130).
- `include/nvs_config.h`: delete the two contrast prototypes (lines 28–29).
- `include/nvs_defaults.h`: delete `#define NVS_DEFAULT_CONTRAST 1`.

- [ ] **Step 4: Run tests**

```bash
cmake --build test/build && ctest --test-dir test/build --output-on-failure
```
Expected: 4/4 PASS.

- [ ] **Step 5: Commit**

```bash
git add src/nvs_config.c include/nvs_config.h include/nvs_defaults.h test/test_nvs_config/test_nvs_config.c
git commit -m "refactor(nvs): remove display-contrast config (e-ink has no contrast)"
```

---

### Task 5: ESP-IDF 6.0.1 toolchain + project layout + baseline build

**Files:**
- Modify: `.devcontainer/Dockerfile`, `.devcontainer/devcontainer.json`, `CMakeLists.txt` (root)
- Rename: `src/` → `main/`
- Delete: `src/display.cpp`; Create: `main/display.c` (temporary stub)

- [ ] **Step 1: Update `.devcontainer/Dockerfile` — bake ESP-IDF into the image**

Replace the PlatformIO install (`USER vscode` / `RUN pip3 install --user platformio` lines and the `mkdir -p /workspaces/.pio ...` clause) with IDF prerequisites and a baked-in IDF install, so devcontainer rebuilds start with a working environment (Docker layer cache makes rebuilds cheap). The apt line becomes:

```dockerfile
RUN apt-get update && apt-get install -y \
    python3-venv \
    python3-pip \
    python-is-python3 \
    udev \
    build-essential \
    git wget flex bison gperf \
    cmake ninja-build ccache \
    libffi-dev libssl-dev dfu-util libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*
```

Keep the nvm/node section and the dialout/plugdev/claude lines; end the file with:

```dockerfile
# Bake ESP-IDF v6.0.1 + ESP32-S2 toolchain into the image so container
# rebuilds start with a working environment. (~3 GB layer, cached.)
USER vscode
RUN mkdir -p /home/vscode/esp \
    && git clone --branch v6.0.1 --depth 1 --recursive --shallow-submodules \
       https://github.com/espressif/esp-idf.git /home/vscode/esp/esp-idf \
    && /home/vscode/esp/esp-idf/install.sh esp32s2 \
    && rm -rf /home/vscode/.espressif/dist
```

- [ ] **Step 2: Update `.devcontainer/devcontainer.json`**

- In `mounts`: **delete** the `platformio-packages` entry. Do NOT add volumes at `/home/vscode/esp` or `/home/vscode/.espressif` — an empty named volume mounted there would shadow the baked-in install.
- In `containerEnv`: remove `PLATFORMIO_CORE_DIR`.
- In extensions: remove `platformio.platformio-ide`.

- [ ] **Step 3: Install ESP-IDF in the *running* container now (same commands the Dockerfile bakes in; the image rebuild only benefits future containers)**

```bash
mkdir -p ~/esp ~/.espressif
git clone --branch v6.0.1 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s2
```
Expected: ~10 min (2 GB clone + toolchain). Run in background if desired; do not proceed until it finishes. Verify:

```bash
source $HOME/esp/esp-idf/export.sh && idf.py --version
```
Expected: `ESP-IDF v6.0.1`.

- [ ] **Step 5: Rename `src/` → `main/` and fix references**

```bash
git mv src main
sed -i 's|\.\./\.\./src/|../../main/|' test/test_timer/test_timer.c test/test_schedule/test_schedule.c test/test_nvs_config/test_nvs_config.c
```

Root `CMakeLists.txt` becomes:

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(magtag_timer)
```

- [ ] **Step 6: Replace `display.cpp` with a temporary stub `main/display.c`**

```bash
git rm main/display.cpp
```

Create `main/display.c` (temporary — fully replaced in Task 9):

```c
#include "display.h"

#include "esp_log.h"

static const char *TAG = "display";

/* Temporary stub so the firmware links while the SSD1680/LVGL stack is
   built (Tasks 6-9). Replaced wholesale by the LVGL implementation. */
void display_init(void) { ESP_LOGI(TAG, "display_init (stub)"); }
void display_update(const display_state_t *state) { ESP_LOGI(TAG, "update: %ld s left (stub)", (long)state->remaining_sec); }
void display_full_refresh(const display_state_t *state) { ESP_LOGI(TAG, "full refresh: %ld s left (stub)", (long)state->remaining_sec); }
void display_timesup(void) { ESP_LOGI(TAG, "TIME'S UP (stub)"); }
void display_sync_failed(void) { ESP_LOGI(TAG, "sync failed screen (stub)"); }

uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec) {
    (void)remaining_sec; (void)allocation_sec;
    return 0; /* real implementation arrives in Task 8 (display_layout.c) */
}
void display_format_remaining(char *buf, size_t len, int32_t remaining_sec) {
    (void)remaining_sec;
    if (len) buf[0] = '\0';
}
```

Rewrite `include/display.h` (drops `contrast_level`, adds `display_sync_failed`, keeps the pure helpers):

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "schedule.h"
#include "timer.h"

typedef struct {
    int32_t remaining_sec;
    uint32_t allocation_sec;
    timer_state_t timer_state;
    day_type_t day_type;
    time_t wall_time;
    time_t last_sync_time;
} display_state_t;

void display_init(void);
void display_update(const display_state_t *state);       /* partial-refresh policy */
void display_full_refresh(const display_state_t *state); /* forced full refresh */
void display_timesup(void);                              /* TIME'S UP layout, full refresh */
void display_sync_failed(void);                          /* "No sync - check WiFi" layout */

/* Pure layout math (main/display_layout.c from Task 8) — host-tested */
uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec);
void display_format_remaining(char *buf, size_t len, int32_t remaining_sec);
```

Update `main/CMakeLists.txt`: in `SRCS`, replace `"display.cpp"` with `"display.c"`.

- [ ] **Step 7: Baseline firmware build**

```bash
source $HOME/esp/esp-idf/export.sh
idf.py set-target esp32s2
idf.py build
```

Expected: eventually `Project build complete`. This is the first-ever build of this code against a real IDF 6.0.1 — compile errors in `ntp.c`, `audio.c`, `neopixel.c`, `buttons.c`, `hal_nvs.c` are likely. Fix each minimally (API renames only, no behaviour changes). Known IDF 6 risk areas:
  - `esp_sleep_get_wakeup_causes()` (bitmap) vs removed `esp_sleep_get_wakeup_cause()` — `buttons.c` already uses the new one.
  - SNTP: `esp_sntp.h` API and `CONFIG_SNTP_TIME_SYNC_METHOD_IMMEDIATE` (already renamed in commit `23164c4`).
  - RMT: must use `driver/rmt_tx.h` (new driver) — `neopixel.c` already does.
  - If `idf.py set-target` complains about stale `sdkconfig`, delete `sdkconfig` and re-run (defaults come from `sdkconfig.defaults`).

- [ ] **Step 8: Host tests still pass after the rename**

```bash
rm -rf test/build && cmake -S test -B test/build && cmake --build test/build && ctest --test-dir test/build --output-on-failure
```
Expected: 4/4 PASS.

- [ ] **Step 9: Commit** (include the regenerated `sdkconfig`)

```bash
git add -A
git commit -m "build: migrate to pure ESP-IDF 6.0.1; rename src/ to main/; stub display pending SSD1680/LVGL stack"
```

---

### Task 6: `components/ssd1680` — refresh guard (TDD) + panel driver

**Files:**
- Create: `components/ssd1680/CMakeLists.txt`, `components/ssd1680/include/ssd1680.h`, `components/ssd1680/ssd1680_guard.c`, `components/ssd1680/ssd1680.c`
- Create: `test/test_ssd1680/test_ssd1680.c`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing guard tests — `test/test_ssd1680/test_ssd1680.c`**

```c
#include <stdbool.h>
#include <stdint.h>
#include <unity.h>

/* Single-TU compilation of the pure guard logic (no ESP-IDF headers) */
#include "../../components/ssd1680/ssd1680_guard.c"

void setUp(void) {}
void tearDown(void) {}

void test_allowed_on_first_refresh(void) {
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(1000, 0, 1));
}

void test_blocked_within_min_interval(void) {
    TEST_ASSERT_FALSE(ssd1680_refresh_allowed(1000, 1000, 1));
}

void test_allowed_at_exactly_min_interval(void) {
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(1001, 1000, 1));
}

void test_allowed_after_min_interval(void) {
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(1055, 1000, 1));
}

void test_allowed_when_clock_steps_backwards(void) {
    /* NTP corrected the clock backwards past the last-refresh stamp;
       blocking here could wedge refreshes for a long time — allow. */
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(900, 1000, 1));
}

void test_blocked_with_larger_interval(void) {
    TEST_ASSERT_FALSE(ssd1680_refresh_allowed(1029, 1000, 30));
    TEST_ASSERT_TRUE(ssd1680_refresh_allowed(1030, 1000, 30));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_allowed_on_first_refresh);
    RUN_TEST(test_blocked_within_min_interval);
    RUN_TEST(test_allowed_at_exactly_min_interval);
    RUN_TEST(test_allowed_after_min_interval);
    RUN_TEST(test_allowed_when_clock_steps_backwards);
    RUN_TEST(test_blocked_with_larger_interval);
    UNITY_END();
    return 0;
}
```

Add to `test/CMakeLists.txt` after the other suites: `add_host_test(test_ssd1680)`.

- [ ] **Step 2: Run to verify failure**

```bash
cmake -S test -B test/build && cmake --build test/build 2>&1 | tail -5
```
Expected: FAIL — `ssd1680_guard.c: No such file or directory`.

- [ ] **Step 3: Implement `components/ssd1680/ssd1680_guard.c`**

```c
/* Pure panel-protection logic — no ESP-IDF dependencies so it is
   host-testable. Policy (partial vs full cadence) lives in display.c;
   this guard only prevents physically harmful refresh rates. */
#include <stdbool.h>
#include <stdint.h>

bool ssd1680_refresh_allowed(int64_t now_sec, int64_t last_refresh_sec, int32_t min_interval_sec) {
    if (last_refresh_sec <= 0)
        return true; /* never refreshed (cold boot) */
    if (now_sec < last_refresh_sec)
        return true; /* clock stepped backwards (NTP correction) */
    return (now_sec - last_refresh_sec) >= min_interval_sec;
}
```

- [ ] **Step 4: Run guard tests**

```bash
cmake --build test/build && ctest --test-dir test/build -R test_ssd1680 --output-on-failure
```
Expected: 6/6 PASS.

- [ ] **Step 5: Commit the guard**

```bash
git add components/ssd1680/ssd1680_guard.c test/test_ssd1680 test/CMakeLists.txt
git commit -m "feat(ssd1680): add host-tested refresh-rate guard"
```

- [ ] **Step 6: Create `components/ssd1680/include/ssd1680.h`**

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SSD1680_WIDTH 128  /* panel sources used (portrait width) */
#define SSD1680_HEIGHT 296 /* panel gates (portrait height) */
#define SSD1680_FB_SIZE ((SSD1680_WIDTH / 8) * SSD1680_HEIGHT) /* 4736 bytes */

/* Minimum seconds between panel refreshes (runaway protection). */
#define SSD1680_MIN_REFRESH_INTERVAL_SEC 1

typedef enum {
    SSD1680_REFRESH_FULL,    /* full inversion flash — best quality */
    SSD1680_REFRESH_PARTIAL, /* differential vs previous frame — no flash */
} ssd1680_refresh_mode_t;

typedef struct {
    int pin_sclk; /* MagTag: 36 */
    int pin_mosi; /* MagTag: 35 */
    int pin_cs;   /* MagTag: 8  */
    int pin_dc;   /* MagTag: 7  */
    int pin_rst;  /* MagTag: 6  */
    int pin_busy; /* MagTag: 5  */
} ssd1680_pins_t;

#ifdef __cplusplus
extern "C" {
#endif

/* SPI bus + panel init + hardware reset. Call once per wake. */
esp_err_t ssd1680_init(const ssd1680_pins_t *pins);

/* fb: SSD1680_FB_SIZE bytes, portrait, row-major, MSB-first,
   bit set = BLACK pixel. Writes to controller RAM (no refresh). */
esp_err_t ssd1680_write_framebuffer(const uint8_t *fb);

/* Trigger a panel update. Serialized on BUSY; rejects calls faster than
   SSD1680_MIN_REFRESH_INTERVAL_SEC with ESP_ERR_INVALID_STATE. */
esp_err_t ssd1680_refresh(ssd1680_refresh_mode_t mode);

/* Panel deep-sleep mode 1 (RAM retained). Call after every refresh. */
esp_err_t ssd1680_sleep(void);

/* Pure guard logic (ssd1680_guard.c) — exposed for host tests. */
bool ssd1680_refresh_allowed(int64_t now_sec, int64_t last_refresh_sec, int32_t min_interval_sec);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 7: Create `components/ssd1680/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "ssd1680.c" "ssd1680_guard.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_gpio
)
```

- [ ] **Step 8: Implement `components/ssd1680/ssd1680.c`**

Init/refresh sequences follow Adafruit_SSD1680 (Adafruit_EPD) and GxEPD2_290_BS, which drive this exact MagTag panel. Before finalizing, diff the command bytes against the reference:
`curl -s https://raw.githubusercontent.com/adafruit/Adafruit_EPD/master/src/drivers/Adafruit_SSD1680.cpp | less` — if bytes differ from below, prefer the reference and note the change in the commit message.

```c
#include "ssd1680.h"

#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ssd1680";

/* SSD1680 command set (datasheet §7) */
#define CMD_DRIVER_OUTPUT 0x01
#define CMD_DEEP_SLEEP 0x10
#define CMD_DATA_ENTRY 0x11
#define CMD_SW_RESET 0x12
#define CMD_TEMP_SENSOR 0x18
#define CMD_MASTER_ACTIVATE 0x20
#define CMD_DISP_UPDATE_CTRL1 0x21
#define CMD_DISP_UPDATE_CTRL2 0x22
#define CMD_WRITE_RAM_BW 0x24
#define CMD_WRITE_RAM_RED 0x26 /* holds "previous" frame for partial diff */
#define CMD_BORDER_WAVEFORM 0x3C
#define CMD_RAM_X_RANGE 0x44
#define CMD_RAM_Y_RANGE 0x45
#define CMD_RAM_X_COUNTER 0x4E
#define CMD_RAM_Y_COUNTER 0x4F

#define BUSY_TIMEOUT_MS 10000 /* full refresh ~3-4 s; init can be slower */

static spi_device_handle_t s_spi;
static ssd1680_pins_t s_pins;
static bool s_initialized;
static uint8_t s_fb_cache[SSD1680_FB_SIZE]; /* last frame, for 0x26 bookkeeping */

/* Survives deep sleep so the refresh-rate guard holds across wakes. */
static RTC_DATA_ATTR int64_t s_last_refresh_sec;

static esp_err_t busy_wait(void) {
    int waited = 0;
    while (gpio_get_level(s_pins.pin_busy)) { /* BUSY is active-high */
        if (waited >= BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY stuck high for %d ms — panel dead or disconnected", waited);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    return ESP_OK;
}

static esp_err_t write_cmd(uint8_t cmd) {
    gpio_set_level(s_pins.pin_dc, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t write_data(const uint8_t *data, size_t len) {
    gpio_set_level(s_pins.pin_dc, 1);
    spi_transaction_t t = {.length = len * 8, .tx_buffer = data};
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t cmd_with_data(uint8_t cmd, const uint8_t *data, size_t len) {
    esp_err_t ret = write_cmd(cmd);
    if (ret != ESP_OK)
        return ret;
    return write_data(data, len);
}

static void hw_reset(void) {
    gpio_set_level(s_pins.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_pins.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t ssd1680_init(const ssd1680_pins_t *pins) {
    s_pins = *pins;

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << pins->pin_dc) | (1ULL << pins->pin_rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio out");
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << pins->pin_busy),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "gpio busy");

    if (!s_spi) {
        spi_bus_config_t bus = {
            .sclk_io_num = pins->pin_sclk,
            .mosi_io_num = pins->pin_mosi,
            .miso_io_num = -1,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SSD1680_FB_SIZE,
        };
        ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");
        spi_device_interface_config_t dev = {
            .clock_speed_hz = 4 * 1000 * 1000, /* conservative for e-paper */
            .mode = 0,
            .spics_io_num = pins->pin_cs,
            .queue_size = 2,
        };
        ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "spi dev");
    }

    /* Mandatory after panel deep sleep (RAM is retained; registers are not). */
    hw_reset();
    ESP_RETURN_ON_ERROR(busy_wait(), TAG, "reset busy");
    ESP_RETURN_ON_ERROR(write_cmd(CMD_SW_RESET), TAG, "swreset");
    ESP_RETURN_ON_ERROR(busy_wait(), TAG, "swreset busy");

    /* 296 gates: 295 = 0x0127 */
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DRIVER_OUTPUT, (const uint8_t[]){0x27, 0x01, 0x00}, 3), TAG, "output");
    /* Data entry: x increment, y increment */
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DATA_ENTRY, (const uint8_t[]){0x03}, 1), TAG, "entry");
    /* RAM x window: 0 .. 15 (16 bytes = 128 px) */
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_RAM_X_RANGE, (const uint8_t[]){0x00, 0x0F}, 2), TAG, "ram x");
    /* RAM y window: 0 .. 295 */
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_RAM_Y_RANGE, (const uint8_t[]){0x00, 0x00, 0x27, 0x01}, 4), TAG, "ram y");
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_BORDER_WAVEFORM, (const uint8_t[]){0x05}, 1), TAG, "border");
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DISP_UPDATE_CTRL1, (const uint8_t[]){0x00, 0x80}, 2), TAG, "ctrl1");
    /* Internal temperature sensor */
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_TEMP_SENSOR, (const uint8_t[]){0x80}, 1), TAG, "temp");

    s_initialized = true;
    return ESP_OK;
}

static esp_err_t set_ram_counters(void) {
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_RAM_X_COUNTER, (const uint8_t[]){0x00}, 1), TAG, "x ctr");
    return cmd_with_data(CMD_RAM_Y_COUNTER, (const uint8_t[]){0x00, 0x00}, 2);
}

static esp_err_t write_ram(uint8_t ram_cmd, const uint8_t *fb) {
    ESP_RETURN_ON_ERROR(set_ram_counters(), TAG, "counters");
    ESP_RETURN_ON_ERROR(write_cmd(ram_cmd), TAG, "ram cmd");
    /* Panel RAM: 1 = white, 0 = black. Our fb: 1 = black — invert. */
    uint8_t row[SSD1680_WIDTH / 8];
    gpio_set_level(s_pins.pin_dc, 1);
    for (int y = 0; y < SSD1680_HEIGHT; y++) {
        const uint8_t *src = fb + y * sizeof(row);
        for (size_t i = 0; i < sizeof(row); i++)
            row[i] = (uint8_t)~src[i];
        spi_transaction_t t = {.length = sizeof(row) * 8, .tx_buffer = row};
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(s_spi, &t), TAG, "ram tx");
    }
    return ESP_OK;
}

esp_err_t ssd1680_write_framebuffer(const uint8_t *fb) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    memcpy(s_fb_cache, fb, SSD1680_FB_SIZE);
    return write_ram(CMD_WRITE_RAM_BW, fb);
}

esp_err_t ssd1680_refresh(ssd1680_refresh_mode_t mode) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    int64_t now = (int64_t)time(NULL);
    if (!ssd1680_refresh_allowed(now, s_last_refresh_sec, SSD1680_MIN_REFRESH_INTERVAL_SEC)) {
        ESP_LOGW(TAG, "refresh rejected: %lld s since last (min %d)", (long long)(now - s_last_refresh_sec),
                 SSD1680_MIN_REFRESH_INTERVAL_SEC);
        return ESP_ERR_INVALID_STATE;
    }

    if (mode == SSD1680_REFRESH_PARTIAL) {
        ESP_RETURN_ON_ERROR(cmd_with_data(CMD_BORDER_WAVEFORM, (const uint8_t[]){0x80}, 1), TAG, "border");
        ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DISP_UPDATE_CTRL2, (const uint8_t[]){0xFF}, 1), TAG, "ctrl2");
    } else {
        ESP_RETURN_ON_ERROR(cmd_with_data(CMD_BORDER_WAVEFORM, (const uint8_t[]){0x05}, 1), TAG, "border");
        ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DISP_UPDATE_CTRL2, (const uint8_t[]){0xF7}, 1), TAG, "ctrl2");
    }
    ESP_RETURN_ON_ERROR(write_cmd(CMD_MASTER_ACTIVATE), TAG, "activate");
    esp_err_t ret = busy_wait();
    if (ret != ESP_OK)
        return ret; /* timeout logged; caller proceeds to deep sleep */

    /* Bookkeeping for the next partial diff: previous-frame RAM = this frame */
    ESP_RETURN_ON_ERROR(write_ram(CMD_WRITE_RAM_RED, s_fb_cache), TAG, "prev frame");

    s_last_refresh_sec = now;
    return ESP_OK;
}

esp_err_t ssd1680_sleep(void) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    /* Mode 1: RAM retained — required for partial diffs across MCU deep sleep */
    return cmd_with_data(CMD_DEEP_SLEEP, (const uint8_t[]){0x01}, 1);
}
```

Note: `ESP_RETURN_ON_ERROR` needs `#include "esp_check.h"` — add it to the includes.

- [ ] **Step 9: Compile gate**

```bash
source $HOME/esp/esp-idf/export.sh && idf.py build
```
Expected: success. (Component is auto-discovered from `components/`; nothing links it yet — that's Task 9.)
Troubleshooting: if `esp_driver_spi`/`esp_driver_gpio` are unknown component names, use `REQUIRES driver` instead (IDF 6 keeps `driver` as an umbrella).

- [ ] **Step 10: Commit**

```bash
git add components/ssd1680
git commit -m "feat(ssd1680): SPI panel driver with full/partial refresh, busy timeout, RAM-retaining sleep"
```

---

### Task 7: LVGL 9 managed component + Kconfig

**Files:**
- Create: `main/idf_component.yml`
- Modify: `sdkconfig.defaults`, `main/CMakeLists.txt`

- [ ] **Step 1: Create `main/idf_component.yml`**

```yaml
dependencies:
  idf: ">=6.0"
  lvgl/lvgl: "^9.3.0"
```

- [ ] **Step 2: Append LVGL config to `sdkconfig.defaults`**

```
# LVGL: 1-bit rendering for the e-paper panel
CONFIG_LV_COLOR_DEPTH_1=y
CONFIG_LV_FONT_MONTSERRAT_12=y
CONFIG_LV_FONT_MONTSERRAT_28=y
CONFIG_LV_FONT_MONTSERRAT_48=y
CONFIG_LV_FONT_DEFAULT_MONTSERRAT_12=y
```

- [ ] **Step 3: Regenerate sdkconfig and build**

```bash
source $HOME/esp/esp-idf/export.sh
rm sdkconfig && idf.py build
```
Expected: component manager downloads `lvgl__lvgl` into `managed_components/`, build succeeds.
Troubleshooting: if a `CONFIG_LV_*` symbol is rejected, run `idf.py menuconfig`, find the real symbol under `Component config → LVGL`, and correct `sdkconfig.defaults` (LVGL renames Kconfig symbols occasionally).

- [ ] **Step 4: Commit** (include regenerated `sdkconfig`)

```bash
git add main/idf_component.yml sdkconfig.defaults sdkconfig
git commit -m "build(lvgl): add LVGL 9 managed component with I1 color depth and Montserrat fonts"
```

---

### Task 8: Extract pure layout math to `main/display_layout.c` (TDD)

**Files:**
- Create: `main/display_layout.c`
- Modify: `test/test_display/test_display.c`, `main/CMakeLists.txt`, `main/display.c` (remove stub helpers)

The current `test_display.c` tests *local stub copies* of the layout helpers — the real code was never under test. Fix that.

- [ ] **Step 1: Point the tests at the real implementation**

In `test/test_display/test_display.c`, replace the two `extern` declarations **and** both stub function bodies (everything from `extern uint16_t display_bar_fill_px...` through the end of the stub `display_format_remaining`) with:

```c
/* Single-TU compilation of the real layout math */
#include "../../main/display_layout.c"
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build test/build 2>&1 | tail -5
```
Expected: FAIL — `display_layout.c: No such file or directory`.

- [ ] **Step 3: Create `main/display_layout.c`** (moved verbatim from the old display.cpp logic)

```c
/* Pure layout math — no LVGL/ESP dependencies; host-tested via ctest. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BAR_FILL_MAX_PX 280u

uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec) {
    if (allocation_sec == 0 || remaining_sec <= 0)
        return 0;
    if ((uint32_t)remaining_sec >= allocation_sec)
        return BAR_FILL_MAX_PX;
    return (uint16_t)((uint32_t)remaining_sec * BAR_FILL_MAX_PX / allocation_sec);
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
```

Remove the stub versions of these two functions from `main/display.c` (Task 5 stub). Add `"display_layout.c"` to `SRCS` in `main/CMakeLists.txt`.

- [ ] **Step 4: Run tests**

```bash
cmake --build test/build && ctest --test-dir test/build --output-on-failure
```
Expected: 5/5 suites PASS (12 display assertions now exercise the real code).

- [ ] **Step 5: Firmware still builds**

```bash
source $HOME/esp/esp-idf/export.sh && idf.py build
```
Expected: success.

- [ ] **Step 6: Commit**

```bash
git add main/display_layout.c main/display.c main/CMakeLists.txt test/test_display/test_display.c
git commit -m "test(display): move layout math to display_layout.c and test the real implementation"
```

---

### Task 9: `main/display.c` — LVGL rendering + refresh policy

**Files:**
- Modify: `main/display.c` (replace the Task 5 stub wholesale), `main/CMakeLists.txt`

- [ ] **Step 1: Replace `main/display.c` with the LVGL implementation**

```c
#include "display.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
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
static lv_display_t *s_disp;
static bool s_initialized;
static ssd1680_refresh_mode_t s_pending_mode = SSD1680_REFRESH_FULL;

/* Bring-up knobs: if the image is rotated 180° or mirrored on hardware,
   flip these (see docs/hardware_smoke_test.md step 3). */
#define ROT_FLIP_X 0
#define ROT_FLIP_Y 1

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)area; /* RENDER_MODE_FULL: always the whole frame */
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

    if (ssd1680_write_framebuffer(s_panel_fb) == ESP_OK) {
        ssd1680_refresh(s_pending_mode); /* errors logged inside; never blocks past timeout */
    }
    ssd1680_sleep();
    lv_display_flush_ready(disp);
}

void display_init(void) {
    if (ssd1680_init(&PINS) != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed — continuing headless");
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

static void build_screen(const display_state_t *st) {
    lv_obj_t *scr = fresh_screen();
    char buf[64];
    struct tm tm;

    /* Row 0-18: date + time (left), last sync (right) — Montserrat 12 */
    localtime_r(&st->wall_time, &tm);
    strftime(buf, sizeof(buf), "%a %b %e  %I:%M %p", &tm);
    lv_obj_t *hdr = lv_label_create(scr);
    lv_label_set_text(hdr, buf);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 4, 3);

    if (st->last_sync_time > 0) {
        struct tm ts;
        localtime_r(&st->last_sync_time, &ts);
        strftime(buf, sizeof(buf), "Last sync: %I:%M %p", &ts);
    } else {
        snprintf(buf, sizeof(buf), "Last sync: --:--");
    }
    lv_obj_t *sync = lv_label_create(scr);
    lv_label_set_text(sync, buf);
    lv_obj_set_style_text_font(sync, &lv_font_montserrat_12, 0);
    lv_obj_align(sync, LV_ALIGN_TOP_RIGHT, -4, 3);

    /* Row 26-50: progress bar, 282x24 with 2 px border */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 284, 24);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 26);
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, display_bar_fill_px(st->remaining_sec, st->allocation_sec), LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);

    /* Row 58-86: remaining time, centred — Montserrat 28 */
    display_format_remaining(buf, sizeof(buf), st->remaining_sec);
    lv_obj_t *rem = lv_label_create(scr);
    lv_label_set_text(rem, buf);
    lv_obj_set_style_text_font(rem, &lv_font_montserrat_28, 0);
    lv_obj_align(rem, LV_ALIGN_TOP_MID, 0, 58);

    /* Row 100+: day-type + allocation (left), state (right) — Montserrat 12 */
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
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);

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
```

- [ ] **Step 2: Wire the component dependency**

In `main/CMakeLists.txt` `REQUIRES`, add `ssd1680`. (LVGL is injected automatically by the component manager via `idf_component.yml`.)

- [ ] **Step 3: Compile gate**

```bash
source $HOME/esp/esp-idf/export.sh && idf.py build
```
Expected: success.
Troubleshooting:
  - `lv_tick_set_cb` missing → LVGL version < 9.1; bump `main/idf_component.yml` to the newest 9.x.
  - `g_rtc_state` undeclared → `display.h` already includes `timer.h`, which declares it; check include order.
  - `%e` in strftime unsupported by newlib → replace `%e` with `%d` (accept a leading zero).
  - `%p` (AM/PM) renders empty under newlib's C locale → format hour manually: `int h12 = tm.tm_hour % 12; if (!h12) h12 = 12; snprintf(..., "%d:%02d %s", h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");`

- [ ] **Step 4: Host tests still pass**

```bash
cmake --build test/build && ctest --test-dir test/build --output-on-failure
```
Expected: 5/5 PASS.

- [ ] **Step 5: Commit**

```bash
git add main/display.c main/CMakeLists.txt
git commit -m "feat(display): LVGL 9 renderer over ssd1680 driver with partial/full refresh policy"
```

---

### Task 10: `buttons.c` — EXT1 deep-sleep wakeup

**Files:**
- Modify: `main/buttons.c`, `include/buttons.h`

- [ ] **Step 1: Rewrite `buttons_configure_wakeup` and `buttons_get_wakeup_button` in `main/buttons.c`**

Replace both functions (keep `buttons_init` and `buttons_is_pressed`) and add the `rtc_io` include:

```c
#include "driver/rtc_io.h"
```

```c
void buttons_configure_wakeup(void) {
    uint64_t mask = 0;
    for (int i = 0; i < 4; i++) {
        gpio_num_t pin = BTN_GPIOS[i];
        rtc_gpio_init(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(pin);   /* buttons are active-low; hold high in sleep */
        rtc_gpio_pulldown_dis(pin);
        mask |= 1ULL << pin;
    }
    /* GPIO wakeup (esp_sleep_enable_gpio_wakeup) is light-sleep-only on
       ESP32-S2 — deep sleep requires EXT1 on RTC-capable pins (11/12/14/15 all are). */
    esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ext1 wakeup config failed: %s", esp_err_to_name(ret));
    }
}

button_id_t buttons_get_wakeup_button(void) {
    if (!(esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1))) {
        return BTN_NONE;
    }

    /* EXT1 status latches which pin(s) triggered the wake */
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    for (int i = 0; i < 4; i++) {
        if (status & (1ULL << BTN_GPIOS[i])) {
            ESP_LOGI(TAG, "Wakeup button: %d (GPIO %d)", i, BTN_GPIOS[i]);
            return (button_id_t)i;
        }
    }

    /* Fallback: latch was empty — debounce then scan levels */
    esp_rom_delay_us(DEBOUNCE_US);
    for (int i = 0; i < 4; i++) {
        if (gpio_get_level(BTN_GPIOS[i]) == 0) {
            return (button_id_t)i;
        }
    }
    ESP_LOGW(TAG, "EXT1 wakeup but no button identified");
    return BTN_NONE;
}
```

Also prepend to `buttons_init` (before the `gpio_config` loop), so pins left in RTC mode by the previous sleep read correctly as digital GPIOs:

```c
    for (int i = 0; i < 4; i++) {
        rtc_gpio_deinit(BTN_GPIOS[i]);
    }
```

- [ ] **Step 2: Update `include/buttons.h` comment**

Change the `BTN_C` comment from `/* GPIO 14 — Cycle contrast */` to `/* GPIO 14 — unbound in v1 */`.

- [ ] **Step 3: Compile gate**

```bash
source $HOME/esp/esp-idf/export.sh && idf.py build
```
Expected: success.
Troubleshooting: if `esp_sleep_enable_ext1_wakeup_io` doesn't exist in IDF 6.0.1, use `esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW)`; if `esp_rom_delay_us` is undeclared, `#include "esp_rom_sys.h"`.

- [ ] **Step 4: Commit**

```bash
git add main/buttons.c include/buttons.h
git commit -m "fix(buttons): use EXT1 wakeup for deep sleep (GPIO wakeup is light-sleep-only on S2)"
```

---

### Task 11: `main/main.c` — full integration

**Files:**
- Modify: `main/main.c` (replace stub wholesale)

Invariants (from spec + CLAUDE.md — the Task 13 review checks these):
- `main.c` NEVER touches `g_rtc_state.expiry_wall_time` directly.
- `neopixel_init()` is the first peripheral call.
- `nvs_flash_init()` + `nvs_config_init_defaults()` precede any `nvs_config_get_*`.
- No WiFi/amp/NeoPixel left on at `esp_deep_sleep_start()`.

- [ ] **Step 1: Replace `main/main.c`**

```c
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio.h"
#include "buttons.h"
#include "display.h"
#include "neopixel.h"
#include "ntp.h"
#include "nvs_config.h"
#include "schedule.h"
#include "timer.h"

static const char *TAG = "main";

/* Compile-time timezone (ProductOverview §1) */
#define MAGTAG_TZ "EST5EDT,M3.2.0,M11.1.0"
#define WAKE_INTERVAL_US (55ULL * 1000000ULL)

static RTC_DATA_ATTR time_t s_last_ntp_sync;

static void enter_deep_sleep(void) {
    buttons_configure_wakeup();
    esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);
    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}

/* WiFi lifecycle is entirely inside ntp_sync(): init->connect->sync->deinit */
static esp_err_t try_ntp_sync(void) {
    esp_err_t ret = ntp_sync();
    if (ret == ESP_OK) {
        s_last_ntp_sync = time(NULL);
        timer_record_ntp_sync(s_last_ntp_sync);
    } else {
        ESP_LOGW(TAG, "NTP sync failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static display_state_t make_state(int32_t remaining, time_t now) {
    day_type_t dt = schedule_get_day_type(now);
    return (display_state_t){
        .remaining_sec = remaining,
        .allocation_sec = schedule_get_allocation_sec(dt),
        .timer_state = timer_get_state(),
        .day_type = dt,
        .wall_time = now,
        .last_sync_time = s_last_ntp_sync,
    };
}

/* ---- expiry alert -------------------------------------------------- */

static volatile bool s_audio_done;

static void neopixel_alert_task(void *arg) {
    (void)arg;
    neopixel_alert_start(); /* blocks until neopixel_stop() sets its flag */
    vTaskDelete(NULL);
}

static void audio_alert_task(void *arg) {
    (void)arg;
    audio_beep_sequence(); /* self-terminates after 5 cycles (~15 s) */
    s_audio_done = true;
    vTaskDelete(NULL);
}

static void run_expiry_alert(void) {
    s_audio_done = false;
    xTaskCreate(neopixel_alert_task, "np_alert", 2048, NULL, 5, NULL);
    xTaskCreate(audio_alert_task, "beep", 2048, NULL, 5, NULL);

    /* Poll for dismissal; cap slightly past the 15 s sequence */
    for (int i = 0; i < 160 && !s_audio_done; i++) {
        for (int b = 0; b < 4; b++) {
            if (buttons_is_pressed((button_id_t)b)) {
                ESP_LOGI(TAG, "Alert dismissed by button");
                i = 160; /* exit outer loop */
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    audio_stop();
    neopixel_stop();
    vTaskDelay(pdMS_TO_TICKS(100)); /* let alert tasks observe stop flags and exit */
}

/* ---- day rollover --------------------------------------------------- */

static void handle_day_rollover(time_t *now) {
    if (!timer_is_new_day(*now))
        return;
    ESP_LOGI(TAG, "Day rollover");
    /* Fail-open: reset to IDLE with today's allocation even if sync fails */
    try_ntp_sync();
    *now = time(NULL);
    timer_reset();
    timer_record_date(*now);
}

/* ---- wake handlers --------------------------------------------------- */

static void handle_timer_tick(void) {
    time_t now = time(NULL);
    handle_day_rollover(&now);

    if (timer_get_state() == TIMER_RUNNING && timer_needs_ntp_sync(now)) {
        try_ntp_sync();
        now = time(NULL);
    }

    timer_state_t before = timer_get_state();
    int32_t remaining = timer_tick(now);
    display_state_t st = make_state(remaining, now);

    if (timer_get_state() == TIMER_EXPIRED && before != TIMER_EXPIRED) {
        display_timesup();
        run_expiry_alert();
    } else if (timer_get_state() != before) {
        display_full_refresh(&st);
    } else {
        display_update(&st); /* partial; policy promotes every 5th to full */
    }
    enter_deep_sleep();
}

static void handle_button_wake(void) {
    time_t now = time(NULL);
    handle_day_rollover(&now);
    button_id_t btn = buttons_get_wakeup_button();
    timer_state_t before = timer_get_state();

    switch (btn) {
        case BTN_A:
            if (before == TIMER_RUNNING) {
                timer_pause(now);
            } else if (before == TIMER_IDLE) {
                /* NTP sync is mandatory before first start */
                if (try_ntp_sync() != ESP_OK) {
                    display_sync_failed();
                    enter_deep_sleep();
                }
                now = time(NULL);
                day_type_t dt = schedule_get_day_type(now);
                timer_start(now, (int32_t)schedule_get_allocation_sec(dt));
            } else if (before == TIMER_PAUSED) {
                /* Best-effort sync; drift self-corrects on next success */
                try_ntp_sync();
                now = time(NULL);
                timer_resume(now);
            }
            break;
        case BTN_B:
            timer_reset();
            timer_record_date(now);
            break;
        case BTN_D:
            try_ntp_sync();
            now = time(NULL);
            break;
        case BTN_C: /* unbound in v1 */
        case BTN_NONE:
        default:
            break;
    }

    int32_t remaining = timer_tick(now);
    display_state_t st = make_state(remaining, now);

    if (timer_get_state() == TIMER_EXPIRED && before != TIMER_EXPIRED) {
        display_timesup();
        run_expiry_alert();
    } else {
        display_full_refresh(&st); /* button wakes always full-refresh */
    }
    enter_deep_sleep();
}

void app_main(void) {
    /* MUST be first peripheral call: GPIO 21 power gate HIGH (NeoPixels off) */
    neopixel_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_config_init_defaults());

    setenv("TZ", MAGTAG_TZ, 1);
    tzset();

    buttons_init();
    audio_init();
    display_init();

    uint32_t causes = esp_sleep_get_wakeup_causes();
    ESP_LOGI(TAG, "Wakeup causes: 0x%08lx", (unsigned long)causes);

    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        handle_button_wake();
    } else {
        handle_timer_tick(); /* RTC timer wake AND cold boot */
    }
}
```

- [ ] **Step 2: Compile gate**

```bash
source $HOME/esp/esp-idf/export.sh && idf.py build
```
Expected: success.
Troubleshooting: `esp_sleep_get_wakeup_causes()` return type is `uint32_t` in IDF 6; if the compiler disagrees, match the header's type.

- [ ] **Step 3: Verify invariants**

```bash
grep -n "expiry_wall_time\|g_rtc_state" main/main.c
```
Expected: no matches.

- [ ] **Step 4: Commit**

```bash
git add main/main.c
git commit -m "feat(main): wire wake dispatch, day rollover, NTP gating, and expiry alert"
```

---

### Task 12: Documentation

**Files:**
- Create: `docs/hardware_smoke_test.md`
- Modify: `docs/developer_setup.md`, `.claude/CLAUDE.md`

- [ ] **Step 1: Rewrite `docs/developer_setup.md`**

Keep the devcontainer/USB sections' structure but replace all PlatformIO content:
- Prerequisites: unchanged except remove PlatformIO extension mention.
- Getting started: ESP-IDF v6.0.1 + the ESP32-S2 toolchain are baked into the devcontainer image (Dockerfile `RUN` layer) — the first image build downloads ~2 GB once; container rebuilds reuse the cached layer and start ready.
- Building: `source ~/esp/esp-idf/export.sh` then `idf.py build` / `idf.py -p /dev/ttyACM0 flash monitor`.
- Tests: `cmake -S test -B test/build && cmake --build test/build && ctest --test-dir test/build --output-on-failure`.
- Project layout table: `main/` (was `src/`), `components/ssd1680/`, `test/` (ctest + vendored Unity), remove `platformio.ini` and `lib/` rows, remove the `project/` path prefix (it is wrong — the repo root is the project).
- Toolchain cache section: replace the `pio-packages` volume text with a note that the toolchain lives in the image layer; `docker builder prune` reclaims it if the image is rebuilt from scratch.

- [ ] **Step 2: Create `docs/hardware_smoke_test.md`**

```markdown
# Hardware Smoke Test — MagTag Screen Timer

Run in order with the MagTag on USB and `idf.py -p /dev/ttyACM0 flash monitor`.
Prerequisite: real WiFi credentials in NVS (set `NVS_DEFAULT_WIFI_SSID/PASS` in
`include/nvs_defaults.h` temporarily, or pre-write NVS; do NOT commit credentials).

1. [ ] **Flash + cold boot**: monitor shows boot, `Wakeup causes: 0x0`, no panics.
2. [ ] **First screen**: IDLE layout renders — date/time header, full bar, allocation
       + `IDLE` footer. If garbled/blank note symptom:
       - all-black or inverted → flip the `!bit` test in `display.c` flush_cb
       - rotated/mirrored → toggle `ROT_FLIP_X` / `ROT_FLIP_Y` in `display.c`
       - shifted columns → panel RAM x-offset; adjust `CMD_RAM_X_RANGE` start byte
3. [ ] **55 s tick**: device deep-sleeps, wakes ~55 s later, partial refresh (no flash).
4. [ ] **Anti-ghosting**: every 5th wake does a full refresh (visible flash).
5. [ ] **Button A (start)**: WiFi joins, SNTP syncs, header shows sync time, bar full,
       state `RUNNING`. With bad WiFi creds: "No sync - check WiFi" screen, still IDLE.
6. [ ] **Countdown**: remaining decreases ~55 s per wake; `< 5 min` shows `M min S sec`.
7. [ ] **Button A (pause/resume)**: pause shows `PAUSED`, remaining freezes across
       wakes; resume continues from the frozen value.
8. [ ] **Button B (reset)**: returns to IDLE with today's full allocation.
9. [ ] **Button D (force sync)**: WiFi cycle + full refresh; sync time updates.
10. [ ] **Button C**: no effect (unbound in v1).
11. [ ] **Expiry**: set a 1-2 min allocation (temporarily lower `weekday_min` default),
        let it expire: TIME'S UP screen, 3 beeps x 5 cycles, red NeoPixel pulse;
        any button stops the alert immediately; device returns to deep sleep after.
12. [ ] **Day rollover**: set the RTC date near midnight (or fake `last_date`), confirm
        wake after midnight re-syncs and resets to IDLE with the new day's allocation.
13. [ ] **Every button wakes from deep sleep** (A, B, C, D each wake the device;
        C just redraws).
14. [ ] **Panel protection**: mash buttons rapidly — refreshes serialize, log shows
        `refresh rejected` if under 1 s apart, no crash.
15. [ ] **Idle current** (optional, needs meter): deep-sleep current < 1 mA.

Record failures with the monitor log snippet and the step number.
```

- [ ] **Step 3: Update `.claude/CLAUDE.md`**

- Module table: `display.cpp/h` row becomes `display.c/h — LVGL 9 layout rendering; partial/full refresh policy` plus new rows `display_layout.c — pure layout math (host-tested)` and `components/ssd1680/ — SSD1680 SPI driver; refresh guard; panel sleep`.
- "Key Implementation Constraints": replace the LovyanGFX bullet with "Display stack: LVGL 9 (managed component, `LV_COLOR_FORMAT_I1`) over the custom `components/ssd1680` driver. The project is pure C."; replace the deep-sleep bullet with "`esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW)` for buttons + 55 s timer wakeup".
- TDD section: replace `pio test -e native` with the ctest commands; add `display_layout.c` and the ssd1680 guard to the priority list.
- RTC struct comment: unchanged (struct still matches).
- Remove stale `Cycle display contrast` from any button table (buttons: C = unbound in v1).

- [ ] **Step 4: Commit**

```bash
git add docs/developer_setup.md docs/hardware_smoke_test.md .claude/CLAUDE.md
git commit -m "docs: idf.py/ctest workflow, hardware smoke-test checklist, CLAUDE.md refresh"
```

---

### Task 13: Final verification

- [ ] **Step 1: Clean firmware build**

```bash
source $HOME/esp/esp-idf/export.sh && idf.py fullclean && idf.py build
```
Expected: `Project build complete`, binary size report shown.

- [ ] **Step 2: All host tests**

```bash
rm -rf test/build && cmake -S test -B test/build && cmake --build test/build && ctest --test-dir test/build --output-on-failure
```
Expected: 5/5 PASS.

- [ ] **Step 3: Lint/hooks**

```bash
pre-commit run --all-files
```
Expected: all hooks pass (clang-format may rewrite files — re-add and re-run until clean).

- [ ] **Step 4: Spec invariant review** (self-check against the spec):
  - `grep -rn "expiry_wall_time" main/main.c main/display.c components/` → no matches.
  - `grep -n "neopixel_init" main/main.c` → first call in `app_main`.
  - `grep -rn "esp_sleep_enable_gpio_wakeup" main/` → no matches (EXT1 only).
  - `grep -rn "LovyanGFX\|lovyan" . --include="*.c*" --include="*.h" --include="*.ini" --include="*.txt"` → no matches.
  - `ssd1680_sleep` called in every flush path (`main/display.c` flush_cb).

- [ ] **Step 5: Commit any remaining fixes, then use superpowers:requesting-code-review**

Focus areas for review: flush_cb transpose math, refresh-guard RTC persistence, EXT1 mask/pull-up correctness, alert task teardown before `esp_deep_sleep_start()`, NTP-failure paths.

- [ ] **Step 6: Finish the branch** with superpowers:finishing-a-development-branch (merge to `integration`; hardware smoke test happens on `integration` before any merge to `main`, per the original workstream design).

---

## Out of Plan (deferred to the hardware session)

- Executing `docs/hardware_smoke_test.md` (user's hardware access is scheduled/later).
- Panel orientation/offset/inversion tuning (knobs are in place: `ROT_FLIP_X/Y`, RAM x-range, bit inversion).
- Real WiFi credentials (placeholder stays in `nvs_defaults.h`; never commit real ones).
