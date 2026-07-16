# Architecture Reference

Module map, hardware target, persistent-state layout, and subsystem design notes for the MagTag Screen Timer firmware. For build/flash/test commands see [developer_setup.md](developer_setup.md).

## Hardware Target

| Attribute | Value |
|-----------|-------|
| Board | Adafruit MagTag 2.9" (2025) |
| MCU | ESP32-S2 @ 240 MHz, 4 MB flash |
| Display | 2.9" grayscale e-ink 296×128 px, SSD1680 controller (FPC-7519rev.b, x-RAM offset 0) |
| Buttons | 4× tactile — GPIO 15/14/12/11 (A/B/C/D) (active-LOW, internal pull-up); Button C swaps timer types (v1.3) |
| NeoPixels | 4× RGB on GPIO 1 (power gate: GPIO 21 LOW = on) |
| Speaker | Amplifier shutdown pin GPIO 16 (HIGH = on) |
| No dedicated RTC chip | ESP32-S2 RTC timer used for deep-sleep wakeup |

## Module Structure

| File | Responsibility |
|------|---------------|
| `main/main.c` | `app_main`: dispatch on `esp_sleep_get_wakeup_causes()` bitmap to button or timer-tick handler; expiry alert orchestration |
| `main/timer.c/h` | Slot-based state machine (IDLE/RUNNING/PAUSED/EXPIRED/BREAK; slot 0 = Screen, slots 1..4 = extra timers); expiry calc; completion counters; RTC memory persistence |
| `main/timer_defs.c` | Extra-timer definition table from `MAGTAG_TIMER<n>_*` Kconfig symbols; installed via `timer_defs_install()` each boot |
| `main/display.c/h` | Flush to ssd1680 (I1 format); partial/full refresh policy; panel sleep handling |
| `main/display_screens.c` | LVGL screen builders (no ESP deps) — golden-tested on host via `test_display_render` |
| `main/display_layout.c` | Pure layout math (bar fill px, remaining-time text) — host-tested |
| `main/wake_policy.c` | Pure wake decisions: render/alert choice, round-minute snap, NTP cadence — host-tested |
| `main/battery_policy.c` | Pure low-battery tiers: <=15% warn badge, <=10% charge lock w/ hysteresis — host-tested |
| `main/battery_soc.c` | Pure LiPo voltage→state-of-charge curve — host-tested |
| `main/sleep_plan.c` | Pure deep-sleep duration planner — host-tested |
| `main/quiet_hours.c` | Pure quiet-hours window math (HHMM→minutes, wraparound) — host-tested |
| `main/config_validate.c` | Pure config field validation shared by the HA config paths — host-tested |
| `components/ssd1680/` | SSD1680 SPI driver: init, full/partial refresh, BUSY timeout, refresh-rate guard, RAM-retaining panel sleep |
| `main/ntp.c/h` | SNTP sync via esp_sntp (immediate mode set at runtime); runs inside a wifi_session window |
| `main/wifi_session.c/h` | WiFi station lifecycle for the periodic network window (begin/connect, end/teardown) |
| `main/mqtt_ha.c/h` | HA MQTT session riding the network window: discovery, retained stat/summary publishes (best-effort) |
| `main/stats_json.c/h` | Pure JSON builders for stat/summary/discovery payloads — host-tested |
| `main/ha_config.c/h` | HA-editable config field registry (bounds, discovery, state docs) — host-tested |
| `main/config_apply.c/h` | Parse+validate+apply the retained HA config document to NVS (cJSON) — host-tested |
| `main/cmd_apply.c/h` | Parse the retained HA command (grant/locate) with apply-once dedup (cJSON) — host-tested |
| `lib/cJSON/` | Vendored cJSON (upstream formatting; excluded from clang-format/cppcheck) |
| `main/device_id.c/h` | MAC-derived MQTT identity (`magtag-xxxxxx`) + NVS-backed friendly name |
| `main/light.c/h` | ALS-PT19 ambient light on GPIO 3; shares battery.c's ADC1 unit |
| `main/nvs_config.c/h` | Typed NVS accessors; first-boot defaults |
| `main/schedule.c/h` | Daily time-block lookup for screen-time allocation |
| `main/buttons.c/h` | EXT1 deep-sleep wakeup config + wake-button decode; 10 ms software debounce; awake-window GPIO ISR feeding the press latch |
| `main/button_latch.c` | Pure debounced press latch (host-tested): ISR records edges, awake checkpoints consume — presses during e-ink/NTP blind spots are never lost |
| `main/audio.c/h` | LEDC PWM tone generation; beep pattern sequencer (stop-flag aware) |
| `main/neopixel.c/h` | RMT-based NeoPixel driver behind a queue-fed LED task (sole RMT + power-gate owner): status pixels, binary countdown, alert pulses; `neopixel_stop_sync()` = ack'd gate-off before sleep |
| `main/hal_time.c/h` | HAL shim: wraps time() for testability |
| `main/hal_nvs.c/h` | HAL shim: wraps NVS API for testability |
| `include/nvs_defaults.h` | Compile-time defaults (holiday list, allocations, WiFi placeholder) |

## RTC Persistent State

Declared with `RTC_DATA_ATTR` so values survive deep sleep:

```c
typedef struct {
    timer_state_t state;              // per-slot machine; only the ACTIVE slot can be RUNNING/BREAK
    int64_t       expiry_wall_time;   // Unix timestamp; 0 if unset. Set at IDLE->RUNNING and PAUSED->RUNNING; shifted by timer_shift_expiry() after the post-start NTP sync.
    int32_t       remaining_at_pause; // seconds saved on PAUSE/BREAK
    int32_t       allocation_sec;
    int32_t       run_accum_sec;      // eye-rest accrual (slot 0 only)
    int64_t       run_started_wall;
    int64_t       break_expiry_wall;  // 0 unless BREAK (slot 0 only)
    uint16_t      completions;        // runs that reached expiry today
} timer_slot_state_t;

typedef struct {
    timer_slot_state_t slots[TIMER_SLOT_COUNT]; // slot 0 = Screen, 1..TIMER_EXTRA_SLOTS = extra timers
    uint8_t       active_slot;        // Button C cycles enabled slots; rollover reverts to 0
    char          last_date[11];      // "YYYY-MM-DD"
    int64_t       next_ntp_sync;      // timestamp of next required sync
} rtc_state_t;
```

(The authoritative definitions live in `include/timer.h` — check there first; this snapshot can lag.)

Extra-timer definitions (name/duration/reloadable) come from `MAGTAG_TIMER<n>_*` menuconfig symbols and live in rodata — `timer_defs_install()` must run each boot before any `timer_*` call (host tests inject their own table via `timer_set_defs()`). The single-timer API (`timer_start/pause/tick/...`) always operates on the active slot.

The ssd1680 component additionally keeps a `RTC_DATA_ATTR` last-refresh timestamp for its refresh-rate guard; display.c keeps its previous-frame buffer and partial/full cadence counter in RTC memory.

## Subsystem Design Notes

- **Display stack**: LVGL 9 managed component (`LV_COLOR_FORMAT_I1`, clib malloc) over the custom `components/ssd1680` driver.
- **Refresh policy vs panel protection**: partial/full cadence (every 5th wake full, state changes full) is display.c policy; the ssd1680 driver independently enforces BUSY serialization + a 1 s minimum-interval guard (RTC-persistent) and puts the panel into RAM-retaining deep sleep (mode 1) after every refresh.
- **SNTP**: `esp_sntp` with `esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED)` set at runtime (the kconfig symbol was removed in IDF 6). `ntp_sync()` NEVER modifies `expiry_wall_time` — it corrects `time(NULL)` and the subtraction self-corrects. Exception: timer start/resume runs BEFORE the sync (immediate UX feedback); main.c measures the sync's clock step against the monotonic clock and applies it via `timer_shift_expiry()`.
- **Partial-refresh ghost cleaning**: display.c runs partials as a double pass (text bands inverted, 1.1 s wait, true frame) so per-wake text stays crisp between the every-5th-wake fulls; skipped when `ssd1680_partial_diff_ready()` is false.
- **Deep sleep wakeup**: `esp_sleep_enable_timer_wakeup(55 * 1000000ULL)` + `esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW)` with RTC-domain pull-ups. Wake buttons only: A (15) and D (11) always, B (14) and C (12) only when their press would succeed — B via `timer_reload_allowed()` (reloadable selected timer or `CONFIG_MAGTAG_PARENT_TESTING`, never while RUNNING), C via `timer_swap_allowed()` (extras configured AND active timer not RUNNING/BREAK). The mask is rebuilt at every sleep entry, so it tracks the state machine. (`esp_sleep_enable_gpio_wakeup()` is light-sleep-only on ESP32-S2.)
- **WiFi lifecycle**: one bounded network window per sync, run on a dedicated network task (main.c): wifi_session_begin → SNTP → snapshot rendezvous → MQTT/HA session (best-effort, skipped when broker URI empty) → wifi_session_end. WiFi is off outside windows. The task never mutates timer state, paints, or touches LEDs — it signals NTP-settled (with the measured clock step) and window-done; grants/bonus/locate are buffered by mqtt_ha and applied by the orchestrator after `net_window_finish()`, which also re-installs the timer-defs blob and runs `timer_reconcile_def` on the active extra slot (config edits that redefine a running timer). Interactive wakes (BTN A start/resume, BTN D) paint once NTP settles while MQTT drains behind the panel; unattended wakes use the blocking `try_net_window()`. `enter_deep_sleep` joins any straggling window (bounded) before sleeping.
- **Awake press latch**: buttons are dispatched on EXT1 wake; while awake a NEGEDGE ISR latches presses (button_latch.c) and checkpoints consume them — only Button A pause + alarm dismissal act on latched presses (B/C/D stay wake-press-only). ISR handlers detach in `buttons_configure_wakeup()` before the pads move to the RTC mux; held-button logic stays level-based.
- **Awake failsafe**: a one-shot esp_timer (`MAGTAG_MAX_AWAKE_SEC`, default 180 s) forces a snapshot-preserving deep sleep if a wake wedges (WiFi hang, stuck BUSY) so the battery cannot drain.
- **Battery charge lock**: `check_charge_lock()` runs before wake dispatch — <=10% pauses a RUNNING timer, paints Charge Me! once, drops button wake sources, and sleeps 600 s battery-recheck intervals; releases >15% (battery_policy.c hysteresis).
- **Bed Time lock**: `check_bedtime()` runs in both wake handlers right after day rollover (policy in host-tested `main/bedtime.c`). At/after the configured HHMM (default 2200; 0 disables; validated to 0-or-1800–2359 so a bad edit can't daytime-lock the device) it pauses a RUNNING timer, paints the inverted Bed Time screen once, alerts audibly only when it interrupted RUNNING/BREAK (a pending screen break that would cross bedtime skips straight to Bed Time), then sleeps charge-lock-style: no button wakes, fixed 7200 s intervals that only run a net window (NTP + HA config pickup — a bedtime edit applies at that same wake) and re-check the gate. Day rollover clears the lock; the flag is RTC-only, so a hard reset just replays the engage.
- **Alert tones**: alert audio is DAC-synthesized (`main/tones.c` note tables + envelope renderer, host-tested; `main/audio.c` pumps `dac_continuous` DMA on the S2's DAC CH0 = GPIO17) with per-alert tone selection (expiry/break/bedtime) stored in NVS and exposed as HA selects, plus a global alert volume (NVS `alert_vol`, HA number, 0–200 %: 100 % = the gentle reference amplitude, above that the renderer/WAV downmix apply digital gain clipped at the DAC rails for real loudness on the small speaker; menuconfig default 200). "Custom WAV" streams 16-bit mono PCM (8–22.05 kHz) from the raw 952 KB `assets` partition (`tools/flash_assets.sh`), falling back to the chime when absent/invalid.
- **Display golden tests**: `test_display_render` compiles vendored LVGL for the host (`test/mocks/lv_conf_host.h`) and byte-compares each screen against `test/test_display_render/golden/*.bin`; regenerate intentionally changed layouts with `MAGTAG_WRITE_GOLDEN=1`.
- **NeoPixel power gate**: GPIO 21 must be HIGH (off) on every boot/wake before any other peripheral code runs.
- **IDF 6 component names**: GPIO/LEDC/SPI drivers are `esp_driver_gpio`/`esp_driver_ledc`/`esp_driver_spi` in `REQUIRES` (the `driver` umbrella no longer pulls them in).
