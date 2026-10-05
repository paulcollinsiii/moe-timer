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
screen, the break screen or the chore checklist. The lock, TIME'S UP, update
and setup screens ([below](#setup-screens)) are full-screen takeovers with
painters of their own. Every
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

## Setup screens

`display_screens.c` also builds the setup-mode screens, each a full-screen
takeover with a full refresh: the setup screen (QR code, AP name, user,
password, form URL), `Release to enter setup`, and the end screens
(`Setup complete` for a WiFi save or an MQTT save, `Setup timed out`, `Setup
failed`). `setup_screens.c` is the thin adapter that wires them to the session's
ops. The end screens' retry line is worded for the build and the state: no
SSID, `Press any button to retry`; with an SSID, the BOOT hold, in the
hold-then-press form (`Hold BOOT, press a button, hold N s`) on a build without
`MAGTAG_BOOT_WAKES`. The main screen's header also takes an optional
`status_hint` string (`display_state_t`), drawn in the Last-sync slot for the
setup hint. Its default is NULL.

**The QR is drawn as rectangle runs.** `qr_render.c` encodes the payload with the
vendored Nayuki `qrcodegen` (`lib/qrcodegen/`) into a module matrix, and a
`LV_EVENT_DRAW_MAIN` handler on a plain object paints each horizontal run of dark
modules with one `lv_draw_rect`, at 2 px per module. `LV_USE_QRCODE` stays off,
and the reason is the ARGB8888 trap: that widget draws through an indexed-image
canvas, LVGL's decoder turns the indexed image into ARGB8888 before the blit, and
the ARGB8888-to-I1 blend sits behind `LV_DRAW_SW_SUPPORT_ARGB8888`, which
`sdkconfig.defaults` turns off. With `LV_USE_LOG` off that fails silently: the QR
draws nothing, on the device and on the host. Rectangle fills on I1 are the path
the bars already use. The real payload has a fixed length (derived in `setup_session.h` beside
`SETUP_SESSION_QR_MAX`) and encodes as QR version 6; up to
`QR_RENDER_MAX_VERSION` still fits the 128 px height, and a longer payload falls
back to text.

That is also why `sdkconfig.defaults` trims LVGL to the label and bar widgets
and the I1 draw path only, mirrored in `test/mocks/lv_conf_host.h`. Re-enabling
any of it is a flash cost and needs the host config changed with it.

## Golden tests

`test_display_render` compiles the real LVGL for the host
(`test/mocks/lv_conf_host.h`), builds each screen and compares it byte for
byte with a file in `test/test_display_render/golden/`. The goldens are the
layout contract. Regenerate them with `MAGTAG_WRITE_GOLDEN=1` only for an
intentional layout change; the flag rewrites every golden, so check out the
ones you did not mean to change. The layout arithmetic and the refresh
cadence are tested separately in `test_display`.
