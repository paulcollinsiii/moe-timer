# RTC memory and the reboot

For an agent adding or changing state that has to outlive a wake. It lists
every variable in RTC memory, says what a reboot does to each, and what to do
when you add one. The human summary, a table of what survives which reset, is
in [architecture/state.md](../architecture/state.md).

The rule behind all of it: `RTC_DATA_ATTR` survives deep sleep and nothing
else. The bootloader reloads `.rtc.data` from the image on every reset except
a deep-sleep wake (`load_rtc_memory` in ESP-IDF's `esp_image_format.c`, cited
in the `main/display.c` comment above `s_takeover_on_panel`). A panic, an EN
reset and the `esp_restart()` that ends every OTA update all bring each
variable back at its initializer, which here is zero.

## Inventory

Re-grep before trusting this: `grep -rn 'RTC_DATA_ATTR\|RTC_NOINIT_ATTR' main components include`.

| Variable | File | Holds | After a reboot |
|----------|------|-------|----------------|
| `g_rtc_state` | `main/timer.c` | All timer state, today's date, chore ticks, release and mode, next sync, consecutive failed network windows (`wifi_join_failures`, layout v4) | Zeroed. Boot restores today's NVS snapshot and chore record (`timer_persist_try_restore`). Without a snapshot dated today, the day resets. The mode comes back as Timers. The failure count is not in the snapshot, so it restarts at 0 and the `No WiFi` hint needs three more failed windows. |
| `s_charge_locked`, `s_bedtime_locked`, `s_config_locked`, `s_clock_locked` | `main/lock_gate.c` | Which locks are engaged | Cleared. The next gate run re-engages any lock that still applies, as after any hard reset. The engage is idempotent: it costs one more paint. |
| `s_prev_fb`, `s_prev_fb_valid` | `main/display.c` | The last frame painted, for the ghost-clean diff | Invalid, so the next partial skips the clean pass. |
| `s_partial_count` | `main/display.c` | Refresh cadence counter | Restarts at 0. |
| `s_takeover_on_panel` | `main/display.c` | "A full-screen takeover is on the glass" | False. The post-OTA first paint is still full, but through the driver flag below, not this one. |
| `s_last_refresh_sec` | `components/ssd1680/ssd1680.c` | The 1 s rate guard's last refresh | Zero, so the guard allows the first refresh. |
| `s_prev_frame_valid` | `components/ssd1680/ssd1680.c` | Controller RAM holds a valid previous frame | False, so `ssd1680_resolve_refresh_mode()` promotes the first partial to full. This is what makes the post-reboot paint full. |
| `s_held_mask_at_sleep`, `s_sleep_entry_time` | `main/wake_flow.c` | Buttons held at sleep entry, for the continuation guard | Zero, meaning "nothing was held". Correct, because the guard fires only on a deep-sleep wake and a reboot is not one. |
| `s_rec` (`RTC_NOINIT_ATTR`) | `main/panic_diag.c` | The panic breadcrumb | Kept across a panic or `esp_restart()`; garbage after power-on, so it is validated by magic and checksum. `include/panic_diag.h` explains why no-init is right here and wrong for timer state. |

Not in RTC memory, deliberately: the update `ota_flow` found but has not
downloaded is a plain static. Neither a sleep nor a reboot keeps it, and the
next check finds it again, at most a day later.

## Why the OTA reboot is safe

The reboot never reaches `enter_deep_sleep()`, whose snapshot save covers
every ordinary path. So `ota_flow_apply` calls `persist_state`
(`timer_persist_save`) after the radio is down and right before
`restart()` (invariant S40). Without it, the post-OTA boot on a rollover day
would find yesterday's snapshot, refuse it, run the rollover a second time and
publish a summary of all-zero slots over HA's record. The comment above that
call in `main/ota_flow.c` also covers why it caps a rollback loop at one
attempt per day.

The snapshot is recognized as today's only because the wall clock survives the
reboot: with `CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER`, ESP-IDF keeps time in RTC
timer registers, not in `.rtc.data`.

One path can hand the new image garbage instead of zeros: a failsafe sleep
landing between the boot-partition switch and `restart()` makes the next boot
a deep-sleep wake of the new image, with the old image's `.rtc.data`. The
magic and version check in `timer_rtc_state_guard()` zeroes a foreign
`rtc_state_t` so the snapshot takes over (S46).

## Adding an RTC variable

1. Ask whether it must outlive a reboot. If so, RTC memory is not enough: put
   it in NVS, or in the timer snapshot.
2. If it lives in `rtc_state_t` or `timer_snapshot_t`, bump
   `RTC_STATE_VERSION` or `TIMER_SNAPSHOT_VERSION` in `include/timer.h`.
3. Decide what a zero means after a reboot, and make sure it is harmless.
   Add a row to the table above.
4. Do not reach for `RTC_NOINIT_ATTR` for state the firmware acts on: after
   power-on it holds whatever the RAM held, and only a validated record such
   as the panic breadcrumb can use it.
