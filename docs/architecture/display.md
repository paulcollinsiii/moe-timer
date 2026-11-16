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
other paint goes through the cadence: every Nth paint is full
(`CONFIG_MAGTAG_FULL_REFRESH_EVERY_N`, default 10, range 1 to 100, build-time
only), and the counter lives in RTC memory (`display.c`) so the cadence runs
across sleeps. A forced full restarts the count. After the update screen, the
next paint is full too. Which of these a paint does to the counter and the
update-screen flag is one host-tested function, `display_refresh_decide()` in
`display_layout.c`; `display.c` only stores the two bytes and renders the
answer.

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
takeover with a full refresh: the setup screen (QR code, `Scan to join, then
open`, the address, AP name, password), `Release to enter setup`, and the end screens
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
modules with one `lv_draw_rect`, at 3 px per module. `LV_USE_QRCODE` stays off,
and the reason is the ARGB8888 trap: that widget draws through an indexed-image
canvas, LVGL's decoder turns the indexed image into ARGB8888 before the blit, and
the ARGB8888-to-I1 blend sits behind `LV_DRAW_SW_SUPPORT_ARGB8888`, which
`sdkconfig.defaults` turns off. With `LV_USE_LOG` off that fails silently: the QR
draws nothing, on the device and on the host. Rectangle fills on I1 are the path
the bars already use. The real payload has a fixed length (derived in `setup_session.h` beside
`SETUP_SESSION_QR_MAX`) and encodes as QR version 3, 29 modules, an 87 px code
in a 111 px block with the 4-module quiet zone; 3 px is the most a 128 px panel
allows. `QR_RENDER_MAX_VERSION` is 3 to match, and a longer payload falls back
to the text lines.

**The four lock-screen art slots (charge, time's up, break, bed time) are drawn
the same way.**
`tools/sprites/charge.png` (148x124, 1-bit), `timesup.png` (187x126, 1-bit),
`break.png` (46x52, 1-bit) and `bedtime.png` (32x25, 1-bit) are the public
templates, each padded to its slot's maximum size (the largest box its screen
leaves for art); a private copy in `sprites.local/`, up to that size, can
replace any of them at build time, and a smaller one is padded on the slot's
grow sides (see [Lock-screen art](../developer_setup.md#lock-screen-art)).
`tools/gen_sprites.py` run-length encodes them into the committed
`main/sprite_data.c` (about 0.8 KB, 1.0 KB, 0.2 KB and 0.07 KB of flash),
`uv run tools/gen_sprites.py --check` fails when the two disagree, and
`test_gen_sprites` round-trips each template PNG and compares it with its screen golden.
`sprite.c` walks the runs with no LVGL and no scratch buffer; a draw handler in
`display_screens.c` paints each ink run with one `lv_draw_rect`, as the QR does.
The handler takes an integer scale and the ink colour per object: the charge
and time's-up art draw at 1x in black, the break screen's art at 2x (each art
pixel a 2x2 block, 92x104 on the panel) in white, because that screen is
inverted, and the bed-time screen's art at 4x (128x100) in white for the same reason. The
bed-time screen is a full refresh only, so it needs no `CLEAN_BANDS` entry.
The break screen partial-refreshes in place, but its layout needs no band of its
own in `CLEAN_BANDS`: the four bands jointly cover every byte of every row, the
art is static, and a host test maps each changing element to its band.
Edit the PNG and regenerate; never edit the generated C. The time's-up art is
placed flush with the bottom edge and its ink may run off it (the template's
alarm clock is cut off there on purpose), the one place ink may touch a panel
edge; a host test pins the exact count for the template.

**A slot can hold several pictures** (`<slot>.png`, `<slot>.1.png`, ...; the
file rules are in [Lock-screen art](../developer_setup.md#lock-screen-art)).
The generator emits, per slot, `SPRITE_<SLOT>` (variant 0), a table
`SPRITE_<SLOT>_VARIANTS[]` and `SPRITE_<SLOT>_VARIANT_COUNT`. The count is a
`const` the C defines, not a macro, so an override that adds a variant changes
the C and never the header. The count is also not a constant expression, so it
cannot size an array or sit in a `_Static_assert`; the header says so. Which
variant shows is decided per **lock episode**, the stretch during which one
lock (time's up, charge, bed time, a screen break) stays in force:

- `display.c` is the one place every lock paint goes through, so it holds the
  pick: a small `RTC_DATA_ATTR` state (`sprite_pick_t`, one byte per lock), and
  the draw from `esp_random()`. `sprite_pick.c` is the pure logic over that
  state and an injected random source, host-tested in `test_sprite_pick`.
- Each lock has its own pick, so an open episode is not disturbed by a paint of
  another lock. An episode **starts at the lock's first paint**, which draws
  and stores the variant, and **ends only when the lock's owner says so**, via
  `display_lock_art_end()`: `lock_gate.c` on the charge release and on both
  bed-time releases, `wake_flow.c` when a break ends (and when a new break
  starts, so a break the day rollover wiped cannot leave its pick behind), and
  `wake_flow.c` again once the time's-up screen has been replaced, so each
  expiry alert is its own episode. Painting never ends one. Repaints, chores
  detours, timer switches, the paint after each deep sleep and a network window
  that repaints in between all show the same art. The other takeovers (OTA,
  setup, sync failed, no clock, config error) neither start nor end an episode.
- The one exception is an `esp_restart` (an OTA commit, a panic): RTC data is
  zeroed, the episode has no pick, and the next paint of the lock draws again.
- The state is not in `rtc_state_t`: it describes what is on the panel, like
  the refresh counter beside it, and `timer_reset` would wipe it at midnight. It
  needs no version, because `sprite_pick()` checks it on every call. RTC data is
  zeroed by `esp_restart` (an OTA reboot), so the next lock simply draws, and an
  OTA failsafe sleep can leave another image's bytes there. A stored variant at
  or above the current count, from a firmware that shipped more pictures, is
  redrawn. The index is always below the count.
- A count of 1 returns 0 without calling the random source, so a build with one
  picture per slot renders the same bytes as before and every golden is
  unchanged.
- The screen builders take the sprite to paint (`NULL` is the base). They read
  no state and call no random source, so the host render tests hand them a
  second, test-only variant built in memory.

Drawing the QR and the sprites with plain rectangle fills is also why
`sdkconfig.defaults` trims LVGL to the label and bar widgets and the I1 draw
path only, mirrored in `test/mocks/lv_conf_host.h`. Re-enabling any of it is a
flash cost and needs the host config changed with it.

## Golden tests

`test_display_render` compiles the real LVGL for the host
(`test/mocks/lv_conf_host.h`), builds each screen and compares it byte for
byte with a file in `test/test_display_render/golden/`. The goldens are the
layout contract. Regenerate them with `MAGTAG_WRITE_GOLDEN=1` only for an
intentional layout change; the flag rewrites every golden, so check out the
ones you did not mean to change. The layout arithmetic and the refresh
cadence are tested separately in `test_display`.
