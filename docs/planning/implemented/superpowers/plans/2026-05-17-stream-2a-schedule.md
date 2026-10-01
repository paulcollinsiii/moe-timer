# Stream 2a — schedule.c Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `schedule.c` using TDD — all tests written and failing before implementation begins. The module determines whether today is a weekday, weekend, or holiday, and returns the NVS-configured allocation in seconds.

**Architecture:** `schedule.c` is pure logic with no ESP-IDF dependency. It reads NVS via `hal_nvs_*()` functions (mocked in tests). Day-type determination uses POSIX `localtime_r()` on the injected timestamp. Holiday matching is a substring search against a newline-delimited blob from NVS. Tests run on native (host) using `pio test -e native`.

**Tech Stack:** C99, Unity test framework, PlatformIO native, mock HAL from Stream 1

**Prerequisite:** Stream 1 (`feature/test-harness`) merged to `integration`.

**Reference timestamps used in tests (all UTC — set `TZ=UTC0` in setUp):**
- `1767225600` = 2026-01-01 Thu (New Year's Day)
- `1767398400` = 2026-01-03 Sat
- `1767484800` = 2026-01-04 Sun
- `1767571200` = 2026-01-05 Mon
- `1767657600` = 2026-01-06 Tue

---

## Files

| Action | Path |
|---|---|
| Modify | `test/test_schedule/test_schedule.c` |
| Modify | `src/schedule.c` |

`include/schedule.h` and `include/hal_nvs.h` are already created in Stream 0 — do not modify their signatures.

---

## Task 1: Create branch

- [ ] **Step 1: Branch from integration**

```bash
git fetch origin
git checkout integration
git checkout -b feature/schedule
```

---

## Task 2: Write failing tests

**Files:** Replace `test/test_schedule/test_schedule.c` with full test suite

- [ ] **Step 1: Write all tests (they will fail until implementation)**

Replace the full content of `test/test_schedule/test_schedule.c`:

```c
#include <unity.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Single-TU compilation: pull in mocks then source under test */
#include "mock_hal_time.c"
#include "mock_hal_nvs.c"
#include "../../src/schedule.c"

/* ------------------------------------------------------------------ */
/* setUp / tearDown                                                     */
/* ------------------------------------------------------------------ */

void setUp(void)
{
    mock_nvs_reset();
    /* Use UTC so timestamps map to predictable dates regardless of host TZ */
    setenv("TZ", "UTC0", 1);
    tzset();
    /* Default: weekday allocation 60 min, weekend 120, holiday 120 */
    hal_nvs_write_u16("weekday_min", 60);
    hal_nvs_write_u16("weekend_min", 120);
    hal_nvs_write_u16("holiday_min", 120);
    /* Default holiday blob: just New Year's Day 2026 */
    const char *holidays = "2026-01-01\n";
    hal_nvs_write_blob("holidays", holidays, strlen(holidays));
    /* Default time: Monday 2026-01-05 */
    mock_time_set(1767571200);
}

void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* schedule_is_holiday — pure string search, no HAL                    */
/* ------------------------------------------------------------------ */

void test_is_holiday_match(void)
{
    const char *blob = "2026-01-01\n2026-07-04\n2026-12-25\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-01-01", blob, strlen(blob)));
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-07-04", blob, strlen(blob)));
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-12-25", blob, strlen(blob)));
}

void test_is_holiday_no_match(void)
{
    const char *blob = "2026-01-01\n2026-07-04\n";
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-06-15", blob, strlen(blob)));
    TEST_ASSERT_FALSE(schedule_is_holiday("2025-01-01", blob, strlen(blob)));
}

void test_is_holiday_partial_date_not_matched(void)
{
    /* "2026-01-0" must NOT match "2026-01-01" */
    const char *blob = "2026-01-01\n";
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-01-0", blob, strlen(blob)));
}

void test_is_holiday_windows_line_endings(void)
{
    /* \r\n blobs should still match full dates */
    const char *blob = "2026-01-01\r\n2026-07-04\r\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-01-01", blob, strlen(blob)));
}

void test_is_holiday_trailing_newline(void)
{
    const char *blob = "2026-12-25\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-12-25", blob, strlen(blob)));
}

/* ------------------------------------------------------------------ */
/* schedule_get_day_type                                               */
/* ------------------------------------------------------------------ */

void test_weekday_monday(void)
{
    mock_time_set(1767571200); /* 2026-01-05 Mon */
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
}

void test_weekday_friday(void)
{
    mock_time_set(1767830400); /* 2026-01-09 Fri */
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
}

void test_weekend_saturday(void)
{
    mock_time_set(1767398400); /* 2026-01-03 Sat */
    TEST_ASSERT_EQUAL(DAY_WEEKEND, schedule_get_day_type(hal_time_now()));
}

void test_weekend_sunday(void)
{
    mock_time_set(1767484800); /* 2026-01-04 Sun */
    TEST_ASSERT_EQUAL(DAY_WEEKEND, schedule_get_day_type(hal_time_now()));
}

void test_holiday_new_years_day(void)
{
    mock_time_set(1767225600); /* 2026-01-01 Thu — in holiday blob */
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

void test_holiday_takes_priority_over_weekend(void)
{
    /* Put a Saturday in the holiday blob; should return HOLIDAY not WEEKEND */
    const char *blob = "2026-01-03\n"; /* Sat */
    hal_nvs_write_blob("holidays", blob, strlen(blob));
    mock_time_set(1767398400); /* 2026-01-03 Sat */
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

void test_non_holiday_weekday_not_holiday(void)
{
    mock_time_set(1767571200); /* 2026-01-05 Mon — not in blob */
    TEST_ASSERT_NOT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

/* ------------------------------------------------------------------ */
/* schedule_get_allocation_sec                                          */
/* ------------------------------------------------------------------ */

void test_allocation_weekday(void)
{
    hal_nvs_write_u16("weekday_min", 60);
    TEST_ASSERT_EQUAL_UINT32(3600, schedule_get_allocation_sec(DAY_WEEKDAY));
}

void test_allocation_weekend(void)
{
    hal_nvs_write_u16("weekend_min", 90);
    TEST_ASSERT_EQUAL_UINT32(5400, schedule_get_allocation_sec(DAY_WEEKEND));
}

void test_allocation_holiday(void)
{
    hal_nvs_write_u16("holiday_min", 120);
    TEST_ASSERT_EQUAL_UINT32(7200, schedule_get_allocation_sec(DAY_HOLIDAY));
}

void test_allocation_missing_key_falls_back_to_default(void)
{
    /* Don't write weekday_min — should fall back to NVS_DEFAULT_WEEKDAY_MIN */
    mock_nvs_reset();
    uint32_t alloc = schedule_get_allocation_sec(DAY_WEEKDAY);
    TEST_ASSERT_EQUAL_UINT32(NVS_DEFAULT_WEEKDAY_MIN * 60, alloc);
}

/* ------------------------------------------------------------------ */
/* Runner                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_is_holiday_match);
    RUN_TEST(test_is_holiday_no_match);
    RUN_TEST(test_is_holiday_partial_date_not_matched);
    RUN_TEST(test_is_holiday_windows_line_endings);
    RUN_TEST(test_is_holiday_trailing_newline);
    RUN_TEST(test_weekday_monday);
    RUN_TEST(test_weekday_friday);
    RUN_TEST(test_weekend_saturday);
    RUN_TEST(test_weekend_sunday);
    RUN_TEST(test_holiday_new_years_day);
    RUN_TEST(test_holiday_takes_priority_over_weekend);
    RUN_TEST(test_non_holiday_weekday_not_holiday);
    RUN_TEST(test_allocation_weekday);
    RUN_TEST(test_allocation_weekend);
    RUN_TEST(test_allocation_holiday);
    RUN_TEST(test_allocation_missing_key_falls_back_to_default);
    return UNITY_END();
}
```

- [ ] **Step 2: Run tests — verify they FAIL**

```bash
pio test -e native -f test_schedule
```

Expected: multiple FAIL lines. If any test passes unexpectedly, check that the stub implementation in `src/schedule.c` is truly empty (returning `DAY_WEEKDAY` and `false` for everything).

---

## Task 3: Implement schedule_is_holiday

**Files:** Modify `src/schedule.c`

- [ ] **Step 1: Implement the pure string-search function**

Replace the `schedule_is_holiday` stub in `src/schedule.c`:

```c
bool schedule_is_holiday(const char *date_str, const char *blob, size_t blob_len)
{
    /* date_str is exactly "YYYY-MM-DD" (10 chars).
       blob is newline-delimited; may have \r\n line endings. */
    size_t date_len = strlen(date_str);
    if (date_len != 10) return false;

    const char *p = blob;
    const char *end = blob + blob_len;
    while (p < end) {
        /* Find next newline or end */
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        /* Strip trailing \r */
        const char *line_end = nl;
        if (line_end > p && *(line_end - 1) == '\r') line_end--;
        /* Compare */
        size_t line_len = (size_t)(line_end - p);
        if (line_len == date_len && memcmp(p, date_str, date_len) == 0) {
            return true;
        }
        p = nl + 1; /* skip past newline */
    }
    return false;
}
```

- [ ] **Step 2: Run is_holiday tests — verify they pass**

```bash
pio test -e native -f test_schedule
```

Expected: `test_is_holiday_*` tests PASS. Other tests still FAIL.

- [ ] **Step 3: Commit**

```bash
git add src/schedule.c test/test_schedule/test_schedule.c
git commit -m "test(schedule): add full test suite; feat(schedule): implement schedule_is_holiday"
```

---

## Task 4: Implement schedule_get_day_type

**Files:** Modify `src/schedule.c`

- [ ] **Step 1: Add required includes to schedule.c**

At the top of `src/schedule.c`, ensure these includes are present:

```c
#include "schedule.h"
#include "hal_nvs.h"
#include "hal_time.h"
#include "nvs_defaults.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
```

- [ ] **Step 2: Implement schedule_get_day_type**

```c
day_type_t schedule_get_day_type(time_t now)
{
    /* Convert to local time using the POSIX TZ string (set at boot). */
    struct tm tm_local;
    localtime_r(&now, &tm_local);

    /* Format date as "YYYY-MM-DD" */
    char date_str[11];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
             tm_local.tm_year + 1900,
             tm_local.tm_mon + 1,
             tm_local.tm_mday);

    /* Check holiday blob from NVS */
    char blob[2048];
    size_t blob_len = sizeof(blob);
    esp_err_t ret = hal_nvs_read_blob("holidays", blob, &blob_len);
    if (ret == ESP_OK && schedule_is_holiday(date_str, blob, blob_len)) {
        return DAY_HOLIDAY;
    }

    /* Check weekend (0 = Sunday, 6 = Saturday in struct tm) */
    if (tm_local.tm_wday == 0 || tm_local.tm_wday == 6) {
        return DAY_WEEKEND;
    }

    return DAY_WEEKDAY;
}
```

- [ ] **Step 3: Run day-type tests — verify they pass**

```bash
pio test -e native -f test_schedule
```

Expected: all `test_weekday_*`, `test_weekend_*`, `test_holiday_*` tests PASS. Allocation tests still FAIL.

---

## Task 5: Implement schedule_get_allocation_sec

**Files:** Modify `src/schedule.c`

- [ ] **Step 1: Implement the allocation lookup**

```c
uint32_t schedule_get_allocation_sec(day_type_t day_type)
{
    const char *key;
    uint16_t default_min;

    switch (day_type) {
    case DAY_WEEKEND:
        key = "weekend_min";
        default_min = NVS_DEFAULT_WEEKEND_MIN;
        break;
    case DAY_HOLIDAY:
        key = "holiday_min";
        default_min = NVS_DEFAULT_HOLIDAY_MIN;
        break;
    case DAY_WEEKDAY:
    default:
        key = "weekday_min";
        default_min = NVS_DEFAULT_WEEKDAY_MIN;
        break;
    }

    uint16_t minutes = default_min;
    hal_nvs_read_u16(key, &minutes); /* silently keeps default on miss */
    return (uint32_t)minutes * 60u;
}
```

- [ ] **Step 2: Run all tests — verify all pass**

```bash
pio test -e native -f test_schedule
```

Expected:
```
test/test_schedule/test_schedule.c:XX:test_is_holiday_match:PASS
test/test_schedule/test_schedule.c:XX:test_is_holiday_no_match:PASS
test/test_schedule/test_schedule.c:XX:test_is_holiday_partial_date_not_matched:PASS
test/test_schedule/test_schedule.c:XX:test_is_holiday_windows_line_endings:PASS
test/test_schedule/test_schedule.c:XX:test_is_holiday_trailing_newline:PASS
test/test_schedule/test_schedule.c:XX:test_weekday_monday:PASS
test/test_schedule/test_schedule.c:XX:test_weekday_friday:PASS
test/test_schedule/test_schedule.c:XX:test_weekend_saturday:PASS
test/test_schedule/test_schedule.c:XX:test_weekend_sunday:PASS
test/test_schedule/test_schedule.c:XX:test_holiday_new_years_day:PASS
test/test_schedule/test_schedule.c:XX:test_holiday_takes_priority_over_weekend:PASS
test/test_schedule/test_schedule.c:XX:test_non_holiday_weekday_not_holiday:PASS
test/test_schedule/test_schedule.c:XX:test_allocation_weekday:PASS
test/test_schedule/test_schedule.c:XX:test_allocation_weekend:PASS
test/test_schedule/test_schedule.c:XX:test_allocation_holiday:PASS
test/test_schedule/test_schedule.c:XX:test_allocation_missing_key_falls_back_to_default:PASS

-----------------------
16 Tests 0 Failures 0 Ignored
OK
```

- [ ] **Step 3: Commit**

```bash
git add src/schedule.c
git commit -m "feat(schedule): implement get_day_type and get_allocation_sec"
```

---

## Task 6: Verify magtag build still compiles

- [ ] **Step 1: Run pio run to check ESP-IDF compile**

```bash
pio run
```

Expected: `[SUCCESS]`. If schedule.c now has a compile error for magtag that didn't exist before, fix it (typically a missing include or an ESP-IDF function mismatch).

---

## Task 7: Code review

- [ ] **Step 1: Run adversarial code review**

Use `superpowers:requesting-code-review` skill. Focus areas:
- `schedule_is_holiday`: buffer overrun if `blob_len` is larger than expected? (No — we read char-by-char and compare by pointer arithmetic)
- `localtime_r` on native: does it correctly use TZ from `setenv()`? (Yes, after `tzset()`)
- `blob` char array on stack is 2048 bytes — is this safe for ESP32-S2 stack? (Default task stack is 8 KB; 2048 is acceptable but note the constraint)
- `hal_nvs_read_u16` return value silently ignored in `get_allocation_sec` — is that the intended design? (Yes — fall through to default)
- Signed/unsigned comparison in `schedule_is_holiday` loop bounds
- Test `test_allocation_missing_key_falls_back_to_default`: does the test actually call `mock_nvs_reset()` before asserting? (Yes — the test body calls it after setUp already wrote keys)

Fix any issues before merging.

---

## Task 8: Merge to integration

- [ ] **Step 1: Push and merge**

```bash
git push -u origin feature/schedule
git checkout integration
git merge --no-ff feature/schedule -m "feat(schedule): implement day-type logic with TDD"
git push origin integration
```
