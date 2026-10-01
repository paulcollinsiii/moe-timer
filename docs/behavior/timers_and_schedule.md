# Timers and the daily schedule

Screen time is a daily allowance that the child starts and pauses with B, and
the extra timers are fixed-length countdowns beside it. This is how both behave
on the device, from the day's allowance to TIME'S UP and midnight. Breaks,
chores and locks have their own documents.

## How much time today

Each day gets one of four day types, and each type has its own allowance. When
more than one type applies, the higher one in this list wins:

| Day type | When | Default |
|----------|------|---------|
| Holiday | The date is on the holiday list | 120 min |
| Weekend | Saturday or Sunday | 120 min |
| Summer | A weekday outside the school year | 120 min |
| Weekday | Any other day | 60 min |

A holiday that falls on a weekend therefore counts as a holiday, and a summer
weekend counts as a weekend. A parent sets the four allowances (1 to 1440
minutes) in Home Assistant. The holiday list and the school-year dates compiled
into the firmware are only what a new device starts with. Once the
configuration automation is installed, Home Assistant keeps them current from
its school calendar, so they never need a firmware update
([Home Assistant setup](../home_assistant/setup.md#4-install-the-config-publishing-automation)).

When a chore list is configured, part of the day's allowance can be held back
until the chores are done ([Chores](chores.md)).

## Screen time and the extra timers

**Screen** is the day's allowance. Besides Screen, a parent can configure up to
four **extra timers** for things like Piano practice or Laundry folding. Each
one has a name, a fixed duration and two switches, *reloadable* and *break
eligible*. They are set in Home Assistant
([configuring](../home_assistant/configuring.md#extra-timers)), and a timer with
an empty name does not exist. An extra timer works like Screen and shares its
alarm, lights, sync and power-loss protection, with these differences:

- **Its length is fixed.** It ignores the day type, and the chore gate never
  holds any of it back.
- **It has no breaks of its own.** Breaks belong to Screen, but every timer
  counts toward one. The *break eligible* switch says whether the activity is
  time away from a screen
  ([Screen breaks](screen_breaks.md#when-a-break-comes-due)).
- **A reloadable timer can run again.** After it runs out, B refills it to its
  full duration, ready to start with another B, and the day line counts the
  runs that reached zero:
  `Meditation (x2) - 10 min`. A timer that is not reloadable stays empty until
  midnight and shows no counter. B never refills a paused timer, because B
  resumes it. Screen is never reloadable, so the child cannot reset it. A parent
  gives Screen time back from Home Assistant instead
  ([below](#adjusting-from-home-assistant)).

**Only the selected timer can run.** C switches timers only while nothing is
running, so a paused timer you switch away from stays paused until you come
back. The one exception is a break: it belongs to Screen and keeps counting down
whichever timer you are looking at.

## Timer states

| State | Meaning |
|-------|---------|
| IDLE | Not started today. The time shown is what a start would give. |
| RUNNING | Counting down. |
| PAUSED | Stopped with time left. B resumes it. |
| TIME'S UP | Ran out. Screen stays here until midnight unless a parent adds time or the chores unlock the rest of the day. |
| BREAK | Screen is held by an eye-rest break. Only Screen has this state ([Screen breaks](screen_breaks.md)). |

## The timer screen

```
┌──────────────────────────────────────────────────┐
│ Sat May 16  12:34 PM         Last sync: 12:30 PM │  header
│ ████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │  progress bar
│ ▮ 85%   v1.4.2                        00:42:00   │  battery, firmware, time left
│ Weekday - 60 min (+15 min today)        PAUSED   │  day line, state
│ Chores        ▶             →             ⟳      │  button labels: A B C D
└──────────────────────────────────────────────────┘
```

- **Header:** the date, the time and the last sync. It is the same on every
  timer ([product overview](../product_overview.md#the-device-at-a-glance)).
- **Progress bar:** time left as a share of today's normal allowance, or of the
  timer's duration for an extra timer. An adjustment does not change the bar's
  scale, so extra time fills more of it. On a day with chores still to do, a
  locked block sits at the left end ([Chores](chores.md#the-bar)). At 15 % battery
  or lower, a `Charge Me!!!` badge rides on the bar
  ([Power and sync](power_and_sync.md#battery)).
- **Battery row:** the battery level and the firmware version.
- **Time left:** always `HH:MM:SS`.
- **Day line:** the day type and its normal allowance, `Weekday - 60 min`, with
  any adjustment for today in brackets, `(-45 min today)`. For an extra timer it
  shows the timer's name and duration instead: `Piano - 10 min`.
- **State:** `IDLE`, `RUNNING`, `PAUSED` or `TIME'S UP`.

**When it repaints.** About once a minute. In IDLE, PAUSED and TIME'S UP the
clock changes on the minute, like a real clock. While a timer runs, the
countdown steps by whole minutes: a fresh start shows one exact figure and then
`01:12:00`, `01:11:00`, and so on
([Power and sync](power_and_sync.md#sleeping-between-minutes)). Most repaints are quiet partial updates. A change of state, and every
fifth update, is a full refresh, which makes the panel flash.

### Buttons on the timer screen

| Button | Label | What a press does |
|--------|-------|-------------------|
| A | `Chores` | Opens the chore checklist ([Chores](chores.md)). Only works when a chore list is configured and nothing is running. Otherwise the label is blank and the press does not even wake the device. |
| B | ▶, ⏸, `Reload` or blank | Start, pause or reload; see the list below the table. B always wakes the device, even when it will do nothing. |
| C | → | **Next timer**, cycling Screen → extra timers → Screen. Only works when extra timers exist and nothing is running. A break does not block it. |
| D | ⟳ | **Sync now:** set the clock, exchange news with Home Assistant, and repaint the whole panel ([Power and sync](power_and_sync.md#d-sync-now)). Under the Config Error and No Clock locks, D is the only button that works ([Locks](locks.md)). |

What B does:

- **Start or resume** an IDLE or PAUSED timer. The countdown begins at once,
  the device then checks the clock, and the panel repaints when the check is
  done.
- **Pause** a RUNNING timer, at once.
- **Reload** a reloadable timer that has run out, even during a break. Another
  B starts it.
- **Nothing**, with a blank label, on Screen or a timer that is not reloadable
  once it has run out, and during a break on an IDLE or PAUSED timer that is
  not break eligible ([Screen breaks](screen_breaks.md)).

**The state light.** While the device is awake after a press, the LED above D
shows the selected timer's state: white for IDLE, green for RUNNING, amber for
PAUSED, red for TIME'S UP and cyan for BREAK. On a start, the old color stays
for about 0.4 s before it turns green, so the change itself acknowledges the
press. It also lights while the device watches a timer's final minute or a
break's end. It is dark during sleep, during routine minute wakes, and in quiet
hours ([product overview](../product_overview.md#quiet-hours)). The LED above A
is the sync light: blue while a sync runs, then green when the clock is set or
red when the sync failed
([Power and sync](power_and_sync.md#the-sync-light)).

## The final minute and TIME'S UP

About 70 seconds before a running timer ends, the device stops sleeping and
watches the end itself:

- The panel updates at 60, 45, 30 and 15 seconds left.
- The last 15 seconds are counted in binary on the four LEDs in light green,
  with the most significant bit above D. These lights stay dark in quiet hours.
- Only B works: it still pauses, which cancels the ending. The other buttons
  do nothing.

When the time reaches zero, which happens within about a second of the real
end, the panel shows **TIME'S UP** with an empty bar. The expiry tone plays five
times and the LEDs pulse red. A parent picks the tone in Home Assistant (the
default is Marimba) and can set the volume there. The alarm also sounds in quiet
hours. Any button silences it, and that press does nothing else. The timer
screen then comes back with the state `TIME'S UP`, and the device goes back to
sleep.

## Adjusting from Home Assistant

A parent adds or takes away Screen time with **Screen adjust (min) today**, a
number from −240 to +240. It sets today's total, so moving it back to 0 gives
the time back ([configuring](../home_assistant/configuring.md#screen-adjust-and-find-my-timer)).
The device applies it at its next sync, and pressing D makes that happen now.
What it does depends on Screen's state:

| Screen is | A grant (+) | A deduction (−) |
|-----------|-------------|-----------------|
| IDLE | Added when Screen starts. The time and day line show it at once. | Taken off when Screen starts, and shown at once. |
| RUNNING | Extends the countdown. | Shortens it. Past zero, Screen runs out with the normal alarm. |
| PAUSED | Added to the time left. | Taken from the time left. At zero, Screen runs out. |
| BREAK | Added to the frozen time, which is there when the break ends. | Taken from the frozen time. |
| TIME'S UP | Screen becomes PAUSED and holds the new minutes. The child presses B to start, and the alarm does not sound again. | Ignored, because there is nothing to take. |

The day line shows the adjustment, `Weekday - 60 min (+15 min today)`, while the
bar keeps the day's normal scale. If an extra timer is selected when the
adjustment arrives, the panel does not repaint for it, and the new figure appears
when you switch back to Screen. Midnight resets the adjustment to 0. Giving
time to an extra timer, and why an adjustment can seem not to apply, are
covered in the [reference](../home_assistant/reference.md#raw-command-topic) and
[troubleshooting](../home_assistant/troubleshooting.md#screen-adjust-did-not-seem-to-apply).

## Midnight

The first wake after local midnight starts the new day:

- every timer goes back to IDLE, Screen with the new day's allowance, and a
  timer that was running at midnight simply stops;
- the run counters reset and Screen is selected again;
- the chore ticks and the chore release reset, and the checklist closes
  ([Chores](chores.md#the-day));
- the break balance resets ([Screen breaks](screen_breaks.md));
- the Home Assistant adjustment goes back to 0;
- the device syncs, sends yesterday's totals to Home Assistant, and checks for a
  firmware update ([Power and sync](power_and_sync.md#firmware-updates)).

That wake is normally within a minute. Under Bed Time it is the lock's next
two-hourly check, up to 2 hours after midnight with the default 22:00 bed time,
and under Charge Me! it waits until the lock clears.

Only a real change of date starts a new day. A restart or a power loss during
the day does not give the day's time back
([Power and sync](power_and_sync.md#when-the-power-goes)).
