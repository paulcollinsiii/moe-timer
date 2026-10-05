# Configuring the device from Home Assistant

This page explains how to change the device's settings from Home Assistant:
the controls on its device page, the two action controls, the bulk config
document, and the automation that publishes that document for you.

Every change reaches the device at its **next network window**, not at once
([Setup](setup.md) explains why). Pressing **D** on the timer screen forces a
window. On the chore checklist, D ticks chore 3 instead.

There are two ways to change a setting:

- **A control** on the device page: one setting at a time. This is the way
  for day-to-day changes.
- **The bulk config document**: one retained JSON document with many fields.
  Holidays, the chore list and the season dates exist only here, and the
  [config-publishing automation](#the-config-publishing-automation) sends
  them for you.

Pick one of the two for each setting. The two interact
([Give each setting one home](#give-each-setting-one-home)).

## The controls

Edit a control and the device applies the change at its next window. It then
republishes what it now holds on `magtag/<id>/cfg`, and the control shows
that. A control that snaps back was refused;
[Troubleshooting](troubleshooting.md#a-control-snapped-back) says how to
find out why. Most settings take effect in the window that delivers them.
The timezone takes effect at the next wake.

Numbers, selects and switches update **optimistically**: the control shows
your edit at once, and the device's next report confirms or corrects it. HA
draws an optimistic switch as two lightning-bolt buttons instead of a
toggle. Text controls have no optimistic mode, so they briefly show the old
value until the device applies the new one.

| Control | Key | Accepts | Notes |
|---------|-----|---------|-------|
| Weekday / Weekend / Holiday / Summer allocation | `weekday_min`, `weekend_min`, `holiday_min`, `summer_min` | 1–1440 min | The day's Screen time. |
| Weekday / Weekend / Holiday / Summer chore-free | `chore_free_wd`, `_we`, `_hol`, `_sum` | 0–1440 min, at most its allocation | Screen time free before chores are done. 0 = fully gated; equal to the allocation = no gate that day. See [below](#chore-free-minutes-and-their-allocation). |
| Quiet hours start / end (HHMM) | `quiet_start`, `quiet_end` | a real time, 0–2359 | |
| Bed time (HHMM, 0=off) | `bedtime` | 0, or 1800–2359 | The device refuses daytime values. |
| Break interval | `break_interval_min` | 0–480 min | 0 turns breaks off. |
| Break duration | `break_duration_min` | 1–120 min | |
| Timer 1–4 name | `timer1_name` … `timer4_name` | up to 15 bytes | Blank disables the slot. |
| Timer 1–4 minutes | `timer1_min` … | 1–1440 | |
| Timer 1–4 reloadable / break eligible | `timer1_reload`, `timer1_break` … | on / off | Break eligible: may run during a Screen Break and counts as time away from a screen ([screen breaks](../behavior/screen_breaks.md#other-timers-during-a-break)). |
| Expiry / Break / Bed time tone | `tone_expiry`, `tone_break`, `tone_bed` | Classic beep, Ding-ding, Gentle chime, Marimba arpeggio, Gran Vals, Custom WAV | Custom WAV plays the file flashed to the assets partition ([Developer setup](../developer_setup.md)). |
| Alert volume | `alert_volume` | 0–200 % | 100 is the clean reference level. Above that adds gain that clips, for real loudness; around 150 is a good start. 0 mutes. |
| Device name | `name` | up to 31 bytes | Blank uses the device ID (`magtag-xxxxxx`). |
| Timezone | `tz` | POSIX TZ string, up to 47 bytes | For example `EST5EDT,M3.2.0,M11.1.0`. Blank means UTC. |
| OTA manifest URL | `ota_url` | `https://…`, up to 127 bytes, or blank | Blank turns updates off. See [below](#firmware-updates). |
| OTA check on sync | `ota_on_sync` | on / off | |

The key is also the entity ID's last part
([Reference](reference.md#entity-ids)) and, except for the timers, the
field's name in the [bulk document](#the-bulk-config-document). A control
refuses a text value that contains `"`, `\` or a control character. The
setting numbers accept any whole value in their range.

**Blanking a text control** needs firmware on discovery schema v24 or later.
HA would otherwise send a blank as an empty retained message, which MQTT
treats as "delete" and never delivers. Blanking **Timezone** moves the device
to UTC at its next wake, so bed time, quiet hours and the day rollover all
shift by your UTC offset. Type the zone back in to undo it.

### Extra timers

The four slots are fixed. Give an empty slot a name and minutes to enable
it, and clear the name to disable it.

Until you first edit a timer, the device runs the timers built into its
firmware. The first timer edit, from a control or from a document's `timers`
array, stores the **whole** table: every named timer at its current values,
not only the one you edited. From then on, timer changes in a new firmware
build no longer reach that device.

### Chore-free minutes and their allocation

Each chore-free number must stay at or below the allocation for the same day
type, and the controls keep it that way from both ends:

- **Raising chore-free above its allocation is refused.** Nothing changes and
  the control snaps back. The edit is not forgotten, though: it stays on the
  broker and is retried every window. Raise the allocation and the waiting
  value applies by itself, in that window or the next. Setting the
  chore-free number back down replaces the waiting value.
- **Lowering an allocation below its chore-free number clamps** the
  chore-free number down to the new allocation. You see it move at the next
  report.

A bulk document can still leave a broken pair standing. See
[Pairs in the document](#chore-free-pairs-in-the-document).

### Firmware updates

**OTA manifest URL** is the `https://` address the device checks for new
firmware ([the OTA manifest](../ota_manifest.md)). Plain `http://` is
refused, because an unauthenticated firmware source would let anyone on the
path run code on the device. A blank URL is the only way to turn updates off.
The device checks once a day at the rollover. **OTA check on sync** makes it
also check when you press D on the timer screen.

Both settings survive every reflash and update: your HA value stays until you
change it.

## Screen adjust and Find my timer

**Screen adjust (min) today** gives or takes Screen time for today only, from
−240 to 240 minutes in steps of 5. The number is **today's total
adjustment**, not an increment. Set it to `-45` and 45 minutes come off the
day exactly once, however many windows follow. Move it to `-60` to take 15
more, or to `0` to give the time back. Sending the same value again does
nothing. The device applies it at its next window and confirms it on
`magtag/<id>/act`, which is what the box then shows. At midnight the device
resets it to 0, so an adjustment never carries into tomorrow. What it does in
each timer state, and how the screen shows it, is under
[Adjusting from Home Assistant](../behavior/timers_and_schedule.md#adjusting-from-home-assistant). To
grant time to one extra timer instead of Screen, use the
[raw command topic](reference.md#raw-command-topic).

**Find my timer**: switch it on. At its next window the device beeps and
flashes red until a button is pressed or 10 minutes pass, and the switch
turns itself off. It always uses the classic beep at full volume, whatever
the tone and volume settings, including mute. A charge-locked device opens
no windows, so check **Charge lock** first.

## The bulk config document

Publish a JSON document to `magtag/<id>/config`. Every field is optional
except `ver`, and an omitted field leaves the stored value alone. Normally
the [automation](#the-config-publishing-automation) publishes this document,
and this section is the contract it follows. You need it yourself for
scripted setup, or for a device with no chore list.

Two requirements fail **silently**: get either wrong and the device does
nothing and says nothing.

1. **Publish it retained** (`retain: true`, or `mosquitto_pub -r`). A
   non-retained message is gone by the time the sleeping device looks.
2. **Change `ver` every time you change the document.** The device applies a
   document only when its `ver` differs from the last one it applied, which
   is what makes leaving it retained safe. A timestamp, a date and counter
   (`"20260923-2"`) or a content hash all work. `ver` may be a string or a
   number. Only its first **23 characters** count, and it may not contain
   `"`, `\` or control characters.

The device reports the result on the retained `magtag/<id>/config_ack`:
your `ver` with `"ok":true`, or `"ok":false` and the refused fields in
`errors`. A refused field never blocks the others.
[Troubleshooting](troubleshooting.md#a-document-did-not-take) reads the ack
for you.

**Size limit: 2047 bytes.** Every field at its longest accepted value comes
well under the limit as compact JSON, which is what HA's `to_json` produces.
Pretty-printed, with indentation, a full document can go over. A document
over the limit is refused whole, with
`{"ok":false,"err":"too_long","len":<size>,"max":2047}`. Publish it compact,
or shorten it (the holiday list is usually the reason).

```json
{
  "ver": "20260708",
  "name": "Kitchen MagTag",
  "tz": "EST5EDT,M3.2.0,M11.1.0",
  "weekday_min": 60, "weekend_min": 120, "holiday_min": 120, "summer_min": 120,
  "chore_free_wd": 0, "chore_free_we": 30, "chore_free_hol": 30, "chore_free_sum": 60,
  "chores": ["Dishes away", "Trash out", "Homework"],
  "quiet_start": 2230, "quiet_end": 800, "bedtime": 2100,
  "break_interval_min": 30, "break_duration_min": 15,
  "tone_expiry": "Classic beep", "alert_volume": 150,
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

The document uses the same ranges as the controls, but checks only the
length of `name`, `tz` and a timer's `name`, so keep `"`, `\` and control
characters out of them yourself. The fields that exist only in the document:

- **`holidays`**: `YYYY-MM-DD` dates. The list replaces the stored one. The
  device keeps at most **46**; dates past the 46th are dropped without a word
  in the ack. A malformed date is named in the ack, and the rest are still
  stored.
- **`summer_start`, `school_start`, `school_end`**: `YYYY-MM-DD`. Days from
  `summer_start` up to `school_start`, and every day after `school_end`, are
  summer days.
- **`chores`**: up to **3** names in checklist order, one per ack button (B,
  C, D). Each must be 1–20 **bytes** (UTF-8, so an emoji costs four) with no
  `"`, `\` or control character. Break any rule and the **whole list is
  refused**: the device keeps the list it had. Leave `chores` out to keep the
  stored list; send `"chores": []` to turn the feature off. Any change to the
  list clears today's ticks, but a day already unlocked stays unlocked
  ([chores](../behavior/chores.md#the-day)).

**`timers`** sets up to four slots in order. `{}` disables a slot. In an
entry, `name` (up to 15 bytes) and `min` (1–1440) are required. A bad entry
refuses the whole array. `reload` and `break` are optional, and an omitted
one keeps the value already stored for that slot (as set by its switch),
falling back to the firmware's build default only for a slot that has none.
So **state `"break": false` explicitly** when you turn a slot into a screen
activity: otherwise it can inherit "break eligible" from the slot's old
activity or from the build, and run during a Screen Break. A flag the
document states also survives anything that wipes the stored table.

`ota_on_sync` must be a JSON `true` or `false`. A chore-free value must be a
JSON number (`"30"` in quotes is refused).

### Chore-free pairs in the document

Each `chore_free_*` must be at most its allocation, judged on the values
after the whole document is applied. Unlike the controls, the document
**applies a broken pair anyway**, and names the chore-free key in the ack.
What happens next depends on the day type:

- **Today's** pair broken: the device locks with a **Config Error** screen
  until you fix it ([Troubleshooting](troubleshooting.md#the-device-shows-config-error)).
- **Another day type's** pair broken: nothing locks today. The **Config
  warning** sensor names the day type at every window until you fix it. Fix
  it before that day type comes round.

A [reseed](#give-each-setting-one-home) that lowers an allocation can break a
pair the same way, with no document involved.

## The config-publishing automation

`tools/ha/magtag_publish_config.yaml`
([installed in Setup](setup.md#4-install-the-config-publishing-automation))
is the one publisher of every device's document. It publishes only the
fields that have no control: `chores`, `holidays`, `summer_start`,
`school_start` and `school_end`. Its header comments are its full contract.

**When it runs:** on every To-do edit made through HA (any list's, not just
the MagTag ones), every 15 minutes (which catches a drag-to-reorder and a
new list), at 09:07, and when HA starts. Each run publishes a fresh retained
document to every device that has a list. Its `ver` is a hash of the
content, so an unchanged document costs the device nothing, and any real
change moves `ver` by itself.

**Chores** come from the device's `MagTag <node> chores` list. Only open
items are sent, in list order. The list is where you write the chores, not
where you tick them: completing an item removes it from the device's
checklist and clears the day's ticks. An empty list sends `"chores": []`,
which turns the feature off. An item the device would refuse (over 20 bytes,
or containing `"`, `\` or a control character) is left out, and the item
after it moves up. The automation raises a notification, *MagTag: chores not
sent to magtag-xxxxxx*, naming it. It never shortens a name. Only the first
three open items are sent.

**The calendar** (`calendar.school_schedule`, set up in
[Setup](setup.md#3-create-the-school-calendar)) gives:

- **`holidays`**: every **weekday** of every non-summer `No School` event,
  from today for one year, at most 46. Weekends are left out because the
  device checks for a holiday before a weekend, so a listed Saturday would
  get the holiday allocation.
- **`summer_start` / `school_start`**: the first and last-plus-one days of
  the next summer break that is not over yet. **`school_end`**: the day
  before the summer after that.

If the calendar is missing or returns no events, the automation still sends
the chores but leaves **every** calendar field out, so the device keeps its
stored holidays rather than having them wiped. It then raises *MagTag: school
calendar*. It raises the same notification, and sends no season dates (or no
`school_end`), when the calendar has fewer than two summer breaks ahead. The
message's "Extend school_calendar.ics" means: add the next `No School:
Summer` event.

Notifications are raised by a run that an edit, HA's start or the 09:07 run
started, never by the 15-minute pass, so a dismissed one stays away until
then. Each clears itself on the first run that no longer has the problem.

**The automation owns the retained document.** Do not publish to
`magtag/<id>/config` from anywhere else for a device that has a list: the
next run, within 15 minutes, replaces your document. A hand-published
document is fine for a device with no list. To keep a controllable field in
the document, add it in your installed copy of the automation and stop
editing it from its control.

## Give each setting one home

A control edit and a document behave differently once applied:

- **A control edit is consumed.** The device applies it and then clears it
  from the broker. A refused edit is the exception: it stays and is retried.
- **A document is replayed.** It stays retained. The device applies it
  whenever its `ver` is new, and again after a **reseed**: the first boot
  after a flash that changed the built-in allocations
  ([What a flash does to NVS](../developer_setup.md#what-a-flash-does-to-nvs)).

So **a document overwrites every field it carries, each time it applies**,
including an edit you made from that field's control in the meantime. There
is no merge and no warning: the control moves back at the next report.
Within one window the device applies the document first and pending control
edits after it, so only an edit still waiting on the broker wins.

Keep each setting either in the document or on its control, never both.
The automation follows this rule by sending only fields without controls.

**After a reseed**, the document's fields come back, but allocations you
manage from the controls return to the built-in defaults. Check them.
