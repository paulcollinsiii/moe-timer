# OTA updates — HTTPS manifest, per-device targeting, on-panel feedback

Status: **plan only, nothing implemented.** Written 2026-08-10 against
`integration` (`bad81d8`).
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

| Layout | Slot size | Headroom after OTA (~1.49 MB) | `assets` | WAV @16 kHz |
| --- | --- | --- | --- | --- |
| Keep as-is `0x180000` | 1,572,864 | ~81 KB (5.3 % free) | 952 KB | ~30 s |
| **`0x1A0000` (recommended)** | **1,703,936** | **~209 KB (12.6 % free)** | **696 KB** | **~22 s** |
| `0x1C0000` (aggressive) | 1,835,008 | ~337 KB (18.8 % free) | 440 KB | ~14 s |

(Headroom assumes the +55 KB midpoint of the estimate above. Task 1 replaces
the estimate with a measurement before the table is frozen.)

Recommended table (`0x1A0000`). `nvs`, `phy_init` and `ota_0` keep their
current offsets, so an existing device's NVS and running app survive the
reflash exactly as the current file's comment promises; only `ota_1`,
`otadata` and `assets` move:

```
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   0x6000
phy_init,   data, phy,     0xf000,   0x1000
ota_0,      app,  ota_0,   0x10000,  0x1A0000
ota_1,      app,  ota_1,   0x1B0000, 0x1A0000
otadata,    data, ota,     0x350000, 0x2000
assets,     data, 0x40,    0x352000, 0xAE000
```

Alignment checks: app partitions land on 64 KB boundaries
(`0x10000`, `0x1B0000`), data partitions on 4 KB (`0x350000`, `0x352000`),
and the table ends exactly at `0x400000`.

Consequence to accept: **a re-partition invalidates the flashed WAV** — the
`assets` partition moves, so `tools/flash_assets.sh` must be re-run on any
device that has a custom alert tone. That is a one-time cost paid during the
same serial session that installs OTA support.

**Task 1 measures before this is committed.** Add the three components to
`REQUIRES`, build, and read the real number off `idf.py size` / the linked
`.bin`. The +40–60 KB above is an estimate; the layout choice should be made
against the measurement, not the estimate.

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

**Recommendation: extend the existing battery label's string.**

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
        │                                  │
        │      UPDATING FIRMWARE           │   28 pt
        │                                  │
        │         v1.5.0                   │   18 pt
        │                                  │
        │   Do not remove power            │   12 pt
        │                                  │
        └──────────────────────────────────┘
```

- `display_screens_build_ota(const char *version)` — pure LVGL, golden-tested.
- `display_ota(const char *version)` in `display.h`/`display.c` — full refresh,
  same shape as `display_charge_me()` / `display_bedtime()`.

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

**Make the flag runtime, not build-time.** A Kconfig symbol would mean
reflashing over USB to toggle a testing convenience — which defeats the
purpose of the feature it is testing. Use an NVS-backed boolean surfaced as an
HA switch (`ota_on_sync`, default off), matching how every other operational
knob in this tree is exposed.

### What must gate the check

All pure, all in `ota_policy.c`, all host-tested:

- **NTP settled this window.** X.509 validity checking needs a correct clock.
  A device with a bad clock will fail the handshake, so checking is wasted
  radio time — and it is the failure mode that looks like a broken server.
- **Battery above a floor** (suggest 30 %) and not charge-locked. A sustained
  radio burst on a weak cell risks a brownout reset.
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
  │  wifi_session_begin → SNTP → snapshot rendezvous → MQTT    │
  │  → OTA CHECK: one HTTPS GET of the manifest (~300 B)       │
  │    parse, decide, BUFFER {version, url}                    │
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

```json
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
}
```

Resolution order, pure and host-tested:

1. Look up `devices[device_id()]`. `device_id()` already returns
   `magtag-xxxxxx` derived from the WiFi STA MAC — a stable, zero-config
   targeting key that needs no new state on the device.
2. Otherwise use `default`.
3. Update iff the resolved `version` differs from the running version.
4. A resolved entry with `"version"` absent or `null` means **pinned** — do
   nothing. This is how a device is frozen without deleting its entry.

`schema` exists so a future format change is detectable rather than silently
misparsed; an unrecognised `schema` means "do nothing", which fails safe.

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

  **Verdict: start with `devices` + `default`.** It directly expresses the
  stated use case ("override a default") with zero new config plumbing, and
  `ota_policy_resolve()` can grow a channel lookup between steps 1 and 2 later
  without disturbing anything around it. This is the plan's one genuinely
  arguable call, and it is cheap to revisit because it lives behind a pure
  function.

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
prevent. Strongly recommended.

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

| Failure | Behaviour | Recovery |
| --- | --- | --- |
| No WiFi / no NTP | Check skipped | Next rollover |
| Manifest 404 / malformed / unknown `schema` | Logged, published to HA, no update | Next rollover |
| Cert validation fails | `esp_https_ota` errors out, screen repaints | Next rollover; if permanent, serial reflash |
| Download interrupted (power, failsafe) | Inactive slot holds a partial image; boot partition **not** switched | Next rollover retries from scratch |
| Image header invalid / wrong chip | `esp_https_ota` rejects before switching | Next rollover |
| New app boots and crashes | Bootloader rolls back (if enabled) | Automatic |
| New app boots but is broken (no crash) | Not detected | Republish the old version in the manifest |
| Manifest points at a downgrade | Applied — this is deliberate | Republish |

The last row is the accepted cost of the "different, not newer" test.

---

## Architecture

Three new modules, placed by the layer model in `docs/architecture.md`:

| Module | Layer | Responsibility | Test |
| --- | --- | --- | --- |
| `main/ota_policy.c` | **1 — pure** | Manifest parse, device targeting, "should update?" decision, precondition gating. Total function over its arguments; no clock, no NVS, no ESP includes. Uses cJSON, as `config_apply.c` already does. | `test_ota_policy` — host, direct |
| `main/ota_flow.c` | **2 — orchestration** | Sequences check → buffer → paint → download → reboot. Device effects injected via an ops struct, exactly like `net_apply_ops_t`. | `test_ota_flow` — host, single-TU with stubs |
| `main/ota.c` | **3 — driver** | `esp_http_client` manifest GET, `esp_https_ota` download, `esp_ota_mark_app_valid_cancel_rollback`. Thin — every decision is already made upstream. | Hardware smoke test |

`main/main.c` gains **nothing**. Under the residency rule the only candidate
would be an awake-failsafe extension for the download, and that is handled by
mirroring the existing `alerts_set_extend_awake()` pattern: `ota_flow` receives
the extender as an injected function pointer, `app_main` installs it in one
branch-free line, and the `esp_timer` handle stays where it already lives.

### The awake failsafe

`CONFIG_MAGTAG_MAX_AWAKE_SEC` is 180 s. A ~1.5 MB HTTPS download on an
ESP32-S2, with TLS and flash writes, should run 15–30 s — comfortably inside
the cap. But a slow link would have the failsafe fire mid-download, forcing
deep sleep with a partial image (harmless, but a wasted download).

Extend the failsafe to ~300 s for the duration of the download window via the
injected extender, and restore it afterwards. Note that `MAX_AWAKE_SEC`'s
Kconfig `range` is 30–600 — the extender writes the timer directly and is not
bound by it, but staying inside 600 s keeps the two consistent.

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

| Key | Type | Default | Purpose |
| --- | --- | --- | --- |
| `ota_url` | str | `CONFIG_MAGTAG_OTA_URL` | Manifest endpoint. Empty = OTA disabled. |
| `ota_on_sync` | u16 (0/1) | 0 | Also check on every Button D full sync. |
| `ota_result` | str | `""` | Last attempt outcome, for HA visibility. |

Accessors in `nvs_config.c`/`.h` following the existing shape, plus defaults in
`include/nvs_defaults.h` and a `credentials.local.h` slot for the URL — same
treatment `MAGTAG_MQTT_URI` gets, so an endpoint never lands in a committed
`sdkconfig.defaults`.

**Both new settable fields must go in three places, not one.** This is the
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

Also worth adding to the stat payload (`stats_json.c`): the OTA result string,
alongside the existing `fw` and `reset_reason` fields. This tree already treats
MQTT as the channel that survives when the USB console does not — a failed
overnight update is exactly the kind of event that is otherwise invisible.

`sdkconfig.defaults` gains `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and the
OTA URL default. Note the standing hazard: `sdkconfig` is gitignored and
carries hand-set values, so the regenerated config must be diffed against
`sdkconfig.bak` after any change here.

---

## TDD contract

Per the project's process rules, tests come first. The pure module carries the
weight.

**`test_ota_policy`** — resolution and decision:
- default entry applies when the device is not listed
- device entry overrides the default
- device entry with `version` null/absent = pinned, no update
- version equal to running = no update
- version different (higher **and lower**) = update — the downgrade case is
  deliberate and must be pinned by a test, not left to be "fixed" later
- unknown / missing `schema` = no update
- malformed JSON, truncated JSON, empty body = no update, no crash
- missing `url`, empty `url`, non-`https://` `url` = rejected
- oversized version string (> 31 chars) = rejected
- preconditions: no NTP → skip; battery below floor → skip; charge-locked →
  skip; empty endpoint → skip

**`test_ota_flow`** — sequencing, with injected effects:
- check runs at rollover; does not run on a plain tick wake
- check runs on Button D iff `ota_on_sync` is set
- paint happens **after** the check window closes and **before** the download
  window opens — assert the ordering, since it is the brownout contract
- download failure repaints the normal screen and does not reboot
- download success calls set-boot-partition then restart, in that order
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

0. Cut `feature/ota` from `integration`.
1. **Measure.** Add `esp_http_client`, `esp_https_ota`, `app_update` to
   `main/CMakeLists.txt` `REQUIRES`; build; record the real image growth.
   *Nothing else in this list is safe to order before this one.*
2. **Freeze the partition table** against that measurement. Update
   `partitions.csv` and the `sdkconfig.defaults` comment; note the WAV
   re-flash requirement in `docs/developer_setup.md`.
3. Add `version.txt`; verify `esp_app_get_description()->version` reads
   `1.5.0` and the HA `fw` sensor agrees.
4. `test_ota_policy` **first**, then `main/ota_policy.c`.
5. NVS keys + accessors + defaults + `credentials.local.h` slot.
6. `CFG_BOOL` kind in `ha_config.c`; register both fields; add them to
   `config_apply.c`'s bulk parser; bump `DISC_SCHEMA_VER` to 17; update
   `docs/home_assistant.md`.
7. `https://` validator in `config_validate.c`, with tests.
8. `display_state_t.fw_version` → `app_state_display()` → battery row.
   Regenerate main-screen goldens; `git checkout` the untouched ones.
9. `display_screens_build_ota()` + `display_ota()` + golden.
10. `test_ota_flow` **first**, then `main/ota_flow.c` with its injected ops.
11. `main/ota.c`: manifest GET, `esp_https_ota`, mark-valid. Embed the CA PEM.
12. Wire the rollover trigger, the Button D trigger, the pre-sleep apply point,
    and the failsafe extender install in `app_main`.
13. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; call `ota_mark_valid_if_pending()`
    from the pre-sleep point.
14. OTA result into the stat payload.
15. Build-size guard (warn at 85 % slot occupancy).
16. Hardware smoke test (below).
17. Update `docs/architecture.md`: module table, layer lists, a subsystem note
    for the two-window design, and the OTA-result stat field.

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
4. Publish a different version → confirm the update screen paints, the download
   runs with the panel idle, and the device reboots into the new version.
5. Confirm the post-reboot main screen shows the new version.
6. **Rollback:** deliberately publish a build that panics early; confirm the
   bootloader reverts and the device comes back on the previous version.
7. **Targeting:** add the test device to `devices` with a distinct version;
   confirm it updates and a second device does not.
8. **Downgrade:** publish the older version; confirm it applies.
9. Pull WiFi mid-download; confirm the device recovers, repaints, sleeps, and
   retries at the next rollover.
10. Confirm `ota_on_sync=0` means Button D does not check, and `=1` means it
    does.
11. Check the `net_win` and OTA task stack watermarks in the logs.

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

## Non-goals

- Signed images, secure boot, flash encryption, anti-rollback efuses.
- Progress reporting on the panel — static screen only, by request.
- Delta/compressed updates.
- Automatic rollback on *behavioural* failure (only on boot failure).
- Semver ordering. "Different means apply" is the deliberate choice; see above.

## Open decisions

1. **Partition layout** — `0x1A0000` recommended, pending task 1's
   measurement. Confirm the shortened WAV budget (~22 s @16 kHz) is acceptable.
2. **Targeting shape** — `devices` map recommended for v1; channels are the
   documented upgrade path if manifest edits per release become tedious.
3. **Manifest transport** — HTTPS for v1; the retained-MQTT variant is
   strictly cheaper on the wire and the policy module is designed to accept
   either. Worth revisiting after the first few releases.
4. **Version placement** — combined with the battery label vs. a separate
   label on the same row. Settle by looking at it on hardware.
5. **CA rotation escape hatch** (`ota_ca` in NVS) — deferred; confirm that is
   acceptable given the chosen root's expiry date.
