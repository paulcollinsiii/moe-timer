# Power and sync

The device sleeps between minutes and turns WiFi on only for short syncs, which
is what lets it run on a battery. This is how often it wakes and syncs, what the
battery and sync lights mean, what survives a power cut, and when a firmware
update installs.

## Sleeping between minutes

The device spends almost all of its time in deep sleep, with the WiFi radio
off and every LED dark. Between wakes it sleeps for 5 to 64 seconds, as long as
it takes to reach the next whole minute:

- in IDLE, PAUSED and TIME'S UP it wakes on the minute of the clock;
- while a timer runs, or during a break, it wakes when the countdown reaches a
  whole minute. When a sync is due it wakes 20 seconds earlier, so the sync is
  finished before the countdown is repainted.

About 70 seconds before a running timer ends, or before a break ends with its
ding, the device stays awake and watches the end itself. That way the alarm or
the ding comes within about a second of the real time
([Timers and schedule](timers_and_schedule.md#the-final-minute-and-times-up)).
A button press wakes it at any time. The locks replace this rhythm with their
own, longer sleeps ([Locks](locks.md)).

## Syncing

A **sync** is a short network session. The device turns WiFi on, sets its clock
from a time server (it shows local time in the time zone set in Home Assistant,
[configuring](../home_assistant/configuring.md#the-controls)), and exchanges
news with Home Assistant: it reports its
timers, battery and chores, and picks up new settings, time adjustments and the
chore list. Then it turns WiFi off again. WiFi is never on at any other time.

A sync happens:

| When | Why |
|------|-----|
| A timer starts or resumes (B) | The clock is checked as the countdown begins. The countdown starts at once and is not held back by the sync. |
| Every 10 minutes while a timer runs | The board has no accurate clock of its own and drifts in sleep, so this keeps the countdown and the end time accurate. |
| Every hour while nothing runs | Keeps the clock right and Home Assistant up to date. No sync happens during a break with Screen selected. |
| At midnight | Starts the new day ([Timers and schedule](timers_and_schedule.md#midnight)). |
| D on the timer screen | On request ([below](#d-sync-now)). |
| After a failed sync | Retried at every wake, about once a minute, until one succeeds. There is no back-off. |
| During some locks | Bed Time, No Clock and Config Error sync at each of their wakes ([Locks](locks.md)). |

**A sync never gives or takes time.** The countdown is measured against a fixed
end time, so correcting the clock corrects only the drift. A timer started on a
clock that was slightly off keeps its full length after the correction.

**Without WiFi, the timer keeps working, but it tries to sync every minute.**
The sync light shows red each minute outside quiet hours, and the battery
drains faster. The countdown stays right as a length of time, and only the
displayed clock may drift until a sync succeeds. The header's `Last sync` shows
when the clock was last set.

### D: sync now

On the timer screen and on the break screen, D syncs at once and then repaints
the whole panel, which also clears any faint ghosting. The panel repaints once
the clock is set, so the time shown is the corrected one. When the
**OTA check on sync** switch is on in Home Assistant, the same press also checks
for a firmware update. On the chore checklist D ticks chore 3 instead
([Chores](chores.md)).

### The sync light

The LED above A lights blue while a sync is starting. It turns green when the
clock has been set, or red when the sync failed. It goes dark when the sync
ends. It stays dark during quiet hours
([product overview](../product_overview.md#quiet-hours)). On the chore checklist
that LED is the chore gate, so the sync light stays dark while someone is
pressing buttons, but a routine sync while the checklist is up can still light
it ([Chores](chores.md#ticking-a-chore)).

## Battery

The timer screen shows a battery icon and percentage. When the battery is at
**15 % or lower**, a `Charge Me!!!` badge rides on the progress bar, and
everything else keeps working. At **10 % or lower**, the device stops on the
Charge Me! lock until the battery is back above 15 %
([Locks](locks.md#charge-me)).

USB-C is the main power source. The LiPo battery covers the time it is
unplugged.

## When the power goes

A power cut, a crash or a press of the reset button does not give the day's
time back. The device saves its timers whenever they change, and when it comes
back on the same day it restores them. A timer that was running kept counting
down in real time while the power was off, and through any No Clock lock that
followed, so it may come back with less time, or already run out. If the power
was lost and the device comes back with no network, it cannot tell what day it
is and shows the No Clock lock until it can sync ([Locks](locks.md#no-clock)).

## Firmware updates

The device checks for a firmware update at midnight, and also on a D press if
the OTA check on sync switch is on. A check needs an update address, which is
set in Home Assistant or built into the firmware. With no address the device
never checks. A check also needs at least 30 % battery and no charge lock, and
when either is missing it waits for the next check.

When an update is available, the device first finishes its normal repaint. It
then shows **Updating Firmware** with the current and new versions and
`Do not remove power`, downloads the update (for at most 5 minutes) and
restarts on it. The timers come back as they were, and the panel returns to the
timer screen.

- **A failed download changes nothing.** The old firmware keeps running and
  the device tries again at the next check. After three failures in a row for
  the same version it stops trying and reports it in Home Assistant. Publishing
  a new version starts the count again.
- **A bad update undoes itself.** The new firmware has to complete one normal
  wake before it is kept. If it cannot, the next start returns to the previous
  firmware, and Home Assistant's **Update result** sensor reports the rollback.

Publishing an update is covered in [ota_manifest.md](../ota_manifest.md). The
Home Assistant entities that show update status are in the
[reference](../home_assistant/reference.md#entities), and how the update
works inside the firmware is in [OTA](../architecture/ota.md).
