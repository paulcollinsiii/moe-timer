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

## Phases 2 and 3 (planned)

- **Config in**: retained `magtag/<id>/config` JSON — allocations, quiet
  hours, break settings, timezone, holidays (fed from an HA Local
  Calendar), device name, and the four extra-timer definitions.
- **Commands**: retained `magtag/<id>/cmd` — grant extra minutes to a
  timer ("chores done: +15 min"), and a locate alarm ("help, I lost the
  timer") that beeps on the next window until a button is pressed.
