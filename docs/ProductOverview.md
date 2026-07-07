# MagTag Screen Timer — Product Overview

## Purpose
A screen-time countdown timer for a child, running on the Adafruit MagTag (2025, ESP32-S2). Tracks how much screen time remains each day, respects a holiday/weekend schedule stored in NVS, and visually counts down via an e-ink progress bar. The device is wall- or fridge-mounted (magnetic backing) so the child and parent can see remaining time at a glance.

---

## Hardware Target

| Attribute | Value |
|-----------|-------|
| Board | Adafruit MagTag 2025 |
| Platform.io ID | `adafruit_magtag29_esp32s2` |
| MCU | ESP32-S2 @ 240 MHz |
| Flash / PSRAM | 4 MB / 2 MB |
| Display | 2.9" grayscale e-ink, 296×128 px, SSD1680 controller |
| Buttons | 4× tactile (GPIO 15/14/12/11 (A/B/C/D), active-LOW, internal pull-up) |
| NeoPixels | 4× RGB on GPIO 1 (power gate: GPIO 21 LOW = on) |
| Speaker | Onboard amplifier, shutdown pin GPIO 16 (HIGH = on) |
| WiFi | 802.11 b/g/n, 2.4 GHz |
| RTC | None (dedicated RTC chip absent; ESP32-S2 RTC timer used for deep-sleep wakeup) |
| Battery | 3.7 V LiPo via JST connector |

---

## Features

### 1 · Clock & NTP

- Connect to WiFi and sync via SNTP (`pool.ntp.org`) in three situations:
  1. **Timer start/resume**: the countdown starts **immediately** on the
     current clock (instant user feedback); NTP sync runs right after. Any
     clock step the sync applies is measured against the monotonic clock and
     added to `expiry_wall_time` via `timer_shift_expiry()`, preserving the
     remaining duration exactly.
  2. **Every 10 minutes while running**: compensate for ESP32 RTC drift.
  3. **New day detected on wake**: NTP sync to get accurate date for schedule lookup.
- Routine NTP syncs correct only the ESP32 system clock. `expiry_wall_time` is
  **not** modified on sync — since `remaining = expiry_wall_time - time(NULL)`,
  drift compensation is automatic once the system clock is corrected. The one
  exception is the start/resume flow above, where the expiry was computed from
  a possibly-uncorrected clock and is shifted by the measured step.
- If WiFi is unavailable, the timer fails open: it keeps running on the
  uncorrected clock (remaining time stays a consistent duration; only the
  displayed wall-clock time may be off). Sync failure is signalled on the
  WiFi NeoPixel (red blinks) when `CONFIG_MAGTAG_SYNC_LED_FEEDBACK` is on.
- Timezone configured at compile time as a POSIX TZ string `#define` (e.g. `EST5EDT,M3.2.0,M11.1.0`).
- Display shows: date (e.g. `Sat May 16`) + current time (HH:MM AM/PM) and `Last sync: HH:MM`.

### 2 · Timer Architecture — Absolute Expiry Time

Rather than counting elapsed seconds, the timer stores the **absolute Unix timestamp at which it should expire** (`expiry_wall_time`). On every wake:

```
remaining = expiry_wall_time - time(NULL)
```

This makes the countdown inherently drift-resistant: NTP syncs correct `time(NULL)` via SNTP, so remaining time recalculates correctly without ever modifying `expiry_wall_time`. The only time `expiry_wall_time` changes is at timer start (`IDLE → RUNNING`) or resume after pause (`PAUSED → RUNNING`: `expiry_wall_time = time(NULL) + remaining_at_pause`).

Timer state and `expiry_wall_time` are stored in **RTC slow memory** (survives deep sleep) and additionally snapshotted to **NVS** on every state transition (XOR checksum + version + plausibility validation). After a panic, external reset, or power cycle the boot path restores the snapshot as long as its stored date is still today — so losing power does not refund the day's allocation. The allocation resets only on a genuine day rollover or via Button B when `CONFIG_MAGTAG_PARENT_TESTING` is enabled.

### 3 · Deep Sleep Architecture

The device spends almost all of its time in deep sleep. Wake sources:

| Source | Action on wake |
|--------|----------------|
| RTC timer (planner-scheduled) | Partial display refresh + check NTP schedule |
| GPIO (wake-button press) | Handle button event, full refresh if needed |

Sleep durations come from a pure, host-tested planner (`main/sleep_plan.c`):
clock-only states (IDLE/PAUSED/EXPIRED) align wakes to wall-clock minute
boundaries (header time flips with real clocks); RUNNING/BREAK align to the
countdown's own minute grid so the displayed remaining truly reads round
values (start/resume shows one precise value, then 1:12:00, 1:11:00, ...).
RUNNING wakes ~20 s early when an NTP sync is due, and pre-event wakes land
~70 s before an expiry/break end so the awake watch loop fires the event on
time.

**Wake sequence (roughly once per minute)**:
1. Read current time from ESP32 RTC.
2. Check for day rollover (compare date to `last_date` in RTC memory) → if new day: wake WiFi, NTP sync, re-init timer to IDLE with new allocation.
3. If `next_ntp_sync_time` has passed (every 10 min while RUNNING): wake WiFi, NTP sync, adjust `expiry_wall_time`.
4. Compute `remaining = expiry_wall_time - now`.
5. Update display (partial refresh; full refresh on every 5th wake or state change).
6. If RUNNING with <=60 s remaining: stay awake (state pixel lit, clock-locking
   sync if due) and fire TIME'S UP within ~1 s of the expiry wall time.
7. Return to deep sleep.

WiFi is **off by default**; it is only powered up for NTP syncs and then immediately shut down.

### 4 · Daily Schedule & NVS Config

NVS namespace: `timer_cfg`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `weekday_min` | u16 | 60 | Weekday allocation (minutes) |
| `weekend_min` | u16 | 120 | Weekend allocation (minutes) |
| `holiday_min` | u16 | 120 | Holiday allocation (minutes) |
| `summer_min` | u16 | 120 | Summer-break weekday allocation (minutes) |
| `holidays` | blob | (pre-filled) | Newline-separated `YYYY-MM-DD` holiday dates |
| `wifi_ssid` | str | "" | WiFi SSID |
| `wifi_pass` | str | "" | WiFi password |

On first flash the NVS is initialised from `nvs_defaults.h` (holiday list, WiFi from `credentials.local.h`) with the allocation minutes coming from menuconfig (`MagTag Timer` menu → `CONFIG_MAGTAG_WEEKDAY/WEEKEND/HOLIDAY_MIN`). The stored stamp is a fingerprint of those values, so changing any allocation in menuconfig re-seeds NVS on the next boot — no erase needed. Subsequent boots with an unchanged fingerprint read the stored values.

**Day-type logic** (precedence: holiday > weekend > summer > weekday):
1. Check if today's date is in the `holidays` blob → holiday allocation.
2. Else if Saturday or Sunday → weekend allocation.
3. Else if outside the school year (`NVS_DEFAULT_SUMMER_START`/`SCHOOL_START`/`SCHOOL_END` in `nvs_defaults.h`, from the Dublin City Schools calendar — update yearly) → summer allocation.
4. Else → weekday allocation.

The holiday list is the Dublin City Schools (Grizzell MS) 2026-27 calendar's
weekday no-school days, not generic federal holidays — days like Veterans
Day, when school is in session, are deliberately regular weekdays.

### 5 · Timer State Machine

States: `IDLE` → `RUNNING` → `PAUSED` → `EXPIRED`, plus `BREAK` (eye rest)

- `IDLE`: Allocation loaded for today, `expiry_wall_time` not set. Display shows full bar.
- `RUNNING`: `expiry_wall_time` set. Device deep sleeps between 55-second refresh wakes.
- `PAUSED`: `remaining_at_pause` saved in RTC memory; `expiry_wall_time` cleared. Deep sleep continues.
- `EXPIRED`: `remaining = 0`. Alert sequence runs on wake; device skips deep sleep until alert done or dismissed.
- `BREAK`: enforced eye-rest pause (see 5a). Screen time frozen like PAUSED; break end is an absolute wall time.

### 5a · Eye Rest (Screen Break)

Every `CONFIG_MAGTAG_BREAK_INTERVAL_MIN` minutes (default 30, 0 disables) of
**accumulated RUNNING time** — pauses don't reset the accrual — the timer
auto-transitions to `BREAK` for `CONFIG_MAGTAG_BREAK_DURATION_MIN` minutes
(default 15):

- Entry: screen-time frozen (like pause), accrual reset, short break alarm
  (2 beeps × 3, any button silences), display flips to the **inverted**
  SCREEN BREAK layout with its own countdown + draining bar.
- During: Button A is ignored (no early resume); B (parent mode) and D work.
- End: double-beep chime, display returns to the normal layout in `PAUSED`;
  Button A resumes the screen timer. Break end within ~1 s of wall time
  (final-minute stay-awake, same mechanism as expiry).
- Break state and accrual persist in the NVS snapshot (v2): a power cycle
  mid-break resumes the break with the same absolute end time.

Pressing Start from PAUSED re-NTP-syncs and sets `expiry_wall_time = now + remaining_at_pause`.

All state is persisted in **RTC slow memory** (survives deep sleep) with an NVS snapshot as crash/power-loss backup (restored when still same-day; see section 2).

### 6 · Buttons

| Button | GPIO | Action |
|--------|------|--------|
| A | 15 | Start (IDLE/PAUSED → RUNNING, immediate; NTP sync after) / Pause (RUNNING → PAUSED) |
| B | 14 | Reset the **selected** timer to IDLE at full duration: always for a reloadable extra timer (except mid-run), otherwise only when `CONFIG_MAGTAG_PARENT_TESTING=y` |
| C | 12 | Swap timer type (Screen → extra 1 → … → Screen); refused while RUNNING or in a Screen Break |
| D | 11 | Force NTP re-sync + full display refresh |

Wake sources: A and D always; B when `CONFIG_MAGTAG_PARENT_TESTING=y` or any reloadable extra timer is configured; C only when a swap would succeed — extra timers configured AND the active timer not RUNNING/in a Screen Break (the EXT1 mask is rebuilt at every sleep entry; a press that could only be refused must not burn battery or a panel refresh). Buttons are debounced in software (10 ms).

### 6a · Extra timers (v1.3)

Up to four additional countdown timers (menuconfig: `MAGTAG_TIMER<n>_NAME/_MIN/_RELOADABLE`; an empty name disables the slot) for things like Piano practice or Meditation. They are plain countdowns sharing the Screen timer's alerts, NeoPixel sequences, NTP cadence, and RTC + NVS-snapshot persistence, but:

- No eye-rest breaks (Screen-only).
- Fixed configured duration instead of the day-schedule allocation.
- **Reloadable** timers reset to full via Button B on the same day, no ParentTesting needed. The mode line then counts the day's completed runs (reached 00:00): `Meditation (x2) - 10 min`. A mid-run reset does not count; non-reloadable timers never show a counter — once expired they stay depleted until rollover.
- Day rollover resets every timer, clears the counters, and reverts the selection to Screen.

Only the selected timer can be RUNNING — swapping requires a pause, so pause/expiry state of a deselected timer is frozen until you swap back.

### 7 · Display Layout (296×128 px)

```
┌──────────────────────────────────────────────────┐
│  Sat May 16  12:34 PM       Last sync: 12:30 PM  │  ← row 0–18
│                                                  │
│  ████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░  │  ← row 26–50 (progress bar, 24 px tall)
│                                                  │
│  ▮85%                       00:42:30             │  ← row 58–78 (battery left, remaining right)
│                                                  │
│  Weekday · 60 min                    RUNNING     │  ← status row (moved up)
│     ⏸        Reset                  ⟳            │  ← button labels (A B _ D)
└──────────────────────────────────────────────────┘
```

Button labels sit above the physical buttons: A shows the action a press
will take (play when IDLE/PAUSED, pause when RUNNING, hidden when EXPIRED),
"Reset" appears when `CONFIG_MAGTAG_PARENT_TESTING=y` or the selected timer
is reloadable (and not RUNNING), C shows a swap arrow when extra timers are
configured and the state allows swapping, D is the sync/refresh symbol.
When an extra timer is selected, the bottom-left mode line shows its name,
completion counter, and duration (e.g. `Meditation (x2) · 10 min`) instead
of the day-type + allocation.

- **Progress bar**: full-width (280 px usable), fill proportional to `remaining/allocation`. Thick outer border.
- **Remaining time**: centred; always `HH:MM:SS`.
- **State label**: bottom-right (`IDLE`, `RUNNING`, `PAUSED`, `TIME'S UP`).
- **Day-type + allocation**: bottom-left (e.g. `Holiday · 120 min`).

**Refresh strategy**:
- Partial refresh on every ~55-second wake while RUNNING.
- Full refresh on state transitions and every 5th partial refresh (prevents ghosting).
- Full refresh ~3 s; partial ~0.4 s — both acceptable at the 55-second cadence.

### 8 · Expiry Behaviour

When `remaining ≤ 0`:
1. **Display**: full refresh to "TIME'S UP" full-screen layout + empty bar.
2. **Speaker**: 3 short beeps, repeated every 3 seconds, for 5 cycles (15 seconds total). Stops immediately on any button press.
3. **NeoPixels**: slow red pulse during the 15-second alert. Stops on any button press.
4. After alert completes (or is dismissed), device returns to EXPIRED state and resumes deep sleep.

### 9 · Power

- Primary: USB-C.
- Battery: LiPo fallback. Deep sleep and WiFi-off discipline makes battery life viable.
- NeoPixels and speaker amplifier are powered off except during alert.
- Deep sleep current target: < 1 mA (ESP32-S2 deep sleep ~20 µA + display static current).

---

## Build System

**Recommended**: Platform.io with ESP-IDF framework.

```ini
; platformio.ini
[env:magtag]
platform = espressif32
board = adafruit_magtag29_esp32s2
framework = espidf
monitor_speed = 115200
```

Alternatively, pure ESP-IDF v5.x with `idf.py set-target esp32s2` is fully supported.

---

## Module Structure

```
main/
  main.c            — app_main: determine wake reason, dispatch to appropriate handler
  display.c/h       — SSD1680 SPI driver; layout rendering; partial vs full refresh logic
  timer.c/h         — state machine; expiry time calculation; RTC memory persistence
  ntp.c/h           — WiFi init/deinit; SNTP sync; drift correction helper
  nvs_config.c/h    — typed NVS accessors; first-boot defaults init
  schedule.c/h      — day-type determination (weekday/weekend/holiday); allocation lookup
  buttons.c/h       — wake reason decode; GPIO wakeup config; debounce
  audio.c/h         — PWM tone generation; beep pattern sequencer
  neopixel.c/h      — RMT-based NeoPixel driver; alert pulse pattern
  nvs_defaults.h    — compile-time default holiday list, allocations, WiFi placeholder

components/
  ssd1680/          — standalone SSD1680 e-ink SPI driver component
```

**RTC slow memory layout** (persistent across deep sleep):

```c
typedef struct {
    timer_state_t state;             // IDLE / RUNNING / PAUSED / EXPIRED
    int64_t       expiry_wall_time;  // Unix timestamp when timer expires; 0 if not set
    int32_t       remaining_at_pause; // seconds saved on PAUSE
    int32_t       allocation_sec;    // today's allocation in seconds
    char          last_date[11];     // "YYYY-MM-DD" for day-rollover detection
    int64_t       next_ntp_sync;     // Unix timestamp of next required NTP sync
    uint8_t       partial_refresh_count; // resets to 0 after full refresh
} rtc_state_t;
```

---

## Out of Scope (v1)

- OTA firmware updates
- Remote monitoring or companion app
- Multiple child profiles
- SD card usage
- BLE/SmartConfig WiFi provisioning (credentials stored in NVS; set initially via `nvs_gen.py` partition image or a `#warning` placeholder in `nvs_defaults.h`)
- Adjustable timer allocation via buttons (schedule-driven only)

---

## Implementation Notes for Coding Agents

1. **TDD required**: write unit tests for `schedule.c` (day-type logic), `timer.c` (state machine + expiry math), and `nvs_config.c` (serialisation round-trips) before implementing those modules.
2. **Worktrees/branches**: all development on feature branches; never commit directly to main.
3. **Display library**: LovyanGFX (ESP-IDF native). `display.cpp` is the single C++ translation unit; all other modules are C. `display.h` exposes a C-compatible API with `extern "C"` guards. LovyanGFX handles SSD1680 init, partial/full refresh, and font rendering.
4. **SNTP**: use the `esp_sntp` component with `CONFIG_SNTP_TIME_SYNC_METHOD_IMMED`; confirm sync via `sntp_get_sync_status()` before setting `expiry_wall_time`.
5. **Deep sleep wakeup**: `esp_sleep_enable_timer_wakeup(55 * 1000000ULL)` + `esp_sleep_enable_gpio_wakeup()` for all 4 buttons; use `esp_sleep_get_wakeup_cause()` on wake to dispatch correctly.
6. **RTC memory**: declare `rtc_state_t` with `RTC_DATA_ATTR` so the linker places it in RTC slow memory.
7. **WiFi lifecycle**: init → connect → sync → disconnect → deinit on every NTP session; never leave WiFi running between syncs.
