# Hardware checklist

These are the checks only a real MagTag can answer: the panel, the buttons,
the speaker, the LEDs, the power draw, the sensors and a real network. The
host tests cover the logic behind each one (see
[Running host tests](developer_setup.md#running-host-tests)). Each item is a
step and the result to expect.

The device needs working WiFi credentials
([Configuration and credentials](developer_setup.md#configuration-and-credentials));
without them, a power-on stops at the No Clock screen and only D wakes it.
Items marked **HA** also need a broker with the device set up in Home
Assistant ([Home Assistant setup](home_assistant/setup.md)). Items marked
**cell** need a LiPo cell.

Flash the build and watch the log with `tools/monitor.sh`
([Flashing and monitoring](developer_setup.md#flashing-and-monitoring)).
To shorten the waits, change the allocation, the break interval and duration,
or quiet hours from HA instead of rebuilding. Never erase NVS.

## Boot and panel

- [ ] **Boot.** Flash and cold-boot. → The boot log shows no panic.
- [ ] **First paint.** Look at the first screen. → The idle timer screen,
  upright, not mirrored, not inverted, not shifted, at full contrast. If
  not, see the bring-up comment above `ROT_FLIP_X` in `main/display.c`.
- [ ] **Sleep cadence.** Leave it idle for a few minutes. → It wakes on each
  minute and updates with a partial refresh (no flash). Every 5th paint is a
  visible full refresh.
- [ ] **Panel protection.** Mash the buttons. → No crash. Refreshes
  serialize, and the log shows `refresh rejected` for any refresh less than
  1 s after the last.

## Buttons

- [ ] **Wake buttons.** After at least one full sleep cycle, press each
  button from sleep, with no lock showing. → B and D always wake it. A
  wakes it only with a chore list configured and no timer running. C wakes
  it only when it can swap timers (extra timers configured, none running)
  or tick chore 2 on the checklist. Where A or C should not wake it, press
  B from the same state as a control.
- [ ] **Held-button dismissal.** Dismiss a sounding alarm with one button
  and keep holding it past the next sleep. Repeat with two buttons held
  together. → The held button's action never fires. The log shows
  `still held from previous wake - ignoring` until you release.
- [ ] **Presses during a refresh.** Press B during one of the final-minute
  partial refreshes. On the checklist, press D while the panel is still
  refreshing from the previous press. → Each press is acted on when the
  refresh completes: the timer pauses, and chore 3 ticks.

## Sound and lights

- [ ] **Expiry alarm.** Let a timer expire. → The speaker sounds with a red
  LED pulse, and any button silences it.
- [ ] **Break alerts.** Let a break start, then end. → The start sounds with
  a cyan LED pulse, and any button silences it. With no extra timer running,
  the end plays a short chime.
- [ ] **Locate (HA).** Turn on **Find my timer** in HA, then press D on the
  timer screen so a network window picks it up. → The speaker sounds with a
  red LED pulse, past the 180 s awake limit, until a button press or about
  10 min.
- [ ] **Status LEDs.** Watch the LEDs through a start, a pause, a chore tick
  and a timer's final 15 s. → Each pixel shows the color and position
  described in [Timers](behavior/timers_and_schedule.md#buttons-on-the-timer-screen)
  and [Chores](behavior/chores.md#ticking-a-chore), and the final 15 s count
  down in binary in the
  [documented order](behavior/timers_and_schedule.md#the-final-minute-and-times-up).
  During quiet hours the status pixels stay
  dark, but alert pulses still show.

## Power

- [ ] **Reset.** Press Reset with a timer running. → The countdown
  continues and the allocation is not refunded (log:
  `Timer state restored from NVS snapshot`).
- [ ] **Power cycle.** Power-cycle it during a break. → The break resumes
  with its original end time.
- [ ] **Power-on without WiFi.** Power on with WiFi unreachable. → The No
  Clock screen shows, and only D wakes it.
- [ ] **WiFi back.** Then bring WiFi back and press D. → The timer screen
  returns with the day's used time intact.
- [ ] **Battery reading (HA).** Check the header battery icon and HA's
  **Battery voltage** on USB, then on a LiPo. → On USB about 4300 mV and
  100 %. On a LiPo 3300–4200 mV, with a percentage that tracks it.
- [ ] **Low battery (cell, HA).** Run a cell down past 15 % and then 10 %,
  then charge it. → At 15 % the bar shows a **Charge Me!!!** badge. At 10 %
  the panel shows only **Charge Me!**, a running timer pauses, the buttons
  do nothing, and HA's **Charge lock** turns on. Once charged above 15 %,
  the normal screen returns within 10 min.
- [ ] **Light sensor (HA).** Cover the sensor, then shine a light on it. →
  HA's **Ambient light** changes, and the battery reading from the same
  wake is still right.

## Network

- [ ] **Sync (HA).** Press D on the timer screen. → The WiFi pixel is blue
  during the sync, then green. The log shows `published N messages`, and
  the device appears in HA as `magtag-xxxxxx`.
- [ ] **Start.** Start a timer. → It counts down at once, without waiting
  for the sync.
- [ ] **Grant (HA).** Grant screen time from HA to an expired Screen timer.
  → It shows as PAUSED with the granted time, and the TIME'S UP alarm does
  not fire again.
- [ ] **Day rollover (HA).** Let a day roll over. → The day's summary
  reaches HA.
- [ ] **Start without WiFi.** Start a timer with WiFi unreachable. → The
  timer still starts, and the WiFi pixel turns red instead of green.
