# Locks

A lock is a full-screen stop that the device sleeps behind until its cause goes
away: a flat battery, bed time, an unset clock or a broken setting. This is how
to recognize each one and how to get out of it.

## The four locks at a glance

| Screen | Trigger | Buttons | Checks again | Clears when |
|--------|---------|---------|--------------|-------------|
| **Charge Me!** | Battery at 10 % or below | None | Every 10 min | Battery above 15 % |
| **Bed Time** | The bed time is reached | None | Every 2 h, with a sync each time | The first check after midnight, or bed time turned off or moved later |
| **No Clock** | Power came back on and the clock could not be set | D only: retry now | Every 30 min, with a sync each time | The clock is set |
| **Config Error** | Today's chore-free minutes exceed today's allowance | D only: check now | Every 30 min, with a sync each time | The setting is fixed |

Every lock except No Clock pauses a running timer when it starts, so the day's
time does not drain while the device is unusable. In every lock, a press of a
button that the table does not list does nothing at all.

**Which lock wins.** Charge Me! outranks everything. Bed Time outranks Config
Error. No Clock ends the wake before Bed Time or Config Error is checked,
because both of those need to know the time. A device that has both a broken
setting and a flat battery has no way out, neither a button nor a sync, until
the battery is charged.

**Setup mode.** A device with no WiFi opens [setup mode](setup_mode.md) ahead
of No Clock and Config Error, but not ahead of Charge Me! or Bed Time. On a
device that has WiFi, holding BOOT also gets out of No Clock into setup, and
BOOT held with D does so under Config Error. Those are the only button
exceptions to the table above.

## Charge Me!

At 10 % battery or lower, the device pauses a running timer, paints
`Charge Me!` once, and sends one last report to Home Assistant. It then leaves
the panel alone, because refreshing e-ink while the battery is browning out can
leave marks on the panel that do not go away. No button wakes it. Every 10
minutes it rechecks the battery and nothing else; it does not sync, so "Find my
timer" cannot reach a device in this state. When the battery is back above 15 %,
the next wake repaints the normal screen.

Between 10 % and 15 %, the device keeps working and shows a `Charge Me!!!`
badge on the bar ([Power and sync](power_and_sync.md#battery)).

## Bed Time

At the bed time a parent sets in Home Assistant, the device pauses a running
timer and shows the inverted **Bed Time** screen:
`Brush teeth | Get water bottles`, `Goodnight!`. The default bed time is 22:00.
Any value from 18:00 to 23:59 is accepted, 0 turns Bed Time off, and any other
value counts as 22:00, so a mistyped daytime bed time can never lock the device
for the day.

The bed-time tone plays, with a purple LED pulse, only when the lock interrupts
something: a running timer or a break. Otherwise Bed Time arrives silently. The
tone is set in Home Assistant and defaults to Gran Vals.

No button works. The device wakes every 2 hours and syncs each time, so a
changed bed time from Home Assistant takes effect at the next wake. The lock
clears at the first wake after midnight. It also clears at a wake where the bed
time has been turned off or moved later than the current time.

A break that would still be running at bed time is not started. The device goes
straight to Bed Time with the alarm ([Screen breaks](screen_breaks.md)). A
device whose clock has never been set never enters Bed Time; see No Clock below.

## No Clock

After the power comes back on (a pulled or flat battery), the device does not
know the time until it can reach a time server. If that first sync fails, it
shows:

```
No Clock
Time not synced - check WiFi
No screen time until it syncs
Press D to retry
```

Until the clock is set, the device hands out no screen time, because it cannot
tell which day it is. It retries every 30 minutes, and at once when D is
pressed. It never gives up. A network where WiFi works but time sync (NTP, UDP
port 123) is blocked keeps the device locked indefinitely.

The wake that sets the clock restores the day as it stood; if that is a later
day, the day starts fresh
([Power and sync](power_and_sync.md#when-the-power-goes)).

Home Assistant time adjustments wait until the clock is set. How that shows in
Home Assistant is covered in
[troubleshooting](../home_assistant/troubleshooting.md#the-device-shows-no-clock).

## Config Error

Each day type has a chore-free setting that cannot be larger than its allowance
([Chores](chores.md#the-gate-chores-unlock-the-rest-of-the-day)). The controls in
Home Assistant refuse such a value, but a bulk configuration document or a
firmware update that changes the built-in allowances can still leave one in
place. If the broken pair is **today's**, the device pauses a running timer and
shows:

```
Config Error
Weekday: free 90 > 60 min
chore_free exceeds the allocation
Fix in Home Assistant, press D
```

Only D works. To clear the lock, fix the pair in Home Assistant by lowering the
chore-free number or raising the allowance, then press D. That wake syncs,
picks up the fix, and returns to the normal screen. The press does nothing else,
and any button pressed while `Config Error` was on the panel is ignored. Without a press, the lock clears at
the next 30-minute wake. It also clears when the day type changes to one whose
pair is valid.

A broken pair for another day type does not lock today. Home Assistant reports
it in the **Config warning** sensor, and the steps for fixing it from Home
Assistant are in [troubleshooting](../home_assistant/troubleshooting.md#the-device-shows-config-error).
