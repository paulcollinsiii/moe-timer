# Home Assistant reference

This page is the lookup for the device's MQTT topics, its entities and their
IDs, and the raw command topic. [Setup](setup.md) explains why everything is
retained, and [Configuring](configuring.md) explains how to change the
settings these entities expose.

## Topics

`<id>` is the device ID, `magtag-` plus the last three bytes of its WiFi MAC
address in hex (for example `magtag-1a0a5c`).

| Topic | Direction | Retained | What it carries |
|-------|-----------|----------|-----------------|
| `magtag/<id>/stat` | device → HA | yes | The state snapshot, every window. Feeds most sensors. |
| `magtag/<id>/summary` | device → HA | yes | The finished day's totals, at the first window after midnight ([Dashboard](dashboard.md#graphs-and-statistics)). |
| `magtag/<id>/cfg` | device → HA | yes | Every setting's current value, every window. The controls show it. |
| `magtag/<id>/act` | device → HA | yes | Screen adjust and Find my timer state. |
| `magtag/<id>/config_ack` | device → HA | yes | The result of the last bulk document applied ([Troubleshooting](troubleshooting.md#a-document-did-not-take)). |
| `magtag/<id>/event` | device → HA | no | The result of a raw command. |
| `magtag/<id>/set/<key>` | HA → device | yes | One control edit. The device clears it once applied. |
| `magtag/<id>/config` | HA → device | yes | The bulk config document ([Configuring](configuring.md#the-bulk-config-document)). |
| `magtag/<id>/cmd` | HA → device | yes | A raw command ([below](#raw-command-topic)). The device clears it once applied. |
| `homeassistant/<component>/<id>_<key>/config` | device → HA | yes | MQTT discovery, republished only when an entity is added, renamed or removed. |

To watch everything one device says and hears, use the `mosquitto_sub`
command in [Troubleshooting](troubleshooting.md).

## Entity IDs

Every entity ID has the form **`<component>.magtag_<node>_<key>`**, for
example `binary_sensor.magtag_1a0a5c_charge_lock` or
`number.magtag_1a0a5c_weekday_min`. `<node>` is the six hex digits of the
device ID. `<key>` is the firmware's key for the entity, which is not a slug
of its display name: "Weekday allocation" is `weekday_min`, and "Screen
adjust (min) today" is `screen_bonus`. The tables below list the keys.

The IDs are stable:

- **They never follow the device's name.** Renaming the device changes only
  display names, so automations and dashboards keep working.
- **They need Home Assistant 2025.10 or newer.** Older HA ignores the
  firmware's ID hint and derives IDs from the device name. The generated
  dashboard needs 2025.11.
- **HA sets an ID only at first registration.** A device paired on firmware
  before discovery schema v20 keeps its old name-derived IDs until you
  [re-register it](setup.md#re-registering-a-device-on-old-entity-ids).

HA's device page sorts entities alphabetically within each category. The
firmware cannot set a display order; the [dashboard](dashboard.md) gives
you a fixed layout.

## Entities

Each device's entities fall into HA's three categories. Primary entities sit
at the top of the device page. Configuration entities are the controls. The
rest are Diagnostic.

**Primary**

| Key | Name in HA | What it shows |
|-----|-----------|---------------|
| `battery` | Battery | Charge, %. Has long-term statistics. |
| `state` | Timer state | The Screen timer's state, or `NO_CLOCK` (see [Troubleshooting](troubleshooting.md#the-device-shows-no-clock)). |
| `screen_remaining` | Screen time remaining | Minutes left today. |
| `charge_lock` | Charge lock | On when the device has locked itself for a charge. |
| `screen_break` | Screen break | On during a Screen Break. |
| `chores_left` | Chores left | Configured chores not yet ticked today. |
| `chore_1` … `chore_3` | `<chore>` done | On once that chore is ticked today. See [Chores](#chores). |
| `ota_result` | Update result | The last update check's outcome, as a short code ([the OTA manifest](../ota_manifest.md)). |
| `panic_count` | Panic count | Crashes since the count started. |
| `locate` | Find my timer | A switch ([Configuring](configuring.md#screen-adjust-and-find-my-timer)). |

**Configuration**: every control in [Configuring](configuring.md#the-controls),
whose table gives each key, plus `screen_bonus` (Screen adjust (min) today).

**Diagnostic**

| Key | Name in HA | What it shows |
|-----|-----------|---------------|
| `screen_limit` | Screen time limit | Today's Screen allocation, after any adjustment. |
| `active_timer` | Active timer | The timer the panel is showing. |
| `day_type` | Day type | Weekday, weekend, holiday or summer. |
| `break_remaining` | Screen break remaining | Minutes left in the current break. |
| `screen_exposure` | Screen exposure | The screen-exposure balance, in minutes, that decides when the next break fires ([screen breaks](../behavior/screen_breaks.md#when-a-break-comes-due)). |
| `remaining_N`, `limit_N` | `<timer>` remaining / limit | Each extra timer's minutes left and its limit, per slot. Time used today = limit − remaining. |
| `completions_N` | `<timer>` runs | Each extra timer's completed runs today. |
| `chores_done` | Chores done | Chores ticked today. Has long-term statistics. |
| `config_warning` | Config warning | `OK`, or the day types whose chore-free pair is broken. See [Chores](#chores). |
| `screen_used_day`, `day_runs_N`, `day_chores` | Screen time per day, `<timer>` runs per day, Chores done per day | The finished day's figures, from the daily summary ([Dashboard](dashboard.md#graphs-and-statistics)). |
| `ota_target`, `ota_fails`, `ota_dl_ms` | Update target / failures / download time | The version being fetched, failed attempts at it, and the last download's duration. |
| `battery_mv`, `light` | Battery voltage, Ambient light | Raw readings, mV. |
| `last_reset` | Last reset | Why the device last restarted. |
| `heap_free`, `heap_min`, `stack_main`, `stack_net`, `nvs_free` | Memory and storage | Free memory and NVS entries, measured during the window. |
| `panic_phase`, `panic_uptime`, `panic_heap`, `panic_stack_main`, `panic_stack_net` | Panic details | Where and when the last crash happened. |

Per-timer entities take their names from the timer ("Piano remaining"), and
a disabled slot's entities are removed from HA.

### Availability and timing

The sensors fed by `stat` go **unavailable** only when the device really stops
reporting: they expire after 7500 s, a little over twice the hourly idle
sync. A charge-locked device stops reporting too, which is why Charge lock
makes a better trigger than unavailability. The summary sensors, Config
warning, Last reset and the update and panic sensors never expire. How often
the device reports is under
[when it syncs](../behavior/power_and_sync.md#syncing).

In the window that delivers a Screen adjust, `screen_limit` and
`screen_remaining` still show the figures from before it. They catch up at
the next window. The `act` topic carries the applied target at once, so key
an automation that must react to it on an MQTT trigger on
`magtag/<id>/act` (its `screen_bonus` field), not on the number: the number
updates optimistically, as soon as you edit it.

### Chores

The chore entities are read-only: only the device can tick a chore
([chores](../behavior/chores.md#ticking-a-chore)).

- **The done flags are positional.** `chore_1` is whatever is first on the
  list today, named from it ("Homework done"). Renaming or reordering chores
  keeps the ID and its history. Flags past the configured count are removed
  from HA.
- **The flags are the day-by-day record.** They appear in the logbook. The
  two counts have a state class (for statistics), so HA leaves them out of
  the logbook.
- **Times are sync times.** Ticking a chore opens no network window, so HA
  hears of it at the next window: usually when the unlocked timer is
  started, otherwise the next hourly sync. "Homework done at 16:04" means
  the device reported it by 16:04.
- **A list change reaches HA one window late.** In a window the device
  reports first and applies the new document after, so new names (and the
  cleared ticks) show at the following window. If the device cannot read its
  stored list, it leaves the chore entities as they are and retries every
  window.
- **Config warning** names every day type whose chore-free minutes exceed
  its allocation, in the order Weekday, Weekend, Holiday, Summer (e.g.
  `Weekday, Summer`). It is recomputed at every window, so it stays until
  the pair is fixed, and it is logged when it appears and when it clears.

With no chore list configured, both counts read 0.

## Raw command topic

`magtag/<id>/cmd` takes a retained JSON command. Its main use is granting
time to **one extra timer**, which Screen adjust cannot do:

```json
{"id": "piano-2026-10-01", "grant": {"timer": "Piano", "min": 10}}
```

- **`id`** is required, up to 39 characters, and must differ from the last
  command the device applied. A repeated `id` is ignored, which is what
  makes leaving the command retained safe.
- **`grant`** adds `min` (1–240) minutes to the timer with that name, today.
  It adds; it is not a target like Screen adjust. Leave out `timer`, or say
  `"Screen"`, to grant Screen time.
- `{"id": "…", "locate": true}` runs Find my timer.

An applied command is acknowledged on `magtag/<id>/event`, for example
`{"id":"piano-2026-10-01","ok":true,"grant":10}`, and then cleared from
`cmd`. A **refused** command (unknown timer, `min` out of range, no `id`)
gets no event at all and stays retained. The one exception is a command
over 255 bytes, which gets a `"err":"too_long"` event. If no event arrives,
check the timer's name. While the device shows No Clock, a grant waits on the broker
([Troubleshooting](troubleshooting.md#the-device-shows-no-clock)).
