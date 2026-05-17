# Stream 4 — Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `main.c` — wire all modules together into a working firmware image, run compile + native-test gates, then perform a one-time hardware smoke test on the `integration` branch before merging to `main`.

**Architecture:** `app_main()` dispatches on `esp_sleep_get_wakeup_cause()` to either `handle_timer_tick()` or `handle_button_wake()`. Both paths call into modules from Streams 2a/2b/2c/3a/3b/3c, then call `esp_deep_sleep_start()`. Deep sleep uses a 55-second RTC wakeup plus GPIO wakeup on all 4 buttons.

**Tech Stack:** ESP-IDF `esp_sleep`, all modules from prior streams (`timer`, `schedule`, `display`, `ntp`, `nvs_config`, `buttons`, `audio`, `neopixel`)

**Prerequisites:** All of Streams 0–3c merged to `integration`.

---

## Files

| Action | Path |
|---|---|
| Modify | `src/main.c` |

`main.c` is the only file touched — all dependencies are already implemented in prior streams.

---

## Task 1: Create branch

- [ ] **Step 1: Verify all prerequisite streams are merged to `integration`**

```bash
git fetch origin
git checkout integration
git log --oneline -20
```

Expected: commits from `feature/foundation`, `feature/test-harness`, `feature/schedule`, `feature/timer`, `feature/nvs-config`, `feature/display`, `feature/peripherals`, and `feature/wifi-ntp` all visible.

- [ ] **Step 2: Branch from integration**

```bash
git checkout -b feature/integration
```

---

## Task 2: Compile + native-test baseline

Before writing a single line of `main.c`, verify the baseline is clean.

- [ ] **Step 1: Run compile gate**

```bash
pio run
```

Expected: `[SUCCESS]`. If not, resolve all errors from prior streams before proceeding. This task is a hard gate — do not write any `main.c` code until this passes.

- [ ] **Step 2: Run native tests**

```bash
pio test -e native
```

Expected: all schedule, timer, and nvs_config tests pass. If any fail, fix the root cause (do not comment out tests).

---

## Task 3: Implement `main.c`

**Files:** Replace `src/main.c`

- [ ] **Step 1: Implement app_main, handle_timer_tick, and handle_button_wake**

Replace the full content of `src/main.c`:

```c
#include "timer.h"
#include "schedule.h"
#include "display.h"
#include "ntp.h"
#include "nvs_config.h"
#include "buttons.h"
#include "audio.h"
#include "neopixel.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <time.h>
#include <string.h>

static const char *TAG = "main";

/* Full display refresh every N timer-tick wakes to avoid ghosting */
#define FULL_REFRESH_INTERVAL 5

static RTC_DATA_ATTR int    s_tick_count      = 0;
static RTC_DATA_ATTR time_t s_last_ntp_sync   = 0;

static void enter_deep_sleep(void)
{
    esp_sleep_enable_timer_wakeup(55ULL * 1000000ULL);
    buttons_configure_wakeup();
    esp_deep_sleep_start();
}

static void handle_timer_tick(void)
{
    time_t now = time(NULL);

    /* Day rollover: new day triggers NTP sync and timer reset */
    if (timer_is_new_day(now)) {
        ESP_LOGI(TAG, "New day detected — NTP sync and timer reset");
        esp_err_t ret = ntp_sync();
        if (ret == ESP_OK) {
            s_last_ntp_sync = time(NULL);
        } else {
            ESP_LOGW(TAG, "NTP sync failed on day rollover: %s", esp_err_to_name(ret));
        }
        now = time(NULL);
        timer_reset();
        timer_record_date(now);
    }

    /* Periodic NTP sync (every 10 min while RUNNING) */
    if (timer_needs_ntp_sync(now)) {
        ESP_LOGI(TAG, "Periodic NTP sync");
        esp_err_t ret = ntp_sync();
        if (ret == ESP_OK) {
            s_last_ntp_sync = time(NULL);
        } else {
            ESP_LOGW(TAG, "Periodic NTP sync failed: %s", esp_err_to_name(ret));
        }
        timer_record_ntp_sync(time(NULL));
        now = time(NULL);
    }

    /* Tick the state machine */
    int32_t remaining = timer_tick(now);

    /* Build display state */
    day_type_t day_type = schedule_get_day_type(now);
    uint32_t allocation_sec = schedule_get_allocation_sec(day_type);
    uint8_t contrast = 0;
    nvs_config_get_display_contrast(&contrast);

    display_state_t state = {
        .remaining_sec   = remaining,
        .allocation_sec  = allocation_sec,
        .timer_state     = timer_get_state(),
        .day_type        = day_type,
        .wall_time       = now,
        .last_sync_time  = s_last_ntp_sync,
        .contrast_level  = contrast,
    };
    s_tick_count++;
    if (s_tick_count >= FULL_REFRESH_INTERVAL) {
        s_tick_count = 0;
        display_full_refresh(&state);
    } else {
        display_update(&state);
    }

    /* Alert on expiry — blocks until complete; no deep sleep during alert */
    if (timer_get_state() == TIMER_EXPIRED) {
        display_timesup();
        audio_beep_sequence();
        neopixel_alert_start();
        /* After alert completes, go back to sleep; dismiss handled on next button wake */
    }

    enter_deep_sleep();
}

static void handle_button_wake(void)
{
    time_t now = time(NULL);
    button_id_t btn = buttons_get_wakeup_button();

    /* Dismiss any active alert first regardless of which button woke us */
    if (timer_get_state() == TIMER_EXPIRED) {
        audio_stop();
        neopixel_stop();
        enter_deep_sleep();
        return;
    }

    day_type_t day_type = schedule_get_day_type(now);
    uint32_t allocation_sec = schedule_get_allocation_sec(day_type);

    switch (btn) {
    case BTN_A:
        /* Start (IDLE/PAUSED → RUNNING) or Pause (RUNNING → PAUSED) */
        if (timer_get_state() == TIMER_RUNNING) {
            timer_pause(now);
        } else {
            /* NTP sync before starting to get accurate expiry */
            esp_err_t ret = ntp_sync();
            if (ret == ESP_OK) {
                s_last_ntp_sync = time(NULL);
            } else {
                ESP_LOGW(TAG, "NTP sync before start failed: %s", esp_err_to_name(ret));
            }
            now = time(NULL);
            if (timer_get_state() == TIMER_IDLE) {
                timer_start(now, allocation_sec);
                timer_record_date(now);
            } else {
                /* PAUSED → RUNNING */
                timer_resume(now);
            }
            timer_record_ntp_sync(now);
        }
        break;

    case BTN_B:
        /* Reset to IDLE with today's full allocation */
        timer_reset();
        break;

    case BTN_C: {
        /* Cycle display contrast (3 levels: 0, 1, 2) */
        uint8_t c = 0;
        nvs_config_get_display_contrast(&c);
        c = (uint8_t)((c + 1) % 3);
        nvs_config_set_display_contrast(c);
        break;
    }

    case BTN_D:
        /* Force NTP re-sync + full refresh */
        {
            esp_err_t ret = ntp_sync();
            if (ret == ESP_OK) {
                s_last_ntp_sync = time(NULL);
            } else {
                ESP_LOGW(TAG, "Forced NTP sync failed: %s", esp_err_to_name(ret));
            }
            now = time(NULL);
            timer_record_ntp_sync(now);
        }
        break;

    case BTN_NONE:
    default:
        ESP_LOGW(TAG, "No button identified on GPIO wake");
        break;
    }

    /* Rebuild state after any mutation */
    int32_t remaining = timer_tick(now);
    uint8_t contrast = 0;
    nvs_config_get_display_contrast(&contrast);
    display_state_t state = {
        .remaining_sec   = remaining,
        .allocation_sec  = allocation_sec,
        .timer_state     = timer_get_state(),
        .day_type        = day_type,
        .wall_time       = now,
        .last_sync_time  = s_last_ntp_sync,
        .contrast_level  = contrast,
    };

    /* Button wakes always do a full refresh for responsiveness */
    s_tick_count = 0;
    display_full_refresh(&state);

    enter_deep_sleep();
}

void app_main(void)
{
    /* NVS must be initialised before any module that reads NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Init defaults on first boot (idempotent on subsequent boots) */
    ESP_ERROR_CHECK(nvs_config_init_defaults());

    /* NeoPixel power gate: GPIO 21 HIGH (power off) as early as possible */
    neopixel_init();

    /* Peripheral init */
    buttons_init();
    display_init();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wakeup cause: %d", (int)cause);

    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        handle_button_wake();
    } else {
        handle_timer_tick();
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add src/main.c
git commit -m "feat(main): wire all modules into app_main dispatch loop"
```

---

## Task 4: Compile gate

- [ ] **Step 1: Run compile gate**

```bash
pio run
```

Expected: `[SUCCESS]`.

**Troubleshooting:**

- `timer_is_new_day` / `timer_record_date` / `timer_needs_ntp_sync` / `timer_record_ntp_sync` not declared: these must be present in `include/timer.h` from Stream 2b. If they're missing, add their declarations to `timer.h` and implementations to `timer.c` — these are private helpers used only by `main.c`.
- `nvs_config_get_display_contrast` / `nvs_config_set_display_contrast` not declared: add to `nvs_config.h` and implement in `nvs_config.c` using `hal_nvs` get/set with key `"disp_contrast"`.
- `TIMER_EXPIRED` not in `timer_state_t`: add it to the enum in `timer.h` if absent.
- `display_state_t` field mismatch: cross-check against `include/display.h` from Stream 3a and adjust field names to match exactly.
- Any `#include` not found: add the missing header to `src/CMakeLists.txt` REQUIRES if it's from a component not already listed.

- [ ] **Step 2: Run native tests to confirm no regressions**

```bash
pio test -e native
```

Expected: all tests pass.

- [ ] **Step 3: Commit any fixes**

```bash
git add -u
git commit -m "fix(main): resolve API mismatches from stream integration"
```

---

## Task 5: Code review

- [ ] **Step 1: Run adversarial code review**

Use `superpowers:requesting-code-review` skill. Focus areas:

- **`expiry_wall_time` invariant**: grep `src/main.c` for any reference to `expiry_wall_time` or `g_rtc_state`. Must be zero — `main.c` NEVER touches this field directly.
- **NVS init before any NVS call**: `nvs_flash_init()` and `nvs_config_init_defaults()` must both precede any `nvs_config_get_*` or `nvs_config_set_*` call. Verify the `app_main` call order.
- **NeoPixel power gate timing**: `neopixel_init()` (asserts GPIO 21 HIGH) must be called before any other peripheral init. Confirm its position in `app_main` relative to `buttons_init()` and `display_init()`.
- **No peripheral left on at sleep entry**: `enter_deep_sleep()` must not be reached with WiFi active (caller handles WiFi teardown via `ntp_sync()` internal lifecycle), amplifier enabled, or NeoPixel power gate LOW. Trace each code path.
- **Deep sleep after EXPIRED alert**: `audio_beep_sequence()` and `neopixel_alert_start()` both block; after they return, `enter_deep_sleep()` is called. Confirm NeoPixels and amplifier are off before sleep (they must self-terminate in `audio.c` and `neopixel.c`).
- **Day rollover and `timer_reset()` idempotency**: on day rollover, `timer_reset()` clears all state. If `ntp_sync()` fails on rollover, the timer resets anyway. Confirm that is the intended behaviour (fail-open: reset to IDLE even without NTP).
- **BTN_A PAUSED → RUNNING after failed NTP**: if `ntp_sync()` fails before `timer_resume()`, the resume still proceeds (wall clock may drift). Confirm this is acceptable (it is — drift self-corrects on next successful sync).
- **`timer_tick()` called in `handle_button_wake()`**: `timer_tick(now)` is called after button dispatch. This transitions RUNNING → EXPIRED if time has passed. Confirm this is intentional and the EXPIRED case after a button-wake is handled (the alert will fire on the next timer-tick wake, not immediately on button wake — verify this is the desired UX).
- **`s_tick_count` RTC persistence**: `s_tick_count` is `RTC_DATA_ATTR`. On cold boot it may be uninitialized. `timer_reset()` does NOT reset `s_tick_count`. Consider whether first-boot should force a full refresh (it will if `s_tick_count` starts at 0 and wraps after 5 — acceptable).
- **`esp_err_t` returns on NVS init**: `ESP_ERROR_CHECK` aborts on panic. Correct for init path; confirm this is acceptable (it is — NVS failure on boot is unrecoverable).

Fix any issues before proceeding to hardware smoke test.

---

## Task 6: Hardware smoke test

This is the only task in the entire project that flashes hardware. Perform it on the `integration` branch.

**Prerequisites:**
- MagTag 2.9" device connected via USB-C to `/dev/ttyACM0`
- WiFi SSID and password provisioned in NVS (see provisioning note below)

**NVS provisioning note:** On first flash, NVS is blank. `nvs_config_init_defaults()` writes `wifi_ssid = ""` and `wifi_pass = ""`. You must flash once, then use a separate NVS provisioning tool or a debug build to write your credentials. Alternatively, temporarily hardcode credentials in `nvs_defaults.h` for the smoke test only — remove before merging to `main`.

- [ ] **Step 1: Flash the device**

```bash
pio run -t upload
```

- [ ] **Step 2: Open serial monitor**

```bash
pio device monitor
```

- [ ] **Step 3: Execute smoke test checklist**

Check each item:

- [ ] Device boots, IDLE display appears, device enters deep sleep (monitor shows `Wakeup cause: 0` on cold boot)
- [ ] Press any button: device wakes (`Wakeup cause: 3`), full display refresh occurs, device returns to deep sleep
- [ ] Press BTN_A (GPIO 15): NTP sync log appears (`Got IP address`, `SNTP sync complete`), display switches to RUNNING state with correct date/time, 55-second wake cycle begins
- [ ] After timer wake: partial refresh occurs, remaining time decrements correctly
- [ ] Press BTN_A again: display switches to PAUSED state
- [ ] Press BTN_A again: RUNNING resumes, `expiry_wall_time` not reset (remaining time preserved from pause)
- [ ] Press BTN_B: device resets to IDLE, full allocation shown
- [ ] Press BTN_D: NTP sync triggered, display full refresh
- [ ] Let timer expire (set a very short allocation in `nvs_defaults.h` for testing, e.g. `weekday_min = 1`): TIME'S UP display, beep sequence fires, NeoPixels pulse red
- [ ] Press any button: alert dismissed, device returns to deep sleep

- [ ] **Step 4: Restore nvs_defaults.h if modified for smoke test**

```bash
git diff include/nvs_defaults.h
# If changed, revert:
git checkout include/nvs_defaults.h
```

- [ ] **Step 5: Commit smoke test result (no code changes)**

```bash
# If no code changes were needed:
git commit --allow-empty -m "test(integration): hardware smoke test passed on MagTag 2025"
```

If fixes were required, commit them with descriptive messages before this empty commit.

---

## Task 7: Merge to integration and main

- [ ] **Step 1: Push feature branch**

```bash
git push -u origin feature/integration
```

- [ ] **Step 2: Merge to integration**

```bash
git checkout integration
git merge --no-ff feature/integration -m "feat(main): wire all modules into app_main dispatch loop"
git push origin integration
```

- [ ] **Step 3: Final validation on integration**

```bash
pio run
pio test -e native
```

Both must pass clean.

- [ ] **Step 4: Merge integration to main**

```bash
git checkout main
git merge --no-ff integration -m "release: MagTag Screen Timer v1.0 — all streams integrated and hardware validated"
git push origin main
```
