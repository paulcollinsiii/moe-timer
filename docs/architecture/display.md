# Display

How a frame gets from LVGL to the e-ink glass, who decides between a partial
and a full refresh, and what the panel driver protects on its own. Read it
before changing a screen, the refresh cadence or the driver.

## The render stack

```
display_screens.c   builds the LVGL widgets for one screen (pure, no ESP-IDF)
       │
LVGL 9              renders the whole 296×128 frame, 1 bit per pixel
       │
display.c           flush: rotate to the panel's 128×296, ghost-clean pass, refresh
       │
components/ssd1680  SPI writes, refresh, BUSY wait, rate guard, panel sleep
```

`display_screen_for()` picks which of three layouts to build: the timer
screen, the break screen or the chore checklist. The lock, TIME'S UP and
update screens are full-screen takeovers with painters of their own. Every
paint renders synchronously, so the main task is blocked until the panel is
done. After each flush `display.c` puts the panel into deep sleep with its RAM
kept, and wakes it before the next one.

## Partial or full

Two layers decide, and they stay separate on purpose: the firmware above the
driver (`wake_policy`, `lock_gate`, `display_layout`) holds the policy, and
the driver holds the protection.

**Policy.** `wake_policy_render()` asks for a full refresh when the timer
state changed or a break ended; `lock_gate_promote_render()` asks for one
after a lock released, since a lock screen is still on the glass. Every
other paint goes through the cadence in `display_refresh_plan()`: every fifth
paint is full, and the counter lives in RTC memory so the cadence runs
across sleeps. After the update screen, the next paint is full too.

**Protection.** The SSD1680 driver enforces three rules whatever the policy
asks (S25, S26):
- no refresh within 1 s of the last one, timed across deep sleep;
- one refresh at a time, waiting on BUSY (10 s timeout);
- a partial with no valid previous frame in controller RAM, such as after a
  power-on or an OTA reboot, is promoted to full, because a partial diffs
  against that RAM.

## Ghost cleaning

Every partial `display.c` flushes is two passes. The first writes the new
frame with each changed column-band segment inverted, and the second writes
the true frame. A changed pixel is driven once, as in a single partial; the
unchanged pixels around it are driven away and back, which clears the
residue a run of partials leaves. Pass two starts as soon as BUSY releases.
The cost on the glass is estimated at about 0.8 s, not measured. The pass is
skipped when nothing changed and when there is no valid previous frame.

## Golden tests

`test_display_render` compiles the real LVGL for the host
(`test/mocks/lv_conf_host.h`), builds each screen and compares it byte for
byte with a file in `test/test_display_render/golden/`. The goldens are the
layout contract. Regenerate them with `MAGTAG_WRITE_GOLDEN=1` only for an
intentional layout change; the flag rewrites every golden, so check out the
ones you did not mean to change. The layout arithmetic and the refresh
cadence are tested separately in `test_display`.
