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
| `main/main.c` | `app_main`: dispatch on `esp_sleep_get_wakeup_causes()` bitmap to button or timer-tick handler; wake sequencing, event watch loops, sleep entry, lock gates |
| `main/alerts.c` | Audible alert engine (expiry/break/bedtime/locate): press-latch drain, NeoPixel pulse, audio task lifecycle + join, dismissal polling |
| `main/net_apply.c` | Orchestrator side of a network window: pre-window def capture, post-join apply (clock-step shift, bonus, grant, def reconcile, locate) — host-tested via injected device effects |
| `main/app_state.c` | Display-state + stats-snapshot assembly (IDLE full bar, per-slot allocation fallbacks, warn badge, button availability, break chip + swap hint) — host-tested; device reads injected |
| `main/timer.c/h` | Slot-based state machine (IDLE/RUNNING/PAUSED/EXPIRED/BREAK; slot 0 = Screen, slots 1..4 = extra timers); expiry calc; completion counters; RTC memory persistence. BREAK is slot-0-only and independent of the selection — see the state-model invariant below |
| `main/timer_defs.c` | Extra-timer definition table from `MAGTAG_TIMER<n>_*` Kconfig symbols; installed via `timer_defs_install()` each boot |
| `main/display.c/h` | Flush to ssd1680 (I1 format); partial/full refresh policy; panel sleep handling |
| `main/display_screens.c` | LVGL screen builders (no ESP deps) — golden-tested on host via `test_display_render` |
| `main/display_layout.c` | Pure layout math (bar fill px, remaining-time text) — host-tested |
| `main/wake_policy.c` | Pure wake decisions: render/alert choice (incl. the `break_ended` full-refresh promotion), round-minute snap, NTP cadence, render-grid residue, final-minute countdown steps — host-tested |
| `main/battery_policy.c` | Pure low-battery tiers: <=15% warn badge, <=10% charge lock w/ hysteresis — host-tested |
| `main/battery_soc.c` | Pure LiPo voltage→state-of-charge curve — host-tested |
| `main/sleep_plan.c` | Pure deep-sleep duration planner, incl. the optional secondary break-end event — host-tested |
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
    timer_state_t state;              // per-slot machine; only the ACTIVE slot can be RUNNING. BREAK is slot 0's alone and may be held there while ANY slot is active.
    int64_t       expiry_wall_time;   // Unix timestamp; 0 if unset. Set at IDLE->RUNNING and PAUSED->RUNNING; shifted by timer_shift_expiry() after the post-start NTP sync.
    int32_t       remaining_at_pause; // seconds saved on PAUSE/BREAK
    int32_t       allocation_sec;
    int32_t       run_accum_sec;      // screen-exposure balance (slot 0 only, whichever slot drives it)
    int64_t       run_started_wall;   // live segment start; 0 = nothing running
    int64_t       break_expiry_wall;  // 0 unless BREAK (slot 0 only)
    uint16_t      completions;        // runs that reached expiry today
} timer_slot_state_t;

typedef struct {
    timer_slot_state_t slots[TIMER_SLOT_COUNT]; // slot 0 = Screen, 1..TIMER_EXTRA_SLOTS = extra timers
    uint8_t       active_slot;        // Button C cycles enabled slots; rollover reverts to 0
    uint8_t       break_interrupted_slot; // slot selected when the break started; break end returns to it
    char          last_date[11];      // "YYYY-MM-DD"
    int64_t       next_ntp_sync;      // timestamp of next required sync
} rtc_state_t;
```

(The authoritative definitions live in `include/timer.h` — check there first; this snapshot can lag.)

Extra-timer definitions (name/duration/reloadable/break-eligible) come from `MAGTAG_TIMER<n>_*` menuconfig symbols and live in rodata — `timer_defs_install()` must run each boot before any `timer_*` call (host tests inject their own table via `timer_set_defs()`). The single-timer API (`timer_start/pause/tick/...`) always operates on the active slot.

**State-model invariant (v1.4).** Only the active slot can be `RUNNING`, but `TIMER_BREAK` is decoupled from the selection: it lives on slot 0 only and may be held there while any slot is active, so a Screen Break enforces the *screen timer* without freezing the device. Every break helper (`timer_break_active/_remaining/_tick`, `timer_start_break`, `timer_shift_expiry`) therefore reads slot 0 explicitly, never the active slot. `include/timer.h` carries the authoritative version of this comment; `test/test_timer` encodes it as invariants I1–I5 in an `assert_state_legal()` helper that runs from `tearDown`, so every test in that suite trips on an illegal state.

Callers needing the break-end *edge* must call `timer_break_tick()` before `timer_tick()` — `timer_tick()` also ends an elapsed break (so no path can strand one behind a running extra timer), but silently.

**The screen-exposure balance (v1.5).** `break_eligible` is a per-timer flag meaning "this activity is time away from a screen". It has one consequence in two places: an eligible timer may be *started* during a Screen Break, and its RUNNING time *drains* the exposure balance instead of feeding it. Slot 0 is permanently non-eligible — screen time is the original non-eligible activity, which is what collapses the eye-rest counter and the break rules into a single quantity:

> Screen exposure is a signed balance on slot 0. Non-eligible RUNNING time adds 1:1, eligible RUNNING time subtracts 1:1, floored at zero. A break is due when it reaches the interval, and a Screen Break refuses to start any slot that is not `break_eligible`.

The balance lives on **slot 0 whichever slot is running**, so `timer_run_accum()` keys on slot 0's `run_started_wall != 0`, not on any slot's state — slot 0 is routinely IDLE or PAUSED while a chore timer drives it. The direction is **never stored**: it is derived at every fold from the running slot's `break_eligible`, so an HA edit of the flag cannot desynchronise from the balance. Three consequences worth knowing before touching `timer.c`:

- **Every transition into or out of RUNNING is a fold point** (I10) — start, pause, resume, expiry, a reconcile that renames/disables the slot, and a reconcile that flips `break_eligible` (which folds at the *old* sign, available only from `old_def`, since `net_apply` reinstalls the defs table before reconciling). Miss one and the balance either loses a segment or advances forever.
- **The floor is enforced twice**: at fold time and at read time, because the in-flight segment is not folded yet. A long eligible run must report 0, never a negative, to logging, HA and the break check.
- **Only two things reset it** (I9): a break start and the day rollover. `timer_start()` deliberately does *not* — a start that cleared it would let any timer press zero the eye-rest clock.

Idle neither adds nor drains. That is deliberate: the device deep-sleeps whenever nothing runs, so decaying through idle would mean the balance almost never survives to reach the interval, and "paused" is indistinguishable from "walked away".

`timer_break_due()` is gated on neither RUNNING nor slot 0's state — only on a break not already running. The balance can cross while Screen sits IDLE, an earned break is not un-earned by pausing, and eye rest must keep working after the day's allocation is spent: `EXPIRED` is the *normal* end-of-day state, and folding laundry with the TV on is exactly when a break still matters.

Break entry pauses whatever is RUNNING (I6), records `break_interrupted_slot` and `break_prev_state`, resets the balance and snaps the selection to slot 0. Break exit puts slot 0 back into `break_prev_state` (I7).

I6 holds **at entry only**: an HA edit flipping a RUNNING slot to non-eligible mid-break leaves it running behind the BREAK. That is accepted rather than prevented — the reconcile folds at the old sign and re-arms at the new one, so the exposure is counted correctly toward the next break, and pausing a running timer from inside a network window would be a much larger change than the case warrants.

**I7 is stored, not derived, and that is load-bearing.** Deriving slot 0's exit state from `remaining_at_pause` refunds the whole day in two ways, both reachable on a plain 60/30 config: an HA deduction landing *during* the break zeroes the banked value, so the exit reads as "never started" and hands back a fresh allocation; and an `EXPIRED` slot 0 used to carry a stale banked value from an earlier pause, so the exit handed that back instead. (`mark_expired` now clears `remaining_at_pause` — an expired timer holds nothing, which `timer_slot_remaining` already reported.) Deriving from `allocation_sec` instead fails too, because an HA deduction can zero that as well. The byte is free: the snapshot was already being versioned for `break_interrupted_slot`.

Likewise the live segment's sign comes from `run_segment_slot` — the slot that *armed* it — never from the selection. `run_started_wall` lives on slot 0, so nothing about the segment is recoverable from the selected slot, and the selection can move underneath it (`timer_ensure_active_slot_enabled` runs unconditionally on a snapshot restore and does not check RUNNING). Both bytes ride the snapshot; restoring the arming slot as 0 would invert an eligible timer's drain into an accrual.

`test/test_timer` names its cases after the plan's behaviour-table rows (`test_row11_...`), so a failure points at the contract it broke.

The ssd1680 component additionally keeps a `RTC_DATA_ATTR` last-refresh timestamp for its refresh-rate guard; display.c keeps its previous-frame buffer and partial/full cadence counter in RTC memory.

## Subsystem Design Notes

- **Display stack**: LVGL 9 managed component (`LV_COLOR_FORMAT_I1`, clib malloc) over the custom `components/ssd1680` driver.
- **Refresh policy vs panel protection**: partial/full cadence (every 5th wake full, state changes full) is display.c policy; the ssd1680 driver independently enforces BUSY serialization + a 1 s minimum-interval guard (RTC-persistent) and puts the panel into RAM-retaining deep sleep (mode 1) after every refresh.
- **SNTP**: `esp_sntp` with `esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED)` set at runtime (the kconfig symbol was removed in IDF 6). `ntp_sync()` NEVER modifies `expiry_wall_time` — it corrects `time(NULL)` and the subtraction self-corrects. Exception: timer start/resume runs BEFORE the sync (immediate UX feedback); main.c measures the sync's clock step against the monotonic clock and applies it via `timer_shift_expiry()`.
- **Partial-refresh ghost cleaning**: display.c runs partials as a double pass (text bands inverted, 1.1 s wait, true frame) so per-wake text stays crisp between the every-5th-wake fulls; skipped when `ssd1680_partial_diff_ready()` is false.
- **Deep sleep wakeup**: `esp_sleep_enable_timer_wakeup(55 * 1000000ULL)` + `esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW)` with RTC-domain pull-ups. Wake buttons only: A (15) and D (11) always, B (14) and C (12) only when their press would succeed — B via `timer_reload_allowed()` (reloadable selected timer or `CONFIG_MAGTAG_PARENT_TESTING`, never while RUNNING), C via `timer_swap_allowed()` (extras configured AND active timer not RUNNING — a Screen Break deliberately does not refuse, so C stays a wake source right through one). The mask is rebuilt at every sleep entry, so it tracks the state machine. (`esp_sleep_enable_gpio_wakeup()` is light-sleep-only on ESP32-S2.)
- **WiFi lifecycle**: one bounded network window per sync, run on a dedicated network task (main.c): wifi_session_begin → SNTP → snapshot rendezvous → MQTT/HA session (best-effort, skipped when broker URI empty) → wifi_session_end. WiFi is off outside windows. The task never mutates timer state, paints, or touches LEDs — it signals NTP-settled (with the measured clock step) and window-done; grants/bonus/locate are buffered by mqtt_ha and applied by `net_apply_finish()` (host-tested `main/net_apply.c`), which also re-installs the timer-defs blob and runs `timer_reconcile_def` on every extra slot redefined mid-window (only the active slot drives sound/display). A start/resume that painted before the sync settled is marked via `net_apply_note_start_unsynced()`; if the sync lands during the MQTT tail, the finish applies the measured clock step to the expiry (`net_window_take_clock_step()` is consume-once, so the paint-time and finish-time paths can never double-apply). Interactive wakes (BTN A start/resume, BTN D) paint once NTP settles while MQTT drains behind the panel; unattended wakes use the blocking `net_apply_try_window()`. `enter_deep_sleep` joins any straggling window (bounded) before sleeping.
- **Two-event sleep planning**: because a Screen Break keeps running behind another selected timer, a wake can have two future events — the active timer's expiry and the break end. `sleep_plan_in_t` carries the break as an optional *secondary* event (`break_remaining_sec`) with the same `SLEEP_PLAN_EVENT_LEAD_SEC`; it only ever pulls the wake in. `main.c` populates it **only when the end will actually chime** (`timer_break_active() && state != TIMER_BREAK && !timer_any_extra_running()`): a suppressed end is silent, so it needs neither a dedicated wake nor a stay-awake watch and simply drops the header chip at the next tick wake. Suppression can only change via a button press, which is a wake and therefore a re-plan.
- **Break-end edge**: the transition is **latched** in timer.c, not returned: `timer_tick()` ends an elapsed break internally, so an edge reported only as a return value could be consumed by any tick and the chime silently lost (it was — the expiry alert's tick swallowed it). `timer_break_tick()` latches the break's wall end; `timer_break_take_ended(now, &overdue)` consumes it exactly once, reporting how late *the drain* is. Ordering therefore stops mattering: tick whenever, drain whenever. `handle_break_end()` in main.c is the single owner of the policy — it drains the latch, applies `wake_policy_break_chime()` (pure, host-tested), chimes only when nothing else is RUNNING *and* the drain is within `BREAK_CHIME_GRACE_SEC` (= `SLEEP_PLAN_WATCH_SEC`, 75 s: charge/bed-time locks and power cycles can span the end, and the chime is an "it just happened" signal, not a replay), and on chiming also calls `timer_select_screen()` — the chime and the return to Screen are one event. It sets a wake-sticky flag passed to `wake_policy_render` as `break_ended`, which forces a full refresh because the panel changed (chip gone, possibly a different timer's layout) even when `before == after`. It runs at the top of both wake handlers (after rollover + bedtime, before `before` is captured, so the snap is invisible to the state diff) and at a **guaranteed drain** in `maybe_wait_for_event()` — the one point both handlers reach after all timer work and before sleep. `enter_deep_sleep()` logs a warning if a latch ever arrives there undrained, which would mean a new path skipped the drain.
- **Awake press latch**: buttons are dispatched on EXT1 wake; while awake a NEGEDGE ISR latches presses (button_latch.c) and checkpoints consume them — only Button A pause + alarm dismissal act on latched presses (B/C/D stay wake-press-only). ISR handlers detach in `buttons_configure_wakeup()` before the pads move to the RTC mux; held-button logic stays level-based.
- **Awake failsafe**: a one-shot esp_timer (`MAGTAG_MAX_AWAKE_SEC`, default 180 s) forces a snapshot-preserving deep sleep if a wake wedges (WiFi hang, stuck BUSY) so the battery cannot drain.
- **Battery charge lock**: `check_charge_lock()` runs before wake dispatch — <=10% pauses a RUNNING timer, paints Charge Me! once, drops button wake sources, and sleeps 600 s battery-recheck intervals; releases >15% (battery_policy.c hysteresis).
- **Bed Time lock**: `check_bedtime()` runs in both wake handlers right after day rollover (policy in host-tested `main/bedtime.c`). At/after the configured HHMM (default 2200; 0 disables; validated to 0-or-1800–2359 so a bad edit can't daytime-lock the device) it pauses a RUNNING timer, paints the inverted Bed Time screen once, alerts audibly only when it interrupted RUNNING or a break — `bedtime_should_alert(state, timer_break_active())`, since the break may be running behind a different selected timer, and a pending screen break that would cross bedtime skips straight to Bed Time — then sleeps charge-lock-style: no button wakes, fixed 7200 s intervals that only run a net window (NTP + HA config pickup — a bedtime edit applies at that same wake) and re-check the gate. Day rollover clears the lock; the flag is RTC-only, so a hard reset just replays the engage.
- **Alert tones**: alert audio is DAC-synthesized (`main/tones.c` note tables + envelope renderer, host-tested; `main/audio.c` pumps `dac_continuous` DMA on the S2's DAC CH0 = GPIO17) with per-alert tone selection (expiry/break/bedtime) stored in NVS and exposed as HA selects, plus a global alert volume (NVS `alert_vol`, HA number, 0–200 %: 100 % = the gentle reference amplitude, above that the renderer/WAV downmix apply digital gain clipped at the DAC rails for real loudness on the small speaker; menuconfig default 200). "Custom WAV" streams 16-bit mono PCM (8–22.05 kHz) from the raw 952 KB `assets` partition (`tools/flash_assets.sh`), falling back to the chime when absent/invalid.
- **Display golden tests**: `test_display_render` compiles vendored LVGL for the host (`test/mocks/lv_conf_host.h`) and byte-compares each screen against `test/test_display_render/golden/*.bin`; regenerate intentionally changed layouts with `MAGTAG_WRITE_GOLDEN=1`.
- **NeoPixel power gate**: GPIO 21 must be HIGH (off) on every boot/wake before any other peripheral code runs.
- **IDF 6 component names**: GPIO/LEDC/SPI drivers are `esp_driver_gpio`/`esp_driver_ledc`/`esp_driver_spi` in `REQUIRES` (the `driver` umbrella no longer pulls them in).
