# Firmware updates (OTA)

How an update is found, downloaded, committed, and then certified or rolled
back, and what bounds each step. The manifest format is in
[ota_manifest.md](../ota_manifest.md), and publishing a release is in
[developer_setup.md](../developer_setup.md).

The modules split the work three ways: `ota_policy` decides whether an update
may and should happen (pure), `ota_flow` owns the order of the steps
(host-tested through injected effects), and `ota` moves the bytes.

## Which wakes check

A check is armed by `ota_flow_arm()` on the main task, before the window
opens:
- the **day rollover** always arms one;
- a **Button D sync** on the timer screen arms one only when the
  `ota_on_sync` setting is on;
- nothing else does: not a tick sync, a break end or the final-minute sync.

An armed check still has to pass the gates in `ota_policy`: an OTA URL is
set, NTP succeeded in this window (TLS needs a clock), no charge lock, and the
battery is at least `CONFIG_MAGTAG_OTA_MIN_BATT_PCT` (30 %). The download asks
all four again, since the two windows are minutes apart, and adds a free-heap
floor. The URL must be https, at config time and on every redirect (S35).

## Two windows

**The check rides the window the wake was already opening.** It is one small
HTTPS GET of the manifest on the `net_win` task, placed after the snapshot
handoff (the panel is idle by then) and before the MQTT phase (so its result
is in this window's stat payload). An update it finds is buffered in RAM.

**The download is a second window.** At the end of the wake, after the
render and the awake watch, `maybe_apply_update()` in `main/wake_flow.c`
re-samples the battery and the charge lock and runs `ota_flow_apply()` on its
own `ota_dl` task, which exists for the 16 KB stack TLS and the flash write
need. `ota_flow_apply` paints the update screen, then opens the radio (S36):
painting inside an open window is the known brownout. It then downloads,
commits, saves the timer snapshot and reboots.

The apply point is deliberately not in `enter_deep_sleep()`. The awake
failsafe enters that funnel from the `esp_timer` task when something is
already wedged, and a wedged wake must never start a 1.5 MB download. An
update found on a wake that ends early (a break starting, a held button) is
simply found again at the next check.

## What bounds a download

- **The deadline beats the failsafe.** `ota_flow_apply` gives its download
  loop `CONFIG_MAGTAG_OTA_MAX_SEC` (300 s) and re-arms the awake failsafe for
  that plus an abort tail, so the loop always stops the transfer first and
  still has time to abort and drop the radio. `include/ota_timing.h` checks
  that arithmetic with a `_Static_assert` (S38). The transport is four calls
  (begin, one step, finish, abort) so the deadline can be checked between
  steps. If the failsafe cannot be extended (it failed to arm at boot), the
  attempt is declined before anything is painted or charged.
- **The commit tail has its own budget.** After the loop, the failsafe is
  re-armed for an ordinary awake period before the commit, which runs two
  full-image SHA-256 passes and the `otadata` write. The static assert covers
  the abort path only, not this one.
- **The retry budget is charged first.** The attempt count and its target
  version are written to NVS before the first byte, so an attempt the
  failsafe kills still counts (S37). After `CONFIG_MAGTAG_OTA_MAX_FAILS` (3)
  failures for one version the device stops with `gave_up`; publishing a new
  version resets the count.
- **What nothing bounds.** A server that sends headers and then stalls keeps
  the transfer inside `esp_https_ota`'s header loop, one socket timeout per
  turn, until the failsafe ends the wake. The budget above is what stops that
  from repeating forever.

## Certify or roll back

Rollback is in the bootloader ([hardware.md](hardware.md)). A committed slot
boots as pending verify, and the bootloader reverts it unless the app
certifies it during that boot. Deep-sleep wakes re-run the bootloader
(`CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` is off), so the new image
gets exactly one wake to prove itself (S43).

The certification sits in the sleep funnel, `enter_deep_sleep()`, which every
sleep passes through, early-outs and locks included. The awake failsafe
declines it: a new image whose first wake had to be killed is the strongest
evidence that the update is bad. The cost of being wrong that way is one
re-download.

A revert is noticed on the boot that comes back, by `ota_flow_init()`. The
commit wrote the new version to a one-shot NVS token (`ota_pend_ver`), and
the first of two outcomes consumes it: certification, or a boot running a
different version, which records `rolled_back`.

## Where results show up

| Outcome | Reported |
|---------|----------|
| Check failed | In the same window's stat payload |
| Nothing to do, or no check ran | Nothing: `ota_result` keeps the last outcome (S39) |
| Download failed | Saved to NVS and published in the next window, since MQTT is closed by then |
| Success | The device reboots; the new version appears as `fw` and on the panel |
| Rolled back | `rolled_back`, in the first stat payload after the revert |

The four OTA keys reach HA as a separate `ota_stat_t`, read from NVS at
publish time, not carried in the stats snapshot, which is built before the
check runs. The HA entities are listed in
[the HA reference](../home_assistant/reference.md#entities).
