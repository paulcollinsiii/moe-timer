# Peripherals

The buttons, the NeoPixels, the speaker and the two analog readings, seen as
drivers: how each is read or driven, asleep and awake, and which module owns
it. Pins are in [hardware.md](hardware.md); what each button or light means to
the family is in the behavior docs, starting from
[product_overview.md](../product_overview.md#the-device-at-a-glance).

## Buttons

A button is read two different ways, depending on whether the chip is asleep.

**Asleep: EXT1 wake.** At sleep entry `buttons_configure_wakeup_if()` hands
the armed pads to the RTC domain with pull-ups and enables EXT1 `ANY_LOW`.
Which buttons are armed is the pure `buttons_policy_wake_mask()`: a press
that could only be refused does not get to wake the device (S21). A pad that
is not armed is left unconfigured, because a pull-up on it would leak about
70 µA for as long as the button is held. On the wake, `buttons_get_wakeup_button()`
reads the EXT1 status latch. If the latch is empty it debounces for 10 ms and
scans the pad levels, limited to buttons some gate could ever arm, and breaks
a tie between held pads with the same pick order as the awake latch.
`esp_sleep_enable_gpio_wakeup()` is not an alternative: on the ESP32-S2 it
works only for light sleep.

**BOOT (GPIO0).** A fifth EXT1 pad, folded into the same mask but kept out of
`button_id_t`: it has no A–D action, only the hold-for-setup gesture
(`setup_trigger.h`, [wake_cycle.md](wake_cycle.md#setup-mode)).
`buttons_init()` always configures it as a pulled-up input, so
`buttons_is_boot_pressed()` works even where it cannot wake the device.

*Arming* is a second pure decision, `buttons_policy_boot_wake_allowed()`, and
`MAGTAG_BOOT_WAKES` (on by default) gates the call. BOOT is armed on every
sleep that arms buttons, except under the config-error lock, so that lock's one
exit stays D (S21). It is not armed when GPIO0 already reads low at sleep entry:
EXT1 is level-triggered, so a held pad would wake the device the instant it
reached deep sleep, and again on every re-wake. The continuation guard that
covers a held A–D button does not track GPIO0.

*Decoding.* `buttons_woke_by_boot()` requires the EXT1 cause and GPIO0's bit in
the status latch, and only once no A–D button already explained the wake
(`button_latch_boot_wins()`). An empty or absent latch means "not BOOT", with no
level-scan fallback, because there is no second button to disambiguate against.
A BOOT-only wake decodes through `buttons_get_wakeup_button()` as `BTN_NONE`
without the A–D scan, so a merely held A–D pad is never blamed for it.

*The strapping caveat.* GPIO0 selects the ROM boot mode at reset. The design
assumes it is sampled at power-on and other chip resets but not on a deep-sleep
wake, so holding BOOT across a wake does not enter download mode. That is
unconfirmed on the board ([hardware_checklist.md](../hardware_checklist.md)).
Download mode depends on GPIO0's level at the reset, not on which pad caused
the wake. So if a board does enter it, every BOOT-held wake does, including the
held BOOT plus an A–D press that `MAGTAG_BOOT_WAKES` off leaves as the only
route, and turning the symbol off is not a fix. With it off nothing arms GPIO0
for sleep, but the setup gesture (a wake with `buttons_is_boot_pressed()` true
at entry) still needs GPIO0 low across the wake reset, and the awake waits poll
only the A–D latch, so no gesture works without that. A device with no stored
WiFi is unaffected, since any press enters setup. This check gates the feature.

**Awake: the press latch.** EXT1 is only a wake source, so without help the
device would be deaf while awake: during a sync, the render-grid wait or an
e-ink flush. `buttons_init()` installs a falling-edge ISR that records each
press into `button_latch.c`, and the wake's checkpoints consume the latch
later:
- A falling edge counts as a press only after the button was seen released,
  so the bounce of a release is never a second press (S23).
- After a press is accepted, edges on **the same button** are ignored for
  `BUTTON_LATCH_REFRACTORY_US` (300 ms); other buttons are unaffected. This
  makes one physical tap one press: a quick tap's release bounce can land
  after a level sample has reopened the gate. The wake press never passes
  through the latch, so `buttons_watch_begin()` seeds its window
  (`button_latch_begin()`) from the EXT1 status. A deliberate re-press of the same button faster than that is
  lost, by design.
- The window, and the release **settle** (the short anchor on the moment a
  level sample first sees the pad up, default `BUTTON_LATCH_RELEASE_SETTLE_US`,
  10 ms), are **per button**. Every button defaults to 300 ms and 10 ms, and
  `buttons_set_refractory()` and `buttons_set_settle()` change one button's. The
  only caller is Button C's counted-clicks gesture: while it runs C uses
  `BUTTON_LATCH_SWAP_REFRACTORY_US` (120 ms) so a person can tap at 150-250 ms,
  and `BUTTON_LATCH_SWAP_SETTLE_US` (30 ms), because a tap held past 120 ms has
  left the window behind by the time it is released and its release bounce is
  then judged by the settle alone. Both are put back when the gesture ends. In
  Chores mode C is the second chore tick and keeps 300 ms and 10 ms. The
  figures in force are stamped when a press is accepted, so a change never
  reaches into a press already counted. The one exception is the seeded wake
  press: it starts with the defaults, and when the gesture opens it restamps
  that press with 120 ms and 30 ms (`buttons_restamp_running()`), so the tap
  right after it is not refused by 300 ms. Its own release bounce is still
  refused the way every counted tap's is: a pad held at arming keeps the gate
  shut until a sample sees it up and the 30 ms settle runs from that sample,
  and a pad already up at arming was released before it, with the window and
  settle both running from arming. The price of the longer settle is that the shortest
  release-to-press gap that still counts becomes the 20 ms poll plus 30 ms,
  against 20 plus 10. All four figures are starting values, to be settled on
  the bench.
- When several presses are waiting, `button_latch_pick()` takes them in the
  order B > C > D > A, so the time-sensitive action wins and a mode toggle
  can never swallow a start (S22). That order is the only guard against the
  swallow.
- A press made before `buttons_init()` runs, early in boot, is not caught: a
  second button pressed while the first is still waking the device is lost.

The ISR handlers are detached before the pads move to the RTC mux at sleep
entry. Holding a button (the release wait, the 2 s continuation guard) is
judged on pad levels, not on the latch. `buttons.c` has no host suite; its
decisions are in the host-tested `buttons_policy` and `button_latch`.

## NeoPixels

Four WS2812 pixels on GPIO 1, behind a power gate on GPIO 21. `neopixel.c`
drives them through the RMT peripheral from its own LED task (`np_led`),
which other code reaches through a queue. That task is the only owner of
both the RMT channel and the gate.

The gate is the one hard rule: it goes high (LEDs off) in the first
peripheral call of every boot (S27), and before sleep `neopixel_stop_sync()`
waits for the task to confirm the LEDs are dark, or forces the gate high
itself after 500 ms. The pin is then held through the sleep (S42).

Who may light which pixel depends on the mode, and `include/status_led.h`
is the authority: on the timer screen pixel 0 shows the timer state and
pixel 3 the network window; on the chore checklist all four belong to the
chores. Quiet hours and the status brightness are applied inside the driver.

A sixth claimant takes all four for a moment: Button C's count display. While
the counted-clicks gesture runs, the strip shows the candidate timer's place in
binary, least significant bit on the leftmost pixel (driver index 3, because the
strip runs opposite the buttons). The bit-to-pixel mapping is
`neopixel_binary4_pixel_lit()`, the same one the last-15-seconds countdown
draws with, so the two read alike. The count is magenta (25, 0, 25), the one
colour nothing else on the strip uses, and it is HIGHPRI like the chore strip:
the pixels are the only feedback until the panel paints, so quiet hours do not
mute it (the brightness setting still applies). When the gesture ends,
`wake_flow_swap_count_gesture()` wipes the strip (`neopixel_stop()`) and repaints
the state pixel through the one status wrapper. `scripts/check-status-led-wrapper.py`
checks the shape of that release: after the last count frame, the wipe and then
the wrapper are unconditional statements of that function, in that order, with
no `return` before them. It does not follow a `goto` or prove that every path
reaches them; the host test pins the behaviour. Nothing can compete for the strip meanwhile: the gesture only
opens when the selected timer is not running, so there is no countdown or expiry
to run, and no network window opens inside it.

## Audio

Every alert sound goes to the speaker through the ESP32-S2 DAC, channel 0 on
GPIO 17, which `audio.c` feeds by continuous DMA. Most tones are synthesized:
`tones.c` holds the note tables and renders an enveloped sine in chunks
(pure, host-tested). The "Custom WAV" choice streams 16-bit mono PCM at
8–22.05 kHz from the `assets` partition, after `wav_header.c` checks the
header; a missing or invalid file falls back to the chime. Volume is applied
as digital gain, clipped at the DAC rails above 100 %.

The amplifier is enabled on GPIO 16 only while sound plays, and the driver
initializes lazily on first use, so a silent wake never powers it. Alarms
themselves (pattern, LED pulse, dismissal by any press) are run by
`alerts.c`, which plays audio from a task of its own.

## Battery and light

Both readings use one-shot ADC1 conversions on a unit `battery.c` creates
and `light.c` shares.
- **Battery** (`battery.c`): GPIO 4 through a 100k/100k divider, calibrated
  to millivolts. `battery_soc.c` converts that to a percentage on a LiPo
  curve. It is read on every wake by the charge gate, and again before an
  OTA download.
- **Light** (`light.c`): an ALS-PT19 on GPIO 3, reported as raw millivolts for
  HA trend graphs, not lux. It is read only when a stats snapshot is built.
