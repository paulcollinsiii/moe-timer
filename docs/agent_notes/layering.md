# Layering: the known cross-layer edges

For an agent about to add a call that crosses layers. The layers themselves
are defined in [architecture/layers_and_modules.md](../architecture/layers_and_modules.md);
this note lists the calls that do not fit the ladder and are accepted anyway,
and says how to find new ones.

## Re-scan, never count from here

No direction rule holds across the tree, not even "a driver never calls
layer 2". This list is illustrative, not a census. Never write a count of
edges into any doc or comment; it goes stale silently. Before relying on an
edge being absent, scan the source with comments stripped, because comments
name functions they do not call:

```sh
gcc -fpreprocessed -dD -E -P main/buttons.c \
  | grep -oE '\b(wake_flow_|lock_gate_|timer_|config_cache_|config_apply|cmd_apply|ha_config_|net_apply_|app_state_|alerts?_|schedule_|ota_flow_)[a-z_]*\(' \
  | sort -u
```

Swap the file and the prefixes for the layer you are checking. A
`static inline` in a layer-2 header (for example `schedule_day_type_name`) is
not a link edge.

## Driver → layer 1

Normal, and not an exception: a driver reaching for a pure helper is what the
ladder is for. Examples: `audio.c` → `tones_*`, `wav_header_parse`;
`buttons.c` → `button_latch_*`, `buttons_policy_wake_mask`,
`button_a_toggle_allowed`, `button_chore_ack_allowed`; `mqtt_ha.c` →
`stats_json_*`, `mqtt_rx_on_data`, `ha_day_*`.

## Driver → layer 2

Scanned 2026-10-01.

| Driver | Calls | Kind |
|--------|-------|------|
| `net_window.c` | `timer_record_ntp_sync`, in `net_window_wait_ntp` and `net_window_join` | Mutator. Both run on the main task, not inside `net_window_task`. The task itself still rewrites the RTC chore acks through `config_apply.c:apply_chores` (invariant S28). |
| `net_window.c` | `ota_flow_check`, inside `net_window_task` | Control-flow inversion: layer 3 supplies the thread, layer 2 the sequence. The check has to ride the window the wake already opened. |
| `ota_task.c` | `ota_flow_apply` | The same inversion. The module exists only to give the download a 16 KB stack, keeping `ota_flow.c` free of FreeRTOS. |
| `buttons.c` | `timer_swap_allowed`, `lock_gate_wake_d_only` | Reads, to fill the `buttons_policy` input. |
| `mqtt_ha.c` | `timer_slot_def` | Read, for discovery. |
| `mqtt_ha.c` | `ota_flow_stat` | Read at publish time, because the stats snapshot is built before the OTA check runs ([architecture/ota.md](../architecture/ota.md)). |
| `mqtt_ha.c` | `ha_config_discovery_gate`, `ha_config_discovery`, `ha_config_discovery_topic`, `ha_config_fields`, `ha_config_json_escape`, `ha_config_state_json`, `ha_config_set`, `config_apply`, `cmd_apply_for_snapshot` | The densest caller, and an ordinary shape there rather than an exception: the HA session is where the documents arrive. `ha_config_set`, `config_apply` and `cmd_apply_for_snapshot` are mutators. The parsing and validation they delegate to is host-tested. |

## Layer 1 → out

Two layer-1 modules call out. Both stay in layer 1 because their tests are
about the pure table, and the impure part is a thin application of it:

- `status_led.c`: `status_led_for_state()` and `chores_led_for()` are pure
  tables. Their two wrappers drive the pixel: `status_led_show_timer_state()`
  reads `timer_get_state()` (layer 2) and calls `neopixel_status_pixel()`,
  and `chores_led_show()` calls `neopixel_highpri_pixel()` (both layer 3).
- `button_actions.c`: the outcome map is pure, but it applies itself. It
  calls timer mutators, including `timer_start`, `timer_pause`,
  `timer_resume`, `timer_reload`, `timer_set_mode`, `timer_chore_set_acked`,
  `timer_chore_set_released` and `timer_release_gated`; reads `schedule_*`
  and `timer_active_def`; and reads and writes the chore record through
  `chore_store_load_names` and `chore_store_save_ack` (layer 3).
  `test_button_actions` stubs all of these, which is layer-2 treatment for a
  layer-1 file.

## Layer-1 qualifications

Some layer-1 modules bend "no ESP-IDF, no globals" without leaving the layer:

- `battery_soc` implements `battery_percent_from_mv` declared in `battery.h`,
  and `display_layout` implements the pure half of `display.h`. A layer-3
  header does not make them layer-3 code.
- `display_screens` compiles against LVGL and calls `localtime_r`/`strftime`
  on a `time_t` it was handed.
- `ota_policy` parses the manifest with the vendored cJSON, as layer-2
  `config_apply` does.
- `ota_facts` includes ESP-IDF headers (`esp_ota_ops.h`,
  `esp_image_format.h`, `esp_tls_errors.h`) for their error constants only. The
  host build supplies those constants, so it is still tested with nothing
  stubbed.
- `ota_url` composes `config_validate`'s https rule and nothing else.
