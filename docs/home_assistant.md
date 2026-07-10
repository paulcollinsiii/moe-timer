# Home Assistant Integration

The MagTag talks to Home Assistant over MQTT during its periodic network
windows (the same brief WiFi sessions used for NTP). Everything is built on
**retained messages** because the device sleeps ~99% of the time: HA
publishes whenever, the broker holds it, the device consumes it on its next
window. Entities are auto-created via HA's MQTT Discovery.

## Setup

1. HA needs the **MQTT integration** with a broker (the Mosquitto add-on
   works fine). Create a broker user for the device.
2. Set the broker on the device — preferred: `include/credentials.local.h`
   (gitignored, next to the WiFi credentials):
   ```c
   #define NVS_DEFAULT_MQTT_URI "mqtt://homeassistant.local:1883"
   #define NVS_DEFAULT_MQTT_USER "magtag"
   #define NVS_DEFAULT_MQTT_PASS "..."
   ```
   (Alternative: menuconfig → MagTag Timer → MQTT broker URI. An empty URI
   disables MQTT entirely.)
3. Flash. Within one sync window the device appears in
   Settings → Devices & Services → MQTT as `magtag-xxxxxx` (last 3 bytes of
   its WiFi MAC — stable across reflashes, unique per device on the
   network).

## Topics

```
magtag/<id>/stat        retained  device → HA   state snapshot, every window
magtag/<id>/summary     retained  device → HA   daily usage, at rollover
magtag/<id>/config_ack  retained  device → HA   applied config version (phase 2)
magtag/<id>/event       plain     device → HA   command acks (phase 3)
magtag/<id>/config      retained  HA → device   config document (phase 2)
magtag/<id>/cmd         retained  HA → device   one-shot command (phase 3)
```

## Entities (auto-discovered)

Everything appears under one device, grouped by HA `entity_category`:

- **Primary** (top of the device page): Battery %, Timer state, Time
  remaining, Charge-lock.
- **Configuration** (editable — see below): allocations, quiet hours,
  break settings, device name, timezone, the four timer slots, plus the
  Screen-bonus number and Find-my-timer switch.
- **Diagnostic** (read-only detail): battery voltage, ambient light,
  active timer, day type, per-timer completion counters, screen-time-used
  today, and **"Today's limit"** — the computed allocation for today (the
  read-only *result* of the editable allocation settings).

Recorder history on the read-only sensors IS the usage-stats feature —
graph battery over weeks, screen minutes per day, practice completions.

Notes:
- Stat-fed sensors carry `expire_after` a bit over 2× the idle sync
  interval: they stay "available" while the device sleeps but go
  unavailable if it genuinely dies (or is charge-locked, which stops
  network windows — the final stat before locking sets
  `binary_sensor.charge_lock` ON, a good automation trigger for a
  "charge the timer" notification).
- Update cadence = the sync cadence: every 10 min while a timer runs,
  hourly while idle (menuconfig). Button D forces a window immediately.
- The daily summary publishes at the first wake after midnight and covers
  the finished day: `screen_used_s` + completions per extra timer.

## Example: low-battery notification

```yaml
automation:
  - alias: "MagTag needs charging"
    trigger:
      - platform: state
        entity_id: binary_sensor.magtag_xxxxxx_charge_lock
        to: "on"
    action:
      - service: notify.mobile_app_phone
        data:
          message: "The screen timer battery is at 10% — charge it."
```

## Editing config from the HA card (no setup)

The Configuration section of the device page holds native editable
controls — **Number** for the allocations, quiet hours, break interval /
duration; **Text** for the device name, timezone, and each timer's name;
**Switch** for each timer's reloadable flag. Change one and the device
applies it on its next window (Button D forces one), then republishes the
confirmed value to `magtag/<id>/cfg` so the control reflects reality.

- **Add / edit an extra timer:** the four slots are fixed (the firmware
  ceiling). Set an empty slot's **Timer N name** (e.g. "Running") + minutes
  + reloadable to enable it; clear the name to disable it. A timer-slot
  edit takes effect on the device's next wake (~≤1 min or a button press);
  allocations / quiet hours / break settings apply live; timezone at the
  next boot.
- Commands are sent on retained `set/<key>` topics so the sleeping device
  receives edits made while it's asleep. Idempotent, so re-delivery is
  harmless.

### Bulk config document (holidays, scripted setup)

For values that aren't a single control — chiefly the **holiday list** —
publish a **retained** JSON document to `magtag/<id>/config`; the device
applies it and republishes the applied version to
`magtag/<id>/config_ack`. Every field is optional except `ver` — applied
only when `ver` differs from the last one, so a retained message is safe
to leave on the topic. A rejected field is named in the ack's `errors`
list but never blocks the others.

```json
{
  "ver": "20260708",
  "name": "Kitchen MagTag",
  "tz": "EST5EDT,M3.2.0,M11.1.0",
  "weekday_min": 60, "weekend_min": 120, "holiday_min": 120, "summer_min": 120,
  "quiet_start": 2230, "quiet_end": 800,
  "break_interval_min": 30, "break_duration_min": 15,
  "summer_start": "2026-05-29", "school_start": "2026-08-20", "school_end": "2027-05-28",
  "holidays": ["2026-10-16", "2026-11-03"],
  "timers": [
    {"name": "Piano", "min": 15, "reload": true},
    {},
    {"name": "Meditation", "min": 10, "reload": true},
    {}
  ]
}
```

- `tz` is a POSIX TZ string. `quiet_*` are HHMM. `timers` is up to 4
  entries; `{}` disables that slot. Timezone and timer-definition changes
  take effect on the device's next boot/operation (the running slot is
  never disturbed mid-run).
- `holidays` replaces the stored list (rolling ~45-date cap).

The same fields are available here as on the native controls (`name`, `tz`,
`weekday_min`, …, and a `timers` array), so scripted/bulk setup stays
possible — but for day-to-day tweaks the Configuration controls are easier.

### Holidays from a calendar

Keep school days-off in an HA **Local Calendar** ("School Days Off") and
run a nightly automation that reads the next 12 months of all-day events
and republishes the config with the extracted `holidays` array — so the
family manages no-school days on a normal calendar UI, and the device
picks them up automatically.

## Actions (native controls)

Two action controls live in the Configuration section — no scripts needed.

### Grant extra time — "Screen bonus (min) today"

A **Number** (0–240). Set it to how many bonus minutes Screen should have
*today*; the device grants the difference from what it's already given
(idempotent — re-delivery every wake never double-grants), and it resets
to 0 at the day rollover. Behaviour by state: IDLE banks the minutes and
adds them when the timer next starts; RUNNING extends in place;
PAUSED/BREAK add to the frozen remaining; **EXPIRED** (the usual "chores
done, time already ran out" case) flips to PAUSED holding the minutes —
the kid presses A to start it, and the expiry alarm does not re-fire.

Because it's a *target for the day* rather than an increment: setting 15
then 20 grants 20 total (not 35); lowering it never reclaims granted time.

### Locate — "Find my timer" switch

Toggle it **on**; on its next window the device beeps with a red pulse
until a button is pressed or ~10 minutes pass, then the switch returns to
off by itself. (Charge-locked devices don't open windows, so locate won't
reach a dead device — check the charge-lock sensor first.)

### Raw command topic (power users / per-timer grants)

`magtag/<id>/cmd` still accepts a retained JSON command with a unique `id`
(deduped, acked on `magtag/<id>/event`, then cleared) — e.g.
`{"id":"...","grant":{"timer":"Piano","min":10}}` to grant a *specific*
extra timer, which the native Screen-only control doesn't cover.
