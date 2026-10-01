# Chores

Chores let a parent hold back part of the day's Screen time until the child has
ticked off up to three jobs on the device. This is how the gate, the checklist
and its lights behave, and what Home Assistant can see.

## Where the list comes from

A parent sets up to three chores, such as "Dishes away", "Trash out" and
"Homework", in a To-do list in Home Assistant. The configuration automation
sends that list to the device, and the device's dashboard tab shows it
([configuring](../home_assistant/configuring.md#the-config-publishing-automation)). It is the only
way to set the list. **With no list, the feature does not exist:** nothing is
held back, the checklist cannot be opened, and button A does nothing.

## The gate: chores unlock the rest of the day

Each day type has a **chore-free** setting next to its allowance. The first
chore-free minutes of the day's Screen time are free. The rest is held back
until every chore is ticked, and it is released the moment the last one is:

| Day type | Allowance | Chore-free | Effect |
|----------|-----------|------------|--------|
| Weekday | 60 | 0 | No Screen time at all until the chores are done |
| Weekend | 120 | 30 | Cartoons first, chores for the rest |
| Summer | 120 | 120 | Nothing held back: the gate is off for that day type |

Chore-free defaults to **0** on every day type, so sending a chore list holds
back every day's whole allowance until the parent sets these numbers. A
chore-free value larger than its allowance makes no sense, and if it is
today's, the device stops on a Config Error screen
([Locks](locks.md#config-error)).

Starting Screen on a gated day starts only the free minutes, and Screen runs
out when they do. Ticking the last chore releases the rest:

- if Screen has run out or is paused, it becomes PAUSED holding the released
  time, and the child presses B;
- if Screen has not been started, it starts later with the whole allowance;
- during a break, the released time is added to the frozen Screen time, and the
  break still ends when it would have.

The gate holds back **only Screen time**. Extra timers, breaks, Bed Time and the
charge lock work exactly as they would without it.

## The bar

On the timer screen, the time held back appears as an outlined block at the
**left** end of the bar. It carries the count and the minutes, for example
`0/3 Chores - 40 min`, and the free minutes drain to its right. On a fully gated
day the block fills the whole bar: `0/3 Chores to unlock 60 min`. On a narrow
block the label shortens to `0/3 - 40 min`, or just `40 min`. When the chores
are done, the block disappears and the bar uses its full width. The release is
not shown in the day line's `(±n min today)`, which reports only a parent's
adjustment from Home Assistant.

## The checklist

Button A switches the panel between the timer screen (where A is labelled
`Chores`) and the checklist (where A is labelled `Timers`). A is refused while a
timer is running, because the dishes cannot be ticked off while the TV clock
runs. It does work throughout a break, and the break screen invites it
([Screen breaks](screen_breaks.md#the-break-screen)).

```
┌──────────────────────────────────────────────────┐
│ CHORES                                    1 of 3 │
│              Screen time unlocked                │  only once released
│ ✓  Dishes away                                   │
│    Trash out                                     │
│    Homework                                      │
│ Timers       ✓ 1          ✓ 2          ✓ 3       │  button labels: A B C D
└──────────────────────────────────────────────────┘
```

The checklist stays up until the child presses A. Ticking the last chore does
not send the child back to the timers; the screen says `Screen time unlocked`
instead. The panel goes back to the timer screen by itself only in these cases:

- midnight;
- a break starting or ending;
- the list being emptied in Home Assistant;
- a restart, including a firmware update, because the device keeps the open
  screen only in memory that a restart clears. The ticks themselves survive.

## Ticking a chore

On the checklist, **B, C and D tick chores 1, 2 and 3**. Every press
**toggles**, so pressing the same button again undoes a mis-press. The rows never
move. With two chores there is no `✓ 3`, and D does nothing. On the checklist D
is not a sync button. Press A to get back to the timer screen to sync.

**The LEDs answer first.** The panel takes a second or more to repaint, so each
press is answered on the LEDs:

- the LED above each chore's button is red while the chore is outstanding and
  green once it is ticked;
- the LED above A is the gate. It is red until the day is released and green
  from then on. It follows the release, not the minutes: on a day type whose
  chore-free equals its allowance, nothing is held back, yet the LED stays red
  until the last chore is ticked;
- an LED above a button with no chore stays dark.

The chore LEDs light only when someone is pressing buttons, and they light
during quiet hours too, because a tick means somebody is awake at the device.
There is no sound. A minute wake that repaints the checklist leaves them dark,
but if that wake syncs, the LED above A briefly shows the sync light: blue,
then green or red. That green is not the gate
([Power and sync](power_and_sync.md#the-sync-light)).

**Several ticks cost one repaint.** After a tick, the panel waits until no
button has been pressed for 1.2 seconds, within an 8-second limit, before it
repaints. Ticking two or three chores in a row therefore shows up in a single
refresh. A tick made during the repaint is picked up and painted after it.

## The day

- **Midnight** clears the ticks and the release, and the panel goes back to the
  timer screen.
- **A restart does not clear them.** Ticks and the release survive a power cut,
  a crash and a firmware update, so a child never redoes a chore because the
  device rebooted.
- **Unticking after the release does not take the time back.** The release
  holds for the rest of the day. Unticking before the release simply leaves the
  gate shut.
- **Editing the list in Home Assistant clears the day's ticks**, because a tick
  belongs to a position in the list, not to a name. An edit never locks a day
  that has already been released.

## What Home Assistant sees

Home Assistant shows each chore's tick and the counts, but it **cannot tick or
untick a chore**. The device is the only authority. A mis-press is undone on the
device, and the history in Home Assistant is the parent's record.

Ticking a chore does not open a network connection. Home Assistant learns about
the tick at the device's next sync. That is often when the unlocked Screen time
is started, otherwise the next scheduled sync, which is hourly while nothing is
running. Pressing D on the timer screen syncs at once. So "Homework done at
16:04" means the device reported it by 16:04. The entities are listed in the
[reference](../home_assistant/reference.md#chores).

## Trust

A child can tick "Dishes away" without touching a dish. The device cannot see
the room, which is the same trust the Screen and Violin timers already assume.
The device is a ritual and a record, not an enforcer.
