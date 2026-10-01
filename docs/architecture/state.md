# State

What the firmware remembers, where each piece lives (RTC memory, NVS or the
compiled image), and which resets each one survives. The device deep-sleeps
between wakes and is rebooted by every OTA update, so "where does this live?"
decides whether a value is still there on the next wake.

## Where state lives

| Store | What it holds | Owner |
|-------|---------------|-------|
| RTC memory (`RTC_DATA_ATTR`) | The timer state `g_rtc_state`: each slot's state, expiry, banked and allocated seconds, completions; the exposure balance and break end; the selected slot; today's date; today's chore ticks, release and mode; the next sync time. The authoritative layout is `rtc_state_t` in `include/timer.h`. | `main/timer.c` |
| RTC memory, other owners | The four lock flags, the display's previous frame and refresh cadence, the panel driver's rate-guard timestamp, the held-button guard | [agent_notes/rtc_and_reboot.md](../agent_notes/rtc_and_reboot.md) lists every variable |
| RTC no-init memory | The panic breadcrumb, checked by magic and checksum because it is garbage after power-on | `main/panic_diag.c` |
| NVS (namespace `timer_cfg`) | Settings (allocations, holidays, time zone, quiet hours, Bed Time, tones, volume, OTA URL), WiFi and MQTT credentials, the extra-timer table, the chore list and today's tick record, the timer snapshot, the OTA status keys | Key names: `include/nvs_keys.h` |
| The image (rodata) | Compile-time defaults that seed NVS on first boot and back it up when a key is missing: `include/nvs_defaults.h` and the Kconfig values | `main/nvs_config.c` |
| Plain RAM | Everything else, for one wake only: config caches, buffered network results, a found-but-not-downloaded update, the locks' "just released" flags | |

How NVS is seeded from the defaults, and why a reflash reseeds without
erasing anything, is in [developer_setup.md](../developer_setup.md).

## What survives which reset

| | Deep-sleep wake | Software reset (panic, OTA reboot) | Power-on (power loss, Reset button) |
|---|---|---|---|
| `RTC_DATA_ATTR` | kept | **zeroed** | zeroed |
| Panic breadcrumb (`RTC_NOINIT_ATTR`) | kept | kept | garbage, rejected by its checksum |
| NVS | kept | kept | kept |
| Wall clock | kept | kept | **lost** until NTP sets it |
| Plain RAM | lost | lost | lost |

The bootloader reloads RTC data from the image on every reset except a
deep-sleep wake ([invariant S47](../architecture.md#system-invariants)). The
wall clock outlives a software reset because it is kept in RTC timer
registers, not RTC data (`CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER`). A power-on
has no clock at all, which is what the no-clock lock is for
([No Clock](../behavior/locks.md#no-clock)).

## The NVS snapshot bridges a reboot

Anything that must outlive a reboot has to be in NVS. For the timer state that
is a versioned, checksummed copy of the RTC state, `timer_snapshot_t`
(`include/timer.h`). `main/timer_persist.c` saves it only when it changed,
and it is saved:
- on every sleep, in `enter_deep_sleep()`;
- when the Bed Time or config-error lock engages, at a break start, and at an
  expiry alert, because those pause or end a run that should be durable at
  once;
- right before the OTA reboot, which never reaches the sleep funnel
  ([S40](../architecture.md#system-invariants)).

At boot, `timer_rtc_state_guard()` zeroes an RTC image whose magic or version
does not match (S46), and `timer_persist_try_restore()` restores the snapshot
over it only if it validates and is dated today (S14, S15). Today's chore
ticks and release are restored with it; they are also written to NVS on every
tick, so a reboot never makes a kid redo a chore (S16). The mode is not
restored: a restart comes back on the timer screen.

So a power cycle never refunds or costs the day. A snapshot from yesterday is
refused, and the day resets only on a genuine date change.

## Timer state is absolute

A running slot stores the wall time it expires at (`expiry_wall_time`), not a
count of seconds left. Each wake computes `remaining = expiry − now`. Clock
drift, sleep length and NTP corrections therefore cannot move the countdown:
a sync corrects `now`, and the subtraction follows.

The expiry is written by a start or resume, an HA grant or deduction, a
duration change to the running timer's definition, and a measured NTP clock
step. A step is the only case where NTP code is involved, and it goes through
`timer_shift_expiry()` exactly once (S13). The case it exists for is a start
that painted before the sync corrected the clock; [network_and_ha.md](network_and_ha.md)
has the mechanics.

## Where timer definitions come from

Slot 0, Screen, takes today's allocation from the schedule (`main/schedule.c`:
day type, then the stored allocation for it). Slots 1–4, the extra timers,
take their name, duration, reloadable flag and break-eligible flag from a
definition table that `timer_defs_install()` loads into plain statics on
every boot. The table HA writes to NVS is the source; the `MAGTAG_TIMER<n>_*`
Kconfig table is the fallback when NVS holds none or cannot be read.
