# Stream 1 — Test Harness & HAL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Configure PlatformIO's native test environment and create mock HAL implementations so that schedule, timer, and nvs_config modules can be unit-tested on the host without any ESP32 hardware.

**Architecture:** The native test build uses `-include esp_compat.h` to provide ESP-IDF type aliases (`esp_err_t`, `RTC_DATA_ATTR`) without the actual ESP-IDF. Each TDD module's test file includes its source directly (`#include "../../src/module.c"`) and the mock HAL files (`#include "mock_hal_nvs.c"`) — everything in one translation unit, no linker magic needed. Scaffold tests are trivially-passing to confirm the harness compiles and runs.

**Tech Stack:** PlatformIO native platform, Unity test framework, C99

**Prerequisite:** Stream 0 (`feature/foundation`) merged to `integration`.

---

## Files

| Action | Path |
|---|---|
| Modify | `platformio.ini` |
| Create | `test/mocks/esp_compat.h` |
| Create | `test/mocks/mock_hal_time.h` |
| Create | `test/mocks/mock_hal_time.c` |
| Create | `test/mocks/mock_hal_nvs.h` |
| Create | `test/mocks/mock_hal_nvs.c` |
| Create | `test/test_schedule/test_schedule.c` |
| Create | `test/test_timer/test_timer.c` |
| Create | `test/test_nvs_config/test_nvs_config.c` |
| Delete | `test/.gitkeep` |

---

## Task 1: Create branch

- [ ] **Step 1: Branch from integration**

```bash
git fetch origin
git checkout integration
git checkout -b feature/test-harness
```

---

## Task 2: Add native environment to platformio.ini

**Files:** Modify `platformio.ini`

- [ ] **Step 1: Add [env:native] section**

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

[env:native]
platform = native
test_framework = unity
build_flags =
    -DNATIVE
    -std=c11
    -include ${PROJECT_DIR}/test/mocks/esp_compat.h
    -I ${PROJECT_DIR}/include
    -I ${PROJECT_DIR}/test/mocks
```

- [ ] **Step 2: Commit**

```bash
git add platformio.ini
git commit -m "build: add native test environment for host-based unit tests"
```

---

## Task 3: Create ESP compatibility header

**Files:** Create `test/mocks/esp_compat.h`

This header is force-included into every file compiled in the native environment. It provides ESP-IDF type aliases so source files don't need conditional includes.

- [ ] **Step 1: Create the file**

```c
/* test/mocks/esp_compat.h
 * Force-included in native builds via -include build flag.
 * Provides minimal ESP-IDF type aliases for host-based unit tests.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>

typedef int esp_err_t;

#define ESP_OK                   0
#define ESP_FAIL                 (-1)
#define ESP_ERR_NVS_NOT_FOUND    0x1102
#define ESP_ERR_NVS_INVALID_NAME 0x1105
#define ESP_ERR_WIFI_NOT_CONNECT 0x3002

/* RTC_DATA_ATTR places data in RTC slow memory on ESP32.
   On native, it's a no-op — g_rtc_state is a regular static. */
#define RTC_DATA_ATTR
```

- [ ] **Step 2: Commit**

```bash
git add test/mocks/esp_compat.h
git commit -m "test: add ESP-IDF compatibility header for native builds"
```

---

## Task 4: Create mock HAL implementations

**Files:** Create `test/mocks/mock_hal_time.h`, `test/mocks/mock_hal_time.c`, `test/mocks/mock_hal_nvs.h`, `test/mocks/mock_hal_nvs.c`

- [ ] **Step 1: Create test/mocks/mock_hal_time.h**

```c
#pragma once
#include "hal_time.h"

/* Set the time returned by hal_time_now() for the current test. */
void mock_time_set(time_t t);
```

- [ ] **Step 2: Create test/mocks/mock_hal_time.c**

```c
#include "mock_hal_time.h"

static time_t s_mock_time = 1767225600; /* 2026-01-01 00:00:00 UTC */

void mock_time_set(time_t t)
{
    s_mock_time = t;
}

time_t hal_time_now(void)
{
    return s_mock_time;
}
```

- [ ] **Step 3: Create test/mocks/mock_hal_nvs.h**

```c
#pragma once
#include "hal_nvs.h"

/* Reset all stored key-value pairs. Call in setUp() before each test. */
void mock_nvs_reset(void);
```

- [ ] **Step 4: Create test/mocks/mock_hal_nvs.c**

```c
#include "mock_hal_nvs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_ENTRIES  64
#define MAX_KEY_LEN  16
#define MAX_VAL_SIZE 2048

typedef struct {
    char    key[MAX_KEY_LEN];
    uint8_t data[MAX_VAL_SIZE];
    size_t  len;
    int     used;
} Entry;

static Entry s_store[MAX_ENTRIES];

void mock_nvs_reset(void)
{
    memset(s_store, 0, sizeof(s_store));
}

static Entry *find_entry(const char *key)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_store[i].used && strcmp(s_store[i].key, key) == 0) {
            return &s_store[i];
        }
    }
    return NULL;
}

static Entry *alloc_entry(const char *key)
{
    Entry *e = find_entry(key);
    if (e) return e;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!s_store[i].used) {
            s_store[i].used = 1;
            strncpy(s_store[i].key, key, MAX_KEY_LEN - 1);
            s_store[i].key[MAX_KEY_LEN - 1] = '\0';
            return &s_store[i];
        }
    }
    return NULL; /* store full */
}

esp_err_t hal_nvs_read_u16(const char *key, uint16_t *out)
{
    Entry *e = find_entry(key);
    if (!e || e->len != sizeof(uint16_t)) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, e->data, sizeof(uint16_t));
    return ESP_OK;
}

esp_err_t hal_nvs_write_u16(const char *key, uint16_t val)
{
    Entry *e = alloc_entry(key);
    if (!e) return ESP_FAIL;
    memcpy(e->data, &val, sizeof(uint16_t));
    e->len = sizeof(uint16_t);
    return ESP_OK;
}

esp_err_t hal_nvs_read_str(const char *key, char *buf, size_t len)
{
    Entry *e = find_entry(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    size_t copy = (e->len < len) ? e->len : len - 1;
    memcpy(buf, e->data, copy);
    buf[copy] = '\0';
    return ESP_OK;
}

esp_err_t hal_nvs_write_str(const char *key, const char *val)
{
    Entry *e = alloc_entry(key);
    if (!e) return ESP_FAIL;
    e->len = strlen(val);
    if (e->len >= MAX_VAL_SIZE) e->len = MAX_VAL_SIZE - 1;
    memcpy(e->data, val, e->len);
    e->data[e->len] = '\0';
    return ESP_OK;
}

esp_err_t hal_nvs_read_blob(const char *key, void *buf, size_t *len)
{
    Entry *e = find_entry(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) { *len = e->len; return ESP_FAIL; }
    memcpy(buf, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t hal_nvs_write_blob(const char *key, const void *buf, size_t len)
{
    Entry *e = alloc_entry(key);
    if (!e) return ESP_FAIL;
    if (len > MAX_VAL_SIZE) return ESP_FAIL;
    memcpy(e->data, buf, len);
    e->len = len;
    return ESP_OK;
}
```

- [ ] **Step 5: Commit**

```bash
git add test/mocks/
git commit -m "test: add mock HAL implementations for time and NVS"
```

---

## Task 5: Create scaffold test files

**Files:** Create `test/test_schedule/test_schedule.c`, `test/test_timer/test_timer.c`, `test/test_nvs_config/test_nvs_config.c`

These are trivial passing tests. Their only purpose is to confirm the native build compiles and the test runner works. Real tests are added in Streams 2a/2b/2c.

- [ ] **Step 1: Create test/test_schedule/test_schedule.c**

```c
#include <unity.h>
/* Pull in mock implementations (single-TU compilation) */
#include "mock_hal_time.c"
#include "mock_hal_nvs.c"
/* Pull in source under test */
#include "../../src/schedule.c"

void setUp(void)
{
    mock_nvs_reset();
    mock_time_set(1767225600); /* 2026-01-01 00:00:00 UTC */
}

void tearDown(void) {}

void test_harness_scaffold(void)
{
    TEST_ASSERT_TRUE(1); /* harness check — always passes */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_harness_scaffold);
    return UNITY_END();
}
```

- [ ] **Step 2: Create test/test_timer/test_timer.c**

```c
#include <unity.h>
#include "mock_hal_time.c"
#include "../../src/timer.c"

void setUp(void)
{
    timer_reset();
    mock_time_set(1767225600);
}

void tearDown(void) {}

void test_harness_scaffold(void)
{
    TEST_ASSERT_TRUE(1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_harness_scaffold);
    return UNITY_END();
}
```

- [ ] **Step 3: Create test/test_nvs_config/test_nvs_config.c**

```c
#include <unity.h>
#include "mock_hal_nvs.c"
#include "../../src/nvs_config.c"

void setUp(void)
{
    mock_nvs_reset();
}

void tearDown(void) {}

void test_harness_scaffold(void)
{
    TEST_ASSERT_TRUE(1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_harness_scaffold);
    return UNITY_END();
}
```

- [ ] **Step 4: Remove the test/ root gitkeep**

```bash
git rm test/.gitkeep
```

- [ ] **Step 5: Commit**

```bash
git add test/
git commit -m "test: add scaffold test files for schedule, timer, nvs_config"
```

---

## Task 6: Compile gate — verify pio test -e native green

- [ ] **Step 1: Run the native tests**

```bash
pio test -e native
```

Expected output:
```
Testing...
test/test_schedule/test_schedule.c:XX:test_harness_scaffold:PASS
test/test_timer/test_timer.c:XX:test_harness_scaffold:PASS
test/test_nvs_config/test_nvs_config.c:XX:test_harness_scaffold:PASS

-----------------------
3 Tests 0 Failures 0 Ignored
OK
================================= [PASSED] Took X.XXX seconds =================================
```

**Troubleshooting:**

- `esp_err_t` undeclared: check that `build_flags` in `[env:native]` includes `-include ${PROJECT_DIR}/test/mocks/esp_compat.h`
- `hal_nvs_read_u16` undeclared: check that `mock_hal_nvs.c` is included before `nvs_config.c` in the test file
- `RTC_DATA_ATTR` error: ensure `esp_compat.h` defines `#define RTC_DATA_ATTR` (empty)
- `esp_attr.h` not found: the `#include "esp_attr.h"` in `timer.h` is guarded by `#ifndef NATIVE` — verify the guard is present
- Include path errors: verify `-I ${PROJECT_DIR}/include` and `-I ${PROJECT_DIR}/test/mocks` are in `build_flags`

Do NOT proceed until all 3 scaffold tests pass.

---

## Task 7: Code review

- [ ] **Step 1: Run adversarial code review**

Use `superpowers:requesting-code-review` skill. Focus areas:
- Mock NVS store handles max-length keys and values without buffer overflow
- `alloc_entry()` returns NULL when store is full (not a silent corruption)
- Mock time default (1767225600) is a valid future timestamp, not 0 or negative
- `hal_nvs_read_str` null-terminates output even when entry fills the buffer
- `hal_nvs_read_blob` sets `*len` correctly when buffer too small
- No ESP-IDF headers accidentally included in mock files

Fix any issues before merging.

---

## Task 8: Merge to integration

- [ ] **Step 1: Push and merge**

```bash
git push -u origin feature/test-harness
git checkout integration
git merge --no-ff feature/test-harness -m "feat: native test harness with mock HAL for schedule/timer/nvs_config"
git push origin integration
```
