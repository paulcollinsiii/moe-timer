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
  daily summary (below). Also
  **Chores done** and **Config warning** (see
  [Chore checklist](#chore-checklist-read-only-in-ha)).

Recorder history on the read-only sensors IS the usage-stats feature —
graph battery over weeks, screen minutes per day, practice completions.

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
- The daily summary publishes at the first wake after midnight and covers
  the finished day: `screen_used_s`, completions per extra timer, and
  `chores_done` of `chores` configured (captured before the rollover
  clears the ticks). *Screen time per day*, *Chores done per day* and the
  `<Name> runs per day` sensors read it, so
  HA files each day's figures under the **next** day (the time they
  arrived). The shift loses no run: a run finished late in the evening,
  after the day's last window, is in the summary. What can go missing is
  a whole day: the device holds the unsent summary in RAM only, so if it
  goes back to sleep before a window has published it (the first window
  after midnight fails: no Wi-Fi, no broker), that day's summary is
  never sent. The per-day graphs then show a gap, which on a bar graph
  looks like a zero day. (A summary kept across sleep and retried is a
  planned follow-up, M4-D1.) If the chore list cannot be read at the rollover (a flash error), the
  summary leaves `chores_done` and `chores` out and *Chores done per
  day* records that day as *unknown*; a list that is simply empty
  reports 0 of 0.
- **Statistics vs the logbook.** Battery %, the summary sensors and the
  two chore counts declare a `state_class`, so HA keeps long-term
  statistics for them. The summary sensors are `total` with a
  `last_reset` taken from the summary's date: each summary is one new
  day, so a period's statistics *change* is exactly what its summaries
  reported, and a repeat of the same summary adds nothing. HA keeps no
  logbook entries for an entity with a `state_class` or a unit, so none
  of these appear in the activity log. The live `<Name> runs` counts
  keep no `state_class` and still log each run.
- **First summary.** HA's statistics take the first value they ever see
  for a sensor as the starting point, not as a change. After the OTA
  that adds the summary sensors, that is the summary already retained on
  the broker, which is therefore recorded once as a state but counted in
  no graph; the next day's summary is the first to count. On a device
  that has never published a summary the sensors read *unknown* until
  one arrives, and that one is the starting point instead. A summary
  with no `chores_done` (one retained by older firmware, or one sent
  while the chore list could not be read) sets *Chores done per day* to
  *unknown*, which the statistics skip. Before the sensor has ever had a
  value it stays *unknown* until the first summary that carries the
  field, which is then the starting point. After it has had one (say a
  rollback to older firmware), that day is *unknown*, not 0 and not the
  previous day's count again, and the next summary with the field counts
  as usual. Its template checks for the field first, so HA logs no
  template warning for such a summary.

### Entity IDs are stable, and do not follow the device name

**Requires Home Assistant 2025.10 or newer.** Every discovery payload
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
`uniq_id` and will not re-slug an existing entity behind your back. So on
an already-paired device the IDs stay as they are until you re-register:

1. Let the device run one network window on firmware carrying discovery
   schema v20 or later (`STATS_JSON_DISC_SCHEMA_VER` in
   `include/stats_json.h`), so the new retained discovery payloads reach
   the broker.
2. Settings → Devices & Services → MQTT → the device → **Delete**.
3. Restart HA (or wait for the next reconnect). It re-reads the retained
   discovery configs and registers the entities with their new IDs.

Do it in that order. Deleting first makes HA re-add from the *old* retained
payload and slug from the name all over again — deleting the device in HA
does **not** clear the retained discovery topics on the broker, so whatever
is retained there is what comes back.

**What step 2 actually costs.** It is an entity-registry delete, not a
cosmetic refresh, and everything HA stores *about* those entities goes with
them: area assignment, custom entity names, custom icons, labels, and
hidden/disabled flags. They come back with the firmware's own names, in no
area, with nothing customised. Anything that names the old IDs —
dashboards, automations, scripts, template sensors, notification groups —
has to be updated by hand; HA does not rewrite references. Recorder history
and long-term statistics are keyed on the entity ID too, so the graphs on
the read-only sensors restart under the new ID while the old series stays
behind, orphaned, under the old one.

That is the price of the change, and it is paid once. Skipping it is a valid
choice: an already-paired device keeps working exactly as it does now, it
just keeps name-derived entity IDs.

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
  + reloadable to enable it; clear the name to disable it. A timer-slot
  edit takes effect on the device's next wake (~≤1 min or a button press);
  allocations / quiet hours / break settings apply live; timezone at the
  next boot.
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
  rollover check. Clearing the URL to empty is how you turn updates off —
  it is the only off switch, so an empty value is accepted where any
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
JSON document to `magtag/<id>/config`; the device applies it and
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
>    (`"ver": "20260923-2"`) both work; `ver` may be a JSON string or
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
  would have the kid ticking rows nobody asked for. A fourth to-do item is
  the easy way to hit this.

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
matters most for a document that is republished often — the To-do bridge
below publishes on every chore-list change.

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
other's fields from the replay copy.

### Holidays from a calendar

Keep school days-off in an HA **Local Calendar** ("School Days Off") and
run a nightly automation that reads the next 12 months of all-day events
and republishes the config with the extracted `holidays` array — so the
family manages no-school days on a normal calendar UI, and the device
picks them up automatically. (This is a pattern, not a ready-made
automation; no YAML for it is given here.) If you also use the To-do
bridge below, do not give the holidays a publisher of their own: the
retained document has one slot, so two automations would keep replacing
each other's fields. Put the calendar lookup into the bridge's
`doc_fields` instead, so one automation publishes one document.

### Chores from a To-do list (optional bridge)

HA's **To-do list** integration (a Local To-do list is enough) gives the
chore list a real checklist UI — add, rename, remove and drag to reorder
from the phone — and one automation turns it into the `chores` field. It is
a convenience, not the contract: publishing the document by hand or from
any other script is equally supported.

It needs one helper, which remembers the list last published so that the
automation publishes (and moves `ver`) only when the list actually changed:
Settings → Devices & Services → Helpers → **Text**, named
`magtag_chores_published`, maximum length **255**.

```yaml
automation:
  - alias: "MagTag — publish chore list"
    mode: queued
    triggers:
      # Adding, completing or deleting an item changes the open-item count.
      - trigger: state
        entity_id: todo.daily_chores
      # A rename or a reorder does NOT change the count, so a state
      # trigger alone never sees it. The periodic check catches it.
      - trigger: time_pattern
        minutes: "/15"
      - trigger: homeassistant
        event: start
    variables:
      device: magtag-xxxxxx
      # ONLY the fields that have no HA control. Anything with a control
      # (allocations, chore-free minutes, quiet hours, names, ...) stays
      # OUT: this document is republished on every list change, and a
      # field in it would overwrite that control's edits each time.
      doc_fields:
        holidays: ["2026-10-16", "2026-11-03"]
        summer_start: "2026-05-29"
        school_start: "2026-08-20"
        school_end: "2027-05-28"
    actions:
      - action: todo.get_items
        target:
          entity_id: todo.daily_chores
        data:
          status: needs_action
        response_variable: items
      - variables:
          # Items the device would refuse (over 20 BYTES, empty, or
          # containing " \ or a control character) are set aside rather
          # than sent: one of them would get the whole list refused.
          # Then the first three that are left, in list order.
          checked: >-
            {%- set ns = namespace(ok=[], bad=[]) -%}
            {%- for n in items['todo.daily_chores']['items']
                         | map(attribute='summary') -%}
              {%- if n | length > 0
                     and (n.encode('utf-8') | length) <= 20
                     and not (n is search('[\\x00-\\x1f"\\\\]')) -%}
                {%- set ns.ok = ns.ok + [n] -%}
              {%- else -%}
                {%- set ns.bad = ns.bad + [n] -%}
              {%- endif -%}
            {%- endfor -%}
            {{ {'chores': ns.ok[:3], 'skipped': ns.bad} }}
      - condition: template
        value_template: >-
          {{ checked.chores | to_json
             != states('input_text.magtag_chores_published') }}
      - action: mqtt.publish
        data:
          topic: "magtag/{{ device }}/config"
          retain: true
          payload: >-
            {{ dict(doc_fields, ver=(now().timestamp() | int | string),
                    chores=checked.chores) | to_json }}
      - action: input_text.set_value
        target:
          entity_id: input_text.magtag_chores_published
        data:
          value: "{{ checked.chores | to_json }}"
      - if:
          - condition: template
            value_template: "{{ checked.skipped | count > 0 }}"
        then:
          - action: persistent_notification.create
            data:
              notification_id: magtag_chores_skipped
              title: "MagTag: chores not sent"
              message: >-
                Too long for the device (20 bytes), or containing a
                quote, backslash or control character:
                {{ checked.skipped | join(', ') }}
```

What it does and does not do:

- **Retained, and `ver` moves only on a real change.** `ver` is the
  publish time, so it is new on every publish, and the helper comparison
  keeps the 15-minute check from publishing an unchanged list — which
  would otherwise make the device re-apply the whole document every
  window. An empty list publishes `"chores": []`, which turns the feature
  off on the device.
- **Only open items are sent** (`status: needs_action`). The To-do list is
  the *authoring* surface, not a place to tick chores: the device is the
  only thing that records a chore as done. Completing an item in HA
  removes it from the device's list — and, like any list change, clears
  the day's ticks on the device.
- **Names the device would refuse are skipped, not sent.** The device
  refuses the **whole** list if any one name is over 20 bytes or contains
  `"`, `\` or a control character, and keeps the old list. So the template
  checks each open item first — by **bytes**, not characters, so `Räum
  dein Zimmer auf` (21 bytes, 20 characters) is caught — sets the bad ones
  aside, sends the first three of the rest, and raises an HA notification
  (*MagTag: chores not sent*) naming what it skipped. On the device the
  skipped item is simply absent, and the item after it moves up. Names are
  never shortened: a cut-off name on a kid's checklist is worse than a
  visible "fix this" in HA. Rename the item and it is sent at the next
  check. `config_ack` stays the place to confirm the device took the list.
- **The helper can always hold what it is given.** It stores the sent
  list as JSON, and three names of at most 20 bytes come to well under the
  helper's 255-character limit. (That is the second reason the check runs
  *before* the list is stored: a helper value over 255 characters is
  rejected by HA, the guard never settles, and the automation would
  publish a new `ver` every 15 minutes.)
- **`doc_fields` holds only fields with no HA control** — here the
  holidays and the three season dates. Everything with a control is left
  out on purpose: this document is republished on every list change, and
  a field in it would overwrite that control's edits each time (see
  [give each field one home](#the-document-and-the-controls-give-each-field-one-home)).
  If you would rather keep some controllable field in the document (for
  example so it survives a reseed), add it here **and stop editing it
  from its control**. The same goes for a `timers` array.
- **Editing `doc_fields` publishes nothing by itself.** The guard compares
  only the chore list, so a changed holiday list waits for the next chore
  edit. To push it now, clear the helper — set
  `input_text.magtag_chores_published` to an empty value (Developer tools
  → Actions → `input_text.set_value`) — and the next check (at most 15
  minutes) republishes the whole document with a new `ver`.
- **This automation owns the retained document.** Do not publish to
  `magtag/<id>/config` from anywhere else while it runs: the next list
  change replaces whatever you published with `doc_fields` plus the
  chores. Put holidays and any other document field here instead.
- Replace `magtag-xxxxxx` with your device id (Settings → Devices &
  Services → MQTT shows it) and `todo.daily_chores` with your list.

## Chore checklist (read-only in HA)

Up to three chores, pushed as the document's `chores` field, gate part of
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
  **long-term statistics** for them. They are not the per-day chores
  graph: a daily *max* of *Chores done* includes the count carried over
  midnight, so a day with nothing ticked can show the day before's full
  count. **Chores done per day** reads the daily summary instead
  (`state_class: total`, like the other summary sensors). The price of a
  state class is the logbook: HA leaves any sensor
  with a state class out of it, so a change in either count does **not**
  appear in the activity log. The per-chore binary sensors do, which is
  where the day-by-day record lives. With no list configured both counts
  read 0.
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
