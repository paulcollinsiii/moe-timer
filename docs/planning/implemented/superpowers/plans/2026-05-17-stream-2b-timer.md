# Stream 2b — timer.c Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `timer.c` using TDD. The state machine transitions (IDLE→RUNNING→PAUSED→RUNNING→EXPIRED) and expiry math must be fully tested on the host. Critical invariant: `expiry_wall_time` is set only at `timer_start()` and `timer_resume()`; calling `timer_tick()` never modifies it.

**Architecture:** `timer.c` operates on `g_rtc_state` (a global `rtc_state_t`). On the ESP32, this struct is tagged `RTC_DATA_ATTR` to survive deep sleep. In native builds, `RTC_DATA_ATTR` expands to nothing (from `esp_compat.h`), making it a regular static. The module calls `hal_time_now()` only where the caller passes no timestamp; most functions take `time_t now` explicitly for testability.

**Tech Stack:** C99, Unity test framework, PlatformIO native, mock HAL from Stream 1

**Prerequisite:** Stream 1 (`feature/test-harness`) merged to `integration`.

---

## Files

| Action | Path |
|---|---|
| Modify | `test/test_timer/test_timer.c` |
| Modify | `src/timer.c` |

`include/timer.h` is already created in Stream 0 — do not modify its signatures.

---

## Task 1: Create branch

- [ ] **Step 1: Branch from integration**

```bash
git fetch origin
git checkout integration
git checkout -b feature/timer
```

---

## Task 2: Write failing tests

**Files:** Replace `test/test_timer/test_timer.c`

- [ ] **Step 1: Write all tests**

```c
#include <unity.h>
#include <string.h>
#include <time.h>

/* Single-TU compilation */
#include "mock_hal_time.c"
#include "../../src/timer.c"

/* Base timestamp: 2026-01-05 00:00:00 UTC (Monday) */
#define T0 ((time_t)1767571200)

void setUp(void)
{
    timer_reset();
    mock_time_set(T0);
}

void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* timer_reset — cold-boot init                                        */
/* ------------------------------------------------------------------ */

void test_reset_state_is_idle(void)
{
    TEST_ASSERT_EQUAL(TIMER_IDLE, timer_get_state());
}

void test_reset_expiry_is_zero(void)
{
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.expiry_wall_time);
}

void test_reset_remaining_at_pause_is_zero(void)
{
    TEST_ASSERT_EQUAL_INT32(0, g_rtc_state.remaining_at_pause);
}

/* ------------------------------------------------------------------ */
/* IDLE → RUNNING (timer_start)                                        */
/* ------------------------------------------------------------------ */

void test_start_sets_state_running(void)
{
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_start_sets_expiry_wall_time(void)
{
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL_INT64((int64_t)T0 + 3600, g_rtc_state.expiry_wall_time);
}

void test_start_sets_allocation_sec(void)
{
    timer_start(T0, 3600);
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.allocation_sec);
}

/* ------------------------------------------------------------------ */
/* RUNNING state: timer_tick does NOT modify expiry_wall_time          */
/* ------------------------------------------------------------------ */

void test_tick_does_not_modify_expiry_wall_time(void)
{
    timer_start(T0, 3600);
    int64_t expiry_before = g_rtc_state.expiry_wall_time;

    /* Simulate 10 minutes passing; tick several times */
    mock_time_set(T0 + 600);
    timer_tick(T0 + 600);
    mock_time_set(T0 + 1200);
    timer_tick(T0 + 1200);

    TEST_ASSERT_EQUAL_INT64(expiry_before, g_rtc_state.expiry_wall_time);
}

void test_tick_returns_correct_remaining_seconds(void)
{
    timer_start(T0, 3600);
    int32_t remaining = timer_tick(T0 + 1000);
    TEST_ASSERT_EQUAL_INT32(2600, remaining);
}

void test_tick_returns_remaining_near_zero(void)
{
    timer_start(T0, 3600);
    int32_t remaining = timer_tick(T0 + 3599);
    TEST_ASSERT_EQUAL_INT32(1, remaining);
}

/* ------------------------------------------------------------------ */
/* RUNNING → EXPIRED (via timer_tick)                                  */
/* ------------------------------------------------------------------ */

void test_tick_transitions_to_expired_when_time_elapsed(void)
{
    timer_start(T0, 3600);
    timer_tick(T0 + 3601); /* 1 second past expiry */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

void test_tick_returns_non_positive_when_expired(void)
{
    timer_start(T0, 3600);
    int32_t remaining = timer_tick(T0 + 3601);
    TEST_ASSERT_TRUE(remaining <= 0);
}

void test_tick_at_exact_expiry_transitions(void)
{
    timer_start(T0, 3600);
    timer_tick(T0 + 3600); /* exactly at expiry */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

/* ------------------------------------------------------------------ */
/* RUNNING → PAUSED (timer_pause)                                      */
/* ------------------------------------------------------------------ */

void test_pause_sets_state_paused(void)
{
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL(TIMER_PAUSED, timer_get_state());
}

void test_pause_saves_remaining_at_pause(void)
{
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL_INT32(2600, g_rtc_state.remaining_at_pause);
}

void test_pause_clears_expiry_wall_time(void)
{
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.expiry_wall_time);
}

/* ------------------------------------------------------------------ */
/* PAUSED → RUNNING (timer_resume)                                     */
/* ------------------------------------------------------------------ */

void test_resume_sets_state_running(void)
{
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);
    timer_resume(T0 + 2000); /* resumed 1000 s after pause */
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());
}

void test_resume_sets_expiry_from_remaining_at_pause(void)
{
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);          /* 2600 s remaining */
    timer_resume(T0 + 2000);         /* new expiry = (T0+2000) + 2600 */
    int64_t expected = (int64_t)(T0 + 2000) + 2600;
    TEST_ASSERT_EQUAL_INT64(expected, g_rtc_state.expiry_wall_time);
}

void test_resume_preserves_remaining_within_one_second(void)
{
    timer_start(T0, 3600);
    timer_pause(T0 + 1000);   /* 2600 s remaining */
    time_t resume_time = T0 + 5000; /* long time later */
    timer_resume(resume_time);
    int32_t remaining = timer_tick(resume_time);
    /* remaining should be 2600 (the paused value), within ±1 s */
    TEST_ASSERT_INT_WITHIN(1, 2600, remaining);
}

/* ------------------------------------------------------------------ */
/* Pause/resume round-trip: full cycle                                  */
/* ------------------------------------------------------------------ */

void test_full_cycle_idle_run_pause_resume_expire(void)
{
    timer_start(T0, 100);        /* 100 s allocation */
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());

    timer_pause(T0 + 40);        /* 60 s remaining */
    TEST_ASSERT_EQUAL_INT32(60, g_rtc_state.remaining_at_pause);

    timer_resume(T0 + 100);      /* resume at T0+100 */
    TEST_ASSERT_EQUAL(TIMER_RUNNING, timer_get_state());

    int32_t remaining = timer_tick(T0 + 159);
    TEST_ASSERT_INT_WITHIN(1, 1, remaining); /* 1 s left */

    timer_tick(T0 + 161);        /* past expiry */
    TEST_ASSERT_EQUAL(TIMER_EXPIRED, timer_get_state());
}

/* ------------------------------------------------------------------ */
/* Day tracking                                                         */
/* ------------------------------------------------------------------ */

void test_is_new_day_false_when_same_date(void)
{
    timer_record_date(T0);
    TEST_ASSERT_FALSE(timer_is_new_day(T0 + 3600)); /* still same day */
}

void test_is_new_day_true_after_midnight(void)
{
    timer_record_date(T0);
    /* T0 = 2026-01-05; next day starts at T0 + 86400 */
    TEST_ASSERT_TRUE(timer_is_new_day(T0 + 86400));
}

/* ------------------------------------------------------------------ */
/* NTP sync scheduling                                                  */
/* ------------------------------------------------------------------ */

void test_needs_ntp_sync_false_immediately_after_sync(void)
{
    timer_record_ntp_sync(T0);
    TEST_ASSERT_FALSE(timer_needs_ntp_sync(T0 + 1));
}

void test_needs_ntp_sync_true_after_10_minutes(void)
{
    timer_record_ntp_sync(T0);
    TEST_ASSERT_TRUE(timer_needs_ntp_sync(T0 + 601));
}

/* ------------------------------------------------------------------ */
/* Runner                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reset_state_is_idle);
    RUN_TEST(test_reset_expiry_is_zero);
    RUN_TEST(test_reset_remaining_at_pause_is_zero);
    RUN_TEST(test_start_sets_state_running);
    RUN_TEST(test_start_sets_expiry_wall_time);
    RUN_TEST(test_start_sets_allocation_sec);
    RUN_TEST(test_tick_does_not_modify_expiry_wall_time);
    RUN_TEST(test_tick_returns_correct_remaining_seconds);
    RUN_TEST(test_tick_returns_remaining_near_zero);
    RUN_TEST(test_tick_transitions_to_expired_when_time_elapsed);
    RUN_TEST(test_tick_returns_non_positive_when_expired);
    RUN_TEST(test_tick_at_exact_expiry_transitions);
    RUN_TEST(test_pause_sets_state_paused);
    RUN_TEST(test_pause_saves_remaining_at_pause);
    RUN_TEST(test_pause_clears_expiry_wall_time);
    RUN_TEST(test_resume_sets_state_running);
    RUN_TEST(test_resume_sets_expiry_from_remaining_at_pause);
    RUN_TEST(test_resume_preserves_remaining_within_one_second);
    RUN_TEST(test_full_cycle_idle_run_pause_resume_expire);
    RUN_TEST(test_is_new_day_false_when_same_date);
    RUN_TEST(test_is_new_day_true_after_midnight);
    RUN_TEST(test_needs_ntp_sync_false_immediately_after_sync);
    RUN_TEST(test_needs_ntp_sync_true_after_10_minutes);
    return UNITY_END();
}
```

- [ ] **Step 2: Run tests — verify they FAIL**

```bash
pio test -e native -f test_timer
```

Expected: many FAIL lines. The stub `timer.c` returns only 0 and does nothing.

- [ ] **Step 3: Commit failing tests**

```bash
git add test/test_timer/test_timer.c
git commit -m "test(timer): add full state machine test suite (all failing)"
```

---

## Task 3: Implement timer_reset and timer_get_state

**Files:** Modify `src/timer.c`

- [ ] **Step 1: Implement reset and state accessor**

Replace the full content of `src/timer.c`:

```c
#include "timer.h"
#include "hal_time.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ---- RTC state ---- */
#ifndef NATIVE
rtc_state_t RTC_DATA_ATTR g_rtc_state;
#else
rtc_state_t g_rtc_state;
#endif

timer_state_t timer_get_state(void)
{
    return g_rtc_state.state;
}

void timer_reset(void)
{
    memset(&g_rtc_state, 0, sizeof(g_rtc_state));
    g_rtc_state.state = TIMER_IDLE;
}
```

- [ ] **Step 2: Run reset/state tests**

```bash
pio test -e native -f test_timer
```

Expected: `test_reset_*` tests PASS.

---

## Task 4: Implement timer_start

- [ ] **Step 1: Add timer_start to src/timer.c**

```c
void timer_start(time_t now, int32_t allocation_sec)
{
    g_rtc_state.state            = TIMER_RUNNING;
    g_rtc_state.allocation_sec   = allocation_sec;
    g_rtc_state.expiry_wall_time = (int64_t)now + allocation_sec;
}
```

- [ ] **Step 2: Run start tests**

```bash
pio test -e native -f test_timer
```

Expected: `test_start_*` tests PASS.

---

## Task 5: Implement timer_tick

- [ ] **Step 1: Add timer_tick to src/timer.c**

```c
int32_t timer_tick(time_t now)
{
    if (g_rtc_state.state != TIMER_RUNNING) {
        return (int32_t)(g_rtc_state.expiry_wall_time - (int64_t)now);
    }
    int64_t remaining = g_rtc_state.expiry_wall_time - (int64_t)now;
    if (remaining <= 0) {
        g_rtc_state.state = TIMER_EXPIRED;
        return (int32_t)remaining;
    }
    /* expiry_wall_time is NOT modified here */
    return (int32_t)remaining;
}
```

- [ ] **Step 2: Run tick tests**

```bash
pio test -e native -f test_timer
```

Expected: all `test_tick_*` tests PASS.

---

## Task 6: Implement timer_pause and timer_resume

- [ ] **Step 1: Add pause and resume to src/timer.c**

```c
void timer_pause(time_t now)
{
    if (g_rtc_state.state != TIMER_RUNNING) return;
    int64_t remaining = g_rtc_state.expiry_wall_time - (int64_t)now;
    g_rtc_state.remaining_at_pause = (remaining > 0) ? (int32_t)remaining : 0;
    g_rtc_state.expiry_wall_time   = 0;
    g_rtc_state.state              = TIMER_PAUSED;
}

void timer_resume(time_t now)
{
    if (g_rtc_state.state != TIMER_PAUSED) return;
    g_rtc_state.expiry_wall_time = (int64_t)now + g_rtc_state.remaining_at_pause;
    g_rtc_state.state            = TIMER_RUNNING;
}
```

- [ ] **Step 2: Run all timer tests**

```bash
pio test -e native -f test_timer
```

Expected: all pause, resume, and full-cycle tests PASS.

---

## Task 7: Implement day tracking and NTP sync helpers

- [ ] **Step 1: Add date-tracking functions to src/timer.c**

```c
bool timer_is_new_day(time_t now)
{
    if (g_rtc_state.last_date[0] == '\0') return true; /* no recorded date */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[11];
    snprintf(today, sizeof(today), "%04d-%02d-%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
    return (strcmp(today, g_rtc_state.last_date) != 0);
}

void timer_record_date(time_t now)
{
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    snprintf(g_rtc_state.last_date, sizeof(g_rtc_state.last_date),
             "%04d-%02d-%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
}

bool timer_needs_ntp_sync(time_t now)
{
    if (g_rtc_state.next_ntp_sync == 0) return true;
    return (int64_t)now >= g_rtc_state.next_ntp_sync;
}

void timer_record_ntp_sync(time_t now)
{
    g_rtc_state.next_ntp_sync = (int64_t)now + 600; /* 10 minutes */
}
```

- [ ] **Step 2: Run ALL timer tests — verify all pass**

```bash
pio test -e native -f test_timer
```

Expected:
```
-----------------------
23 Tests 0 Failures 0 Ignored
OK
================================= [PASSED] Took X.XXX seconds =================================
```

- [ ] **Step 3: Commit implementation**

```bash
git add src/timer.c test/test_timer/test_timer.c
git commit -m "feat(timer): implement state machine, expiry math, day tracking, NTP sync scheduling"
```

---

## Task 8: Verify magtag build compiles

- [ ] **Step 1: Run pio run**

```bash
pio run
```

Expected: `[SUCCESS]`. If `esp_attr.h` is not found, check that `#include "esp_attr.h"` in `timer.h` is inside `#ifndef NATIVE ... #endif`.

---

## Task 9: Code review

- [ ] **Step 1: Run adversarial code review**

Use `superpowers:requesting-code-review` skill. Focus areas:
- `timer_tick`: signed 64-bit to signed 32-bit cast — can overflow if expiry is far in the future (days away). Is the 32-bit return type sufficient? (It should be: max allocation is 120 min = 7200 s, well within int32)
- `timer_pause`: what if `now > expiry_wall_time`? (`remaining` goes negative; clamped to 0 — verify test covers this)
- `timer_resume` called while not PAUSED: early return — is this the right behaviour, or should it assert?
- `timer_is_new_day` with empty `last_date`: returns true (forces re-init on cold boot) — correct
- `localtime_r` thread safety on native: fine (single-threaded tests)
- `g_rtc_state` declared with `RTC_DATA_ATTR` in production: on cold boot (power cycle), the struct is zero-initialized by hardware. `timer_reset()` replicates this for explicit resets

Fix any issues before merging.

---

## Task 10: Merge to integration

- [ ] **Step 1: Push and merge**

```bash
git push -u origin feature/timer
git checkout integration
git merge --no-ff feature/timer -m "feat(timer): implement state machine with TDD"
git push origin integration
```
