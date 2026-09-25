# MagTag Screen Timer — Product Overview

## Purpose
A screen-time countdown timer for a child, running on the Adafruit MagTag (2025, ESP32-S2). Tracks how much screen time remains each day, respects a holiday/weekend schedule stored in NVS, and visually counts down via an e-ink progress bar. The device is wall- or fridge-mounted (magnetic backing) so the child and parent can see remaining time at a glance.

---

## Hardware Target

| Attribute | Value |
|-----------|-------|
| Board | Adafruit MagTag 2025 |
| Platform.io ID | `adafruit_magtag29_esp32s2` |
| MCU | ESP32-S2 @ 240 MHz |
| Flash / PSRAM | 4 MB / 2 MB |
| Display | 2.9" grayscale e-ink, 296×128 px, SSD1680 controller |
| Buttons | 4× tactile (GPIO 15/14/12/11 (A/B/C/D), active-LOW, internal pull-up) |
| NeoPixels | 4× RGB on GPIO 1 (power gate: GPIO 21 LOW = on) |
| Speaker | Onboard amplifier, shutdown pin GPIO 16 (HIGH = on) |
| WiFi | 802.11 b/g/n, 2.4 GHz |
| RTC | None (dedicated RTC chip absent; ESP32-S2 RTC timer used for deep-sleep wakeup) |
| Battery | 3.7 V LiPo via JST connector |

---

## Features

### 1 · Clock & NTP

- Connect to WiFi and sync via SNTP (`pool.ntp.org`) in three situations:
  1. **Timer start/resume**: the countdown starts **immediately** on the
     current clock (instant user feedback); NTP sync runs right after. Any
     clock step the sync applies is measured against the monotonic clock and
     added to `expiry_wall_time` via `timer_shift_expiry()`, preserving the
     remaining duration exactly.
  2. **Every 10 minutes while running**: compensate for ESP32 RTC drift.
  3. **New day detected on wake**: NTP sync to get accurate date for schedule lookup.
- Routine NTP syncs correct only the ESP32 system clock. `expiry_wall_time` is
  **not** modified on sync — since `remaining = expiry_wall_time - time(NULL)`,
  drift compensation is automatic once the system clock is corrected. The one
  exception is the start/resume flow above, where the expiry was computed from
  a possibly-uncorrected clock and is shifted by the measured step.
- If WiFi is unavailable, the timer fails open: it keeps running on the
  uncorrected clock (remaining time stays a consistent duration; only the
  displayed wall-clock time may be off). Sync failure is signalled on the
  WiFi NeoPixel (red blinks) when `CONFIG_MAGTAG_SYNC_LED_FEEDBACK` is on.
- Timezone configured at compile time as a POSIX TZ string `#define` (e.g. `EST5EDT,M3.2.0,M11.1.0`).
- Display shows: date (e.g. `Sat May 16`) + current time (HH:MM AM/PM) and `Last sync: HH:MM`.

### 2 · Timer Architecture — Absolute Expiry Time

Rather than counting elapsed seconds, the timer stores the **absolute Unix timestamp at which it should expire** (`expiry_wall_time`). On every wake:

```
remaining = expiry_wall_time - time(NULL)
```

This makes the countdown inherently drift-resistant: NTP syncs correct `time(NULL)` via SNTP, so remaining time recalculates correctly without ever modifying `expiry_wall_time`. The only time `expiry_wall_time` changes is at timer start (`IDLE → RUNNING`) or resume after pause (`PAUSED → RUNNING`: `expiry_wall_time = time(NULL) + remaining_at_pause`).

Timer state and `expiry_wall_time` are stored in **RTC slow memory** (survives deep sleep) and additionally snapshotted to **NVS** on every state transition (XOR checksum + version + plausibility validation). After a panic, external reset, or power cycle the boot path restores the snapshot as long as its stored date is still today — so losing power does not refund the day's allocation. A power-on with no WiFi has no clock to date the snapshot against, so the device locks (a "No Clock" screen: check WiFi, press D to retry) and hands out no screen time until NTP succeeds, however long that takes. It retries every 30 min, or at once on Button D, and the wake that sets the clock restores today's snapshot. A timer that was running keeps counting down in wall-clock time through the outage and the lock, as through any power loss, so it may come back expired. Parent grants and bonus changes sent from HA meanwhile wait on the broker and land once the day is settled. The allocation resets only on a genuine day rollover.

### 3 · Deep Sleep Architecture

The device spends almost all of its time in deep sleep. Wake sources:

| Source | Action on wake |
|--------|----------------|
| RTC timer (planner-scheduled) | Partial display refresh + check NTP schedule |
| GPIO (wake-button press) | Handle button event, full refresh if needed |

Sleep durations come from a pure, host-tested planner (`main/sleep_plan.c`):
clock-only states (IDLE/PAUSED/EXPIRED) align wakes to wall-clock minute
boundaries (header time flips with real clocks); RUNNING/BREAK align to the
countdown's own minute grid so the displayed remaining truly reads round
values (start/resume shows one precise value, then 1:12:00, 1:11:00, ...).
RUNNING wakes ~20 s early when an NTP sync is due, and pre-event wakes land
~70 s before an expiry/break end so the awake watch loop fires the event on
time.

**Wake sequence (roughly once per minute)**:
1. Read current time from ESP32 RTC.
2. Check for day rollover (compare date to `last_date` in RTC memory) → if new day: wake WiFi, NTP sync, re-init timer to IDLE with new allocation.
3. If `next_ntp_sync_time` has passed (every 10 min while RUNNING): wake WiFi, NTP sync, adjust `expiry_wall_time`.
4. Compute `remaining = expiry_wall_time - now`.
5. Update display (partial refresh; full refresh on every 5th wake or state change).
6. If RUNNING with <=60 s remaining: stay awake (state pixel lit, clock-locking
   sync if due) and fire TIME'S UP within ~1 s of the expiry wall time.
7. Return to deep sleep.

Buttons are normally dispatched on EXT1 wake, which would make the device
deaf while it is awake. While awake, a GPIO negative-edge ISR latches every
press the moment it lands — even inside an e-ink flush or NTP sync — and a
release gate fed by pad-level samples makes sure the bounce of a button
coming back up is not counted as a second press. The awake checkpoints
consume the latch:

- Button B pauses from the render-grid wait and the final-minute event
  watch (cancelling the pending expiry), and any latched press dismisses
  the TIME'S UP / break alarms.
- The two latch drains — the one at the end of a tick wake and the short
  poll at the tail of a Screen Break — act on **one** latched press, chosen
  B > C > D > A (the time-sensitive action wins), through the same rules as
  a wake press. A, B and C always qualify. **D only in chore mode**, where it
  is the ✓3 button; outside chore mode a latched D is dropped, because its
  timer-screen job — a network sync — must not be bought by a press that
  merely rode in on another wake.
- On a chore wake, every tick pressed while the panel is waiting to paint
  (see 5c) is taken and applied, so several ticks land in one refresh; a
  tick pressed *during* that refresh is picked up after it and repainted.
- On any other button wake, further presses made while the first is being
  handled are treated as stale and dropped — except Button B, which still
  acts (outside chore mode) during the network sync that follows the
  paint.

What is **not** caught yet: the interrupts arm only once the button driver
initialises, well into boot. A second button pressed while the device is
still waking up from the first is never seen — deep sleep records only the
button that woke it. Until early arming lands, the cue that the device is
listening is the first press's LED changing.

The handlers detach at sleep entry before the pads move to the RTC mux;
unconsumed latches are plain RAM and evaporate in deep sleep. Held-button
logic (release wait, continuation guard) stays level-based.

WiFi is **off by default**; it is only powered up for NTP syncs and then immediately shut down.

**Low battery** (checked before every wake dispatch):
- **<= 15%**: the progress bar carries a `Charge Me!!!` badge; everything else keeps working.
- **<= 10% (charge lock)**: a RUNNING timer is paused (the allocation must not burn while the device is unusable), the panel is painted once with `Charge Me!` and then left alone (an e-ink refresh during brownout can leave persistent artifacts), buttons are dropped from the wake mask, and the device sleeps 10-minute intervals that only re-check the battery. The lock releases with hysteresis — only once the reading clears the warn band (> 15%) — and the next normal wake repaints the full layout.

### 4 · Daily Schedule & NVS Config

NVS namespace: `timer_cfg`

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `weekday_min` | u16 | 60 | Weekday allocation (minutes) |
| `weekend_min` | u16 | 120 | Weekend allocation (minutes) |
| `holiday_min` | u16 | 120 | Holiday allocation (minutes) |
| `summer_min` | u16 | 120 | Summer-break weekday allocation (minutes) |
| `chore_free_wd` / `_we` / `_hol` / `_sum` | u16 | 0 | Chore gate's free minutes per day type (see 5c); must not exceed the paired allocation |
| `chores` | blob | (none) | The chore list, up to 3 names — written only by the HA config document |
| `chore_ack` | blob | — | Today's chore ticks (device-owned state, not config) |
| `holidays` | blob | (pre-filled) | Newline-separated `YYYY-MM-DD` holiday dates |
| `wifi_ssid` | str | "" | WiFi SSID |
| `wifi_pass` | str | "" | WiFi password |

On first flash the NVS is initialised from `nvs_defaults.h` (holiday list, WiFi from `credentials.local.h`) with the allocation minutes coming from menuconfig (`MagTag Timer` menu → `CONFIG_MAGTAG_WEEKDAY/WEEKEND/HOLIDAY_MIN`). The stored stamp is a fingerprint of those values, so changing any allocation in menuconfig re-seeds NVS on the next boot — no erase needed. Subsequent boots with an unchanged fingerprint read the stored values.

**Day-type logic** (precedence: holiday > weekend > summer > weekday):
1. Check if today's date is in the `holidays` blob → holiday allocation.
2. Else if Saturday or Sunday → weekend allocation.
3. Else if outside the school year (`summer_start`/`school_start`/`school_end`; first-boot fallbacks `NVS_DEFAULT_SUMMER_START`/`SCHOOL_START`/`SCHOOL_END` in `nvs_defaults.h`, from the Dublin City Schools calendar) → summer allocation.
4. Else → weekday allocation.

The holiday list is the Dublin City Schools (Grizzell MS) 2026-27 calendar's
weekday no-school days, not generic federal holidays — days like Veterans
Day, when school is in session, are deliberately regular weekdays.

The `nvs_defaults.h` holidays and season dates are only what a device
starts with on first boot (or after a reseed). With the [config-publishing
automation](home_assistant.md#the-config-publishing-automation-chores-and-school-calendar)
installed, Home Assistant keeps all of them current from its school
calendar, so they need no yearly firmware update.

### 5 · Timer State Machine

States: `IDLE` → `RUNNING` → `PAUSED` → `EXPIRED`, plus `BREAK` (eye rest)

- `IDLE`: Allocation loaded for today, `expiry_wall_time` not set. Display shows full bar.
- `RUNNING`: `expiry_wall_time` set. Device deep sleeps between 55-second refresh wakes.
- `PAUSED`: `remaining_at_pause` saved in RTC memory; `expiry_wall_time` cleared. Deep sleep continues.
- `EXPIRED`: `remaining = 0`. Alert sequence runs on wake; device skips deep sleep until alert done or dismissed.
- `BREAK`: enforced eye-rest pause (see 5a). Screen time frozen like PAUSED; break end is an absolute wall time. Lives on the **Screen slot only**, and may be held there while a different timer is selected.

### 5a · Eye Rest (Screen Break)

Every `CONFIG_MAGTAG_BREAK_INTERVAL_MIN` minutes (default 30, 0 disables) of
**screen exposure** — see 5b; pauses don't reset it — a `BREAK` starts for
`CONFIG_MAGTAG_BREAK_DURATION_MIN` minutes (default 15).

The break enforces the **Screen timer**, not the whole device (v1.4). Screen
time stays frozen, there is no early resume, and the end is an absolute wall
time — but the other timers stay fully usable, which is the point of a break:
a kid on a 15 min eye rest can go and run Piano or Violin.

- Entry: whatever is RUNNING is paused, screen time frozen, balance reset,
  short break alarm (2 beeps × 3, any button silences), display flips to the
  **inverted** SCREEN BREAK layout with its own countdown + draining bar,
  plus a swap hint over Button C. The break can be earned entirely by a
  non-eligible extra timer, with Screen never started that day.
- During, with Screen selected: Button B is ignored — no early resume, and
  no reload either, since Screen is never reloadable; A switches to the
  chore checklist when one is configured (a BREAK is not RUNNING, so the
  mode toggle stays live right through a break); C and D work.
- During, with an extra timer selected: the normal layout for that timer,
  with an inverted `BREAK m:ss` chip in the header where `Last sync`
  normally sits. A **break-eligible** timer starts, pauses, expires and
  alerts as usual. A non-eligible one is fully visible and reachable by
  Button C, but Button B is refused and draws no ▶ — a chore is not a break.
- The swap hint on the break screen is suppressed when no break-eligible
  timer is configured: the break has nothing to offer, so it behaves like
  the older locking break.
- End: double-beep chime, and the selection returns to whichever timer the
  break interrupted, with Screen left `PAUSED` if it has banked time or
  `IDLE` if it never started today. Break end within ~1 s of wall time
  (stay-awake watch, same mechanism as expiry).
- **The chime is suppressed when any extra timer is RUNNING** at the moment
  the break ends — the kid is mid-activity and will get that timer's own
  alert. The selection is then left alone too (stealing it mid-run would be
  hostile); the chip simply disappears and Button C gets you back to Screen.
  `EXPIRED` counts as not-running, so a finished Piano still snaps back.
- **A late-observed end never chimes and never snaps.** If the transition is
  first seen more than 75 s after its wall time — a charge lock, a bed-time
  lock or a power cycle spanning it — it lands silently. The chime is an "it
  just happened" signal, not a replay.
- Break state, the balance and the interrupted slot persist in the NVS
  snapshot: a power cycle mid-break resumes the break with the same absolute
  end time, even when a different timer was selected and running.

### 5b · Screen exposure (v1.5)

Not every extra timer is a real break from a screen. "Laundry folding" is a
chore done *with the TV on*: running it during an eye rest defeats the
break, and running it outside one is real screen exposure the break
scheduler used to be blind to. So each extra timer declares whether it is a
genuine break activity (`MAGTAG_TIMER<n>_BREAK_ELIGIBLE`, or the per-timer
switch in Home Assistant).

Exposure is tracked as a **signed balance**:

- a **non-eligible** timer running (including Screen itself) adds to it 1:1;
- a **break-eligible** timer running subtracts from it 1:1;
- nothing running freezes it — idle neither adds nor drains;
- it never goes below zero, so hours of violin cannot bank hours of TV.

A break is due when the balance reaches the interval. So: fold laundry for
15 minutes, practise violin for 15, sit down to watch TV, and the break does
**not** fire immediately — the kid genuinely was off screens. Equally, fold
laundry for the whole interval without ever starting Screen and the break
fires anyway, because the eyes do not care which timer was selected.

The exposure balance is published to Home Assistant as the `Screen exposure`
diagnostic sensor; read against the configured break interval it answers
"why didn't my break fire?" directly. The panel deliberately does not show
it.

**Configure non-eligible timers with `RELOADABLE=n`.** One run of a chore
timer is capped by its own duration, which is the earned-by-the-chore
intent; but Button B reloads a reloadable timer once it has expired, so a
reloadable chore can be re-earned without doing the chore again.

A kid can of course leave Violin running without touching the violin. That
is unfixable in principle — the device cannot see the room — and it is the
same trust the Screen timer already assumes. It is also self-policing: at
1:1, dodging a 30-minute break costs 30 real minutes of not watching TV,
which is the outcome the break wanted. Do not harden it.

Pressing Start from PAUSED re-NTP-syncs and sets `expiry_wall_time = now + remaining_at_pause`.

All state is persisted in **RTC slow memory** (survives deep sleep) with an NVS snapshot as crash/power-loss backup (restored when still same-day; see section 2).

### 5c · Chore checklist

Up to **three** chores ("Dishes away", "Trash out", "Homework") gate part of
the day's Screen time. The list comes from Home Assistant — it is the
`chores` field of the bulk config document, and nowhere else
([home_assistant.md](home_assistant.md#chore-checklist-read-only-in-ha)).
In practice a parent edits a per-device HA To-do list, and the committed
automation `tools/ha/magtag_publish_config.yaml` publishes it; the
dashboard from `tools/gen_ha_dashboard.py` puts that list on the device's
tab ([home_assistant.md](home_assistant.md#dashboard)).
With no list configured the whole feature is inert: no mode, no gate, no
extra wakes, and Button A does nothing.

**The gate — a split allocation.** Each day type has a `chore_free` setting
(`chore_free_wd` / `_we` / `_hol` / `_sum`, in minutes) beside its
allocation. The day's first `chore_free` minutes of Screen time are
unconditional; the rest of the allocation is withheld until every chore is
ticked, and lands the moment the last one is:

| Day type | Allocation | `chore_free` | Effect |
|----------|-----------|--------------|--------|
| Weekday | 60 | 0 | Fully gated: no Screen time until the chores are done |
| Weekend | 120 | 30 | Cartoons first, chores for the rest |
| Summer | 120 | 120 | `chore_free` equal to the allocation: the gate is off that day type |

`chore_free` defaults to **0** on every day type, so pushing a chore list
fully gates every day until the parent sets them. Starting Screen on a
gated day starts only the free minutes; when they run out the timer
expires. Ticking the last chore releases the rest: an EXPIRED or PAUSED
Screen goes PAUSED holding it (press B), an IDLE one simply starts with the
full allocation, and during a Screen Break it is added to the frozen time
without moving the break's end. The gate touches **only** Screen: extra
timers, the exposure balance and break scheduling, Bed Time and the charge
lock all behave exactly as before.

On the main screen the bar shows the withheld amount as an outlined block
held at the **left**, labelled with the count and the minutes — e.g.
`0/3 Chores - 40 min` — while the free minutes drain to its right; on a
fully gated day the block is the whole bar (`0/3 Chores to unlock 60
min`). On release the block disappears and the bar goes full width. The
release is deliberately **not** shown in the `(±n min today)` parenthetical,
which belongs to the HA screen adjustment alone.

**Buttons — the mode.** Button A switches the panel between the timer
screen and the checklist (labelled `Chores` on the timer screen and
`Timers` on the checklist). It is refused while a timer is RUNNING — you
cannot tick off dishes while the TV clock runs — but works throughout a
Screen Break, which is when the break screen prompts for it (`Chores 2 of
3`). On the checklist:

```
┌──────────────────────────────────────────────────┐
│  CHORES                                  1 of 3  │
│               Screen time unlocked               │  ← only once released
│  ✓  Dishes away                                  │
│     Trash out                                    │
│     Homework                                     │
│  Timers      ✓ 1         ✓ 2         ✓ 3         │
└──────────────────────────────────────────────────┘
```

B, C and D tick chores 1, 2 and 3, and every tick **toggles**, so a
mis-press is undone by pressing the same button again. The rows never move;
with two chores there is no ✓3 and D does nothing. D is *not* a sync button
here — press A to get back to the timer screen for that. The checklist
stays up until you press A; ticking the last chore does not bounce you out
(the screen says `Screen time unlocked` instead). Only the day rollover, a
Screen Break starting or ending, the list being emptied, or a restart (the
mode is held in RTC memory only, so a power cycle, a panic or a firmware
update comes back on the timers) takes the panel back to the timers by
itself.

Unticking a chore **after** the release does not take the time back: the
release is latched for the day. Unticking before it simply re-arms the
gate.

**NeoPixels are the instant feedback.** The panel takes a second or more to
repaint, so each press is answered on the LEDs first. Each chore's pixel
sits over its button — red while outstanding, green once ticked — and the
pixel over A is the gate: red until the day is released (every chore
ticked), green from then on. It follows the release, not the minutes: on
a day type whose `chore_free` equals its allocation nothing is actually
withheld, yet the pixel stays red until the last chore is ticked.
A pixel over a button that has no chore stays dark. The strip lights only
when someone is pressing buttons; a wake nobody caused repaints the panel
and leaves the LEDs dark. The panel waits for a gesture to settle before it
paints (`MAGTAG_CHORE_PAINT_QUIET_MS`, default 1200 ms after the last tick,
within an overall `MAGTAG_CHORE_ACK_BURST_MS` budget, default 8000 ms), so
ticking two or three chores in a row costs one refresh. There is no sound.

**The day.** Ticks reset at the day rollover, together with the release,
and the panel reverts to the timers. They survive a power cycle, a panic
and a firmware update (stored in NVS alongside the RTC copy), so a kid
never redoes a chore because the device rebooted. Editing the list in HA
clears the day's ticks — they are positional — but never re-locks a day
that has already been released.

**Trust.** A kid can tick "Dishes away" without touching a dish; the device
cannot see the room, and this is the same trust the Screen and Violin
timers already assume. Home Assistant **mirrors** the ticks read-only — it
cannot tick or untick one — and its per-chore history is the parent's
record. The device is a ritual and a record, not an enforcer.

**The config-error lock.** `chore_free` greater than the allocation cannot
mean anything. The HA controls refuse or clamp it, so it takes one of two
other routes to store one: a hand-published bulk document, or a firmware
flash that changes the compiled-in defaults and so resets the allocations
while leaving `chore_free` alone
([home_assistant.md](home_assistant.md#chore_free-pairs-in-the-document)).
If the broken pair is **today's**, the device
pauses a running timer and locks on a **Config Error** screen naming the
pair (`Weekday: free 90 > 60 min`) and saying `Fix in Home Assistant, press
D`. Every button except D is dead. It sleeps 30-minute intervals, each
running a network window so the fix can arrive, and releases as soon as the
pair is valid — fix it in HA, then press D. That press only brings the
check forward: it does not also tick chore 3 or run D's sync, and presses
made while Config Error was showing are dropped. A broken pair for another day
type does not lock today; HA's *Config warning* sensor names it. This is
the device's third lock, beside the charge lock (section 3) and Bed Time,
and both of those outrank it.

### 6 · Buttons

| Button | GPIO | Action |
|--------|------|--------|
| A | 15 | **Mode toggle**: switches the panel between the timer screen and the chore checklist (5c) — labelled `Chores` on the timer screen and `Timers` on the checklist. Refused — and not a wake source, and no label — while the active slot is RUNNING or when no chore list is configured |
| B | 14 | Start (IDLE/PAUSED → RUNNING, immediate; NTP sync after) / Pause (RUNNING → PAUSED) / Resume. During a Screen Break, a **start** is refused on any slot that is not break-eligible — including Screen — and the ▶ label is not drawn (see 5a/5b); pausing is never gated. On an **EXPIRED** slot, where B has no start or pause job left that day, B instead **reloads** the timer to full duration when the slot is reloadable; Screen has no def and is never reloadable. **On the checklist:** ticks / unticks chore 1 |
| C | 12 | Swap timer type (Screen → extra 1 → … → Screen); refused while RUNNING (a Screen Break does **not** refuse — see 5a). **On the checklist:** ticks / unticks chore 2 |
| D | 11 | Force NTP re-sync + full display refresh. **On the checklist:** ticks / unticks chore 3 instead, with no sync. **Under the config-error lock** (5c) the only live button, and it only brings the lock's check forward — neither of the above |

Wake sources: B and D always (outside the locks); A and C only when their press would succeed, since the EXT1 mask is rebuilt at every sleep entry and a press that could only be refused must not burn battery or a panel refresh. C: extra timers configured AND the active timer not RUNNING — a Screen Break leaves C live, so the mask keeps it as a wake source throughout. A: a chore list configured AND the active slot not RUNNING — a Screen Break leaves A live too, which is what makes the checklist reachable during a break. **A's two gates are deliberately not C's**: the mode toggle does not inherit C's "extra timers must exist" condition, so a device with no extra timers still reaches its chore list. On a device that has never had a chore list pushed to it — which is every device until Home Assistant sends one — A never wakes at all. On the checklist C is also a wake source whenever there is a second chore for it to tick. The locks narrow all of this: the charge and Bed Time locks arm no button at all, and the config-error lock (5c) arms D alone. Buttons are debounced in software (10 ms).

B is armed unconditionally even though a handful of states refuse it — an expired slot that is not reloadable, or Screen during a break. That is a deliberate overshoot of the "a press that could only be refused must not wake" rule, because the failure is asymmetric: arming B when it would do nothing costs a single wake, while failing to arm it when it *would* have acted makes the device's primary control dead to the press, with no feedback to tell that apart from a flat battery. C can be gated safely because a refused swap has a visible alternative.

### 6a · Extra timers (v1.3)

Up to four additional countdown timers (menuconfig: `MAGTAG_TIMER<n>_NAME/_MIN/_RELOADABLE/_BREAK_ELIGIBLE`; an empty name disables the slot) for things like Piano practice or Laundry folding. They are plain countdowns sharing the Screen timer's alerts, NeoPixel sequences, NTP cadence, and RTC + NVS-snapshot persistence, but:

- No eye-rest breaks of *their own* — the break always belongs to the Screen slot — but a **non-eligible** timer feeds the shared screen-exposure balance (5b) and so can earn one, and a **break-eligible** timer stays usable during a break and drains the balance, which is what the break time is for (see 5a).
- Fixed configured duration instead of the day-schedule allocation.
- **Reloadable** timers reload to full via Button B on the same day, but only **once they have EXPIRED** — that is where B has no start/pause/resume job to do. The mode line then counts the day's completed runs (reached 00:00): `Meditation (x2) - 10 min`. Non-reloadable timers never show a counter — once expired they stay depleted until rollover.

  There is no longer any way to reset a timer *before* it expires. Earlier firmware let Button B reset from any non-running state, including PAUSED; B now resumes a paused timer instead, and since a paused timer never expires on its own, a part-finished run cannot be restarted from the device. The `(x2)` counter is unaffected — its flow is expire-then-reload — and the parent-facing screen-time adjustment in Home Assistant remains the way to hand back time directly.
- **Break-eligible** timers are genuine time away from a screen (see 5b). Configure chore timers non-eligible *and* `RELOADABLE=n`.
- Day rollover resets every timer, clears the counters, and reverts the selection to Screen.

Only the selected timer can be RUNNING — swapping requires a pause, so pause/expiry state of a deselected timer is frozen until you swap back. The one state that is *not* tied to the selection is `BREAK`: it belongs to the Screen slot and keeps counting down whichever timer you are looking at.

### 7 · Display Layout (296×128 px)

```
┌──────────────────────────────────────────────────┐
│  Sat May 16  12:34 PM       Last sync: 12:30 PM  │  ← row 0–18
│                                                  │
│  ████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░  │  ← row 26–50 (progress bar, 24 px tall)
│                                                  │
│  ▮85%                       00:42:30             │  ← row 58–78 (battery left, remaining right)
│                                                  │
│  Weekday · 60 min                    RUNNING     │  ← status row (moved up)
│              ⏸                      ⟳            │  ← button labels (_ B _ D)
└──────────────────────────────────────────────────┘
```

While a Screen Break runs behind another selected timer, the header's
`Last sync` is replaced by an inverted **BREAK** chip (16 pt, rows 3–21 —
inside the header's clean band, so no other widget moves):

```
┌──────────────────────────────────────────────────┐
│  Sat May 16  12:34 PM        ██ BREAK 12:34 ██   │  ← chip instead of Last sync
│  ████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░  │
│  ▮85%                       00:07:30             │
│  Piano - 10 min                      RUNNING     │
│              ⏸                      ⟳            │  ← C unlabelled: swap refused while RUNNING
└──────────────────────────────────────────────────┘
```

The break screen itself (Screen selected) carries a bottom row instead of
its old centred footer whenever extra timers or a chore list are
configured — the frozen screen time on the left, the swap affordance over
C, refresh over D. With a chore list, cell A carries `Chores` instead (the
mode toggle stays live through a break), and the frozen screen time moves
up to a line of its own beside the chore prompt, `Chores 2 of 3` (a tick
once all are done). B is never labelled there: the break is still
enforced, a press would be refused, so the panel does not offer it.

```
┌──────────────────────────────────────────────────┐
│               SCREEN BREAK                       │  ← 28 pt, white on black
│  ████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░  │  ← break bar
│                  12:34                           │  ← 48 pt break countdown
│  Screen 1:30            ▶| Piano         ⟳       │  ← 16 pt bottom row
└──────────────────────────────────────────────────┘
```

With neither extra timers nor a chore list configured there is nothing to
swap to and no checklist to open, so the break screen keeps its original
centred `Timer paused - 1:30:00 left` footer and no button row.

Button labels sit above the physical buttons. A shows `Chores` when the
mode toggle would be honoured (a chore list configured and nothing
RUNNING) and is blank otherwise. B shows the single action its press will take: play when
IDLE/PAUSED (and only when a start would be allowed), pause when RUNNING,
"Reload" when the slot is EXPIRED *and* reloadable, and nothing at all
otherwise. C shows a swap arrow when extra timers are configured and the
state allows swapping, D is the sync/refresh symbol.

Because one cell carries all of B's actions, the label is decided by a
single rule rather than by each drawing site, so the panel cannot offer
something the press would refuse. Note "Reload" appears only on an
EXPIRED slot: after you press it the timer returns to IDLE at full
duration and the label goes back to play, even though the slot is still
reloadable.
When an extra timer is selected, the bottom-left mode line shows its name,
completion counter, and duration (e.g. `Meditation (x2) · 10 min`) instead
of the day-type + allocation.

- **Progress bar**: full-width (280 px usable), fill proportional to `remaining/allocation`. Thick outer border.
- **Remaining time**: centred; always `HH:MM:SS`.
- **State label**: bottom-right (`IDLE`, `RUNNING`, `PAUSED`, `TIME'S UP`).
- **Day-type + allocation**: bottom-left (e.g. `Holiday · 120 min`).

**Refresh strategy**:
- Partial refresh on every ~55-second wake while RUNNING.
- Full refresh on state transitions and every 5th partial refresh (prevents ghosting).
- Full refresh ~3 s; partial ~0.4 s — both acceptable at the 55-second cadence.

### 8 · Expiry Behaviour

When `remaining ≤ 0`:
1. **Display**: full refresh to "TIME'S UP" full-screen layout + empty bar.
2. **Speaker**: 3 short beeps, repeated every 3 seconds, for 5 cycles (15 seconds total). Stops immediately on any button press.
3. **NeoPixels**: slow red pulse during the 15-second alert. Stops on any button press.
4. After alert completes (or is dismissed), device returns to EXPIRED state and resumes deep sleep.

### 9 · Power

- Primary: USB-C.
- Battery: LiPo fallback. Deep sleep and WiFi-off discipline makes battery life viable.
- NeoPixels and speaker amplifier are powered off except during alert.
- Deep sleep current target: < 1 mA (ESP32-S2 deep sleep ~20 µA + display static current).

---

## Build System

**Recommended**: Platform.io with ESP-IDF framework.

```ini
; platformio.ini
[env:magtag]
platform = espressif32
board = adafruit_magtag29_esp32s2
framework = espidf
monitor_speed = 115200
```

Alternatively, pure ESP-IDF v5.x with `idf.py set-target esp32s2` is fully supported.

---

## Module Structure

Abridged. The full module map and the three-layer model live in
[architecture.md](architecture.md), which is authoritative.

```
main/
  main.c            — composition root: boot ordering, wiring, deep-sleep entry; no decisions
  wake_flow.c       — the wake orchestration: wake-cause decode, both wake handlers,
                      button guards, the event watches, the break-end owner
  lock_gate.c       — the four screen locks (low battery, Bed Time, config error, no clock)
  chores.c          — the chore model: ack toggles, list identity, the gate arithmetic
  chore_store.c     — the chore list and today's ticks in NVS
  display.c/h       — SSD1680 SPI driver; layout rendering; partial vs full refresh logic
  timer.c/h         — state machine; expiry time calculation; RTC memory persistence
  ntp.c/h           — SNTP sync inside a network window (WiFi lifecycle is wifi_session.c)
  nvs_config.c/h    — typed NVS accessors; first-boot defaults init
  schedule.c/h      — day-type determination (weekday/weekend/holiday); allocation lookup
  buttons.c/h       — wake reason decode; GPIO wakeup config; debounce
  audio.c/h         — DAC playback (dac_continuous on CH0/GPIO 17); tones.c renders the audio
  neopixel.c/h      — RMT-based NeoPixel driver; alert pulse pattern
  nvs_defaults.h    — compile-time default holiday list, allocations, WiFi placeholder

components/
  ssd1680/          — standalone SSD1680 e-ink SPI driver component
```

**RTC slow memory layout** (persistent across deep sleep) — the v1 single-timer
shape, kept here as the original design record; the authoritative per-slot
definition is `include/timer.h` (see architecture.md):

```c
typedef struct {
    timer_state_t state;             // IDLE / RUNNING / PAUSED / EXPIRED
    int64_t       expiry_wall_time;  // Unix timestamp when timer expires; 0 if not set
    int32_t       remaining_at_pause; // seconds saved on PAUSE
    int32_t       allocation_sec;    // today's allocation in seconds
    char          last_date[11];     // "YYYY-MM-DD" for day-rollover detection
    int64_t       next_ntp_sync;     // Unix timestamp of next required NTP sync
    uint8_t       partial_refresh_count; // resets to 0 after full refresh
} rtc_state_t;
```

---

## Out of Scope (v1)

- OTA firmware updates
- Remote monitoring or companion app
- Multiple child profiles
- SD card usage
- BLE/SmartConfig WiFi provisioning (credentials stored in NVS; set initially via `nvs_gen.py` partition image or a `#warning` placeholder in `nvs_defaults.h`)
- Adjustable timer allocation via buttons (schedule-driven only)

---

## Implementation Notes for Coding Agents

> **Superseded — read as the original v1 intent, not as guidance.** Several of
> these were overtaken during the build: the display stack is LVGL 9 over the
> custom `components/ssd1680` driver (not LovyanGFX, and there is no C++ TU),
> and the IDF 6 sleep/SNTP APIs below have been renamed. For what the firmware
> actually does, use [architecture.md](architecture.md).

1. **TDD required**: write unit tests for `schedule.c` (day-type logic), `timer.c` (state machine + expiry math), and `nvs_config.c` (serialisation round-trips) before implementing those modules.
2. **Worktrees/branches**: all development on feature branches; never commit directly to main.
3. **Display library**: LovyanGFX (ESP-IDF native). `display.cpp` is the single C++ translation unit; all other modules are C. `display.h` exposes a C-compatible API with `extern "C"` guards. LovyanGFX handles SSD1680 init, partial/full refresh, and font rendering.
4. **SNTP**: use the `esp_sntp` component with `CONFIG_SNTP_TIME_SYNC_METHOD_IMMED`; confirm sync via `sntp_get_sync_status()` before setting `expiry_wall_time`.
5. **Deep sleep wakeup**: `esp_sleep_enable_timer_wakeup(55 * 1000000ULL)` + `esp_sleep_enable_gpio_wakeup()` for all 4 buttons; use `esp_sleep_get_wakeup_cause()` on wake to dispatch correctly.
6. **RTC memory**: declare `rtc_state_t` with `RTC_DATA_ATTR` so the linker places it in RTC slow memory.
7. **WiFi lifecycle**: init → connect → sync → disconnect → deinit on every NTP session; never leave WiFi running between syncs.
