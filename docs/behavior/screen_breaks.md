# Screen breaks

By default, every 30 minutes of screen exposure earns a 15-minute eye-rest
break that Screen cannot skip. This is how a break is earned, enforced and
ended, and why one you expected may not have happened.

## When a break comes due

A break comes due after a set amount of **screen exposure**. A parent sets that
interval and the break's length in Home Assistant
([configuring](../home_assistant/configuring.md#the-controls)), and an
interval of 0 turns breaks off.

Exposure is a running balance, not a clock:

- Screen, or any extra timer that is **not** break eligible, adds to the
  balance minute for minute while it runs.
- A **break eligible** extra timer, such as Violin or Reading, subtracts from
  the balance minute for minute while it runs.
- When nothing runs, the balance does not change, so pausing neither earns a
  break nor cancels one.
- The balance never goes below zero, so hours of violin cannot bank hours of TV.
- Only two things reset it: a break starting, and midnight.

So a child who folds laundry for 15 minutes, practices violin for 15, and then
sits down to watch TV does not go straight into a break, because they really
were off screens. A child who folds laundry with the TV on for the whole
interval gets a break even if Screen was never started, because the eyes do not
care which timer was selected. A break can also come due after the day's Screen
time has run out.

The panel does not show the balance. Home Assistant shows it as the **Screen
exposure** sensor. Read it against the break interval to see why a break has or
has not fired yet.

A break that would still be running at bed time is skipped. The device goes to
Bed Time at once, with the bed-time alarm ([Locks](locks.md#bed-time)).

## When a break starts

The device checks for a due break at every minute wake and right after each
button press. When one is due:

- whatever is running is paused, and Screen is selected;
- the chore checklist closes if it was open;
- the panel switches to the inverted **SCREEN BREAK** screen;
- the break tone plays three times and the LEDs pulse cyan. The tone is set in
  Home Assistant and defaults to Chime. Any button silences it, and the alarm
  also sounds in quiet hours.

## The break screen

The break screen appears while Screen is selected. It shows the break's own
countdown and a bar that drains as the break runs:

```
┌──────────────────────────────────────────────────┐
│                  SCREEN BREAK                    │
│ ████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │  break bar
│                     12:34                        │  break countdown
│ Chores 2 of 3                       Screen 1:30  │  only with a chore list
│ Chores                    → Piano          ⟳     │  button labels: A _ C D
└──────────────────────────────────────────────────┘
```

| Button | Label | During a break, with Screen selected |
|--------|-------|--------------------------------------|
| A | `Chores` | Opens the chore checklist. A break is the time the checklist is meant for, so the line above shows the chores left, for example `Chores 2 of 3`, or a tick when they are all done ([Chores](chores.md)). |
| B | none | Does nothing. Screen cannot be resumed early. |
| C | `→ Piano` | Switches to the next timer. The hint names that timer and appears only when at least one extra timer is break eligible, since otherwise there is nothing to run. C still switches without the hint. |
| D | ⟳ | Sync now, as on the timer screen. |

Without a chore list, cell A shows the frozen Screen time (`Screen 1:30`) in
place of `Chores`. That is information only: A does nothing. When there are
neither break-eligible extra timers nor a chore list, the screen has no button
row at all. It ends with `Timer paused - 01:30:00 left` instead, but C and D
still work.

## Other timers during a break

The break holds Screen, not the whole device: a child on a 15-minute eye rest
can go and practice piano. With an extra timer selected, the panel shows the
normal timer screen with an inverted `BREAK 12:34` chip in the header where
`Last sync` normally sits:

- a **break eligible** timer starts, pauses, runs out and sounds its alarm as
  usual;
- a timer that is **not** break eligible can be selected and viewed, but B will
  not start or resume it, and no ▶ label is shown. A chore done with the TV on
  is not a break. If it has run out and is reloadable, B still reloads it.

## When a break ends

The device stays awake for the last stretch, so the break ends within about a
second of its end time. A short double "ding" then plays once. It is a fixed
sound, whatever the alarm tones are set to. The selection goes back to the
timer the break interrupted, and Screen returns to the state the break found
it in. If Screen
was running, it is now paused and the child presses B to resume.

A break ends **silently** in two cases:

- **An extra timer is running.** The child is in the middle of an activity and
  will hear that timer's own alarm. The selection is left alone, the chip
  disappears at the next minute wake, and C gets back to Screen. A timer that
  has run out does not count as running.
- **The end is noticed late.** If the device first sees the end more than 75
  seconds after it happened, because a lock or a power cut spanned it, the
  break is simply over. There is no ding and no change of selection. The ding
  means "it just happened" and is never a replay.

A power cut in the middle of a break does not cancel it. The break resumes with
the same end time.

## Setting up timers for breaks

- **Make an activity break eligible only if it is truly off-screen.** Music
  practice and reading are break eligible. Folding laundry in front of the TV is
  not.
- **Make chore timers that are not break eligible non-reloadable too.** A
  reloadable timer can be refilled with B once it runs out, so a reloadable
  chore could be earned again without doing the chore.
- **Trust is the design.** A child can leave Violin running without touching
  the violin, and the device cannot see the room. It is self-policing, though:
  at one minute for one minute, dodging a 30-minute break costs 30 real minutes
  away from the TV, which is what the break wanted anyway.
