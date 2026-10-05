# The dashboard

This page covers how to generate the Home Assistant dashboard and how to read
what it shows, including the figures it cannot show.

`tools/gen_ha_dashboard.py` builds a dashboard with **one tab per device**.
It prints everything that goes with the dashboard, in three parts:

1. **setup**: the per-device setup steps, with each device's names filled in
   (the same steps as [Setup](setup.md#5-set-up-each-device));
2. **automation**: the config-publishing automation, verbatim from
   `tools/ha/magtag_publish_config.yaml`, headed by its install notes;
3. **dashboard**: the dashboard YAML to paste into HA.

The generator never talks to Home Assistant: no API, no access token. You
paste what it prints. [Setup](setup.md#7-paste-the-dashboard) says where.

## Requirements

- **Home Assistant 2025.11 or newer.** The cards name entities with
  `name: {type: entity}`, which first appeared in the 2025.11 frontend.
- **Devices on current firmware, with `magtag_<node>` entity IDs.** A device
  still on name-derived IDs needs
  [re-registering](setup.md#re-registering-a-device-on-old-entity-ids)
  first.
- **[uv](https://docs.astral.sh/uv/).** The script declares its own Python
  dependencies, and `uv run` supplies them. The first run needs network
  access to fetch them.

## Running it

Run it from the repository root. `--part setup|automation|dashboard` prints
one part; the default is all three.

### From the broker (`--mqtt`)

This is the precise mode. The generator reads the retained discovery
messages on the broker and builds each tab from the entities that device
really has, so a disabled timer slot or an unused chore gets no row. Tab
titles are the device names.

```sh
uv run tools/gen_ha_dashboard.py --mqtt --sdkconfig ~/magtag-mqtt.cfg > ha_setup.txt
```

`--mqtt` needs `--sdkconfig PATH`, with no default. The firmware build does not
hold the broker (you enter it on the device), so the generator reads
the broker settings from an sdkconfig-style file you keep outside the
repository, with three lines:

```
CONFIG_MAGTAG_MQTT_URI="mqtt://homeassistant.local:1883"
CONFIG_MAGTAG_MQTT_USER="magtag"
CONFIG_MAGTAG_MQTT_PASS="..."
```

`mqtt://` and `mqtts://` both work. The password is never printed, not even
in an error.

- **`--wait SECONDS`** (default 10) caps the scan. The scan stops 2 s after
  the last retained message arrives. If time runs out while messages are
  still arriving, the generator warns you: raise `--wait` and run it again.
- **`--devices FILE`**, if given, overrides tab titles per device.
- **A device on older firmware** (one that lacks entities this checkout's
  firmware publishes) still gets a tab, built from what it does publish,
  plus a warning to update it and run the generator again. A device too old
  to send stable entity IDs (before discovery schema v20) gets no tab.
- **An entity the generator does not know** (firmware newer than your
  checkout) goes into an "Other" section on its tab, with a warning.

### From a local file

File mode needs no broker. Copy the example (`tools/ha_devices.yaml` is
gitignored) and list your devices:

```sh
cp tools/ha_devices.example.yaml tools/ha_devices.yaml
```

```yaml
devices:
  - node: "1a0a5c"          # the 6 hex digits after "magtag-"; quote it
    label: "Testing Timer"  # the tab's title
```

```sh
uv run tools/gen_ha_dashboard.py
```

File mode takes the entities from the firmware source in your checkout, so
every tab gets all four timer slots and names all three chores. A device with
a disabled slot shows "entity not available" rows for it. `--mqtt` avoids
this.

## What a tab shows

Each tab is a sections view, top to bottom:

- **Screen Timer Settings**: each day type's allocation followed by its
  chore-free minutes (they are a pair; see
  [Configuring](configuring.md#chore-free-minutes-and-their-allocation)),
  then Screen adjust (min) today, then the break interval and duration.
- **Additional Timers**: timers 1–4, each with its name, minutes,
  reloadable and break eligible.
- **Quiet hours & bed time**, then **Tones & volume**.
- **Daily Chores**: the device's To-do list, where you edit its chores. The
  list shows the chores, not the device's ticks.
- **The graphs**, two columns wide:
  - *Timer Burndown (Last 4 days)*: Screen time remaining and each extra
    timer's remaining time over 96 hours;
  - *Additional Timer Runs (last 7 days)*: each extra timer's runs per day,
    as bars;
  - *Battery Charge*: the hourly mean of Battery over 7 days.
- **System & OTA**: device name, time zone, OTA manifest URL, OTA check on
  sync and Find my timer, then the last update's result, target, failures and
  download time. Below it, **Status** has the live state: timer state, active
  timer, Screen time limit, screen break and its time remaining, exposure,
  charge lock, chores left and done, day type, and each extra timer's runs
  today.
- **Diagnostics**: health (config warning, battery voltage, light, last
  reset, NVS free entries), memory, and the last panic.
- **Activity Log**: a logbook of every entity on the device over the last
  48 hours.

A card with nothing to show is left out, so a tab built for older firmware
has no empty cards. For example, firmware that gives Battery no state class
gets no Battery Charge graph, and Battery moves onto the Status card instead.

Some entities are deliberately on no card:

- **the per-chore done flags**: the Activity Log shows each tick;
- **each extra timer's limit**: it equals the minutes under Additional Timers,
  except on a day a [grant](reference.md#raw-command-topic) moved it;
- ***Screen time per day*** and ***Chores done per day***: HA still records
  their statistics, so you can add a graph by hand (a *Statistics graph*
  card, statistic *change*, period *day*, chart type bar).

## Graphs and statistics

HA's recorder **is** the usage history: battery over weeks, screen minutes
per day, runs of each extra timer, and chores done per day. The device keeps
none of it.

**Per-day figures come from the daily summary.** At the first wake after
midnight, the device publishes the finished day on the retained
`magtag/<id>/summary` topic: screen time used, each extra timer's runs, and
chores done out of chores configured. Three kinds of sensor read it:
*Screen time per day*, *`<Name>` runs per day* and *Chores done per day*.
They are built for statistics: each summary starts a new cycle, so the
*change* over a period is exactly what the summaries in it reported, and a
repeated summary (a retained redelivery, an HA restart) adds nothing. The
live counters would get this wrong. A run finished after the day's last
window is cleared at midnight before HA sees it, and the day's maximum of
*Chores done* includes the count carried over midnight.

**The day shift.** A summary arrives after midnight, so HA files each day's
figures under the **following** day, the day they arrived. A run finished on
Sunday shows on Monday's bar. No run is lost: one finished late in the
evening is still in the summary. This holds for the runs graph and for any
summary graph you add by hand.

**A missed summary is a missing day.** The device holds the unsent summary
in memory only. If the first window after midnight fails (no WiFi, no
broker), that day's summary is never sent, and a bar graph shows the gap as
a zero day.

**The first summary only sets the baseline.** HA's statistics take the first
value they see for a sensor as a starting point, not a change, so the first
summary a sensor receives counts in no graph. The next day's does.

***Chores done per day* can read *unknown*.** That happens when a summary
has no chore count: one from older firmware, or one sent when the chore list
could not be read at midnight. The statistics skip *unknown*, so the day
counts as neither 0 nor a repeat of the day before. A device with no chore
list reports 0.

## What the Activity Log cannot show

HA keeps **no logbook entries for a sensor that has a unit or a state
class.** So the Activity Log never shows Battery, Chores left or Chores done
(they have a state class for their statistics), the three summary sensors, or
any sensor measured in minutes, millivolts, bytes, seconds or milliseconds:
the remaining times and limits, screen break remaining, exposure, battery
voltage, light, memory, the update download time, and the panic sizes and
uptime.

It does show timer state changes, the charge lock, screen break, each extra
timer's runs, each chore's done flag (the day-by-day chore record), Config
warning, Panic count and phase, and the other unit-less diagnostics. Every
**control** (number, select, switch, text) is logged whatever its unit, so a
settings change always shows.

The current values of the hidden sensors are on Status or Diagnostics, and
the graphs show the history of the ones they plot. Hovering over the battery
graph gives the last full hour's mean, not the current reading.
