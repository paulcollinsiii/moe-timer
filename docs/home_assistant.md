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
4. Optional, but the easy way to run it day to day: install the
   [config-publishing
   automation](#the-config-publishing-automation-chores-and-school-calendar)
   and generate the [dashboard](#dashboard).

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

- **Primary** (top of the device page): Battery %, Timer state, Screen
  time remaining, Charge-lock, **Chores left**, and one **`<chore> done`**
  binary sensor per configured chore (see
  [Chore checklist](#chore-checklist-read-only-in-ha)).
- **Configuration** (editable — see below): allocations, the four
  chore-free numbers (Weekday / Weekend / Holiday / Summer chore-free),
  quiet hours,
  bed time (HHMM number; 0 disables, otherwise 1800–2359 — the device
  rejects daytime values), break settings, alert-tone selects (Expiry /
  Break / Bed time tone, incl. "Custom WAV" from the assets partition),
  Alert volume (0–200 %; 100 = clean reference level, above that adds
  clipping gain for real loudness, 0 mutes), device name, timezone, the
  four timer slots, the OTA manifest URL + OTA check-on-sync switch,
  plus the Screen-bonus number and Find-my-timer switch.
- **Diagnostic** (read-only detail): battery voltage, ambient light,
  active timer, day type, last reset, **Screen time limit** (the computed
  allocation for today — the read-only *result* of the editable allocation
  settings), and per extra timer: `<Name> remaining` / `<Name> limit` /
  `<Name> runs`. Remaining/limit are **per-slot** (not active-timer
  scoped), so each timer keeps its own recorder history; screen time used
  = limit − remaining (a template sensor if you want it as an entity).
  **Screen time per day**, **Chores done per day** and, per extra timer,
  `<Name> runs per day` are the finished day's figures, read from the
  daily summary (see [Graphs and statistics](#graphs-and-statistics)).
  Also **Chores done** and **Config warning** (see
  [Chore checklist](#chore-checklist-read-only-in-ha)).

Recorder history and long-term statistics on the read-only sensors ARE the
usage-stats feature — battery over weeks, screen minutes per day, runs of
each extra timer, chores done per day. The generated
[dashboard](#dashboard) graphs the battery and the runs; HA records the
statistics of the other two as well, so a graph of them can be added by
hand. How each figure is recorded is under [Graphs and
statistics](#graphs-and-statistics).

Notes:
- Stat-fed sensors carry `expire_after` a bit over 2× the idle sync
  interval: they stay "available" while the device sleeps but go
  unavailable if it genuinely dies (or is charge-locked, which stops
  network windows — the final stat before locking sets
  `binary_sensor.magtag_xxxxxx_charge_lock` ON, a good automation trigger
  for a "charge the timer" notification).
- Update cadence = the sync cadence: every 10 min while a timer runs,
  hourly while idle (menuconfig). Button D forces a window immediately —
  from the **timer screen**. On the chore checklist Button D is the ✓3
  button and opens no window; press A to get back to the timer screen
  first.
- The daily summary (`magtag/<id>/summary`) publishes at the first wake
  after midnight and covers the finished day. The per-day sensors that
  read it, and what that means for graphs — a day's figures land under
  the next day, and a day whose summary never gets out is missing — are
  under [Graphs and statistics](#graphs-and-statistics).

### Entity IDs are stable, and do not follow the device name

**Requires Home Assistant 2025.10 or newer** (the generated
[dashboard](#dashboard) needs 2025.11). Every discovery payload
carries `def_ent_id` (`default_entity_id`), added to MQTT discovery in HA
2025.10, so HA builds entity IDs from the MAC-derived device id rather than
from the device's friendly name:

    binary_sensor.magtag_xxxxxx_charge_lock
    switch.magtag_xxxxxx_ota_on_sync
    number.magtag_xxxxxx_weekday_min

The published value is a **full** entity ID, component prefix and all —
`sensor.magtag-xxxxxx_battery` — because HA keeps only the part after the
first dot. A value without the prefix would give it nothing to work with.
HA then slugifies what it kept, which is why the hyphen in the device id
comes out as an underscore in the IDs above.

Below 2025.10 the key is simply unknown: HA's MQTT platform schemas drop
extra keys without a warning, so entity IDs stay exactly as they are today.
The field costs nothing and breaks nothing on an older HA — it just does not
help. (Firmware before this carried `obj_id`, which HA removed from MQTT
discovery in 2026.4.0. On any HA from 2026.4.0 on, that field was dropped
silently and did nothing at all.)

Renaming the device in HA changes the display name and nothing else. This
matters more than it sounds: without it HA derives the entity ID from the
device name, so a rename silently re-slugs every entity underneath it, and
any automation that matched on the old IDs stops matching. A house
automation that turned off every switch except ones matching `magtag` did
exactly that after two devices were renamed — it stopped recognising them
and swept their configuration switches off overnight, which reads on the
device side as settings reverting by themselves.

**One-time step on a device HA already knows.** `def_ent_id` seeds an entity
ID only at that entity's *first* registration; HA keys its registry on
`uniq_id` and will not re-slug an existing entity behind your back. So a
device paired before discovery schema v20 (`STATS_JSON_DISC_SCHEMA_VER` in
`include/stats_json.h`) keeps its name-derived IDs — for example
`sensor.testing_timer_violin_remaining_2` — until it re-registers. A device
whose IDs already read `<component>.magtag_xxxxxx_<key>` needs none of this.

Two facts decide the order. First, HA core's MQTT integration clears a
deleted device's retained discovery topics on the broker (it publishes an
empty payload to each; `async_remove_discovery_payload` in
`homeassistant/components/mqtt/entity.py`), so after a delete nothing on
the broker brings the device back by itself. Second, the firmware
republishes its discovery only when its discovery fingerprint or the
schema version moves (see [When HA sees a list
change](#when-ha-sees-a-list-change)), not every window. So the old advice
— let the new firmware publish, delete, restart HA — can leave a device
that never comes back. Re-register in this order instead:

1. **OTA the device to current firmware first**, and let it run a network
   window. Firmware older than v20 sends no `def_ent_id`, so a device
   deleted while still on it would come back under name-derived IDs again.
2. **Delete it in HA:** Settings → Devices & services → MQTT → the device →
   **Delete**.
3. **Make it republish its discovery:** rename one of its chores in its
   `MagTag <node> chores` To-do list (the chore list is part of the
   fingerprint; the [config-publishing
   automation](#the-config-publishing-automation-chores-and-school-calendar)
   sends the renamed list). Any change to the list does it — on a device
   with no chores yet, adding the first one to its new list. The device applies the new list after that
   window's discovery pass, so it republishes in the window after: within
   **two network windows**. To force them, press **Button D** on the timer
   screen twice, a minute apart — if the chore checklist is showing, D
   ticks chore 3 there instead, so press **Button A** first to get back to
   the timer screen. HA adds the device back with the new IDs. **Once it
   has re-appeared**, rename the chore back (which republishes once more,
   under the same IDs). Not sooner: renamed back before the device's next
   window, the list is the one it already has, so the document and its
   `ver` are unchanged, the device skips it, and nothing republishes.

Whether a given HA install really clears the retained topics on a delete
has **not been confirmed** on the owner's hardware. The order above works
either way: if HA clears them, step 3 brings the device back; if it does
not, the old retained payloads (already current, thanks to the OTA in step
1) bring it back the next time HA subscribes to discovery — an HA restart
or an MQTT reconnect — or step 3's republish does, whichever comes first.

**What step 2 costs: probably less than it looks.** This is read from HA
core's source and has **not yet been confirmed on the owner's HA**:

- **Customisations come back.** Since HA 2025.7 the entity registry
  remembers a deleted entity, and when the same integration creates one
  with the same unique ID again, it restores the old entry: area, custom
  name and icon, labels, and hidden/disabled flags. MQTT entities stay tied
  to their config entry, so HA does not purge those remembered entries.
- **The new ID still wins.** The restored entry comes back under its *old*
  entity ID. MQTT then sees that it differs from the one `def_ent_id` asks
  for and renames it (`_init_entity_registry` in
  `homeassistant/components/mqtt/entity.py`).
- **History and statistics most likely move with it.** That rename is an
  ordinary entity ID change, and the recorder moves an entity's history and
  long-term statistics to its new ID on one. The graphs should carry on
  under the new IDs.

What does break is **every reference to the old IDs**: dashboards,
automations, scripts, template sensors, notification groups. HA does not
rewrite those; update them by hand. (The generated
[dashboard](#dashboard) already uses the new IDs.) Should the owner's HA
behave differently, the worst case is entities back in no area with
nothing customised, and their history left behind under the old IDs.

**A possible alternative without the delete, also unverified.** HA 2025.10
added a **Recreate entity IDs** action, which re-derives entity IDs in
place. Whether it follows `def_ent_id` or the device's name has not been
checked. If you try it, check that the IDs it produces read
`<component>.magtag_xxxxxx_<key>`; if they do, the delete and republish are
not needed.

That is the price of the change, and it is paid once. Skipping it is a valid
choice: an already-paired device keeps working exactly as it does now, it
just keeps name-derived entity IDs — but the generated
[dashboard](#dashboard) names every entity by its
`<component>.magtag_xxxxxx_<key>` ID, so none of its cards would find that
device's entities.

## Dashboard

`tools/gen_ha_dashboard.py` generates a Home Assistant dashboard with **one
tab per device**, and prints everything that goes with it, in three parts:

1. **the setup steps**, with each device's names filled in (below);
2. **the config-publishing automation**, verbatim from
   `tools/ha/magtag_publish_config.yaml`, headed by its install and
   calendar notes (see [the config-publishing
   automation](#the-config-publishing-automation-chores-and-school-calendar));
3. **the dashboard YAML**, to paste into HA.

The generator never talks to Home Assistant — no API, no access token. You
paste what it prints.

### Requirements

- **Home Assistant 2025.11 or newer.** The cards name entities with
  `name: {type: entity}`, which first appears in the 2025.11 frontend. The
  entity IDs alone need only 2025.10 (see [Entity
  IDs](#entity-ids-are-stable-and-do-not-follow-the-device-name)).
- **Devices on current firmware, with `magtag_<node>` entity IDs.** A
  device still on name-derived IDs needs the [one-time
  step](#entity-ids-are-stable-and-do-not-follow-the-device-name) first.
- **[uv](https://docs.astral.sh/uv/)** on the machine you run it on. The
  script declares its Python dependencies (PyYAML, and paho-mqtt for
  `--mqtt`) in its own header (PEP 723 inline metadata), and `uv run`
  supplies them: there is no virtualenv or `pip install` step. The first
  run fetches them into uv's cache, so it needs network access once.

### Running it: file mode

File mode takes the device list from a small local file. Copy the example
and edit it (`tools/ha_devices.yaml` is gitignored):

```sh
cp tools/ha_devices.example.yaml tools/ha_devices.yaml
```

```yaml
devices:
  - node: "1a0a5c"          # the 6 hex digits after "magtag-"; quote it
    label: "Testing Timer"  # the tab's title
```

Then run it from the repository root:

```sh
uv run tools/gen_ha_dashboard.py                      # all three parts
uv run tools/gen_ha_dashboard.py --part dashboard     # just one part
uv run tools/gen_ha_dashboard.py --devices tools/ha_devices.example.yaml --part setup
```

`--part` takes `setup`, `automation`, `dashboard` or `all` (the default);
`--devices FILE` reads another devices file.

File mode derives the entities from the firmware tables in the checkout, so
every tab gets **every** timer slot (1–4), and every tab's Activity Log
names **every** chore's done flag (1–3). A device with a disabled slot
shows "entity not available" rows for it, and the Activity Log of a device
with fewer than three chores names flags it does not have. `--mqtt` avoids
that.

### Running it: `--mqtt` mode

```sh
uv run tools/gen_ha_dashboard.py --mqtt
uv run tools/gen_ha_dashboard.py --mqtt --wait 20 --part dashboard
```

- **Broker settings** come from `sdkconfig` at the repository root
  (`CONFIG_MAGTAG_MQTT_URI`, `_USER`, `_PASS`; `--sdkconfig PATH` for
  another file). `mqtt://` and `mqtts://` both work. The **password is
  never printed** — not in the output, a warning or an error; errors name
  the host only.
- **It reads `sdkconfig` only, never `include/credentials.local.h`.** If
  you set the broker the preferred way, in `credentials.local.h` (see
  [Setup](#setup)), that file overrides the Kconfig values in the
  firmware, and `sdkconfig`'s URI may well be empty; the tool then stops
  with "`CONFIG_MAGTAG_MQTT_URI` is empty". Put the three settings in a
  small file of their own, keep it out of git, and pass it with
  `--sdkconfig FILE`:
  ```
  CONFIG_MAGTAG_MQTT_URI="mqtt://homeassistant.local:1883"
  CONFIG_MAGTAG_MQTT_USER="magtag"
  CONFIG_MAGTAG_MQTT_PASS="..."
  ```
  Or use file mode, which needs no broker at all.
- It reads the retained discovery documents and builds the device list and
  **each device's exact entity set** from them. A disabled timer slot or
  an unused chore gets no row. Tab labels are the device names from
  discovery; a `--devices` file, if given, overrides them per node.
- **A device on older firmware** — one missing entities the checkout's
  firmware always publishes — still gets a tab, built from what it
  publishes, plus a warning naming it: OTA it, then re-run. Its graph
  cards keep only the entities whose published `state_class` suits them,
  and a card left with none is dropped. A device whose discovery carries
  no `def_ent_id` (firmware before schema v20) gets no tab, because its
  entity IDs cannot be known from here. An entity this generator does not
  know (newer firmware than the checkout) goes into an "Other" section on
  its tab, with a warning.
- **`--wait SECONDS`** (default 10) is the longest the scan may take; it
  stops 2 s after the last retained message arrives. If the time runs out
  while messages are still arriving it warns: raise `--wait` and re-run.

### Setup, in this order

Once, for all devices: install the [config-publishing
automation](#the-config-publishing-automation-chores-and-school-calendar).
Then for each device:

1. **OTA it to current firmware.** This comes first because of step 3:
   firmware older than discovery schema v20 sends no `def_ent_id`, so a
   device deleted in HA while still on it comes back under the old
   name-derived IDs.
2. **Create its chore list:** Settings → Devices & services → Add
   integration → Local To-do, named exactly `MagTag <node> chores` (for
   example `MagTag 1a0a5c chores`, which becomes
   `todo.magtag_1a0a5c_chores`, the ID the automation looks for). Rename
   its display name later if you like; the entity ID stays.
3. **Only if its entity IDs are the old name-derived ones** (for example
   `sensor.testing_timer_…` rather than `sensor.magtag_1a0a5c_…`): delete
   the device in HA (Settings → Devices & services → MQTT → the device →
   **Delete**). From HA's source, not yet confirmed on a real install, it
   most likely **keeps** its area, custom names and icons, labels, and its
   history and statistics, which move to the new IDs. What breaks is
   anything that names the old IDs — dashboards, automations, scripts —
   which you update by hand. See [what the delete
   costs](#entity-ids-are-stable-and-do-not-follow-the-device-name), and
   the unverified no-delete alternative there. A device already on
   `magtag_<node>` IDs skips steps 3 and 4.
4. **After a delete, make it republish its discovery**, exactly as in the
   [one-time step](#entity-ids-are-stable-and-do-not-follow-the-device-name):
   rename one of its chores in its To-do list. It republishes within two
   network windows (to force them, press Button D on the timer screen
   twice, a minute apart, pressing Button A first if the checklist is
   showing), and HA adds it back with the new IDs. **Once it has
   re-appeared**, rename the chore back. Renaming it back sooner, before
   the device's next window, leaves the document and its `ver` as the
   device already has them, so it skips the document and never
   republishes.

Finally **paste the dashboard** (part 3): Settings → Dashboards → Add
dashboard → New dashboard from scratch; open it, then Edit → three-dot menu
→ **Raw configuration editor**; replace everything in it with the
generated YAML, and Save. After a firmware update that adds entities, or
when you add a device, regenerate and paste again rather than editing the
dashboard by hand.

### What a tab shows

Each device is one tab, laid out as the owner arranged it: a sections view
with dense section placement (HA fills a gap with a later section that
fits), top to bottom. A heading names each card outside the graphs (a
graph's title is its own); there are no part banners.

- **Screen Timer Settings** — one card of divider-separated groups: per
  day type, the **allocation directly followed by its chore-free
  minutes**, since the two are a pair (see [Chore-free minutes and their
  allocation](#editing-config-from-the-ha-card-no-setup)); then Screen
  adjust (min) today; then the break interval and duration.
- **Additional Timers** — timers 1–4, a group each: name, minutes,
  reloadable and break eligible.
- **Quiet hours & bed time**, **Tones & volume**, and **Daily Chores**:
  the device's **To-do list card**, where its chores are edited, in one
  section. The list shows the chores, not the device's ticks: nothing
  writes a tick back to it. The per-chore done flags are not on the tab;
  the Activity Log shows each tick, and Status has the counts.
- **The graphs** — one section two columns wide, with no heading:
  - *Timer Burndown (Last 4 days)*: Screen time remaining and each extra
    timer's remaining, a history graph over 96 hours;
  - *Additional Timer Runs (last 7 days)*: each extra timer's runs per
    day, bars of the daily *change* of its `<Name> runs per day` sensor;
  - *Battery Charge*: the hourly mean of Battery over 7 days, on a 0–100 %
    axis.
- **System & OTA** — the device name, time zone, OTA manifest URL, OTA
  check on sync and Find my timer; then the last update's result, target,
  failures and download time. Below it, in the same section, **Status**:
  the live state — timer state, active timer, Screen time limit, Screen
  break and its time remaining, exposure, charge lock, chores left and
  done, day type, and each extra timer's runs today (no graph shows the
  current day). Screen time remaining and Battery are in the graphs
  instead; on a tab with no Battery Charge graph (older firmware, below),
  Battery is back on Status, after day type. Each extra timer's limit is
  left off: it is the minutes set under Additional Timers, except on a day
  a [raw-command grant](#raw-command-topic-power-users--per-timer-grants)
  moved it, or
  after its minutes were changed while the timer had run out (it keeps the
  old figure until its next start or the next day).
- **Diagnostics** — health (config warning, battery voltage, light, last
  reset, NVS free), memory, and the last panic.
- **Activity Log** — two columns wide: a logbook card over every entity of
  the device, for the last 48 hours. Some entities never appear in it; see
  [what the Activity Log cannot show](#what-the-activity-log-cannot-show).

An entity the generator does not know (`--mqtt` meeting newer firmware)
gets an "Other" section after Status. A card with nothing to show is left
out with its heading, and a section with no card at all, so a tab built for
older firmware has no empty cards: with no state class on Battery
(firmware before schema v23), for example, the Battery Charge graph goes,
and Battery goes on the Status card instead. Without it there, the tab
would show Battery nowhere: the Activity Log skips a sensor with a unit.

Off the tab, though the firmware publishes them: the chore done flags and
each extra timer's limit (above), and *Screen time per day* and *Chores
done per day*. The owner dropped those two graphs; HA still records their
statistics, so a graph can be added by hand (see below). The Activity Log
card names all of them, though HA logs nothing for the two summary sensors
(see [what it cannot show](#what-the-activity-log-cannot-show)).

### Graphs and statistics

**The daily summary.** At the first wake after midnight the device
publishes the finished day on the retained `magtag/<id>/summary` topic:
`screen_used_s`, the runs of each extra timer, and `chores_done` of
`chores` configured (counted before the rollover clears the ticks). Three
kinds of sensor read it:

| Entity | Name in HA | From the summary |
|--------|-----------|------------------|
| `sensor.magtag_xxxxxx_screen_used_day` | Screen time per day | `screen_used_s`, in minutes |
| `sensor.magtag_xxxxxx_day_runs_1` … `_4` | `<Name> runs per day` | that extra timer's runs; retired with a disabled slot |
| `sensor.magtag_xxxxxx_day_chores` | Chores done per day | `chores_done` |

The dashboard graphs the runs: a statistics graph of the `day_runs_N`
sensors using the statistic *change*, per day. Its battery graph is the
hourly mean of *Battery*, which declares `state_class: measurement`. It
has no graph of *Screen time per day* or *Chores done per day*: the owner
dropped them. The sensors still record their long-term statistics, so a
graph can be added by hand — a *Statistics graph* card on the sensor,
statistic *change*, period *day*, chart type bar.

- **Why the summary, not the live sensors.** The summary sensors carry
  `state_class: total` with a `last_reset` taken from the summary's date.
  Each summary starts a new cycle, so a period's *change* is exactly what
  the summaries in it reported, and a repeat of the same summary (a
  retained redelivery, an HA restart) adds nothing. The live counters
  would get it wrong: a run finished after the day's last window is
  cleared by the rollover before HA sees it, and a daily *max* of *Chores
  done* includes the count carried over midnight, so a day with nothing
  ticked could show the day before's full count.
- **The day shift.** A summary arrives after midnight, so HA files each
  day's figures under the **following** day — the time they arrived. A
  run finished on a Sunday shows on Monday's bar, and on a weekly graph
  it would count in the next week. The shift loses no run: a run finished
  late in the evening, after the day's last window, is in the summary. The
  dashboard does not repeat this on its runs graph, and it holds for any
  summary graph added by hand.
- **A missed summary is a missing day.** This is a known limitation. The
  device holds the unsent summary in RAM only, so if it goes back to sleep
  before a window has published it (the first window after midnight
  fails: no Wi-Fi, no broker), that day's summary is never sent. The
  per-day graphs then show nothing for it, which on a bar graph looks like
  a zero day.
- **The first summary.** HA's statistics take the first value they ever
  see for a sensor as the starting point, not as a change. After the OTA
  that adds the summary sensors, that is the summary already retained on
  the broker, which is therefore recorded once as a state but counted in
  no graph; the next day's summary is the first to count. On a device that
  has never published a summary the sensors read *unknown* until one
  arrives, and that one is the starting point instead.
- **Chores done per day can be *unknown*.** A summary with no
  `chores_done` — one retained by older firmware, or one sent when the
  chore list could not be read at the rollover (a flash error) — sets it
  to *unknown*, which the statistics skip: not 0, and not the previous
  day's count again. Before the sensor has ever had a value it stays
  *unknown* until the first summary that carries the field, which is then
  the starting point; after that, the next summary with the field counts
  as usual. A list that is simply empty reports 0 of 0. Its template checks
  for the field first, so HA logs no template warning for such a summary.

### What the Activity Log cannot show

HA keeps **no logbook entries for a sensor that has a unit or a
`state_class`** — the price of long-term statistics. So the Activity Log
never shows:

- *Battery*, *Chores left* and *Chores done* (`state_class: measurement`,
  kept so they have statistics);
- the three kinds of summary sensor (`state_class: total`);
- any sensor with a unit: Screen time remaining and limit, Screen break
  remaining, Screen exposure, each extra timer's remaining and limit
  (minutes); Battery voltage and Ambient light (mV); Free heap, Free heap
  low water, Main and Network task stack free (bytes); Update download
  time (ms); and Panic uptime (s), Panic free heap and the two panic stack
  figures (bytes).

Their current values are on the Status card, in the Diagnostics section
and, for Update download time, under System & OTA, except these: Screen
time remaining and each extra timer's remaining, which the burndown
history graph shows (hover for the latest value); Battery, whose graph
plots hourly means, so hovering gives the last completed hour's mean, not
the current value (on a tab with no Battery Charge graph, Battery is on
the Status card); each extra timer's limit, which is the minutes set
under Additional Timers; and *Screen time per day* and *Chores done per
day*, which are on no card (see [what a tab shows](#what-a-tab-shows)).
The graphs keep the history of the ones they plot. What the log
does show: timer state
changes, the charge lock and Screen Break, each extra timer's live
`<Name> runs` count (no `state_class`, so every run is logged), each
chore's done flag (the day-by-day chore record), Config warning, and the
other unit-less diagnostics (day type, last reset, the update result,
target and failures, panic count and phase, NVS free entries). The rule
covers sensors only: every **control** — number, select, switch, text —
is logged whatever its unit, so a change to an allocation or a timer's
settings does show.

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
controls — **Number** for the allocations, the four chore-free minutes,
quiet hours, break interval / duration; **Text** for the device name,
timezone, and each timer's name;
**Switch** for each timer's reloadable flag. Change one and the device
applies it on its next window (Button D on the timer screen forces one —
on the chore checklist D ticks chore 3 instead), then republishes the
confirmed value to `magtag/<id>/cfg` so the control reflects reality.

- **Add / edit an extra timer:** the four slots are fixed (the firmware
  ceiling). Set an empty slot's **Timer N name** (e.g. "Running") + minutes
  + reloadable to enable it; clear the name to disable it (this needs
  discovery schema v24 or later; see *Blanking a text control* below). A
  timer-slot edit takes effect on the device's next wake (~≤1 min or a
  button press); allocations / quiet hours / break settings apply live;
  timezone at the next boot.
- **Blanking a text control** (a timer name, the OTA manifest URL, the
  device name, the timezone) works only once the device runs discovery
  schema **v24** (`STATS_JSON_DISC_SCHEMA_VER`) or later. HA publishes
  every control edit retained, and a retained message with an empty
  payload is MQTT's "delete this topic's retained message", so on older
  firmware a blank never reaches the device (BUG-13). From v24 each text
  control's discovery carries a command template,
  `{{ value if value else '""' }}`, so a blank goes out as the two
  characters `""`, and the device reads exactly `""` as empty. A real value
  can never be spelled that way, since the device refuses `"` in every
  text value. The device then applies the field's own rule for an empty
  value. All four kinds accept one: an empty **Timer N name** disables that
  slot, an empty **OTA manifest URL** turns updates off, an empty **Device
  name** makes the device use its id (`magtag-xxxxxx`) as its name, and an
  empty **Timezone** is stored as it is, so after the next boot the clock
  runs on UTC. It does not bring back the build's default zone.
  - **Blanking Timezone moves the lock by hours.** From the next boot the
    device runs on UTC, so bed time, quiet hours and the day rollover all
    shift by the zone's UTC offset (4-5 h on the default US Eastern zone).
    A bulk `"tz": ""` has always done the same. To go back to local time,
    type the zone string (e.g. `EST5EDT,M3.2.0,M11.1.0`) in again.
  - **To get blanking at all**, OTA the device to v24 firmware or later.
    The schema bump makes it republish its discovery at its next sync, and
    HA picks up the template then. Until that happens, use the bulk config
    document instead: `"name": ""`, `"tz": ""` or `"ota_url": ""`, and for
    a timer slot a `timers` entry of `{}`.
  - **After a rollback or downgrade to pre-v24 firmware**, a blank edited
    in between stays retained as `""`, which the old firmware refuses
    (`err:"char"`) at every window. Clear it by typing a real value into
    the control, or by clearing the retained `set/<key>` topic.
- **Timer defaults are a fallback, not a seed.** The `MAGTAG_TIMER<n>_*`
  menuconfig values are what the device runs when NVS holds no timer table,
  but boot does **not** copy them into NVS — a stored table means somebody
  chose it, and inventing one made a build-time default indistinguishable
  from your edit. The table is created the first time an *authority* writes
  one: a bulk-config `timers` array, or a per-timer control edit. After
  that the device is **provisioned**, HA edits win, and changing the
  menuconfig defaults no longer affects it (erase NVS to go back to them).
  Note the second half of that: the first control edit provisions the whole
  table, not only the slot you edited. The slots you did not touch are
  carried across at their menuconfig values — the alternative was writing
  blanks over them and disabling those timers. They are recorded as *not*
  chosen by you, but that record does **not** hand them back to the
  menuconfig defaults: a slot that menuconfig **names** is carried across
  with that name, and a named slot counts as already defined. So a later
  `timers` document that omits a key for such a slot keeps the *stored*
  value, and the slot no longer follows a menuconfig change in a future
  firmware build. Only a slot menuconfig leaves unnamed still falls back to
  the build-time defaults. The one-line version: the first control edit
  provisions every named timer, not just the one you edited.
  If a stored table exists but **cannot be read** (version drift, a bad
  read), the per-timer controls do still NAK, with
  `{"key":"timer1_min","ok":false,"err":"nodefs"}` — the device will not
  write a guess over bytes it could not load. That NAK is not visible in
  Home Assistant (see below); the symptom you will actually see is a
  control that snaps back to its old value at the next `cfg` republish.
  It can clear itself, but only if you have published a config document:
  a retained document carrying a **valid** `timers` array is re-applied on
  the next window even when its `ver` has not changed, precisely so an
  unreadable table gets rebuilt instead of stranding the device. Read that
  condition strictly — there are three ways to have no recovery at all, and
  none of them says anything in Home Assistant:
  - **No retained config document.** If you drive the device only from the
    HA controls and have never published to `magtag/<id>/config`, there is
    nothing to re-apply and this recovery never runs. The controls stay
    dead until you publish a document (or erase NVS).
  - **A retained document with no `timers` array.** It is re-read and
    performs zero writes; the table stays unreadable.
  - **A retained document whose `timers` array is invalid.** Also zero
    writes — and on this path the errors are discarded, so the ack still
    reads `{"ok":true,"skipped":true}`. A failed recovery is indistinguishable
    from a successful one from HA; the device log is the only place it shows.

  A rejected `set/` command is *not* cleared, so it is retried and the edit
  is not lost.
- **The per-field ack is a log line, not a topic.** `config_ack` carries the
  ack for the **bulk config document** only. A per-field `set/<key>` result
  — `ok`, or `err` of `range` / `char` / `value` / `nodefs` / `pair` — is written to
  the device's serial log and nowhere else. There is no `set_ack` topic and
  no HA entity for it. To confirm a control edit took, watch the value the
  device republishes to `magtag/<id>/cfg`: it is what the device now
  believes, so a control that snaps back was rejected.
- **Chore-free minutes and their allocation are a pair.** Each
  `chore_free_*` control (0–1440 min) must stay at or below the allocation
  for the same day type; the device enforces it differently from each end,
  so the pair can never go invalid from these controls:
  - Raising a **chore-free** number above its allocation is **refused**
    (`err` `pair` — the value is in range, it is the *other* field that
    makes it impossible). Nothing is written, and because a refused
    `set/` is left retained it is re-tried every window, and the control
    snaps back at each `cfg` republish. It is not forgotten: raise the
    allocation and the waiting chore-free value **applies by itself** —
    in the same window if the device happens to process the allocation
    edit first, otherwise in the one after. Setting the chore-free number
    back down replaces the waiting value instead.
  - Lowering an **allocation** below its chore-free number **clamps** the
    chore-free number down to the new allocation and applies both. The
    serial log shows `"clamped":"chore_free_wd","clamped_to":30`; in HA
    you see the chore-free control move at the next `cfg` republish.
  A bulk document, or a firmware flash that reseeds the allocations, can
  still leave an invalid pair standing — see
  [below](#chore_free-pairs-in-the-document).
- **Firmware updates:** **OTA manifest URL** (Text) is the https endpoint
  the device checks for a new build; **OTA check on sync** (Switch) makes
  it also check during a Button D full sync, on top of the daily
  rollover check. Clearing the URL to empty is how you turn updates off.
  From this control, that works only on discovery schema v24 or later (see
  *Blanking a text control* above); older firmware needs the bulk
  document's `"ota_url": ""`. It
  is the only off switch, so an empty value is accepted where any
  other non-`https://` value is rejected. Plain `http://` is refused by
  both this control and the bulk document: an unauthenticated firmware
  endpoint is an arbitrary-code-execution channel. A rejected value is
  logged as `{"key":"ota_url","ok":false,"err":"value"}` on the serial
  console — not published anywhere — and the control snaps back at the next
  `cfg` republish, which is the only sign of it you get in HA.

  These override `CONFIG_MAGTAG_OTA_URL` / `CONFIG_MAGTAG_OTA_CHECK_ON_SYNC`
  permanently: unlike the allocation defaults, the OTA keys are excluded
  from the defaults fingerprint, so a later menuconfig change (or any
  change that reseeds NVS) will **not** revert what you set here.
- Commands are sent on retained `set/<key>` topics so the sleeping device
  receives edits made while it's asleep. Idempotent, so re-delivery is
  harmless.
- Numbers render as **numeric entry boxes** (`mode: box`) with `step: 1`, so
  any minute value is accepted — HA's slider/step grid would otherwise
  reject round values like 30 or 45.
- Numbers and switches are **optimistic**: because the device is asleep, the
  `cfg`/`act` state topics only catch up a whole window later, so the
  control shows your edit immediately and the device's next republish
  confirms (or corrects) it. Without this HA snaps the control back to the
  stale retained value the instant you change it. Trade-off on switches:
  HA renders optimistic (assumed-state) switches as **two lightning-bolt
  buttons** instead of a toggle — both variants were tried on-device and
  the buttons beat the snap-back. (MQTT **Text** entities — device name,
  timezone, timer names — have no optimistic mode, so those briefly revert
  to the old value until the next window applies them; names change rarely,
  so this is left as-is.)

### Ordering the controls

The device page auto-sorts entities alphabetically by name within each
category — the firmware can't set a display order (MQTT discovery has no
ordering field). For a custom layout, add a **dashboard Entities card** and
list them in the order you want.

The IDs are `<component>.magtag_xxxxxx_<key>`, where `<key>` is the
registry key in `main/ha_config.c` — **not** a slug of the display name the
card shows. "Weekday allocation" is `weekday_min`; "Quiet hours start
(HHMM)" is `quiet_start`; "Break interval" is `break_interval_min`; "Device
name" is `name`; "Timer 1 name" is `timer1_name`; "Weekday chore-free" is
`chore_free_wd` (`_we`, `_hol`, `_sum` for the others). Use the entity
picker if in doubt.

```yaml
type: entities
title: Kitchen MagTag
entities:
  - entity: text.magtag_xxxxxx_name            # Device name
  - entity: text.magtag_xxxxxx_tz              # Timezone
  - type: section
    label: Daily limits
  - entity: number.magtag_xxxxxx_weekday_min
  - entity: number.magtag_xxxxxx_weekend_min
  - entity: number.magtag_xxxxxx_holiday_min
  - entity: number.magtag_xxxxxx_summer_min
  - type: section
    label: Chore-free minutes (each ≤ its allocation above)
  - entity: number.magtag_xxxxxx_chore_free_wd
  - entity: number.magtag_xxxxxx_chore_free_we
  - entity: number.magtag_xxxxxx_chore_free_hol
  - entity: number.magtag_xxxxxx_chore_free_sum
  - type: section
    label: Quiet hours
  - entity: number.magtag_xxxxxx_quiet_start
  - entity: number.magtag_xxxxxx_quiet_end
  - entity: number.magtag_xxxxxx_bedtime
  - type: section
    label: Breaks
  - entity: number.magtag_xxxxxx_break_interval_min
  - entity: number.magtag_xxxxxx_break_duration_min
  - entity: number.magtag_xxxxxx_screen_bonus  # Screen adjust (min) today
  - type: section
    label: Extra timers
  - entity: text.magtag_xxxxxx_timer1_name
  - entity: number.magtag_xxxxxx_timer1_min
  - entity: switch.magtag_xxxxxx_timer1_reload
  - entity: switch.magtag_xxxxxx_timer1_break
  # ...timers 2-4: same four keys with the digit changed
  - type: section
    label: Sound
  - entity: select.magtag_xxxxxx_tone_expiry
  - entity: select.magtag_xxxxxx_tone_break
  - entity: select.magtag_xxxxxx_tone_bed
  - entity: number.magtag_xxxxxx_alert_volume
  - type: section
    label: Updates
  - entity: text.magtag_xxxxxx_ota_url
  - entity: switch.magtag_xxxxxx_ota_on_sync
```

`screen_bonus` is the one control above that is not in the config registry —
it is published from `main/mqtt_ha.c` alongside `switch.magtag_xxxxxx_locate`
("Find my timer"), which is left off the card because it belongs on a button
rather than in a settings list.

**"Screen adjust (min) today" is a target, not an increment.** The number is
*today's total adjustment* (-240..240), reconciled against what the device has
already applied — so setting it to `-45` takes 45 minutes off the day exactly
once, however many syncs run afterwards. To take a further 15, move it to
`-60`; to give the time back, move it to `0`. Re-sending the same value is
deliberately a no-op, which is what makes a retained command safe to leave on
the broker and safe to replay across a reboot. The device applies it on the
next sync (a Button D press, the day rollover, or any wake that opens a
network window) and confirms it back on `magtag/<id>/act`, which is what the
number box then shows. The day rollover resets the target to `0` and clears
the retained command, so an adjustment never carries into tomorrow.

An adjustment that lands while the Screen timer is still IDLE is held until
the day's first start folds it into the allocation — but the panel and the
`limit`/`remaining` sensors report the adjusted figure without waiting for
that (see the sensor timing note below), and the sync that applies it
repaints the idle screen itself. So a `-45` set in the morning is visible
before anyone presses A, rather than appearing as a jump from X to Y at the
moment the timer starts.

The panel reports the two halves separately: the status line reads
`Weekday - 60 min (-45 min today)`, so the day's normal allowance is still
legible on a day that was adjusted, and the countdown and the progress bar
show what is actually left. With no adjustment the parenthetical is absent
and the line is just `Weekday - 60 min`. The bar's scale stays the day's
default, so a grant simply fills it rather than quietly redrawing the day
against a different yardstick.

The repaint follows the **selected** timer. Screen's adjustment redraws the
panel when Screen is what the panel is showing; adjust it while Piano is
selected and nothing on screen has changed, so the device does not spend a
full e-ink refresh saying so. It will be there the moment you swap back.
The adjustment itself is applied either way — this is about the refresh,
not about the apply.

One timing wrinkle on the sensors: the stats snapshot that feeds
`limit`/`remaining` is collected *before* the deferred adjustment is
applied, so in the very window that carries an adjustment those two
sensors still publish the pre-adjustment figures and catch up on the next
sync. Only the `act` confirmation carries the new target immediately. If
an automation has to act on the new limit in the same breath, key it on
`act`, not on `limit`.

### Bulk config document (holidays, scripted setup)

For values that aren't a single control — chiefly the **holiday list**
and the **chore list**, which exist *only* here — publish a **retained**
JSON document to `magtag/<id>/config` (for those fields the
[config-publishing automation](#the-config-publishing-automation-chores-and-school-calendar)
does it for you; this section is the contract it follows); the device applies it and
republishes the applied version to `magtag/<id>/config_ack`. Every field
is optional except `ver`. A rejected field is named in the ack's `errors`
list but never blocks the others. If more fields fail than the ack can
name, it carries `"errors_truncated": true` alongside the ones it did —
so a shortened list never reads as "everything else was fine".

> **Two requirements that fail silently.** Get either wrong and the device
> does nothing and says nothing — no ack at all, not even an error:
>
> 1. **Publish it retained** (`retain: true`, or `-r` for
>    `mosquitto_pub`). The device is asleep almost all the time; it only
>    reads the topic during a network window, and a non-retained message
>    published while it sleeps is gone by the time it wakes.
> 2. **Change `ver` every time you change the document.** The device
>    applies a document only when its `ver` differs from the last one it
>    applied (stored in NVS as `cfg_ver`), which is what makes a retained
>    message safe to leave on the topic. Republish edited content under
>    the same `ver` and it is skipped, and a skipped document publishes no
>    ack. A timestamp (`"ver": "1758650000"`) or a date-and-counter
>    (`"ver": "20260923-2"`) both work, and so does a hash of the content,
>    which is what the config-publishing automation uses; `ver` may be a JSON string or
>    number, and a string must not contain `"`, `\` or control characters
>    (`{"ok":false,"err":"ver"}`). A document with no `ver` is refused as
>    `{"ok":false,"err":"no_ver"}`. Only the first **23 characters** of
>    `ver` are kept and compared, so two versions that differ only after
>    the 23rd character count as the same one and the second is skipped
>    silently — keep `ver` short.
>
> **`config_ack` is the diagnostic.** After the next window it should carry
> your new `ver` with `"ok":true`, or `"ok":false` and an `errors` list
> naming each field that was refused. If it still shows the *previous*
> `ver`, the device never applied your document: it was not retained, the
> `ver` did not change, or the device has not had a window yet (Button D
> on the timer screen forces one).

**The document has a size limit: 2047 bytes.** Every field below, each at
its longest accepted value — 46 holidays, three 20-byte chore names, four
timers with 15-character names, a 127-character `ota_url` — comes to
**1717 bytes as compact JSON** (no spaces or line breaks, which is what
HA's `to_json` filter produces), leaving 330 bytes spare; the chore fields
account for 166 of those 1717. That figure is measured by a host test
(`test_the_worst_case_document_fits_the_receive_buffer` in
`test/test_config_apply/`), not estimated, so it tracks the code. Layout
counts: the same document written with `", "` / `": "` separators is about
1850 bytes and still fits, but **pretty-printed** (indented, one field per
line) it is about 2250 and does not — nor does text inflated with `\u`
escapes. A document past the limit is refused
whole — nothing in it is applied — and the refusal is published to
`config_ack` as `{"ok":false,"err":"too_long","len":<size>,"max":2047}`,
where `len` is the size of the document you published. Because a retained
document is re-delivered on every reconnect, an over-size one would
otherwise be refused again on every wake for the life of the retained
message with no sign of it anywhere; the ack is that sign. Publish it
compact, shorten it (the holiday list is usually the reason) and
republish with a new `ver`.

```json
{
  "ver": "20260708",
  "name": "Kitchen MagTag",
  "tz": "EST5EDT,M3.2.0,M11.1.0",
  "weekday_min": 60, "weekend_min": 120, "holiday_min": 120, "summer_min": 120,
  "chore_free_wd": 0, "chore_free_we": 30, "chore_free_hol": 30, "chore_free_sum": 60,
  "chores": ["Dishes away", "Trash out", "Homework"],
  "quiet_start": 2230, "quiet_end": 800,
  "break_interval_min": 30, "break_duration_min": 15,
  "summer_start": "2026-05-29", "school_start": "2026-08-20", "school_end": "2027-05-28",
  "ota_url": "https://example.com/magtag/manifest.json", "ota_on_sync": false,
  "holidays": ["2026-10-16", "2026-11-03"],
  "timers": [
    {"name": "Piano", "min": 15, "reload": true, "break": true},
    {},
    {"name": "Meditation", "min": 10, "reload": true, "break": true},
    {}
  ]
}
```

- `tz` is a POSIX TZ string. `quiet_*` are HHMM. `timers` is up to 4
  entries; `{}` disables that slot. Timezone and timer-definition changes
  take effect on the device's next boot/operation (the running slot is
  never disturbed mid-run).
- Within a `timers` entry, `name` and `min` are **required**; `reload` and
  `break` are optional, and each one resolves down a ladder, **per field**:

      this document  >  the per-timer switch (stored)  >  menuconfig

  So an omitted flag **leaves the stored value alone** — omitting them here
  will not undo a switch you flipped in HA — and if there is no stored value
  either, it falls back to the compile-time `MAGTAG_TIMER<n>_RELOADABLE` /
  `MAGTAG_TIMER<n>_BREAK_ELIGIBLE` chosen in `menuconfig` for that slot.
  Omission is never an assertion of false; to turn one off from the
  document, say so explicitly (`"break": false`).
  Two consequences worth knowing.
  **The menuconfig rung is what survives an NVS erase.** The per-timer
  switch stores its value in the erased blob and its retained `set/` command
  is consumed when applied, so a "break off" that exists only as a switch
  flip does not come back after a wipe: the document is silent and
  menuconfig's value wins. Writing `"break": false` into the document is the
  durable way to say off — a flag the document states explicitly is never
  overridden by menuconfig.
  **"Already defined" is tracked by slot, not by name.** Repurposing a slot
  index carries the previous activity's flags over, and on a first
  definition after an erase it carries menuconfig's over. If slot 1 or 2 is
  break-eligible in your `sdkconfig` and you repurpose that index to a
  *screen* activity, omitting `"break"` will inherit `true` and let it run
  during — and drain — a Screen Break. Repurposing a slot to a different
  kind of activity should set `"break": false` explicitly rather than rely
  on the rename.
  **An unreadable table counts as no table, and that direction is unsafe.**
  A stored table that fails its version/size check (a firmware layout
  change, a bad read) is treated as absent, because there is no way to tell
  the two apart without a migration. The stored rung is then skipped for
  *every* slot, so an omitted `break` falls all the way to menuconfig — and
  on a build where slots 1 and 2 ship `BREAK_ELIGIBLE=y`, a deliberate
  break **off** you set from the switch comes back **on**, which is the
  unsafe direction for a gate that decides what may run during a Screen
  Break. The result is then written back over the bytes that were still
  physically there. This is a known, accepted cost of treating unreadable
  as absent; the mitigation is the same as above — `"break": false` stated
  in the document survives any blob loss.
- `break` marks a timer as a genuine break activity (music practice,
  reading): it may be started during a Screen Break and its time does not
  count against the screen-exposure balance.
- `ota_url` must be `https://…` (max 127 chars) or `""` to disable update
  checks; `ota_on_sync` is a JSON boolean (`1` or `"ON"` is rejected, so a
  mistyped document is named in the ack rather than half-applied). Both
  mirror the native controls above, and — like `reload`/`break` — **an
  omitted one leaves the stored value alone**: a document written before
  OTA existed will not clear an endpoint you set from the HA card.
- `holidays` replaces the stored list. The cap is **46 dates**, exactly:
  the store is 512 bytes and each date costs 11 of them (`YYYY-MM-DD` plus
  a separator), so a 47th does not fit. Dates past the 46th are **dropped
  without being named in the ack** — unlike `chores`, which refuses rather
  than truncates — so publish the next twelve months rather than every
  date you know. 46 dates cost about 600 bytes of the document, which
  leaves the whole rest of the schema inside the size limit above.
- `chores` is the chore checklist (see
  [Chore checklist](#chore-checklist-read-only-in-ha)) — an array of up to
  **3** names, in the order they appear on the device, one per ack button
  (B, C, D). It is **document-only**: there is no HA control for it, and
  the bulk document is the one place a chore list can come from. The
  rules, each enforced by refusal, never by truncation:
  - at most **3** entries (`CHORE_MAX`);
  - each a non-empty JSON **string** of at most **20 bytes**
    (`CHORE_NAME_MAX`). Bytes, not characters: names are UTF-8, so
    `"Räum dein Zimmer"` is 16 characters in 17 bytes and an emoji costs
    four;
  - no `"`, no `\`, and no control characters (the same rule every
    config string follows).

  Break any rule and the **whole array is refused**: `"chores"` is named in
  the ack's `errors`, nothing is written, and the list already on the
  device stays exactly as it was — the device never keeps "the entries
  that were fine", because the acks are positional and a half-applied list
  would have the kid ticking rows nobody asked for. A hand-written
  document with a fourth entry is the easy way to hit this; the
  config-publishing automation sends only the first three open items.

  **Absent and empty mean different things.** Leave `chores` out and the
  stored list is untouched (so a document written before the feature
  cannot wipe it). `"chores": []` is the way to **turn the feature off**:
  no list, no gate, no mode button — the device behaves as if the feature
  did not exist.

  Changing the list (a rename, an addition, a removal or a reorder)
  **clears today's ticks** — they are positional, so they stop meaning
  anything — but if the day's screen time has already been unlocked it
  **stays unlocked**; a list edit never re-locks a day. The HA side of a
  list change is under
  [When HA sees a list change](#when-ha-sees-a-list-change).
- `chore_free_wd`, `chore_free_we`, `chore_free_hol`, `chore_free_sum` are
  the chore gate's free minutes for weekday / weekend / holiday / summer
  days — integer **minutes, 0–1440**, the same keys as the four chore-free
  controls. The day's first `chore_free` minutes of Screen time are
  unconditional; the rest of that day's allocation is withheld until every
  chore is ticked. So `0` (the default, on every day type) is **fully
  gated** — no Screen time at all until the chores are done — and a value
  **equal** to the allocation withholds nothing, which is how you switch
  the gate off for one day type while keeping it on the others. The gate
  does nothing at all while no chore list is configured. Like every other
  optional field, an omitted one leaves the stored value alone. Out of
  range, or not a JSON number (`"30"` in quotes is refused), names that
  key in the ack and leaves it unchanged.

#### `chore_free` pairs in the document

Each `chore_free_*` must be **at most** the allocation it is paired with —
`chore_free_wd` ≤ `weekday_min`, `_we` ≤ `weekend_min`, `_hol` ≤
`holiday_min`, `_sum` ≤ `summer_min`. The document is judged on the values
that actually land, after every field in it has been applied, so key order
does not matter. The rule is checked for each pair the document mentions
from **either** side (it can break a pair by raising the free minutes or by
lowering the allocation under them), whatever day it is.

A broken pair **is applied anyway**. The chore-free key is named in the
ack's `errors`, but nothing is clamped and nothing is rolled back — the
device does not guess which half you meant. The HA controls refuse or
clamp (see [above](#editing-config-from-the-ha-card-no-setup)), so they
cannot break a pair. Two things can:

- **a document** that breaks it, as above; and
- **a reseed** — a firmware flash that changes the compiled-in defaults
  resets the four allocations to their menuconfig values but leaves the
  `chore_free_*` minutes alone (see
  [What a reseed resets](#the-document-and-the-controls-give-each-field-one-home)).
  With `chore_free_we` at 150 and `weekend_min` at 180, a reseed to a
  120-minute weekend default breaks the weekend pair with no document
  involved and no ack. It repairs itself in the next window if the
  retained document carries that allocation (the reseed makes the device
  re-apply it); if the allocation lives on its control, set it again
  there.

What happens next depends on the day:

- If the broken pair is **today's** day type, the device locks with a
  **Config Error** screen until it is fixed — see
  [The config-error lock](#the-config-error-lock).
- If it is **another** day type (a broken summer pair written in
  December), nothing locks today. The ack names it once, and the next
  document overwrites that ack — so the durable report is the
  **Config warning** sensor, which names every day type whose stored pair
  is broken in every network window until it is fixed. Fix it before that
  day type comes round, or the device locks on its first day.

Fix a broken pair by publishing a corrected document (new `ver`), or from
the HA controls: lowering the chore-free number, or raising the allocation,
both repair it.

The same fields are available here as on the native controls (`name`, `tz`,
`weekday_min`, `chore_free_wd`, …, and a `timers` array), so scripted/bulk
setup stays possible — but for day-to-day tweaks the Configuration controls
are easier. The fields that exist *only* here are `holidays`, `chores`,
`summer_start`, `school_start` and `school_end`.

#### The document and the controls: give each field one home

The device's NVS holds the live value of every setting. The two ways of
changing one behave differently afterwards:

- **A control edit is consumed.** HA publishes it retained on
  `set/<key>`; the device applies it in its next window and then **clears**
  that retained `set/` topic (a refused edit is the exception — it stays
  retained and is retried). From then on the edit exists only in the
  device's NVS and in what `cfg` reports.
- **A document is replayed.** The broker keeps it retained, and the device
  applies it whenever its `ver` is new — and also after a *reseed* (below),
  which forgets the applied `ver` precisely so the retained document is
  re-applied.

**The override rule: a document that includes a field overwrites that
field every time it applies**, including an edit you made from the control
since the last time. There is no merge and no warning; the control simply
moves back at the next `cfg` republish. The one exception is timing inside
a single window: the device applies the document first and pending `set/`
edits after it, so a control edit still waiting on the broker when a new
document lands wins. An earlier, already-applied control edit does not.

So pick **one home per field**. A field you adjust from the Configuration
controls should be left **out** of any document that gets republished
(omitted = unchanged); a field you keep in the document should not be
edited from its control, because the next document puts it back. This
matters most for a document that is republished often — the
[config-publishing automation](#the-config-publishing-automation-chores-and-school-calendar)
below republishes on every chore-list edit and every 15 minutes, and so
carries only the fields that have no control.

**What a reseed resets, and what brings it back.** When a firmware flash
changes the compiled-in defaults (the menuconfig allocations, or the WiFi /
MQTT credentials), the device resets the **four allocations** and the
**holiday list** to those defaults and forgets its applied `ver`. The
retained document is then re-applied at the next window, so every field it
carries comes back; control edits do not (their `set/` topics were cleared
when they applied). An allocation you manage from the controls therefore
returns to its menuconfig default after such a flash — check the
allocation controls afterwards. Everything else (`chore_free_*`, quiet
hours, names, tones, the chore list, …) is not touched by a reseed.

**The retained document is the replay copy — one publisher, one
document.** The broker holds exactly one retained document on
`magtag/<id>/config`, and each publish replaces it outright. Anything the
new document leaves out is no longer replayed after a reseed, and the
unreadable-timer-table recovery described
[above](#editing-config-from-the-ha-card-no-setup) re-applies only a
`timers` array the retained document still carries. So whatever publishes
this topic should publish **every field that lives in the document** each
time, not just the one it is changing — and if two things publish it,
they must build one document between them, or each publish drops the
other's fields from the replay copy. The config-publishing automation below
is built to be that one publisher: the chores and the calendar fields go
out together in every document it sends.

### The config-publishing automation (chores and school calendar)

The document fields with no HA control — the chore list, the holidays and
the three season dates — come from one committed Home Assistant
automation, **`tools/ha/magtag_publish_config.yaml`**. It serves every
device at once, and it is the **only** publisher of `magtag/<id>/config`.
Read the file itself for the detail: its header comments are its contract,
and this section does not repeat its YAML, so there is only one copy to
keep right. (The dashboard generator prints it too, with these install
notes: `uv run tools/gen_ha_dashboard.py --part automation`.) The header
says it was adapted from "the To-do bridge in docs/home_assistant.md": that
was this section's predecessor, since replaced by it. The device's side of
the contract is under [Bulk config
document](#bulk-config-document-holidays-scripted-setup).

**Installing it.** The file is a YAML list with one entry, `- alias:
"MagTag — publish config"`. Add it to your Home Assistant configuration and
reload automations (Developer tools → YAML → Automations), in one of these
ways:

- **In `automations.yaml`:** append it after the automations already
  there. A fresh HA's `automations.yaml` holds only `[]`: **replace** the
  `[]` with the file's contents rather than appending below it. A list
  entry after `[]` is invalid YAML, and HA then loads no automations at
  all.
- **In a directory of its own:** with `automation manual:
  !include_dir_merge_list automations/` in `configuration.yaml`, the file
  goes into that directory as it is.
- **In a package:** wrap it under an `automation:` key.
- **Wherever your configuration repository keeps automations** (the
  owner's is managed by FluxCD), in any of the forms above.

It has no `id:`, so HA treats it as a YAML-managed automation: it runs
normally, but the automation editor cannot edit it and keeps no traces of
its runs. Edit the YAML, not the UI. It is **not** part of the dashboard:
do not paste it into the dashboard's raw configuration editor. It is
installed once, however many devices you have.

**What it needs:** the MQTT integration (for `mqtt.publish`), one Local
To-do list per device, and the calendar `calendar.school_schedule` (both
below); without the calendar it still publishes the chores but raises a
standing notification. There are no helpers to create.

**Which list feeds which device** is read off the entity id. For each
device, create a **Local To-do** list (Settings → Devices & services → Add
integration → Local To-do) named exactly **`MagTag <node> chores`**, where
`<node>` is the six hex digits after `magtag-` in the device id —
`MagTag 1a0a5c chores` for `magtag-1a0a5c`. HA makes that
`todo.magtag_1a0a5c_chores`, and the automation publishes every list whose
id has that shape to its device. Rename the list's display name afterwards
if you like; the entity id stays, and it is what counts. Adding a device is
creating its list — nothing in the automation changes. Deleting a list
stops publishing to that device but leaves its last document retained on
the broker. The generated dashboard shows each device's list on its tab
(see [Dashboard](#dashboard)).

**What each run does.**

- It runs on every To-do edit made through HA's to-do services (add,
  rename, complete, delete, clear completed), every 15 minutes (which
  catches a drag-to-reorder, a list created since the last run, and the
  holiday window rolling forward at midnight), at 09:07 daily, and when HA
  starts. A burst of edits collapses into one run. "Every To-do edit"
  means **any** list's, not only the MagTag ones: an edit to the shopping
  list runs it too (harmless, since an unchanged document changes
  nothing on the device).
- **Its notifications** (*MagTag: chores not sent…* and *MagTag: school
  calendar*, below) are raised only on a run started by an edit, by HA
  starting or at 09:07 — not by the 15-minute pass, so dismissing one
  keeps it away until the next of those. Since any To-do edit counts, an
  unrelated one such as the shopping list brings a dismissed notification
  straight back if its problem is still there. Each clears itself on the
  first run that no longer has the problem.
- For each device it builds one document: the chores, plus the calendar
  fields below, plus `ver`, and publishes it **retained**.
- **`ver` is a hash of the document's content**, not a clock. An unchanged
  document carries an unchanged `ver`, which the device skips without an
  ack, so publishing every 15 minutes costs nothing on the device, and any
  real change moves `ver` by itself.
- **Only fields with no HA control go in** (`chores`, `holidays`,
  `summer_start`, `school_start`, `school_end`). A controllable field in a
  document republished this often would overwrite that control's edits
  every time — see [give each field one
  home](#the-document-and-the-controls-give-each-field-one-home).

**Chores.**

- **Only open items are sent** (`status: needs_action`). The To-do list is
  the *authoring* surface — add, rename, remove and reorder from the phone
  — not a place to tick chores: the device is the only thing that records
  a chore as done. Completing an item in HA removes it from the device's
  list and, like any list change, clears the day's ticks on the device.
  An empty list publishes `"chores": []`, which turns the feature off on
  the device.
- **Names the device would refuse are skipped, not sent.** The device
  refuses the **whole** list if any one name is over 20 bytes or contains
  `"`, `\` or a control character, and keeps the old list. So the
  automation checks each open item first — by **bytes**, not characters,
  so `Räum dein Zimmer auf` (21 bytes, 20 characters) is caught — sets the
  bad ones aside, sends the first three of the rest, and raises a
  persistent notification (*MagTag: chores not sent to magtag-xxxxxx*)
  naming what it skipped. On the device the skipped item is simply absent,
  and the item after it moves up. Names are never shortened: a cut-off
  name on a kid's checklist is worse than a visible "fix this" in HA.
  Rename the item and it is sent straight away; the notification clears on
  the next run that finds nothing to skip. A fourth open item is not an
  error: only the first three are sent.
- The device takes the list at its next network window, and HA's chore
  entities follow one window later (see [When HA sees a list
  change](#when-ha-sees-a-list-change)). `config_ack` stays the place to
  confirm the device took the document.

**The school calendar.** The automation reads **`calendar.school_schedule`**
(any HA calendar entity with that id), two years ahead.

- **Creating it.** The simplest is a **Local Calendar** (Settings →
  Devices & services → Add integration → Local Calendar) named **`School
  schedule`**, which HA makes `calendar.school_schedule`; add the events
  by hand. A remote or ICS calendar integration works as well, as long as
  its entity ID is `calendar.school_schedule` (rename the entity ID in its
  settings if it came out different).
- **The events** are **all-day** events, one per break, spanning the days
  off. The title decides what an event is, and the match is exact: it is
  case-sensitive and anchored at the start of the title. A title that
  **starts with `No School: Summer`** is a summer break; any other title
  that **starts with `No School`** (`No School`, `No School: Winter
  break`, …) is a holiday. `no school`, `School closed` or `Holiday: No
  School` are ignored.

What it takes from those events:

- **`holidays`** — every **weekday** of every event whose name starts with
  `No School` (other than the summer ones), from today for one year, at
  most **46** (the device's cap; it would drop the rest silently).
  Weekends are left out because the device checks for a holiday before a
  weekend, so a Saturday listed as a holiday would get the holiday
  allocation instead of the weekend one.
- **`summer_start` / `school_start` / `school_end`** — from events named
  `No School: Summer`. `summer_start` and `school_start` are the start and
  end of the next summer break that is not over yet (an all-day event's end
  is exclusive, so its end is the first day of school); `school_end` is the
  day before the summer after that. Between them they classify every day
  until that second summer.
- **Without the calendar** — the entity missing, or returning no events at
  all — it still publishes the chores, leaves **every** calendar field out
  of the document (publishing `"holidays": []` would wipe the device's
  stored list; left out, the stored list stays), and raises a persistent
  notification, *MagTag: school calendar*. It raises the same notification,
  and sends no season dates or no `school_end`, when the calendar has no
  summer break ahead or only one. That last warning ends "Extend
  school_calendar.ics.": that file is the owner's own calendar source, and
  for any calendar the words mean "add the next summer break to wherever
  your calendar's events come from". With a Local Calendar, add the next
  `No School: Summer` event.
  When these are raised and cleared is under *What each run does*, above.

**Keeping a controllable field in the document.** Leaving every field with a
control out is deliberate. If you would rather keep one in the document
(for example so it survives a [reseed](#the-document-and-the-controls-give-each-field-one-home)),
add it to the document in your installed copy **and stop editing it from
its control**; the same goes for a `timers` array. The committed file stays
as it is.

**This automation owns the retained document.** Do not publish to
`magtag/<id>/config` from anywhere else for a device that has its list:
the automation's next run (within 15 minutes) replaces whatever you
published, and the device applies the replacement, since its `ver` differs
from yours. A hand-published document is fine for a device with no list;
to keep a field in the document permanently, put it in the automation, as
above.

## Chore checklist (read-only in HA)

Up to three chores, pushed as the document's `chores` field (normally by
the [config-publishing
automation](#the-config-publishing-automation-chores-and-school-calendar),
from the device's own To-do list), gate part of
each day's Screen time: the first `chore_free_*` minutes are free, the rest
unlocks when every chore is ticked. The kid ticks them **on the device** —
Button A switches the panel to the checklist, and B, C and D tick (and
untick) chores 1, 2 and 3. The device-side behaviour is described in
[ProductOverview.md §5c](ProductOverview.md#5c--chore-checklist); this
section is what Home Assistant sees.

### The entities

| Entity | Name in HA | Category | State |
|--------|-----------|----------|-------|
| `sensor.magtag_xxxxxx_chores_left` | Chores left | Primary | Configured chores not yet ticked today |
| `sensor.magtag_xxxxxx_chores_done` | Chores done | Diagnostic | Chores ticked today |
| `sensor.magtag_xxxxxx_day_chores` | Chores done per day | Diagnostic | Chores ticked on the finished day, from the daily summary |
| `binary_sensor.magtag_xxxxxx_chore_1` … `_chore_3` | `<chore name> done` | Primary | On = ticked today |
| `sensor.magtag_xxxxxx_config_warning` | Config warning | Diagnostic | `OK`, or the day types whose `chore_free` pair is broken |

- **The two counts** carry `state_class: measurement`, so HA keeps
  **long-term statistics** for them. They are not the source for a
  per-day chores graph: a daily *max* of *Chores done* includes the count carried over
  midnight, so a day with nothing ticked can show the day before's full
  count. **Chores done per day** reads the daily summary instead
  (`state_class: total`, like the other summary sensors; see [Graphs and
  statistics](#graphs-and-statistics)). The price of a
  state class is the logbook: HA leaves any sensor
  with a state class out of it, so a change in either count does **not**
  appear in the activity log (see [what the Activity log cannot
  show](#what-the-activity-log-cannot-show)). The per-chore binary sensors
  do, which is where the day-by-day record lives. With no list configured
  both counts read 0.
- **One binary sensor per configured chore**, named from the list —
  `"Homework"` gives *Homework done*. Rows past the configured count are
  **removed** from HA, not left unavailable: with two chores there is no
  `chore_3`. A row whose stored name is empty (which the document cannot
  produce) falls back to its default name, *Chore 3 done*. The entity
  **ID** is positional and never follows the name — `chore_1` is whatever
  chore is first on the list today — so renaming or reordering chores
  keeps the ID and its history, and the history of `chore_1` then spans
  both chores.
- **Config warning** is the durable report of a broken `chore_free` pair
  (see [above](#chore_free-pairs-in-the-document)): `OK` when every pair is
  valid, otherwise every broken day type by name, in the order Weekday,
  Weekend, Holiday, Summer — e.g. `Weekday, Summer`. It is recomputed from
  the stored settings for **every** stat publish (every network window), so it stays until the
  pair is fixed, unlike the retained `config_ack`, which the next document
  overwrites. It has no state class, so its appearing and clearing *are*
  logged ("Config warning changed to Summer"), and unlike the stat-fed
  sensors it has no `expire_after`: it describes stored configuration,
  which stays broken while the device is quiet.
- The counts and the chore sensors carry the same `expire_after` as the
  other stat-fed sensors (see the notes above).

### HA can see the ticks, not make them

The device is the **only** authority on whether a chore is done. HA shows
the ticks and cannot set or clear one: there is no control, no `set/` key
and no command for it, by design. A parent override would need a retained
command and a rule for when it collides with a press; a mis-press is
instead undone on the device, where every chore button **toggles**. If the
ticks are being gamed, that is a conversation, not a race over MQTT.

**The timestamps are sync times, not press times.** Ticking a chore does
not open a network window — the device ticks, repaints and goes back to
sleep — so HA hears about it in the **next** window the device opens, and
the logbook time is the time of that window. In practice that is often the
moment the unlocked Screen time is started (starting a timer opens one),
otherwise the next scheduled sync: the checklist can only be up while no
timer is running, so that is the idle cadence
(`MAGTAG_IDLE_SYNC_INTERVAL_MIN`, default 60 min). Button D on the timer
screen forces one. "Homework done at 16:04" means the device reported it
by 16:04.

At the day rollover every tick clears and `chores_left` goes back to the
full count, reported by the rollover's own window.

### When HA sees a list change

The device fingerprints what its discovery documents depend on — the device
name, the firmware version, the four timer slots' names (and whether each
slot is enabled), and the chore list — and republishes discovery once
whenever that fingerprint moves. For the chore list that is a rename, an
addition, a removal or a reorder.

Within one network window the device publishes discovery and the stat
payload **first** and applies the config document **after**. So a list
change applied in window *N* reaches HA's entity names — and the cleared
ticks — in window *N + 1*. Publish, then press Button D on the timer screen
twice (one window applies it, the next reports it), or let the scheduled
syncs do it.

If the stored list **cannot be read** in a window (an NVS fault, a stored
record from an incompatible layout), the chore entities are left exactly as
they are rather than renamed or removed on a guess, the discovery pass is
not marked done, and it is retried every window until a read succeeds.

### The config-error lock

The device has three locks, each a full-screen takeover it sleeps behind:
the **charge lock** (battery ≤ 10 %), the **Bed Time** lock, and the
**config-error lock**. The last is the chore feature's, and it engages when
**today's** `chore_free` minutes exceed today's allocation — a setting
that cannot mean anything, so the device refuses to guess. The HA controls
cannot create that state; a bulk document or a reseeding firmware flash
can (see [above](#chore_free-pairs-in-the-document)), and a broken pair for some
*other* day type does not lock today; it shows in **Config warning**
instead.

On the device: a running timer is paused, and the panel shows
**Config Error** with the pair it objects to — e.g. `Weekday: free 90 > 60
min` — and `Fix in Home Assistant, press D`. Every button except **D** is
dead. The device then sleeps 30-minute intervals (`CONFIG_ERR_SLEEP_SEC`,
`include/sleep_plan.h`), and every one of those wakes, like every D press,
runs a network window — so HA keeps receiving stats, and the fix can
arrive — and then re-checks the pair.

To clear it, fix the pair from HA — lower that day's chore-free number or
raise its allocation, or publish a corrected document with a new `ver` —
then **press D**. That wake's window applies the fix, the re-check passes,
and the lock lets go and repaints the normal screen. The press does nothing
else: on the checklist it does not tick chore 3, and on the timer screen it
does not start a second sync, and any button pressed while Config Error was
showing — right up to the repaint — is ignored. Without the press it
clears by itself at the next 30-minute wake, and it also lets go if the day
type changes to one whose pair is valid.

**Config warning** can trail the panel by one window. That wake's window
publishes the device's stats *before* it applies the fix, so the sensor
still names the broken day type after the device has let go; it clears at
the device's next network window (the press does not open a second one).

The other two locks outrank it. At bed time the Bed Time screen wins, and a
device that is both config- and charge-locked has no exit — neither a
button nor a network window — until the battery recovers.

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
off by itself. Locate always uses the classic square-wave beeps at
maximum volume — it ignores the configured alert tone and Alert volume
(including mute), because its whole job is being found. (Charge-locked devices don't open windows, so locate won't
reach a dead device — check the charge-lock sensor first.)

### Raw command topic (power users / per-timer grants)

`magtag/<id>/cmd` still accepts a retained JSON command with a unique `id`
(deduped, acked on `magtag/<id>/event`, then cleared) — e.g.
`{"id":"...","grant":{"timer":"Piano","min":10}}` to grant a *specific*
extra timer, which the native Screen-only control doesn't cover.
