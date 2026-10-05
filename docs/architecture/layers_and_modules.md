# Layers and modules

Where each module sits, and what its layer means for testing it. Read this
before adding a module or moving code between modules: the layer decides
whether the code can be host-tested, and how.

## Three layers

The layers are a testability ladder, not a call graph. Layer 1 is what a host
test can call with nothing stubbed, layer 2 needs its effects stubbed, and
layer 3 needs the board. Calls do not only go down: layer 2 calls both ways,
some drivers call layer 2, and two layer-1 modules, `status_led` and
`button_actions`, call out. Those edges are known and accepted;
[agent_notes/layering.md](../agent_notes/layering.md) lists them and says how
to re-scan for new ones.

**1. Pure policy.** Functions of their arguments only: nothing here reads the
clock, NVS or a GPIO, and a `time_t` arrives only as a parameter. These are the
firmware's decisions (which buttons wake the device, how long to sleep, what a
manifest means), and each has a host suite that calls it directly.

**2. Stateful orchestration.** Owns the RTC and NVS state, sequences layer-1
decisions, and calls drivers for the effects. A suite `#include`s the `.c`
under test and resolves its device calls with link-time stubs;
`test/test_wake_flow` is the worked example. `net_apply` and `ota_flow` go one
step further: every device effect arrives through an ops table
(`net_apply_ops_t`, `ota_flow_ops_t`) that `main.c` fills in, so their suites
run the real sequence against counters instead of a radio and a panel.

**3. Device drivers.** Touch silicon or FreeRTOS, and are verified on
hardware ([hardware_checklist.md](../hardware_checklist.md)). There are three kinds of
exception:
- `nvs_config` and `chore_store` reach flash only through `hal_nvs`, so they
  are host-tested against `test/mocks/mock_hal_nvs.c`.
- `net_window` and `ota_task` touch no silicon, but they are FreeRTOS task
  mechanics with nothing a host suite could call. Each is the task half of a
  split whose sequence half is layer 2 (`net_apply`, `ota_flow`).
- Several drivers keep their decisions in a pure half that is host-tested:
  `battery_soc` for `battery`, `display_layout` for `display`,
  `ssd1680_guard.c` for the panel driver, and the part of `panic_diag.c`
  outside its `NATIVE` fence.

## `main.c`, the composition root

`main/main.c` belongs to no layer. It holds wiring and device calls, never a
decision: every line must name one of four residency reasons (boot order is a
hardware contract; it runs in ISR or `esp_timer` context; it owns an ESP-IDF
handle with no module home; it is a branch-free thunk of three lines or
fewer). The rule is in `.claude/CLAUDE.md`, and the header comment of
`main.c` is the ledger that names each symbol's reason and the one branch
still recorded as debt.

## Module map

One line per module; the detail is in each module's header. The host-test
column names the suite under `test/`, and "no" means the module is verified
on hardware only.

### Layer 1: pure policy

| Module | Job | Host test |
|--------|-----|-----------|
| `wake_policy` | Refresh vs. alert choice for a render, round-minute snap, sync cadence, render-grid wait, final-minute countdown steps | `test_wake_policy` |
| `sleep_plan` | Sleep length (minute grid, sync lead, event lead, a second break event), the lock sleep that overrides it, and the two setup sleeps (1 s, and none) | `test_sleep_plan` |
| `bedtime` | Bed Time: time validation, window test, whether an engage alerts, whether a break would cross it | `test_bedtime` |
| `quiet_hours` | Minutes-of-day window math, including windows that wrap midnight | `test_quiet_hours` |
| `battery_policy` | Low-battery tiers: warn at 15 %, charge lock at 10 % with release above 15 % | `test_battery` |
| `battery_soc` | LiPo voltage to state of charge | `test_battery` |
| `chores` | Chore ticks as a bit mask, the list hash that clears them on an edit, the withheld time and its release | `test_chores` |
| `button_actions` | What a press of A, B or a chore button does, and the allocation a start receives | `test_button_actions` |
| `buttons_policy` | The EXT1 wake mask: which A–D buttons may wake the device, and whether BOOT may | `test_buttons_policy` |
| `setup_trigger` | Whether a wake enters setup mode, the BOOT hold tracker, and the "No WiFi" header hint | `test_setup_trigger` |
| `mqtt_form` | Parses and validates the setup page's MQTT form, as urlencoded or JSON, and escapes its prefill | `test_mqtt_form` |
| `qr_render` | Encodes the setup QR payload through the vendored `lib/qrcodegen` and answers the module matrix | `test_qr_render` |
| `button_latch` | Press latch fed by the button ISR, the release gate, and the B > C > D > A pick | `test_button_latch` |
| `status_led` | Colors for the timer-state pixel and the chore strip | `test_status_led` |
| `display_layout` | Layout arithmetic, the partial/full refresh cadence, ghost-clean bands | `test_display` |
| `display_screens` | LVGL screen builders (no ESP-IDF dependencies) | `test_display_render` (goldens) |
| `config_validate` | Field validators shared by both HA config paths, and the MQTT URI grammar the setup form uses | `test_config_validate` |
| `stats_json` | JSON for the stat, summary and discovery payloads; owns `STATS_JSON_DISC_SCHEMA_VER` | `test_stats_json` |
| `mqtt_rx` | Routes inbound MQTT by topic and reassembles chunked payloads | `test_mqtt_rx` |
| `mqtt_topics` | Topic string formatters | `test_mqtt_topics` |
| `ha_day_cmds` | The day-scoped decisions of an HA window: hold or drop a Screen-adjust target, publish the bonus clear | `test_ha_day_cmds` |
| `tones` | Alert-tone note tables and the streaming sine renderer | `test_tones` |
| `wav_header` | Validates the custom alert WAV's RIFF header | `test_wav_header` |
| `ota_policy` | Manifest parse, this device's entry, precondition gates, retry budget, failure reason codes | `test_ota_policy` |
| `ota_url` | Redirect acceptance: absolute https only, bounded hops | `test_ota_url` |
| `ota_facts` | Classifies transport and image errors | `test_ota_facts` |

### Layer 2: stateful orchestration

| Module | Job | Host test |
|--------|-----|-----------|
| `wake_flow` | The wake: decode, both handlers, the setup route and BOOT hold, day rollover, break start and end, the awake watches, latched presses, the OTA apply point | `test_wake_flow` |
| `setup_session` | The setup session: AP name and password, QR payload, the `/mqtt` page, the poll loop, and which sleep each outcome owes. Every device effect arrives through an ops table, like `ota_flow` | `test_setup_session` |
| `lock_gate` | The four locks: flags, engage and release, the full-refresh promotion, the sleep mode each implies | `test_lock_gate` |
| `timer` | The slot state machine (slot 0 is Screen, 1–4 are extra timers), breaks, the exposure balance, the RTC state | `test_timer` |
| `timer_defs` | Installs the extra-timer definitions each boot: NVS table first, Kconfig table as fallback | `test_timer_defs` |
| `timer_persist` | When the NVS snapshot is saved and when it may be restored | `test_timer_persist` |
| `schedule` | Today's day type, allocation and `chore_free` slice | `test_schedule` |
| `config_cache` | Wake-scoped cache of quiet hours and Bed Time | `test_config_cache` |
| `config_apply` | Parses and applies the retained HA config document to NVS | `test_config_apply` |
| `cmd_apply` | Parses the retained HA command (grant, locate) with apply-once dedup | `test_cmd_apply` |
| `ha_config` | Registry of HA-editable fields: bounds, discovery, state, apply; the discovery republish gate | `test_ha_config` |
| `net_apply` | Main-task side of a network window: applies clock steps, grants and redefinitions after the join | `test_net_apply` |
| `app_state` | Builds the display state and the stats snapshot from timer and device readings | `test_app_state` |
| `alerts` | One engine for every alarm (expiry, break, Bed Time, locate): sound, LEDs, dismissal | `test_alerts` |
| `ota_flow` | The order of an update: check, paint, download, commit, certify | `test_ota_flow` |

### Layer 3: device drivers

| Module | Job | Host test |
|--------|-----|-----------|
| `display` | LVGL flush to the panel, refresh cadence, ghost cleaning | no (policy half: `display_layout`) |
| `components/ssd1680` | SSD1680 SPI driver: init, full and partial refresh, BUSY wait, 1 s rate guard, panel sleep | `ssd1680_guard.c` only: `test_ssd1680` |
| `buttons` | EXT1 wake setup and decode, the awake ISR, pad level samples | no |
| `neopixel` | RMT LED driver behind its own task; the only owner of the power gate | no |
| `audio` | DAC playback of rendered tones or the streamed WAV | no |
| `battery` | Calibrated battery-voltage read | no (curve: `battery_soc`) |
| `light` | Ambient light read, in millivolts | no |
| `net_window` | The network window's task, its two completion signals and the snapshot rendezvous | no |
| `wifi_session` | WiFi station up and down for one window, and the one STA netif shared with setup | no |
| `setup_session_idf` | The real ops for `setup_session`: SoftAP, `esp_http_server`, the provisioning manager, SRP6a, the `/mqtt` handlers. `setup_mode_ops()` builds the table, so `main.c` supplies only the failsafe extend | no (decisions: `setup_session`, `mqtt_form`) |
| `setup_screens` | Thin adapters from the session's render ops to the `display` setup screens | no |
| `sleep_plan_idf` | Arms the deep-sleep timer, or nothing for a zero interval | no (decision: `sleep_plan_timer_armed`) |
| `ntp` | SNTP sync inside an open window | no |
| `mqtt_ha` | The HA MQTT session inside a window: publish, receive, apply | no (decisions: `ha_day_cmds`, `ha_config`, `mqtt_rx`) |
| `ota` | OTA transport: manifest GET, stepwise download, rollback cancel | no |
| `ota_task` | The download's own 16 KB task | no |
| `device_id` | MAC-derived MQTT id (`magtag-xxxxxx`) and the friendly name | no |
| `nvs_config` | Typed NVS accessors and first-boot defaults | `test_nvs_config` |
| `chore_store` | The chore list and the day-stamped tick record in NVS | `test_chore_store` |
| `hal_nvs`, `hal_time` | Thin NVS and `time()` wrappers that host tests replace | mocked in `test/mocks` |
| `panic_diag` | Panic counter and phase breadcrumb, published to HA | pure half: `test_panic_diag` |

### Shared headers with no `.c`

| Header | Holds |
|--------|-------|
| `include/nvs_keys.h` | Every NVS key name |
| `include/nvs_defaults.h` | Compile-time defaults that seed NVS; pulls in `credentials.local.h` for the OTA URL fallback. WiFi and MQTT are not defaults: setup mode enters them |
| `include/time_util.h` | Clock plausibility floor, local minutes of day (`test_time_util`) |
| `include/date_fmt.h` | The ISO date format every date comparison uses |
| `include/ota_timing.h` | Socket timeout, read buffer and the failsafe abort tail, tied by a `_Static_assert` |
| `include/panic_soak.h` | A reset-loop soak harness, compiled out unless hand-enabled |
| `include/esp_compat.h` | `esp_err_t` for code that also builds on the host |

`lib/cJSON` and `lib/qrcodegen` (Nayuki's QR generator, with the LVGL wrapper
stripped) are vendored upstream code. The scripts in `tools/` are described
in [developer_setup.md](../developer_setup.md).
