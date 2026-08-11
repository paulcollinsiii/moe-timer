# OTA updates — HTTPS manifest, per-device targeting, on-panel feedback

Status: **plan only, nothing implemented.** Written 2026-08-10 against
`integration` (`bad81d8`); **reviewed 2026-08-11** — see "Settled in review"
at the end for what that pass decided, and the shortened "Open decisions"
list for what it left.
Branch to cut: `feature/ota`, from **`integration`**.
Motivation: ship firmware to the MagTags without a USB cable, and be able to
put a build on the test device before it becomes everyone's build.

Requirements, as given, treated as settled:

1. The endpoint checked is configurable.
2. TLS is required; a specific CA cert is supplied.
3. A specific MagTag can be targeted, overriding a default.
4. On-panel feedback when an update is found and applied — static, no
   progress bar.
5. Daily check at day rollover; a flag makes it also run on every manual full
   sync (Button D).
6. A version number shown on screen if there is room.

Explicitly out of scope, and the plan does not smuggle them back in: signed
binaries, secure boot, flash encryption, anti-rollback efuses.

---

## The groundwork is already laid — and one thing is already load-bearing

Three facts measured against the tree, not assumed:

**The partition table is already dual-slot.** `partitions.csv` has carried
`ota_0`/`ota_1`/`otadata` since 2026-07-16, with a comment saying "until OTA
plumbing ever lands". `nvs`/`phy_init`/`ota_0` offsets match the old
single-app layout, so nothing about installing OTA support disturbs a device's
existing NVS.

**The whole TLS stack is already in the image.** This is the surprise, and it
changes the size budget materially. `main`'s `REQUIRES` lists `esp-tls`, and
`espressif__mqtt` pulls it in as well — so even though the broker URI is plain
`mqtt://` and no code in this tree opens a TLS socket, the final ELF already
contains:

```
mbedtls_ssl_handshake_client_step   3064 B
mbedtls_ssl_handshake_server_step   4322 B
mbedtls_x509_crt_parse_der_internal 2269 B
esp_tls_conn_new_sync / _async / _destroy / _read / _write
543 mbedtls_* symbols total
```

The expensive half of "add HTTPS" is therefore already paid for. What is
genuinely absent:

| Symbol | Status |
| --- | --- |
| `esp_ota_get_running_partition` | **linked** (via `esp_app_format`) |
| `esp_app_get_description` | **linked** |
| `esp_http_client_init` | absent |
| `esp_https_ota` | absent |
| `esp_ota_begin` / `esp_ota_set_boot_partition` | absent |
| `esp_ota_mark_app_valid_cancel_rollback` | absent |
| `esp_crt_bundle_attach` | absent (not needed — own CA) |

Adding `esp_http_client`, `esp_https_ota` and `app_update` to `REQUIRES` is the
whole dependency change. Estimated cost **+40–60 KB**, which brings us to the
one constraint that actually bites.

**Flash is 100 % allocated and the app slot is 91 % full.**

```
app image  build/magtag_timer.bin   1,434,656 B
ota slot   0x180000                 1,572,864 B
headroom                              138,208 B   (91.2 % full)

assets     0xEE000                    974,848 B
nvs+phy+ota_0+ota_1+otadata+assets  4,194,304 B of 4,194,304 B
```

After OTA lands the image is ~1.49 MB in a 1.57 MB slot: **~5 % headroom.**

---

## Decide the partition table first — it is the one thing OTA cannot fix

**An OTA update cannot change the partition table.** The bootloader reads it
from a fixed offset and `esp_https_ota` writes only into the inactive app
slot. So the layout that ships with the first OTA-capable firmware is the
layout those devices have forever, short of a serial cable and a physical
visit to each one.

That makes this the single highest-consequence decision in the plan, and it
has to be made *before* the first flash, not after.

Flash is fully allocated, so bigger app slots come out of `assets` (the raw
partition holding the optional alert WAV — `tools/flash_assets.sh`). Three
candidate layouts:

| Layout | Slot size | Headroom after OTA (1,479,728 B) | `assets` | WAV @16 kHz |
| --- | --- | --- | --- | --- |
| Keep as-is `0x180000` | 1,572,864 | 91 KB (5.9 % free) | 952 KB | ~30 s |
| `0x1A0000` | 1,703,936 | 219 KB (13.2 % free) | 696 KB | ~22 s |
| **`0x1C0000` — CHOSEN** | **1,835,008** | **347 KB (19.4 % free)** | **440 KB** | **~14 s** |

**Measured, not estimated (task 1, 2026-08-11).** Baseline image 1,435,232 B
(`0x15e660`); with `esp_http_client` + `esp_https_ota` + `app_update` linked
and referenced, 1,479,728 B (`0x169430`) — **+44,496 B (43.5 KB)**, the bottom
of the 40–60 KB estimate. Measured with a throwaway TU referencing the real
API surface and held by `-u`, because IDF builds with `--gc-sections`: adding
the components to `REQUIRES` alone changes the image by zero bytes and would
have measured nothing.

That figure covers the ESP-IDF side only. Still to come: `ota_policy.c` /
`ota_flow.c` / `ota.c`, the CA PEM (~1.5 KB), the update screen and the new
NVS/HA fields — call it another 15–25 KB. Even at the top of that range the
chosen layout lands near 18 % free, while the existing `0x180000` layout would
land near 4 % and trip the 85 % guard of task 15 immediately.

**Decided (2026-08-11): the aggressive layout.** 14 s of WAV is ample for an
alert tone, and the app slot is the resource that cannot be renegotiated later.
`nvs`, `phy_init` and `ota_0` keep their current offsets, so an existing
device's NVS and running app survive the reflash exactly as the current file's
comment promises; only `ota_1`, `otadata` and `assets` move:

```
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0x6000
phy_init,   data, phy,     0xf000,   0x1000
ota_0,      app,  ota_0,   0x10000,  0x1C0000
ota_1,      app,  ota_1,   0x1D0000, 0x1C0000
otadata,    data, ota,     0x390000, 0x2000
assets,     data, 0x40,    0x392000, 0x6E000
```

Alignment checks: app partitions land on 64 KB boundaries
(`0x10000`, `0x1D0000`), data partitions on 4 KB (`0x390000`, `0x392000`),
and the table ends exactly at `0x400000`. `assets` is 450,560 B — ~14 s at
16 kHz, ~28 s at 8 kHz, 16-bit mono.

Update `tools/flash_assets.sh`'s header comment (it currently advertises
"952 KB ~= 30 s at 16 kHz") along with the table.

Consequence to accept: **a re-partition invalidates the flashed WAV** — the
`assets` partition moves, so `tools/flash_assets.sh` must be re-run on any
device that has a custom alert tone. That is a one-time cost paid during the
same serial session that installs OTA support.

**Task 1 measured before this was committed** — see the table above. The
measurement confirms the choice rather than changing it: 43.5 KB of ESP-IDF
OTA machinery leaves the current layout at 5.9 % free, which is not enough to
absorb the project's own OTA code, let alone a future font.

### Guard the headroom afterwards

Once the layout is frozen, headroom erosion becomes a slow-motion version of
the same problem: a build that outgrows the slot is a *loud* failure (the
image-size check fails the build), but it is a failure you cannot fix in the
field. Add a build-time check that warns at, say, 85 % slot occupancy — in
keeping with how this project already re-arms `-Werror=unused-function` rather
than trusting review to notice. LVGL is the largest single library in the
image (233 objects); one more Montserrat font is ~20–40 KB.

---

## Version scheme — there is not one today, and OTA needs one

`esp_app_get_description()->version` is what the HA `fw` stat publishes today.
No `version.txt` exists and `PROJECT_VER` is not set, so IDF falls back to
`git describe`, which on this tree currently yields:

```
difftest-base/cycle11-21-g01c7694
```

That is not a version anyone can compare, and `esp_app_desc_t.version` is a
32-byte field it very nearly overflows.

**Add `version.txt` at the project root** containing a bare semver
(`1.5.0`). IDF picks it up automatically and stamps it into the app
descriptor; it becomes both the on-screen string and the OTA comparison key,
which is what keeps the two from ever disagreeing.

### The update test: "different", not "newer"

Do **not** implement semver ordering on the device. Compare the manifest's
`version` string against `esp_app_get_description()->version` and update when
they **differ**.

This is a smaller, purely-testable decision, and it is strictly better for the
stated use case:

- Rolling back is publishing the old version string again. With a
  "greater-than" test, a rollback needs a version-number lie.
- Targeting the test device with `1.6.0-rc1` and everyone else with `1.5.0`
  works without a pre-release ordering rule (which is the fiddly part of
  semver, and the part nobody gets right in ten lines of C).
- It composes with "no anti-rollback", which is already a non-goal.

The cost is that a mis-published manifest can downgrade a fleet. That is
acceptable here — the fleet is a handful of devices under one person's
control, and the recovery is republishing the manifest.

---

## Showing the version on the panel

There is room, on the row that already carries the battery percentage.

Current main-screen geometry (`main/display_screens.c`), with the
partial-refresh clean bands from `main/display.c`:

```
band {3,17}    header:  "Fri Aug 10  3:42 PM"        │        "Last sync: 3:40 PM"
band {26,49}   progress bar (+ Charge Me!!! badge)
band {58,87}   "[batt] 85%"  at (4, 66) 12 pt        │  "0:42:19"  at (-4, 58) 28 pt
band {95,125}  "Weekday - 60 min"                    │             "RUNNING"
               [play]      Reset       [swap]        [refresh]
```

The `{58,87}` band is the one with slack: the battery label ends around x≈55
and the 28 pt remaining-time is right-aligned from roughly x≈180. **~120 px of
empty row.** A 12 pt `v1.5.0` needs ~45 px.

**Decided: extend the existing battery label's string.**

```c
snprintf(buf, sizeof(buf), "%s %u%%   %s",
         BATT_SYMS[display_battery_icon_level(st->battery_pct)],
         (unsigned)st->battery_pct, st->fw_version);
```

One label, one existing clean band, no new geometry to calibrate, and no risk
of straddling a framebuffer byte boundary — which is the failure mode the
band comments warn about ("bands must not share a framebuffer byte; a shared
byte would be inverted twice and cancel out"). A separate label at
`LV_ALIGN_TOP_LEFT, 70, 66` is equally safe if the combined string reads badly
on hardware; that is a taste call to settle at smoke-test time.

Rejected alternatives: the header row (both ends are load-bearing — the
break chip already displaces "Last sync"), and the bottom status row (mode
line and state string, both variable-width).

### Wiring it — the field already exists

`app_state_in_t` already carries `const char *fw_version`, commented
`/* stats only */`, injected by `wake_flow.c`'s `stats_collect()` from
`esp_app_get_description()->version`. So:

1. Add `const char *fw_version;` to `display_state_t` (`include/display.h`).
2. Populate it in `app_state_display()` from the input struct that already
   has it, and drop "stats only" from the comment.
3. Read it in `build_main_status()`.

`display_screens.c` stays ESP-free and host-renderable — it receives a string,
it never calls `esp_app_get_description()`. That is what keeps the golden test
deterministic: the suite injects a fixed `"1.5.0"`.

**Golden churn:** every main-screen golden changes. Per the project's own
rule these are regenerated only for an intentional layout change — this is
one — and per `MAGTAG_WRITE_GOLDEN` behaviour the regen rewrites *all*
goldens, so the unchanged ones (break / timesup / charge / bedtime) must be
`git checkout`-ed back rather than committed with incidental churn.

---

## The update screen

New screen, full refresh, static, no progress bar:

```
        ┌──────────────────────────────────┐
        │     UPDATING FIRMWARE            │   28 pt   y=6
        │                                  │
        │       Current: v1.5.0            │   12 pt   y=46
        │                                  │
        │     Installing v1.6.0            │   18 pt   y=66
        │                                  │
        │      Do not remove power         │   12 pt   y=98
        └──────────────────────────────────┘
                    296 x 128
```

Both versions are shown: the one being left and the one being installed. That
makes the panel self-diagnosing — if a device gets stuck, the screen alone says
which transition it was attempting, without needing the logs or HA.

- `display_screens_build_ota(const char *from_version, const char *to_version)`
  — pure LVGL, golden-tested.
- `display_ota(const char *from, const char *to)` in `display.h`/`display.c` —
  full refresh, same shape as `display_charge_me()` / `display_bedtime()`.

Row budget: 34 + 15 + 22 + 15 px of text plus gaps fits 128 px with room to
spare. Unlike the main screen this needs **no clean-band entry** — the clean
bands exist to stop partial-refresh ghosting, and this screen only ever renders
as a full refresh.

The wording is **"Installing v1.6.0"**, not "Upgrading to" — settled in
review. This plan deliberately supports downgrades (see "different, not
newer"), and on a rollback "Upgrading" would be actively wrong; the direction-
neutral verb is correct in both directions and the "Current:" line above it
already supplies the direction for anyone who cares.

"Do not remove power" is honest here: the write goes to the *inactive* slot
and the boot partition only flips after the image verifies, so a power cut is
recoverable — but a user who yanks the cable mid-download simply wastes the
download, and the line costs nothing.

**When it paints, and why that ordering matters.** It paints while the radio
is **down**, between the check window and the download window (see below).
`main/net_window.c` documents that panel refresh and WiFi TX bursts coinciding
**browned out the rail on device** — that is why the stats snapshot is a
rendezvous released only after the paint finishes. A screen painted during a
sustained OTA download would walk straight into the same fault, which is the
reason this plan splits check from apply rather than doing the obvious thing.

After a successful flash the device reboots and the *new* firmware paints the
normal screen — carrying the new version string, which is the confirmation.
No separate "update complete" screen is needed.

On failure: log, publish the reason to HA, repaint the normal screen, sleep.


---

## Trigger design

### Daily, at the day rollover

`wake_flow_handle_day_rollover()` (`main/wake_flow.c:537`) already opens a
network window via `net_apply_try_window()`. The OTA check rides that window —
**no additional WiFi bring-up on the common path.**

The rollover fires on the first wake after midnight. During a Bed Time lock
that is still true: the tick handler runs `wake_flow_handle_day_rollover()`
*before* `lock_gate_check_bedtime()`, and the bedtime lock's own 7200 s wakes
run a network window anyway. So the daily check naturally lands in the middle
of the night, when nobody is looking at the device. That is the right time for
it and it needs no extra scheduling.

### Manual, on Button D

`wake_flow_handle_button_wake()`'s `BTN_D` arm (`main/wake_flow.c:1059`) opens
a window and waits for NTP. Gate the check on a runtime flag.

**Runtime-settable, with a build-time default — both, which is already the
house pattern.** A Kconfig symbol *alone* would mean reflashing over USB to
toggle a testing convenience, which defeats the purpose of the feature it is
testing. But Kconfig is not thereby redundant: in this tree Kconfig supplies
the **seed** and HA supplies the **override**, exactly as
`CONFIG_MAGTAG_MQTT_URI` → `NVS_DEFAULT_MQTT_URI` → NVS → HA text entity works
today.

So `ota_on_sync` gets all three layers:

```
CONFIG_MAGTAG_OTA_CHECK_ON_SYNC (bool, default n)
        │  seeds
        ▼
NVS_DEFAULT_OTA_ON_SYNC  →  NVS "ota_on_sync"  ←  HA switch (runtime override)
```

Same for the endpoint (`CONFIG_MAGTAG_OTA_URL`) and the battery floor
(`CONFIG_MAGTAG_OTA_MIN_BATT_PCT`). A production build can therefore ship with
the flag off and no endpoint compiled in, while the test device is flipped from
Home Assistant without touching a cable.

**Decide explicitly whether these join the defaults fingerprint.**
`nvs_config_defaults_fingerprint()` reseeds NVS whenever the compile-time
allocation defaults change. If the OTA defaults participate, changing the
Kconfig default silently overwrites an HA-set value on the next boot — which is
surprising for `ota_on_sync` specifically, since the whole point is that HA
owns it at runtime. **Recommend: keep the OTA keys out of the fingerprint**,
seeded once on first boot and owned by HA thereafter.

### What must gate the check

All pure, all in `ota_policy.c`, all host-tested:

- **NTP settled this window.** X.509 validity checking needs a correct clock.
  A device with a bad clock will fail the handshake, so checking is wasted
  radio time — and it is the failure mode that looks like a broken server.
- **Battery at or above 30 %** (`CONFIG_MAGTAG_OTA_MIN_BATT_PCT`, default 30)
  and not charge-locked. A sustained radio burst on a weak cell risks a
  brownout reset, and a brownout part-way through a flash write is the one
  interruption worth actively avoiding. Settled at 30 %; it sits well above
  the 15 % warn tier and the 10 % charge lock in `battery_policy.c`, so the
  three thresholds stay ordered and independently meaningful.
- **Endpoint configured.** Empty URL = OTA disabled entirely, exactly as an
  empty `mqtt_uri` disables the MQTT session.

- **Not already pending.** One attempt per wake.

---

## Check and apply are two phases, in two windows

This is the load-bearing structural decision.

`net_window.c`'s task contract is explicit: it "owns the radio and NOTHING
else — it never mutates timer state, never paints, never touches LEDs". Every
network→device effect in this tree (grants, bonus, locate, def reconcile) is
**buffered by the network task and applied by `net_apply_finish()` on the main
task**. OTA follows the same pattern rather than inventing a new one.

```
  ┌─ window 1 (the existing one) ──────────────────────────────┐
  │  wifi_session_begin → SNTP                                 │
  │  → snapshot rendezvous          ← blocks until paint done  │
  │  → OTA CHECK: one HTTPS GET of the manifest (~300 B)       │
  │    parse, decide, BUFFER {version, url}, record result     │
  │  → MQTT               ← publishes THIS check's result      │
  │  → wifi_session_end                                        │  radio down
  └────────────────────────────────────────────────────────────┘
                              │
   net_apply_finish() returns; orchestrator sees a buffered OTA
                              │
   PAINT "UPDATING FIRMWARE v1.5.0"   ← radio down, no brownout risk
                              │
  ┌─ window 2 (update days only) ──────────────────────────────┐
  │  wifi_session_begin                                        │
  │  esp_https_ota → inactive slot     ← no panel activity     │
  │  esp_ota_set_boot_partition                                │
  │  wifi_session_end                                          │
  └────────────────────────────────────────────────────────────┘
                              │
                        esp_restart()
```

The second association costs a few seconds **only on a day when an update
actually exists** — once per release, not once per day. The common path (no
update) adds one small HTTPS GET to a window that was already happening.

**The check sits between the rendezvous and MQTT, and that placement is
deliberate.** The rendezvous already blocks until the wake's paint has
finished, so by the time the check transmits, the panel is idle — the brownout
condition is satisfied without any new mechanism. And running before
`mqtt_ha_window()` means the check's outcome is in hand when the stat payload
is built, so **a failed check is reported to Home Assistant in the same window
that produced it**, not a day later.

### Where the apply happens in the wake

Not inside the rollover. `wake_flow_handle_day_rollover()` calls
`queue_rollover_summary()` and resets the day; rebooting from the middle of it
would replay the rollover on the next boot and double-publish yesterday's
summary to HA.

Instead, apply at the **single late point in the wake, after all timer work
and the snapshot save, immediately before sleep** — the same place
`wake_flow`'s guaranteed break-end drain already lives. A reboot from there is
indistinguishable from a power cut at sleep entry, which
`timer_persist_try_restore()` already handles correctly. This is a property
the tree has already paid for; the plan just needs to not step outside it.

### The timeout problem, and the sad loop

Yes — and there are **two** independent timers that can kill a download, which
is worth separating because they need different answers:

| Timer | Setting | Effect if it fires |
| --- | --- | --- |
| Project awake failsafe | `CONFIG_MAGTAG_MAX_AWAKE_SEC` = 180 s | Forces deep sleep mid-download |
| ESP task watchdog | `CONFIG_ESP_TASK_WDT_TIMEOUT_S` = 5 s, idle-task checked on CPU0 | Panic + reboot if idle starves |

The failure mode identified in review is the important one and it is not
hypothetical: **a download that reliably exceeds the cap never completes.** The
device wakes at every rollover, opens a window, paints the update screen, gets
part-way, is forced to sleep, and does it all again tomorrow — forever. That is
strictly worse than having no OTA at all, because it is a daily battery cost
that produces nothing and looks, from the outside, like nothing is happening.

Three mitigations, and all three are needed:

**1. Extend the failsafe before the download window opens, not during it.**
A dedicated `CONFIG_MAGTAG_OTA_MAX_SEC` (default 300, ≤ 600 to stay consistent
with `MAX_AWAKE_SEC`'s own Kconfig range) applied via the injected extender at
the top of the OTA phase, and restored afterwards. The extension must be
unconditional and must precede `wifi_session_begin()` — an extension applied
"once we see it's taking a while" is an extension that races the thing it is
protecting against.

**2. Use the incremental `esp_https_ota` API, not the one-shot call.**
`esp_https_ota_begin()` → loop on `esp_https_ota_perform()` →
`esp_https_ota_finish()`, rather than the single blocking `esp_https_ota()`.
This matters for both timers:

- Each `perform()` returns after one chunk, so the task yields and the **idle
  task runs** — which is what keeps the 5 s TWDT satisfied on a slow link.
- The loop is where a **deadline check** goes. The download aborts *cleanly*
  when it runs past its own budget, discarding the partial image, instead of
  being killed mid-write by the failsafe.

A clean abort is not cosmetic: it is what guarantees the boot partition is
never switched to a partial image, and it is what makes the failure reportable
rather than silent.

**3. A retry budget — this is the actual fix for the sad loop.**
Persist `ota_fails` (a count) and `ota_target` (the version it is counting
against) in NVS. After **3 consecutive failures against the same target
version**, stop attempting that version and publish the give-up to HA. A
*different* target version resets the counter to zero.

So a genuinely un-downloadable build costs three windows and then goes quiet,
instead of one window per day indefinitely. Publishing a new version — or
republishing the same one under a new version string — re-arms it. This is
pure, sits in `ota_policy.c`, and is host-tested with the rest of the decision
logic.

Alongside it, record the **download duration** in the stat payload. A link that
is trending toward the cap is then visible in Home Assistant *before* it
becomes chronic, rather than being discovered as a stuck fleet.

---

## Efficiency — and the option that removes the HTTPS check entirely

Ranked by what they actually buy:

1. **Ride the existing window.** No extra WiFi association on non-update days.
   This is the whole game — association + DHCP dominates a short window's
   cost.
2. **Once per day**, at rollover, not per wake.
3. **Tiny manifest** (~300 B). One request, one response.
4. **Skip when the preconditions fail** (no NTP, low battery, no URL).

Not worth doing, and the plan says so rather than leaving them as loose ideas:

- **ETag / `If-None-Match` → 304.** Saves ~300 B of a ~6 KB exchange. The TLS
  handshake dominates and a 304 still pays it in full.
- **TLS session resumption.** Tickets do not survive deep sleep without being
  persisted to NVS, and one handshake per day does not justify that.

### The efficient variant worth considering: carry the manifest over MQTT

This device already opens an MQTT session to Home Assistant in the same
window, and already consumes **retained** documents through `mqtt_rx.c` and
`config_apply.c`. A retained `{"version": "...", "url": "..."}` on a topic the
device is already subscribed to arrives at **zero marginal network cost** —
no HTTPS request at all on the 364 days nothing changed. The device would
open an HTTPS connection only to download an image it already knows it wants.

Targeting falls out for free, too: publish a shared default to `magtag/ota`
and a per-device override to `magtag/<id>/ota`, retained. Changing which
device gets a build is an MQTT publish, not a file edit.

Trade-off: it couples updates to the broker being reachable. A device deployed
away from that HA instance could never update.

**Recommendation: build the HTTPS manifest path (it is what was asked for and
it is broker-independent), but keep `ota_policy.c` agnostic about the
source** — its entry point takes a JSON string and the running version, and
does not know or care whether MQTT or HTTPS delivered it. Adding the MQTT
source later is then ~30 lines and reuses the entire test suite. If the MQTT
route turns out to be preferable in practice, nothing about the decision logic
is rewritten.

---

## The manifest, and targeting

A single static JSON file, fetched over HTTPS. The device does the matching,
so hosting stays a static file with no server-side logic.

**The top level is an array of schema-versioned blocks**, each carrying a
distinct `schema` integer. A device reads the newest block it understands and
ignores the rest.

```json
[
  {
    "schema": 1,
    "default": {
      "version": "1.5.0",
      "url": "https://ota.example.com/magtag/magtag_timer-1.5.0.bin"
    },
    "devices": {
      "magtag-a1b2c3": {
        "version": "1.6.0-rc1",
        "url": "https://ota.example.com/magtag/magtag_timer-1.6.0-rc1.bin"
      }
    }
  },
  {
    "schema": 2,
    "default": { "version": "1.9.0", "url": "https://ota.example.com/..." },
    "channels": {
      "beta": { "version": "2.0.0-rc1", "url": "https://ota.example.com/..." }
    },
    "devices": { "magtag-a1b2c3": { "channel": "beta" } }
  }
]
```

### Why the array, and what it buys

This is what makes a **rolling fleet upgrade** work from one static file. When
the format needs to change, schema 2 is added alongside schema 1 rather than
replacing it:

- Firmware that only understands schema 1 keeps reading the schema-1 block and
  keeps updating — so laggards are still reachable, and the schema-1 block is
  precisely the lever that drags them forward.
- Firmware that understands schema 2 reads schema 2 and gets the newer
  capabilities (channels, in the sketch above).
- Once every device is on firmware that understands schema 2, the schema-1
  block is deleted. Nothing coordinates that moment — it is just a file edit
  made when HA shows the fleet has caught up.

Without this, introducing any format change means every device that has not yet
updated is stranded, which is the one failure OTA is supposed to eliminate.

### Block selection, then resolution — both pure and host-tested

Selecting the block:

1. The top level must be an array. Anything else = do nothing.
2. Discard blocks whose `schema` exceeds `OTA_SCHEMA_MAX` — a compile-time
   constant in `ota_policy.c`, bumped when the parser learns a new format.
3. Of what remains, take the **highest** `schema`.
4. Duplicate `schema` values are malformed; take the first and log it. (The
   schemas are meant to be non-overlapping — this is defensive, not a feature.)
5. No compatible block = do nothing, and report the reason to HA. This is the
   fails-safe case, and it is now *reportable* rather than silent.

**Unknown keys inside a compatible block are ignored, never fatal.** This is
load-bearing and easy to get wrong: if a schema-1 parser rejects a block
because it contains a key added for schema 1.x readers, the forward
compatibility the array is meant to provide evaporates.

Resolving within the selected block (schema 1):

1. Look up `devices[device_id()]`. `device_id()` already returns
   `magtag-xxxxxx` derived from the WiFi STA MAC — a stable, zero-config
   targeting key that needs no new state on the device.
2. Otherwise use `default`.
3. Update iff the resolved `version` differs from the running version.
4. A resolved entry with `"version"` absent or `null` means **pinned** — do
   nothing. This is how a device is frozen without deleting its entry.

### Other targeting mechanisms considered

- **Per-device URL** (`https://host/ota/magtag-a1b2c3.json`): no shared file,
  but N files to maintain and a 404 round-trip for the fallback. Worse on both
  counts.
- **Server-side decisioning** from a device header: needs dynamic hosting,
  which was ruled out.
- **Channels** (`stable` / `beta`) with the device storing its channel in NVS:
  genuinely nicer at fleet scale — moving the test device to `beta` is a
  one-time HA toggle, after which no manifest edit is ever needed to ship a
  release candidate. It costs one more config field and one more indirection.

  **Verdict: ship `devices` + `default` as schema 1; channels become schema 2
  if and when manifest edits per release start to grate.** Schema 1 directly
  expresses the stated use case ("override a default") with zero new config
  plumbing. And the array structure above means the eventual move costs
  nothing at the fleet level — schema 2 is added beside schema 1, devices pick
  up channels as they update, and schema 1 is deleted once nothing reads it.
  The sketch in the manifest example shows the shape that migration would
  take.

---

## TLS and the CA certificate

**Embed the supplied PEM at build time** via `EMBED_TXTFILES` in
`main/CMakeLists.txt` (`certs/ota_ca.pem`), and pass it as `cert_pem` in the
`esp_http_client_config_t`. ~1–2 KB of rodata. `CONFIG_ESP_TLS_INSECURE` stays
off; `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` is already `not set` in `sdkconfig` and
must stay that way.

Two things to get right, because both of them brick OTA in a way that requires
a physical visit:

**Pin the root, never the leaf or the intermediate.** If the host uses Let's
Encrypt, embed **ISRG Root X1** (valid to 2035), not the server certificate
and not the intermediate — those rotate roughly every 90 days alongside the
leaf, and a device pinned to an intermediate stops being able to update the
moment it rotates. This is the single most common way a hobby OTA deployment
kills itself.

**Enforce `https://` structurally.** Add a validator to
`main/config_validate.c` (pure, host-tested, already shared by the HA config
paths) that rejects any endpoint URL not beginning with `https://`. That turns
"TLS is required" from a convention into something the config channel cannot
violate — including via a typo in the HA text entity.

**Escape hatch, deferred but designed for:** an NVS key (`ota_ca`) checked
before the embedded PEM, so a cert rotation can be pushed without a serial
cable. Deferred because a PEM is ~1–2 KB and the retained-config path would
need chunking to carry it; noted here so the read order (`NVS ?: embedded`) is
established from the start rather than retrofitted.

---

## Rollback on failed boot — recommended, and it is not a "security feature"

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is a **reliability** feature and is
distinct from `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`, which is the security one
and stays out of scope (it would also break targeting a device with an older
build, which is a stated requirement).

With rollback enabled a freshly-OTA'd app boots in `PENDING_VERIFY`. If it
does not call `esp_ota_mark_app_valid_cancel_rollback()`, the bootloader
reverts to the previous slot. Without this, one bad build that boot-loops
means a serial visit to every device — precisely the outcome OTA exists to
prevent. **Decided: enabled.**

**Where the mark-valid call lives.** Per the residency rule,
`esp_ota_mark_app_valid_cancel_rollback()` is a *bare call*, not a handle, so
it may not live in `main.c` — it goes in `ota.c` behind
`ota_mark_valid_if_pending()`, called from `wake_flow`'s pre-sleep point once
the wake has demonstrably worked (NVS read, panel painted).

**Verify on hardware:** confirm that a deep-sleep wake does not itself consume
the pending-verify state before the first full wake completes. This is a
smoke-test item, not something to assume from the docs.

---

## Failure modes

**Every failure reports to Home Assistant.** Nothing in this list fails
silently — the USB console is unreachable when these happen (overnight, on
battery, mid-sleep-cycle), so MQTT is the only channel that can tell you what
died. This tree already treats it that way for `reset_reason`; OTA gets the
same treatment.

| Failure | `ota_result` | Behaviour | Recovery |
| --- | --- | --- | --- |
| No WiFi / no NTP | *(not set — no check ran)* | Check skipped | Next rollover |
| Manifest fetch failed | `http_404`, `http_500`, … | No update | Next rollover |
| Manifest malformed JSON | `bad_manifest` | No update | Next rollover |
| No compatible schema block | `no_schema` | No update | Fix the manifest |
| **Cert validation fails** | **`tls_cert`** | Errors out, screen repaints | Next rollover; if permanent, serial reflash |
| Other TLS failure | `tls` | Errors out, screen repaints | Next rollover |
| Deadline abort (download too slow) | `timeout` | Clean abort, partial image discarded | Retry budget, then gives up |
| Download interrupted (power) | *(persisted at next boot)* | Boot partition **not** switched | Next rollover retries from scratch |
| Image header invalid / wrong chip | `bad_image` | Rejected before switching | Next rollover |
| Battery below floor | `low_batt` | Check skipped | Next rollover |
| Free heap too low | `low_heap` | Download skipped | Next rollover |
| Retry budget exhausted | `gave_up` | Stops attempting this version | Publish a new version |
| New app boots and crashes | *(previous value survives)* | Bootloader rolls back | Automatic |
| New app boots but is broken (no crash) | — | Not detected | Republish the old version |
| Manifest points at a downgrade | — | Applied — deliberate | Republish |

`tls_cert` being distinguishable from `tls` is the difference between "my cert
expired / I pinned the wrong thing" and "the network flaked", and those have
completely different fixes. `esp_https_ota` surfaces enough error detail to
separate them (`ESP_ERR_ESP_TLS_*` and the HTTP status), so the mapping is a
small pure function in `ota_policy.c` rather than a lossy "it failed".

**Reporting mechanics — the two windows report differently.** A *check* failure
(window 1) publishes in the same window, because the check runs before the MQTT
phase. A *download* failure (window 2) cannot: MQTT is already closed and
reopening it would cost another association. So the download result is
persisted to NVS (`ota_result`) and published on the **next** window.

That asymmetry is acceptable precisely where it lands: a successful download
reboots immediately and announces itself by the new version appearing on the
panel and in the `fw` sensor, so success needs no report. Only failures take
the deferred path, and a failure that is visible tomorrow morning is visible
enough.

The downgrade row is the accepted cost of the "different, not newer" test.

---

## Architecture

Three new modules, placed by the layer model in `docs/architecture.md`:

| Module | Layer | Responsibility | Test |
| --- | --- | --- | --- |
| `main/ota_policy.c` | **1 — pure** | Manifest parse, device targeting, "should update?" decision, precondition gating. Total function over its arguments; no clock, no NVS, no ESP includes. Uses cJSON, as `config_apply.c` already does. | `test_ota_policy` — host, direct |
| `main/ota_flow.c` | **2 — orchestration** | Sequences check → buffer → paint → download → reboot. Device effects injected via an ops struct, exactly like `net_apply_ops_t`. | `test_ota_flow` — host, single-TU with stubs |
| `main/ota.c` | **3 — driver** | `esp_http_client` manifest GET; the **incremental** download (`esp_https_ota_begin` / `_perform` loop / `_finish`) with the deadline check in the loop; `esp_ota_mark_app_valid_cancel_rollback`. Thin — every decision is already made upstream, including when to abort. | Hardware smoke test |

`main/main.c` gains **nothing**. Under the residency rule the only candidate
would be an awake-failsafe extension for the download, and that is handled by
mirroring the existing `alerts_set_extend_awake()` pattern: `ota_flow` receives
the extender as an injected function pointer, `app_main` installs it in one
branch-free line, and the `esp_timer` handle stays where it already lives.

### The awake failsafe

`CONFIG_MAGTAG_MAX_AWAKE_SEC` is 180 s. A ~1.5 MB HTTPS download on an
ESP32-S2, with TLS and flash writes, should run 15–30 s — comfortably inside
the cap. But a slow link would have the failsafe fire mid-download, and the
consequence is worse than one wasted download: see **"The timeout problem, and
the sad loop"** above for the full treatment, which is where the failsafe
extension, the incremental-API deadline check and the retry budget are
specified together. They are one mechanism and should be implemented as one.

The piece that belongs *here*, in the architecture: the extension is delivered
through the same injected-function-pointer pattern as
`alerts_set_extend_awake()`, so `main.c` gains one branch-free install line and
the `esp_timer` handle stays where it already lives. `MAX_AWAKE_SEC`'s Kconfig
`range` is 30–600; the extender writes the timer directly and is not bound by
it, but keeping `CONFIG_MAGTAG_OTA_MAX_SEC` inside 600 keeps the two
consistent.

### Task and stack sizing

`net_window.c` creates `net_win` with a **10240 B** stack. `esp_https_ota` with
TLS wants more headroom than the main task's 7168 B leaves. Run the download
on its own task sized ~12–16 KB and tune from
`uxTaskGetStackHighWaterMark()`, which `net_window.c` already logs per window
and which the OTA task should log the same way.

### Heap

The download needs roughly 40 KB free during the handshake and transfer (TLS
buffers + http client). Mitigations, in order of preference:

1. Run it in its **own window with the MQTT session already closed** — the
   architecture above does this for brownout reasons, and it happens to free
   `mqtt_ha.c`'s large static buffers' working set too.
2. Check `esp_get_free_heap_size()` before starting and skip with a logged
   reason if short. Cheap, and turns a hard failure into a diagnosable one.
3. `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` can be cut from 16384 to save ~12 KB —
   **but only if the host serves small TLS records.** Many CDNs do not.
   Verify against the actual host before touching it; a wrong setting here
   fails the handshake in a way that looks like a cert problem.

The 2025 MagTag has 2 MB PSRAM (`CONFIG_SPIRAM=y`), but
`CONFIG_SPIRAM_IGNORE_NOTFOUND=y` means a PSRAM-less board must still work —
so none of the above may *depend* on PSRAM being present.

---

## Configuration surface

New NVS keys (`include/nvs_keys.h`; NVS caps keys at 15 chars):

| Key | Type | Default | HA-settable | Purpose |
| --- | --- | --- | --- | --- |
| `ota_url` | str | `CONFIG_MAGTAG_OTA_URL` | yes (text) | Manifest endpoint. Empty = OTA disabled. |
| `ota_on_sync` | u16 (0/1) | `CONFIG_MAGTAG_OTA_CHECK_ON_SYNC` | yes (switch) | Also check on every Button D full sync. |
| `ota_result` | str | `""` | no | Last attempt outcome (reason code). |
| `ota_target` | str | `""` | no | Version the retry budget is counting against. |
| `ota_fails` | u16 | 0 | no | Consecutive failures against `ota_target`. |

The last three are device-owned state, not config — they are written by the
firmware and read by the stat payload, so they get accessors but no HA entity
and no bulk-document key.

New Kconfig symbols (`main/Kconfig.projbuild`), each seeding the NVS default
above or configuring behaviour that has no runtime override:

| Symbol | Default | Purpose |
| --- | --- | --- |
| `MAGTAG_OTA_URL` | `""` | Seeds `ota_url`; prefer `credentials.local.h` |
| `MAGTAG_OTA_CHECK_ON_SYNC` | `n` | Seeds `ota_on_sync` |
| `MAGTAG_OTA_MIN_BATT_PCT` | `30` | Battery floor for a check |
| `MAGTAG_OTA_MAX_SEC` | `300` | Failsafe budget for the download window |
| `MAGTAG_OTA_MAX_FAILS` | `3` | Retry budget before giving up on a version |

Accessors in `nvs_config.c`/`.h` following the existing shape, plus defaults in
`include/nvs_defaults.h` and a `credentials.local.h` slot for the URL — same
treatment `MAGTAG_MQTT_URI` gets, so an endpoint never lands in a committed
`sdkconfig.defaults`.

**Keep the OTA keys out of the defaults fingerprint.**
`nvs_config_defaults_fingerprint()` reseeds NVS when the compile-time defaults
change; including `ota_on_sync` there would let a Kconfig edit silently
overwrite an HA-set value on the next boot, which is the opposite of what the
runtime override is for.

**Both HA-settable fields must go in three places, not one.** This is the
durability rule from `docs/architecture.md`, and it has already bitten this
project once — `break_eligible` shipped with an HA entity and a parser but no
bulk-document schema, so every application of the retained config document
silently cleared it. Therefore:

1. `main/ha_config.c` — the field registry (drives discovery, the `cfg` state
   JSON, and `set/<key>` apply).
2. `main/config_apply.c` — parsed from the **bulk retained document**.
3. `docs/home_assistant.md` — documented.

`ota_on_sync` is a boolean. The registry has switch support only via the
slot-bound `CFG_TRELOAD`/`CFG_TBREAK` kinds, so this needs a generic `CFG_BOOL`
kind (component `"switch"`, stored as u16 0/1) — a small, host-tested addition
to `ha_config.c`.

**`DISC_SCHEMA_VER` in `main/mqtt_ha.c` must be bumped (16 → 17).** New
entities do not appear in Home Assistant otherwise.

Add to the stat payload (`stats_json.c`), alongside the existing `fw` and
`reset_reason` fields:

- `ota_result` — the reason code from the failure table.
- `ota_target` + `ota_fails` — what it is retrying and how close it is to
  giving up.
- `ota_dl_ms` — last download duration, so a link trending toward the
  deadline is visible before it becomes chronic.

This tree already treats MQTT as the channel that survives when the USB console
does not; a failed overnight update is exactly the kind of event that is
otherwise invisible.

`sdkconfig.defaults` gains `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and the
OTA URL default. Note the standing hazard: `sdkconfig` is gitignored and
carries hand-set values, so the regenerated config must be diffed against
`sdkconfig.bak` after any change here.

---

## TDD contract

Per the project's process rules, tests come first. The pure module carries the
weight.

**`test_ota_policy`** — schema selection, resolution and decision:

*Block selection (the array):*
- single block, schema 1 = selected
- two blocks, both supported = **highest** schema wins
- two blocks, newest unsupported = falls back to the older supported one
  (this is the rolling-fleet property; it is the reason the array exists and
  it must be pinned by a test)
- no supported block = no update, reports `no_schema`
- duplicate `schema` values = first wins, no crash
- unknown keys inside a supported block are **ignored, not fatal** — the
  forward-compatibility guarantee, and the easiest one to regress
- top level not an array = no update

*Resolution:*
- default entry applies when the device is not listed
- device entry overrides the default
- device entry with `version` null/absent = pinned, no update
- version equal to running = no update
- version different (higher **and lower**) = update — the downgrade case is
  deliberate and must be pinned by a test, not left to be "fixed" later
- malformed JSON, truncated JSON, empty body = no update, no crash
- missing `url`, empty `url`, non-`https://` `url` = rejected
- oversized version string (> 31 chars) = rejected

*Preconditions and the retry budget:*
- no NTP → skip; battery below 30 % → skip; charge-locked → skip; empty
  endpoint → skip
- `ota_fails` below the budget → attempt
- `ota_fails` at the budget for the **same** target → give up, report
  `gave_up`
- `ota_fails` at the budget but a **different** target → counter resets,
  attempt
- error-to-reason-code mapping: a TLS cert error yields `tls_cert` and not
  the generic `tls` (the two have different fixes, so the distinction is
  behaviour, not logging detail)

**`test_ota_flow`** — sequencing, with injected effects:
- check runs at rollover; does not run on a plain tick wake
- check runs on Button D iff `ota_on_sync` is set
- the check runs **after** the snapshot rendezvous and **before** the MQTT
  phase, so its result reaches the same window's stat payload
- paint happens **after** the check window closes and **before** the download
  window opens — assert the ordering, since it is the brownout contract
- the failsafe extender is called **before** the download window opens, not
  during it
- a download that exceeds the deadline aborts cleanly: no set-boot-partition,
  no restart, `ota_fails` incremented
- download failure repaints the normal screen and does not reboot
- download success calls set-boot-partition then restart, in that order
- a failure result persists to NVS and appears in the *next* window's payload
- no OTA pending = no extra window opened (the common path costs nothing)

**`test_display_render`** — golden for `display_screens_build_ota()`, plus
regenerated main-screen goldens carrying a fixed injected version string.

**`test_config_apply` / `test_ha_config`** — the two new fields round-trip
through the bulk document and the per-entity set path, and a non-`https://`
URL is rejected by both.

Sanitizers are on for every host suite (`-fsanitize=address,undefined`), which
is what gives the malformed-JSON cases teeth.

---

## Tasks

**Status (2026-08-11):** tasks 1-9 are implemented, reviewed and committed on
`worktree-ota-plan` — `d7e28f6` (partition freeze + `version.txt`), `6f61f50`
(config surface + the HA `sw`-staleness fix), `b084917` (`ota_policy`),
`6f5d618` (version on panel + update screen), plus a follow-up fix round on
the display package. 35/35 host suites green; firmware 1,436,928 B, 22 % free.
Tasks 10-17 remain.

0. Cut `feature/ota` from `integration`.
1. **Measure.** Add `esp_http_client`, `esp_https_ota`, `app_update` to
   `main/CMakeLists.txt` `REQUIRES`; build; record the real image growth.
   *Nothing else in this list is safe to order before this one.*
2. **Freeze the partition table** at the chosen `0x1C0000` layout (confirm the
   measurement leaves it comfortable). Update `partitions.csv`, the
   `sdkconfig.defaults` comment, and `tools/flash_assets.sh`'s size/duration
   header; note the WAV re-flash requirement in `docs/developer_setup.md`.
3. Add `version.txt`; verify `esp_app_get_description()->version` reads
   `1.5.0` and the HA `fw` sensor agrees.
4. `test_ota_policy` **first**, then `main/ota_policy.c` — schema-array
   selection, resolution, preconditions, retry budget, reason-code mapping.
5. NVS keys + accessors + defaults + `credentials.local.h` slot; the five new
   Kconfig symbols. Keep the OTA keys out of the defaults fingerprint.
6. `CFG_BOOL` kind in `ha_config.c`; register `ota_url` + `ota_on_sync`; add
   them to `config_apply.c`'s bulk parser; bump `DISC_SCHEMA_VER` to 17;
   update `docs/home_assistant.md`.
7. `https://` validator in `config_validate.c`, with tests.
8. `display_state_t.fw_version` → `app_state_display()` → battery row.
   Regenerate main-screen goldens; `git checkout` the untouched ones.
9. `display_screens_build_ota(from, to)` + `display_ota(from, to)` + golden.
10. `test_ota_flow` **first**, then `main/ota_flow.c` with its injected ops.
    Three constraints the display review surfaced, all of which `test_ota_flow`
    must pin rather than leave to convention:
    - **The OTA paint must happen with the radio down.** `display_ota()` carries
      no `net_window_active()` guard, and its flush blocks for up to
      `ssd1680_refresh_wait()` plus a full refresh. Called inside an open
      window it walks straight into the brownout this project already paid for
      (see `net_window.c:65-79`). Pin the order: window joined →
      `wifi_session_end()` → paint → new session.
    - **Paint before `wifi_session_begin()` AND before the failsafe extension**,
      or ~3 s of panel time is charged to the 180 s `MAX_AWAKE_SEC` budget
      instead of to `OTA_MAX_SEC`.
    - **The failure path must force a full refresh.** `display_ota()` zeroes
      `s_partial_count`, so the next `display_update()` would repaint a
      full-screen 28 pt takeover with a *partial* refresh. Use the mechanism
      task 9 left for this; the precedent is `lock_gate.c:49-54`.
11. `main/ota.c`: manifest GET, then the **incremental** OTA
    (`esp_https_ota_begin` / `_perform` loop / `_finish`) with the deadline
    check in the loop, plus mark-valid. Embed the CA PEM.
12. Wire the rollover trigger, the Button D trigger, the pre-sleep apply point,
    and the failsafe extender install in `app_main`.
13. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; call `ota_mark_valid_if_pending()`
    from the pre-sleep point. **Measure the bootloader before flipping this
    symbol**: it is currently 22,640 of 28,672 B (79 %, 6,032 B free), rollback
    support grows it, and the 2nd-stage bootloader at `0x1000`-`0x8000` is as
    un-updatable as the partition table.
    Also promote the **first paint after a successful OTA reboot** to a full
    refresh: `s_partial_count` is `RTC_DATA_ATTR` and survives `esp_restart()`,
    so the panel comes back still showing the update screen and would otherwise
    repaint it partially.
14. OTA result / target / fail-count / download duration into the stat payload.
15. Build-size guard (warn at 85 % slot occupancy).
16. Hardware smoke test (below).
17. Update `docs/architecture.md`: module table, layer lists, a subsystem note
    for the two-window design and the timeout/retry mechanism, and the new
    stat fields.

Task 12 is the one that touches `main.c`, and it should be a **single
branch-free line** installing the extender. Anything more than that in `main.c`
needs to name one of the four residency reasons at review, or move.

---

## Hardware smoke test

1. Flash the new partition table over USB; confirm NVS survived (config intact,
   device keeps its HA identity) and re-flash the WAV if one was installed.
2. Confirm the version string renders on the main screen and is legible at
   12 pt next to the battery percentage.
3. Point `ota_url` at a manifest whose version equals the running version →
   confirm **no** second window opens and the wake is no longer than usual.
4. Publish a different version → confirm the update screen paints (both
   versions legible), the download runs with the panel idle, and the device
   reboots into the new version.
5. Confirm the post-reboot main screen shows the new version.
6. **Rollback:** deliberately publish a build that panics early; confirm the
   bootloader reverts and the device comes back on the previous version.
   Also confirm a normal deep-sleep wake does **not** consume the
   pending-verify state before the first full wake marks it valid.
7. **Targeting:** add the test device to `devices` with a distinct version;
   confirm it updates and a second device does not.
8. **Downgrade:** publish the older version; confirm it applies.
9. **Schema array:** publish a manifest carrying a schema-1 block and a
   fabricated schema-99 block; confirm the device reads schema 1 and ignores
   the unknown block rather than failing. Add an unknown key inside the
   schema-1 block and confirm it is still accepted.
10. **Timeout + retry budget:** throttle the host (or set
    `MAGTAG_OTA_MAX_SEC` very low) so the download cannot finish; confirm the
    abort is clean, `ota_fails` increments, and after 3 attempts the device
    reports `gave_up` and stops trying. Then publish a new version and confirm
    the counter resets and it attempts again.
11. **Failure reporting:** point at a host with a cert the device does not
    trust; confirm HA shows `tls_cert` and not a generic failure.
12. Pull WiFi mid-download; confirm the device recovers, repaints, sleeps, and
    retries at the next rollover.
13. Confirm `ota_on_sync=0` means Button D does not check, and `=1` means it
    does; confirm the Kconfig default seeds a fresh device correctly.
14. Confirm a check is skipped below 30 % battery.
15. Check the `net_win` and OTA task stack watermarks, and the reported
    download duration, in the logs.

---

## Risks

- **Flash headroom.** The dominant risk, and the reason task 1 is a
  measurement. Mitigated by re-partitioning now and guarding occupancy after.
- **Partition table is frozen at first flash.** Cannot be fixed by OTA. This
  is why the layout decision precedes everything else.
- **CA cert expiry or mis-pinning.** Bricks OTA fleet-wide, requires physical
  access. Mitigated by pinning a long-lived root and by designing the NVS
  override path even while deferring it.
- **A bad build that boots but misbehaves.** Rollback does not catch it.
  Mitigated by the targeting feature itself — that is what the test device is
  for, and it is why targeting is in the first release rather than a follow-up.
- **Golden-test churn** obscuring a real layout regression during regeneration.
  Mitigated by regenerating once, in its own commit, and reverting the
  incidentally-rewritten goldens.
- **Heap during TLS on a PSRAM-less board.** Mitigated by the free-heap
  precondition check and by running the download with MQTT closed.
- **A download that never fits the budget.** Raised in review: a device that
  cannot finish inside the failsafe retries daily forever, costing a window
  and a full refresh each time and achieving nothing. Mitigated by the
  three-part mechanism above — extend, deadline-abort, retry budget — and made
  visible by publishing the download duration before it becomes chronic.
- **Task watchdog during the download.** `CONFIG_ESP_TASK_WDT_TIMEOUT_S` is
  5 s with idle-task checking on CPU0. Mitigated by the incremental OTA API,
  which yields between chunks; the one-shot `esp_https_ota()` call is the
  shape that risks it.

## Non-goals

- Signed images, secure boot, flash encryption, anti-rollback efuses.

  **Nothing in this design requires image signing.** It was left open in case
  it turned out to be forced, and it does not: `esp_https_ota` verifies the
  image header, magic byte and chip target before switching the boot
  partition, so a corrupt or wrong-target download is rejected without a
  signature. Transport integrity and server authenticity come from the TLS
  requirement — a pinned CA already means only the intended host can serve
  an image. Signing would add authenticity *at rest*, which matters when the
  hosting itself is untrusted; here the same person controls the host, the
  manifest and the devices. The one place it would help is a compromised
  host, and that threat is better answered by the fact that the CA is pinned
  to a root the attacker would also have to control.
- Progress reporting on the panel — static screen only, by request.
- Delta/compressed updates.
- Automatic rollback on *behavioural* failure (only on boot failure).
- Semver ordering. "Different means apply" is the deliberate choice; see above.

## Settled in review (2026-08-11)

- **Partition layout: `0x1C0000`**, the aggressive option. 1.75 MB per app
  slot, `assets` down to 440 KB (~14 s of WAV at 16 kHz).
- **Version on the battery row**, folded into the existing label's string.
- **Update screen shows both versions** — current and the one being installed.
- **`ota_on_sync` is HA-settable *and* Kconfig-seeded**, along with the URL,
  battery floor, timeout and retry budget. Kconfig seeds, HA overrides.
- **Battery floor: 30 %.**
- **Rollback on failed boot: enabled.**
- **Manifest is an array of schema-versioned blocks**, so a fleet mid-upgrade
  reads one file and laggards stay reachable.
- **Every failure reports to HA**, with `tls_cert` distinguishable from a
  generic TLS or network error.
- **The timeout mechanism is three parts, not one**: extend the failsafe before
  the download, deadline-abort cleanly inside the incremental OTA loop, and
  give up after 3 failures against the same target version.

## Open decisions — all closed (2026-08-11)

1. **Wording: "Installing", not "Upgrading to".** The only one of these four
   that changed the design; folded into "The update screen" above and into the
   golden for `display_screens_build_ota`.
2. **Manifest transport: HTTPS for v1.** The retained-MQTT variant is deferred,
   not rejected. `ota_policy.c` still takes the manifest as a caller-supplied
   buffer, so switching transports later touches `ota.c` only.
3. **CA rotation escape hatch: not needed.** The chosen root is valid to 2035,
   so the embedded PEM outlives any plausible life of this firmware. `ota_ca`
   in NVS stays off the list.
4. **Channels: not schema 2.** The likelier use for a second schema is pushing
   **asset payloads (WAV files)** to the device rather than release channels —
   the manifest would carry an asset URL and the flow would write the `assets`
   partition instead of an app slot. Out of scope for v1, and the array is what
   makes it additive later. Note the interaction: the frozen `0x1C0000` layout
   leaves `assets` at 440 KB, which bounds anything that feature could push.
