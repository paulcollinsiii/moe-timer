# MagTag Screen Timer: product overview

Start here. This page is the map: what the timer is for, which document
describes each feature, and the few things every feature shares.

## What it is for

The timer counts down a child's daily screen time. It runs on an Adafruit MagTag,
a small board with a 2.9" e-ink panel, four buttons and four LEDs, and its
magnetic back lets you mount it on the fridge or a wall where the child and the
parent can both see it. It knows how much time today allows, shows what is left
on a progress bar, and takes enforced eye-rest breaks. It can also hold back part
of the day until the chores are done. A parent sets it up, adjusts it and
watches it from Home Assistant.

## Features

| Feature | What it does | Described in |
|---------|--------------|--------------|
| Daily screen time | A per-day allowance (weekday, weekend, holiday, summer) that the child starts and pauses, with an alarm when it runs out | [Timers and schedule](behavior/timers_and_schedule.md) |
| Extra timers | Up to four more countdowns, such as Piano or Laundry, with fixed durations | [Timers and schedule](behavior/timers_and_schedule.md) |
| Screen breaks | An enforced eye-rest break after a set amount of screen exposure | [Screen breaks](behavior/screen_breaks.md) |
| Chore checklist | Up to three chores that unlock the rest of the day's screen time when they are ticked | [Chores](behavior/chores.md) |
| Locks | Full-screen stops for a flat battery, bed time, an unset clock and a broken setting | [Locks](behavior/locks.md) |
| Sleep, sync and updates | Minute-by-minute deep sleep, clock and Home Assistant sync, the battery display, and firmware updates | [Power and sync](behavior/power_and_sync.md) |
| Setup mode | Entering the WiFi and the MQTT broker on the device, by scanning a QR code and filling in a web page | [Setup mode](behavior/setup_mode.md) |
| Home Assistant | Settings, time adjustments, the chore list, the dashboard, and "Find my timer" | [Home Assistant setup](home_assistant/setup.md) |

## The device at a glance

**Buttons.** Four buttons run along the bottom edge: A on the left, then B, C,
and D on the right. The bottom row of the panel puts a label above each button
that usually shows what a press does right now. A blank label means the press
would be refused. Each button's job depends on the screen, so each feature document
describes the buttons for its own screens.

**Lights.** Four LEDs sit along the bottom edge, one above each button. On the
timer screen, the LED above D shows the timer's state and the LED above A shows
a sync. On the chore checklist, all four show the chores. In the last 15
seconds of a countdown, all four count down together. Every light is off while
the device sleeps.

**Screens.**

| Screen | Shown when | Described in |
|--------|------------|--------------|
| Timer screen | Normally, for Screen time and for each extra timer | [Timers and schedule](behavior/timers_and_schedule.md#the-timer-screen) |
| TIME'S UP | A timer has just run out | [Timers and schedule](behavior/timers_and_schedule.md#the-final-minute-and-times-up) |
| SCREEN BREAK | A break is running and Screen is selected | [Screen breaks](behavior/screen_breaks.md#the-break-screen) |
| CHORES | The child pressed A | [Chores](behavior/chores.md#the-checklist) |
| Charge Me!, Bed Time, No Clock, Config Error | A lock is on | [Locks](behavior/locks.md) |
| Updating Firmware | An update is installing | [Power and sync](behavior/power_and_sync.md#firmware-updates) |
| Setup, Release to enter setup, Setup complete, Setup timed out, Setup failed | The device has no WiFi, or BOOT was held | [Setup mode](behavior/setup_mode.md) |

**The header.** The top line of the timer screen looks the same for Screen and
for every extra timer. The left side shows the date and time, for example
`Sat May 16  12:34 PM`, and the clock changes on the minute. The right side
shows `Last sync: 12:30 PM`, which is the last time the clock was set from the
network (`--:--` before the first sync). While a break runs behind another
timer, that spot shows an inverted `BREAK 12:34` chip instead
([Screen breaks](behavior/screen_breaks.md#other-timers-during-a-break)). After
repeated failed syncs it shows a setup hint
([Setup mode](behavior/setup_mode.md#the-no-wifi-hint)).

## How a press is handled

- **A press that can only be refused does not wake the device.** A sleeping
  device wakes only for a button whose press would do something, so mashing a
  dead button costs no battery and no refresh. B and D are the exceptions: they
  always wake it, so the main controls never seem dead. The locks narrow this
  further ([Locks](behavior/locks.md)).
- **When the device is busy on its own**, with its minute repaint or a
  scheduled sync, a press is held and acted on once it is free. If several pile
  up, one is acted on, in the order B, C, D, A, so a start or a pause wins. D is
  the exception: it is dropped unless the chore checklist is up.
- **When your press woke it**, further presses are dropped until it has
  answered, so a double tap does not undo itself. B still pauses during the
  sync that follows a start, and chore ticks all count
  ([Chores](behavior/chores.md#ticking-a-chore)). A press made during the
  wake-up itself is not seen at all ([below](#known-limitations)).
- **Holding a button does not repeat it.**
- **Any press silences an alarm**, and that press does nothing else.

## Quiet hours

From 22:30 to 08:00 by default, the status lights stay dark: the state light,
the sync light and the final-seconds countdown. You can change the hours in Home
Assistant. Setting the start and end to the same time turns quiet hours off.
Alarm pulses still light, because each one comes with a sound, and so do the
chore LEDs, because a tick means somebody is awake and pressing buttons.

## Known limitations

- **A press made while the device is still waking up from another is not
  seen.** Deep sleep records only the button that woke the device. Wait until
  the device answers the first press, on the LEDs or, in quiet hours, on the
  panel.
- **The device cannot see the room.** A ticked chore or a running Violin timer
  is taken on trust. [Chores](behavior/chores.md#trust) and
  [Screen breaks](behavior/screen_breaks.md#setting-up-timers-for-breaks)
  explain why that is the intended design.

## Further reading

- [Home Assistant setup](home_assistant/setup.md): connecting the
  device, the dashboard and the configuration automation.
- [Developer setup](developer_setup.md): building, configuring and flashing the
  firmware.
- [Architecture](architecture.md): how the firmware is built, and the hardware
  ([hardware](architecture/hardware.md)).
