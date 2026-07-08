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

Battery %, battery voltage, ambient light, timer state, active timer, time
remaining, today's allocation, day type, per-timer completion counters,
charge-lock binary sensor, and screen-time-used-today (from the daily
summary). Recorder history on these entities IS the usage-stats feature —
graph battery over weeks, screen minutes per day, practice completions, etc.

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

## Config from Home Assistant

Publish a **retained** JSON document to `magtag/<id>/config`; the device
applies it on its next window and republishes the applied version to
`magtag/<id>/config_ack`. Every field is optional except `ver` — the
device applies a document only when `ver` differs from the last one it
applied, so a retained message is safe to leave on the topic. A rejected
field is named in the ack's `errors` list but never blocks the others.

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

### Driving it from helpers

Create `input_number`/`input_text`/`input_boolean` helpers for the
settings you want to expose, then one automation republishes the whole
retained document (with a fresh `ver`) whenever any of them changes:

```yaml
automation:
  - alias: "MagTag push config"
    trigger:
      - platform: state
        entity_id:
          - input_number.magtag_weekday_min
          - input_text.magtag_tz
          # ...one line per helper
    action:
      - service: mqtt.publish
        data:
          topic: "magtag/magtag-xxxxxx/config"
          retain: true
          payload: >
            {"ver":"{{ now().timestamp() | int }}",
             "tz":"{{ states('input_text.magtag_tz') }}",
             "weekday_min":{{ states('input_number.magtag_weekday_min') | int }}}
```

### Holidays from a calendar

Keep school days-off in an HA **Local Calendar** ("School Days Off") and
run a nightly automation that reads the next 12 months of all-day events
and republishes the config with the extracted `holidays` array — so the
family manages no-school days on a normal calendar UI, and the device
picks them up automatically.

## Commands

Publish a **retained** JSON command to `magtag/<id>/cmd`. Each command
carries a unique `id`; the device applies it once (dedup on `id`), acks on
`magtag/<id>/event`, and then clears the retained topic so it isn't
re-applied. Applied within one sync window (Button D forces it).

### Grant extra time

```json
{"id": "1751990400", "grant": {"timer": "Screen", "min": 15}}
```

`timer` defaults to Screen if omitted; `min` is 1–240. Behaviour by state:
IDLE banks the minutes and adds them when the timer next starts; RUNNING
extends in place; PAUSED/BREAK add to the frozen remaining; **EXPIRED**
(the usual "chores done, time already ran out" case) flips to PAUSED
holding the granted minutes — the kid presses A to start it, and the
expiry alarm does not re-fire.

```yaml
script:
  magtag_grant_15:
    sequence:
      - service: mqtt.publish
        data:
          topic: "magtag/magtag-xxxxxx/cmd"
          retain: true
          payload: '{"id":"{{ now().timestamp() | int }}","grant":{"min":15}}'
```

### Locate ("help, I lost the timer")

```json
{"id": "1751990500", "locate": true}
```

On its next window the device beeps with a red pulse until a button is
pressed or ~10 minutes pass. (Battery-charge-locked devices don't open
windows, so locate won't reach a dead device — check the charge-lock
sensor first.)

A dashboard button per command (grant / locate), each publishing with
`id: "{{ now().timestamp() | int }}"`, is the simplest HA surface.
