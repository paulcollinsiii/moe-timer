# Troubleshooting Home Assistant

Use this page when Home Assistant and the device disagree: an edit that did
not take, a control that snaps back, or a device stuck on a lock screen. Each
section gives the symptom, how to find the cause, and the fix.

Start with the right tool. This shows everything the device says and what
is waiting for it on the broker:

```sh
mosquitto_sub -h <broker> -u <user> -P <password> -v -t 'magtag/magtag-xxxxxx/#'
```

Some answers exist only in the device's serial log
([Developer setup](../developer_setup.md) covers the monitor). And nothing
happens until the device's next network window: press **D** on the timer
screen to force one.

## The device never appears in HA

- **No broker set.** The serial log says `MQTT disabled: no broker
  configured; enter it in setup mode`. The broker is entered on the device, so
  hold BOOT, join the `MagTag-` network and open `http://192.168.4.1/mqtt`
  ([Setup](setup.md#1-point-the-device-at-the-broker)).
- **WiFi does not work.** The timer works without it, but no window can reach
  the broker. After repeated failed windows the header shows a setup hint, and
  setup mode lets you enter the WiFi again
  ([Setup mode](../behavior/setup_mode.md#the-no-wifi-hint)).
- **It is charge-locked.** A charge-locked device opens no network windows
  until it is charged.

## A document did not take

Read the retained `magtag/<id>/config_ack`:

| `config_ack` shows | Meaning | Fix |
|--------------------|---------|-----|
| your new `ver`, `"ok":true` | Applied. | — |
| your new `ver`, `"ok":false`, `errors: [...]` | Applied, except the fields named. Add `"errors_truncated":true` and more fields failed than the ack could name. | Fix those fields; republish with a new `ver`. |
| the **previous** `ver` | The device never applied your document. | It was not retained, its `ver` did not change, or the device has not had a window yet. |
| `"err":"too_long"` | Over 2047 bytes, refused whole. | Publish compact JSON, or shorten the holiday list. |
| `"err":"no_ver"` / `"ver"` / `"parse"` | No `ver`; a `ver` with `"`, `\` or a control character; or invalid JSON. | Fix and republish. |

A document whose `ver` matches the last one applied publishes **no ack at
all**, so the ack keeps showing the last real result. A refusal such as
`too_long` therefore stays until you publish a document with a new `ver`.

Watch for silent drops inside an accepted document, too: holidays past the
46th are dropped without a mention. And if the device has a chore list, the
[config-publishing automation](configuring.md#the-config-publishing-automation)
replaces a hand-published document within 15 minutes.

## A control snapped back

A control that returns to its old value at the next report was **refused**.
Why it was refused appears only in the serial log, as a line such as
`set chore_free_wd: {"key":"chore_free_wd","ok":false,"err":"pair"}`. No
topic or entity carries it. Common causes:

| `err` | Cause |
|-------|-------|
| `pair` | A chore-free value above its allocation. It is retried every window and applies once you raise the allocation ([Configuring](configuring.md#chore-free-minutes-and-their-allocation)). |
| `range`, `time`, `len` | Out of range, not a real HHMM time, or too long. |
| `char` | The text contains `"`, `\` or a control character. |
| `value` | An OTA URL that is not `https://`. |
| `nodefs` | The stored timer table cannot be read ([below](#timer-controls-stopped-working)). |

A refused edit stays retained on `set/<key>` and is retried every window
until you send a valid value. A control that moves **without** your edit was
usually overwritten by the bulk document
([Give each setting one home](configuring.md#give-each-setting-one-home)).

**After a downgrade to firmware before discovery schema v24**, a text control
you blanked stays retained as `""`, which the old firmware refuses as
`char` at every window. Type a real value into the control, or clear the
retained `set/<key>` topic.

## Every control went unavailable

All the controls read from the one `cfg` document. If the device cannot fit
it in its buffer, it does not publish it, and every control goes unavailable
at once. The serial log says `cfg state JSON truncated`. This needs a
firmware fix, not a settings change.

## Timer controls stopped working

If the stored timer table becomes unreadable (after a firmware change to its
layout, or a bad read), every timer control is refused with `nodefs`, and the
device runs its build-default timers. The device rebuilds the table from the
retained bulk document, at the next window, but only when that document
carries a **valid `timers` array**. There is no recovery, and no sign of it in
HA, in three cases:

- no document has ever been published to `magtag/<id>/config`;
- the retained document has no `timers` array (the config-publishing
  automation never sends one);
- its `timers` array is invalid.

To recover, publish a document with a valid `timers` array and a new `ver`.

## The device shows Config Error

Today's chore-free minutes exceed today's allocation, and the panel names the
pair, for example `Weekday: free 90 > 60 min`. The controls refuse such a
value, so the cause is a bulk document, or a
[reseed](../developer_setup.md#what-a-flash-does-to-nvs) that lowered an
allocation.

Fix the pair from HA: lower that day's chore-free number, raise its
allocation, or publish a corrected document with a new `ver`. Then press
**D** on the device. [Locks](../behavior/locks.md#config-error) covers the
lock itself, including how it clears without a press.

**Config warning** can still name the day type for one more window after the
device lets go, because the device reports before it applies the fix. It
clears at the next window.

## The device shows No Clock

The device cannot date the day until it reaches a time server, usually after
a power loss. [Locks](../behavior/locks.md#no-clock) covers the lock
itself, including why it can last. What HA sees, if WiFi and the broker work:

- **Timer state** reads `NO_CLOCK`, and every timer's remaining time and limit
  read 0. Treat `NO_CLOCK` as "not in service" in automations.
- **No daily summary** is sent for the day the device cannot date.
- **Grants and Screen adjust wait.** A raw-command grant and a Screen adjust
  target stay on the broker, unapplied and unacknowledged, until the clock is
  back. Send **one grant at a time** while No Clock shows: the broker keeps
  only the last retained command, so a second grant replaces the first.
- **A Screen adjust set during the lock is dropped if the device comes back
  on a new day**, because a new day always starts with no adjustment. The box
  then shows 0; set it again.

Settings and bulk documents are not tied to a day, so they still apply while
the device is locked.

## Screen adjust did not seem to apply

- **Screen time limit and remaining still show the old figures.** In the
  window that delivers the adjustment, the device reports before it applies
  it. They catch up at the next window.
- **The device shows No Clock.** The adjustment waits, and is dropped if the
  device comes back on a new day ([above](#the-device-shows-no-clock)).
- **It is a new day.** Midnight resets Screen adjust to 0.
- **Screen had already run out.** A deduction then does nothing: there is
  no time left to take
  ([adjusting from HA](../behavior/timers_and_schedule.md#adjusting-from-home-assistant)).

## A grant or Find my timer did nothing

- **No event ack after a grant:** the command was refused (wrong timer name,
  `min` outside 1–240, no `id`), its `id` repeats the last applied one
  ([Raw command topic](reference.md#raw-command-topic)), or the device shows
  No Clock, which holds grants ([above](#the-device-shows-no-clock)).
- **Find my timer did nothing:** it runs at the device's next window, which
  can be an hour away while the device is idle. A charge-locked device opens
  no windows at all, so check **Charge lock**. No Clock does not stop it.
