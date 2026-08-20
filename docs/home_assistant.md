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
  time remaining, Charge-lock.
- **Configuration** (editable — see below): allocations, quiet hours,
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

### Entity IDs are stable, and do not follow the device name

Every discovery payload carries `obj_id`, so HA builds entity IDs from the
MAC-derived device id rather than from the device's friendly name:

    binary_sensor.magtag_xxxxxx_charge_lock
    switch.magtag_xxxxxx_ota_check_on_sync
    number.magtag_xxxxxx_weekday_min

Renaming the device in HA changes the display name and nothing else. This
matters more than it sounds: without it HA derives the entity ID from the
device name, so a rename silently re-slugs every entity underneath it, and
any automation that matched on the old IDs stops matching. A house
automation that turned off every switch except ones matching `magtag` did
exactly that after two devices were renamed — it stopped recognising them
and swept their configuration switches off overnight, which reads on the
device side as settings reverting by themselves.

**One-time step on a device HA already knows.** `obj_id` seeds an entity ID
only at that entity's *first* registration; HA keys its registry on
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
payload and slug from the name all over again.

Automations, dashboards and scripts referencing the old IDs need updating —
that is the cost of the change, and it is paid once.

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
  — `ok`, or `err` of `range` / `char` / `value` / `nodefs` — is written to
  the device's serial log and nowhere else. There is no `set_ack` topic and
  no HA entity for it. To confirm a control edit took, watch the value the
  device republishes to `magtag/<id>/cfg`: it is what the device now
  believes, so a control that snaps back was rejected.
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
list them in the order you want; the entity IDs follow
`number.<device>_<field>` / `text.<device>_<field>` (use the entity picker
if unsure of the exact slug):

```yaml
type: entities
title: Kitchen MagTag
entities:
  - entity: text.kitchen_magtag_device_name
  - type: section
    label: Daily limits
  - entity: number.kitchen_magtag_weekday_allocation
  - entity: number.kitchen_magtag_weekend_allocation
  - entity: number.kitchen_magtag_holiday_allocation
  - entity: number.kitchen_magtag_summer_allocation
  - type: section
    label: Quiet hours
  - entity: number.kitchen_magtag_quiet_hours_start_hhmm
  - entity: number.kitchen_magtag_quiet_hours_end_hhmm
  - type: section
    label: Breaks
  - entity: number.kitchen_magtag_break_duration
  - entity: number.kitchen_magtag_break_interval
  - entity: number.kitchen_magtag_screen_bonus_min_today
  - type: section
    label: Extra timers
  - entity: text.kitchen_magtag_timer_1_name
  - entity: number.kitchen_magtag_timer_1_minutes
  - entity: switch.kitchen_magtag_timer_1_reloadable
  - entity: switch.kitchen_magtag_timer_1_break_eligible
  # ...timers 2-4
```

### Bulk config document (holidays, scripted setup)

For values that aren't a single control — chiefly the **holiday list** —
publish a **retained** JSON document to `magtag/<id>/config`; the device
applies it and republishes the applied version to
`magtag/<id>/config_ack`. Every field is optional except `ver` — applied
only when `ver` differs from the last one, so a retained message is safe
to leave on the topic. A rejected field is named in the ack's `errors`
list but never blocks the others. If more fields fail than the ack can
name, it carries `"errors_truncated": true` alongside the ones it did —
so a shortened list never reads as "everything else was fine".

```json
{
  "ver": "20260708",
  "name": "Kitchen MagTag",
  "tz": "EST5EDT,M3.2.0,M11.1.0",
  "weekday_min": 60, "weekend_min": 120, "holiday_min": 120, "summer_min": 120,
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
off by itself. Locate always uses the classic square-wave beeps at
maximum volume — it ignores the configured alert tone and Alert volume
(including mute), because its whole job is being found. (Charge-locked devices don't open windows, so locate won't
reach a dead device — check the charge-lock sensor first.)

### Raw command topic (power users / per-timer grants)

`magtag/<id>/cmd` still accepts a retained JSON command with a unique `id`
(deduped, acked on `magtag/<id>/event`, then cleared) — e.g.
`{"id":"...","grant":{"timer":"Piano","min":10}}` to grant a *specific*
extra timer, which the native Screen-only control doesn't cover.
