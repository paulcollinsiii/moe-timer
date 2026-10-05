# The wake cycle

The life of one wake, from reset to the next deep sleep: what runs in what
order, where the four locks cut in, how the next wake is chosen, and what
stops a wake that hangs. The device spends almost all its time asleep, and
every wake, whether from the timer, a button or a power-on, is a fresh boot
through `app_main()`.

## Boot

`app_main()` in `main/main.c` runs the same sequence on every wake, and its
order is a hardware contract:

1. Start the panic breadcrumb (touches only RTC memory).
2. `neopixel_init()`, the first peripheral call: drive GPIO 21 high so the
   LEDs are off ([S27](../architecture.md#system-invariants)).
3. Arm the awake failsafe (below).
4. NVS: init, seed missing defaults, file a previous panic.
5. Set the time zone from NVS.
6. Validate the RTC timer state, install the timer definitions, and restore
   the NVS snapshot if RTC memory was wiped ([state.md](state.md)).
7. Run the OTA rollback detector ([ota.md](ota.md)), then init the buttons,
   the battery ADC and the display.
8. **Charge lock gate.** A battery at or below 10 % locks the device: the
   wake that engages the lock pauses any running timer, paints Charge Me and
   opens one network window to report it, and every locked wake sleeps 600 s
   without arming a button. It does not return.
9. Hand the wake-cause register to `wake_flow_handle_wake()`.

## Two handlers

`wake_flow_handle_wake()` (`main/wake_flow.c`) sends a wake whose causes
include EXT1 to the button handler; everything else, including a power-on,
which reports no cause, goes to the tick handler. The button handler first
drops a press that is a hold carried over from the last sleep: the same
button, still down, after a sleep of 2 s or less goes straight back to sleep
(S24). A wake caused by BOOT is taken ahead of that guard, because BOOT decodes
as no A–D button. After that, both follow the same shape:

```
setup route ─▶ day rollover ─▶ lock gates ─▶ break-end drain ─▶ the wake's own
      work ─▶ render ─▶ latched presses ─▶ awake watch ─▶ OTA download if pending
      ─▶ sleep
```

- **Setup route.** A no-SSID power-on or press, and every BOOT wake, leave the
  path here and do not return: a completed BOOT hold enters setup, and a short
  one just sleeps ([below](#setup-mode)). A no-SSID timer wake, or one under Bed
  Time, carries on.
- **Day rollover.** On a new local date: queue yesterday's summary, arm the
  daily update check, open a network window, then restore today's snapshot
  or reset the day. The window comes first so the date is judged on a
  corrected clock.
- **Lock gates.** Three of the four locks run here, after the rollover
  because they need the day settled; the charge lock already ran in
  `app_main`. `lock_gate_check_bedtime()` runs all three, despite its name:
  no clock, then Bed Time, then config error.
- **Break-end drain.** Settles a break that ended since it was last looked
  at ([below](#the-break-end-edge)).
- **The wake's own work.** A tick wake opens a network window if a sync is
  due: by default every 10 minutes while a timer runs and hourly otherwise
  (`CONFIG_MAGTAG_RUNNING_SYNC_INTERVAL_MIN`,
  `CONFIG_MAGTAG_IDLE_SYNC_INTERVAL_MIN`), never during a break. It then
  waits up to 25 s so the render lands on the minute. A button wake runs the
  press: B, A and C through one dispatch; D syncs on the timer screen and
  ticks chore 3 on the checklist. A start, a resume or a D sync paints
  early ([network_and_ha.md](network_and_ha.md#one-window-in-order)).
- **Render.** `wake_policy_render()` picks a partial refresh, a full one, or
  the expiry alert ([display.md](display.md)).
- **Latched presses.** A press made while the device was awake is acted on
  now, through the same rules as a wake press ([peripherals.md](peripherals.md)).
- **Awake watch.** With 75 s or less to an expiry or a chiming break end, the
  wake stays up and fires the event within a second of its wall time instead
  of sleeping through it.

A lock gate that engages paints its own screen and ends the wake in
`enter_deep_sleep()`. One that releases lets the wake carry on, owes the
panel a full refresh, and treats every press made while the lock screen was
up as the lock's, so none of them acts. When several locks hold, the sleep
follows the precedence in [S20](../architecture.md#system-invariants). What
each lock means to the family is in [locks.md](../behavior/locks.md).

Off this path: either handler starts a screen break, before or after its
render, once the exposure balance has crossed its interval. It paints the
break screen and sleeps at once, skipping the rest of the wake.

## Setup mode

Setup is a wake outcome, not a timer mode: it records no day, it has no
`app_mode_t`, and it never calls `esp_restart()`, which would zero the RTC state
(S47). It always ends in one sleep through the funnel (S48). The charge gate
runs in `app_main` first, so a charge-locked device never reaches it.

**The no-SSID route.** `wake_flow_route_no_ssid()` is the first step of both
handlers, ahead of the rollover and the lock gates. It asks
`lock_gate_bedtime_in_force()` first, because Bed Time on a plausible clock
outranks setup, then the pure `setup_trigger_decide()` over four facts: has an
SSID, button wake, cold boot, hold completed. With no SSID, a cold boot or any
button wake enters setup. `wake_flow_reset_class()` folds the reset reason into
three classes: a deep-sleep wake, COLD (power-on, EN, software restart, USB) and
FAULT (everything else). A FAULT reset with no SSID does not open setup
unasked: it paints the no-SSID `Setup failed` screen over whatever a crashed
session left on the glass and sleeps buttons-only, so the next press starts
setup. A timer wake with no SSID changes nothing.

It runs ahead of the rollover because a rollover window cannot succeed with no
SSID and would count a join failure before setup ran, and ahead of the no-clock
gate because a new device's clock is unset and that gate would end the wake. The
rollover then happens on the first wake after setup, since the stored day is
still empty.

**The BOOT route.** `buttons_woke_by_boot()` identifies a BOOT-only wake, and a
press of A–D with GPIO0 low is routed the same way, so the gesture works on
builds where BOOT cannot wake the device. Both go to
`wake_flow_handle_boot_wake()`, never the press path: no strip claim, no LED
ack, no rollover, no window. With no SSID it enters setup at once, unless Bed
Time is in force on a plausible clock: then it sleeps the ordinary schedule,
whose timer wake engages the lock. With one,
`wake_flow_wait_for_boot_hold()` samples GPIO0 every 100 ms through the pure
`setup_trigger_boot_hold_sample()` tracker. Crossing `MAGTAG_BOOT_HOLD_MS` paints
the release hint, and a release after that returns true. A release before it, or
a pad that was never down, does nothing and paints nothing. The wait is bounded
at the threshold plus 10 s, below the awake failsafe by a `_Static_assert`, and a
bound hit after the hint repaints the standing lock or the current screen. The
config-error sleep arms D alone, so there the gesture is D with BOOT held.

**The session.** `wake_flow_run_setup_and_sleep()` darkens the LEDs and runs
`setup_session_run()` with the ops table that `setup_mode_ops()`
(`setup_session_idf.c`) builds; `main.c` supplies only the failsafe extend.
The order is: extend the failsafe to `MAGTAG_SETUP_MAX_SEC` + `SETUP_SESSION_TAIL_SEC`, paint the
setup screen, start the SoftAP and provisioning ([network_and_ha.md](network_and_ha.md#provisioning)),
poll for events until one ends it, stop everything, paint the end screen. The
screen is painted before the AP starts because a refresh during a WiFi transmit
burst has browned out this board. The result picks the sleep:

| Session result | Sleep | Next wake |
|----------------|-------|-----------|
| WiFi or MQTT saved | `WAKE_SLEEP_SETUP_NET_WINDOW`: 1 s, buttons armed | A tick wake. `timer_force_ntp_sync()` makes its cadence check open a network window at once, and the failure count is cleared, because it described the old credentials |
| Timeout or failure, no SSID | `WAKE_SLEEP_BUTTONS_ONLY`: no timer, buttons armed | A press |
| Timeout or failure, with an SSID | The ordinary mode from `lock_gate_sleep_mode()` | The normal schedule, any standing lock kept |

Neither new mode is a lock, and `wake_sleep_mode_select()` never returns them;
only the setup path chooses them.

## The break-end edge

A screen break ends on its own wall clock, often inside some other tick.
`timer.c` latches the end at the moment any tick observes it, and
`wake_flow_break_end()` is the one owner that drains it: chime if nothing else
is running and the end is less than 75 s old, return to the timer the break
interrupted, and force a full repaint. Both handlers drain it before
capturing the state they diff against, and again at the guaranteed drain
before sleep, so no tick can lose the edge and none can chime it twice (S10).

## Choosing the next wake

`enter_deep_sleep()` gathers the timer readings into one struct and passes it
to the planner in `main/sleep_plan.c`, which is pure and host-tested:

| State | Wakes on |
|-------|----------|
| Idle, paused, expired | the next wall-clock minute, so the header clock flips on time |
| Running, or Screen on its break | the countdown's own minute grid, so the display reads round minutes |

Either way the nap is 5–64 s. Running wakes come 20 s early when a sync is
due. Any wake is pulled in to land about 70 s before an expiry or a break
end. A break running behind another timer counts as a second event only when
its end will chime, because a silent end needs no wake of its own. A lock
replaces the plan with a fixed interval: 600 s for charge, 7200 s for Bed
Time, 1800 s for config error and no-clock. The two setup sleeps
([above](#setup-mode)) are fixed too: 1 s, and none.

## The sleep funnel

Every sleep goes through `enter_deep_sleep()`, the only call to
`esp_deep_sleep_start()` (S42). In order, it:
1. joins any open network window (at most 15 s);
2. certifies the running image if it is new ([ota.md](ota.md));
3. saves the timer snapshot;
4. waits up to 3 s for buttons to be released and notes any still held;
5. turns the LEDs off with an acknowledged stop, then holds GPIO 21 (gate)
   and GPIO 16 (amp) through the sleep;
6. reports a break end that was never drained;
7. arms the button wake mask and the planned timer wake, and sleeps. The timer
   is armed by `sleep_plan_arm_timer()`, which arms nothing for a zero
   interval: that is the buttons-only sleep, and no planned sleep is zero.

## The awake failsafe

A one-shot `esp_timer` armed at boot forces a sleep after
`CONFIG_MAGTAG_MAX_AWAKE_SEC` (180 s), so a hung radio or a stuck panel cannot
drain the battery. It enters the same funnel, so the snapshot is still saved,
but it first tells `ota_flow` that this sleep must not certify a new image.
Only the locate alarm, an OTA download and a setup session may push it out
(S41). A setup session extends it to its own budget plus `SETUP_SESSION_TAIL_SEC`; if the failsafe
still fires mid-session, it sleeps on the ordinary schedule and paints nothing.
