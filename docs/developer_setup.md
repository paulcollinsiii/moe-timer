# Developer Setup

This guide takes you from a fresh clone to a firmware image. It covers the dev
container, where credentials and settings go, and building, flashing, running
the host tests and releasing an image over the air. Everything runs inside a
Docker dev container, so you don't install a toolchain on your machine.

The [README](../README.md) has the short version of the first flash. This doc
adds the details behind it.

---

## Prerequisites

| Tool | Purpose |
|------|---------|
| [Docker Desktop](https://www.docker.com/products/docker-desktop/) (Windows) or Docker Engine (Linux) | Container runtime |
| [VS Code](https://code.visualstudio.com/) | Editor |
| [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) | Opens the repo inside the container |

The container mounts the host's `/dev` and `/dev/bus/usb`, so it expects a
Linux host. On Windows, run it under WSL2 and pass the board through with
[`usbipd-win`](https://github.com/dorssel/usbipd-win) (see
[USB device access](#usb-device-access)). Docker Desktop for macOS cannot pass
USB through. There, flash from a Linux VM, or install ESP-IDF 6 on the Mac and
run `idf.py -p <port> flash` there.

## Opening the container

```bash
git clone <repo-url>
cd magtag-espidf
code .
```

VS Code finds `.devcontainer/devcontainer.json` and offers **Reopen in
Container**. The first image build takes a while: it downloads about 2 GB to
bake ESP-IDF v6.0.1 and the ESP32-S2 toolchain into the image, along with `uv`
for the Python tools. The toolchain lives in the image (`~/esp/esp-idf`,
`~/.espressif`), not in a volume, so a container rebuild reuses the cached
layer. Only a `--no-cache` image rebuild downloads it again, and
`docker builder prune` reclaims the space from stale layers.

Activate the toolchain in each new terminal:

```bash
source ~/esp/esp-idf/export.sh
```

## USB device access

The container runs `--privileged` with the host's USB devices mounted. Check
that the board is visible inside it:

```bash
ls /dev/ttyACM*   # native USB CDC, the ESP32-S2 default
ls /dev/ttyUSB*   # a UART adapter, if you use one
```

- **Linux:** works as is. On a permissions error, add your host user to
  `dialout` (`sudo usermod -aG dialout $USER`), then log out and back in.
- **Windows (WSL2):** share the board once from an elevated PowerShell, then
  attach it to WSL2 before opening the container, and it shows up as
  `/dev/ttyACM0`. Sharing survives reboots, but attaching does not: attach
  again whenever the board drops out of `/dev` (a reboot, a replug or a
  reset).

  ```powershell
  usbipd list                          # find the MagTag's bus ID
  usbipd bind --busid <ID>             # once, as administrator
  usbipd attach --wsl --busid <ID>
  ```

---

## Configuration and credentials

### Where settings live

- **WiFi and the MQTT broker are not build-time settings.** The device owner
  enters both on the device, in [setup mode](behavior/setup_mode.md), and they
  live only in NVS. No file or menu in the build carries them.
- **`include/credentials.local.h`** holds one optional value: the OTA manifest
  URL. Copy it from `include/credentials.local.h.example`. It is gitignored.
- **`idf.py menuconfig`**, under the **MagTag Timer** menu, holds every other
  build-time default: allocations, timer slots, breaks, quiet hours, alarms,
  the setup-mode knobs and so on. Home Assistant overrides most of them once
  the device is in HA. The menu also has an OTA URL field. Leave it empty:
  it is used only when `credentials.local.h` leaves the key undefined, and
  anything typed there ends up in `sdkconfig`.
- **The dashboard generator** needs the broker address for `--mqtt`, and the
  build does not carry it. Keep it in a small file outside the repo:
  [From the broker](home_assistant/dashboard.md#from-the-broker---mqtt).

### What a flash does to NVS

The compiled defaults only *seed* NVS. On every boot the firmware compares a
fingerprint of the seeded defaults with the one stamped in NVS. The seeded
defaults are the four allocations (weekday, weekend, holiday, summer). When
the fingerprint changes, the firmware reseeds: it rewrites those values and the
holiday list, and clears the applied HA config version, so HA's retained
config is applied again in the next network window. A reseed never touches the
stored WiFi and MQTT keys.

So to change an allocation default, edit it, rebuild and reflash. **Never
erase NVS** to pick up new values. `idf.py erase-flash` or an erased `nvs`
partition also wipes the HA config, the chore ticks, the timer table and the
saved timer state, and it wipes the WiFi and MQTT credentials. Nothing
restores those: the device comes back with no SSID and opens setup mode, and
the owner enters them again.

The OTA URL is the exception to seeding. It is not in the fingerprint, and it
is only the fallback while NVS holds no URL. To change it on a device that
already has one, set it from HA (see [ota_manifest.md](ota_manifest.md)).
`include/nvs_defaults.h` includes `credentials.local.h` only if it exists,
and an incremental build does not notice a file created after the first
build, so create the file before you build, or run `idf.py fullclean` after.

A `credentials.local.h` that still defines `NVS_DEFAULT_WIFI_*` or
`NVS_DEFAULT_MQTT_*` gets a build notice until those lines are removed. An
`sdkconfig` that still holds `CONFIG_MAGTAG_MQTT_*` may hold the only copy of
the broker: save those lines before the first build, because the next CMake
configure drops them ([below](#sdkconfig-holds-your-hand-set-values)). Keep
them in the [broker file](home_assistant/dashboard.md#from-the-broker---mqtt).

### `sdkconfig` holds your hand-set values

`idf.py menuconfig` writes to `sdkconfig`, which is gitignored.
`sdkconfig.defaults` is committed, and it holds only a few of the options. So
any value you set in menuconfig exists only in your `sdkconfig`, and
deleting or regenerating the file silently drops it.

Copy it before you pull changes to `main/Kconfig.projbuild` or
`sdkconfig.defaults`:

```bash
cp sdkconfig sdkconfig.bak   # gitignored
```

After the pull, run `idf.py reconfigure` before `idf.py build`, because a
plain build can compile a new option's `#else` fallback without warning.
Then diff `sdkconfig` against your copy: it should differ only in the options
the pull added or removed.

**Which file wins** follows one rule. kconfgen loads `sdkconfig.defaults`
first, then `sdkconfig`:
- A `sdkconfig` line under a `# default:` marker holds the Kconfig default.
  Nothing set it, so a changed `sdkconfig.defaults` line wins over it.
- Any unmarked line is a user value, and it wins over `sdkconfig.defaults`.
  That includes a value set in menuconfig or by hand, **and a value an earlier
  `sdkconfig.defaults` line put there**: once a configure has applied a
  `sdkconfig.defaults` line, `sdkconfig` holds it unmarked. So a change to an
  existing `sdkconfig.defaults` line does not reach a checkout that has already
  configured with the old line.

**When it is applied.** `sdkconfig.defaults` is read whenever CMake configures:
on `idf.py reconfigure`, and on any `idf.py build` that follows a change to a
`CMakeLists.txt`, an `idf_component.yml` or `sdkconfig` itself. A build after a
change to `sdkconfig.defaults` or `Kconfig.projbuild` alone does not
re-configure, so it can pass while it quietly runs the old configuration.

To pick up one changed default whose `sdkconfig` line is unmarked, change that
one value in menuconfig. Don't delete the file. To see what a reconfigure
would do before you let it, run it on a copy and read the symbol back:

```bash
cp sdkconfig /tmp/sdkconfig.probe
idf.py -B /tmp/build.probe -D SDKCONFIG=/tmp/sdkconfig.probe reconfigure
```

What was actually compiled is in `build/config/sdkconfig.h`.

---

## Building

```bash
idf.py build       # compile the firmware
idf.py fullclean   # wipe the build directory
```

The first build (or `idf.py reconfigure`) downloads the managed components,
LVGL and esp-mqtt, into `managed_components/`. cJSON is vendored in
`lib/cJSON/`.

### Build-size guard

Every build runs `tools/check_slot_size.py`. It fails the build once the app
image passes 85 % of the 0x1C0000 app slot, and a passing build prints the
headroom left. The slot size can never change on a device that takes OTA
updates (see [the two one-way constraints](architecture/hardware.md#the-two-one-way-constraints)), so headroom is
one-way. To raise the limit, edit `set(MAGTAG_MAX_SLOT_PCT 85)` in the root
`CMakeLists.txt` in its own commit, and say in the message what the bytes
bought. `-D MAGTAG_MAX_SLOT_PCT=…` is refused, and nothing above 100 is
accepted. The comment above that line explains why.

## Flashing and monitoring

1. Put the MagTag in download mode: hold **Boot**, press **Reset**, release
   **Boot**.
2. Flash:

   ```bash
   idf.py -p /dev/ttyACM0 flash
   ```

   Use `flash`, not `app-flash`. `flash` also rewrites `otadata`, so the new
   image boots even on a device that has switched slots after an OTA
   update. `app-flash` writes only the first slot.
3. If the device doesn't start once flashing finishes, press **Reset**. It can
   take a few presses.
4. Watch the log with `tools/monitor.sh [port]` (Ctrl+C quits).

Don't use `idf.py monitor` on a running device. It toggles DTR/RTS every time
it reopens the port, and the S2's native USB console takes that as a reset
request. So the device reboots about a second into every wake. `monitor.sh`
keeps those lines steady and reconnects across deep sleep.

The native USB console loses output by design. Lines printed before the port
reopens after a wake are dropped, so a short wake may log nothing. Judge it
by the panel and the LEDs. Lines can also come out truncated or merged. For a
complete boot log, use a UART adapter on the debug pads.

To check a build on real hardware, work through the
[hardware checklist](hardware_checklist.md).

## Custom alert WAV

The `assets` partition can hold a WAV that HA offers as the **Custom WAV**
alert tone. The file must be 16-bit mono PCM at 8–22.05 kHz, and it must fit
the 376 KB partition: about 12 s at 16 kHz, or 24 s at 8 kHz.

```bash
tools/flash_assets.sh -p /dev/ttyACM0 path/to/tone.wav   # write
tools/flash_assets.sh -p /dev/ttyACM0 --erase            # wipe
```

An empty or invalid partition is harmless. The firmware plays the Gentle
chime instead. After you flash a changed partition table, run
`flash_assets.sh` again, because the table can move `assets` and leave the
WAV behind at the old offset.

---

## Running host tests

The host tests build with plain gcc and CMake. They do need LVGL's source in
`managed_components/`, which only `idf.py` fetches, so run one `idf.py build`
(or `idf.py reconfigure`) before the first `cmake -S test`.

```bash
cmake -S test -B test/build
cmake --build test/build
ctest --test-dir test/build --output-on-failure
ctest --test-dir test/build -R '^test_timer$'    # one suite
```

Each C suite in `test/test_*/` is a single-file Unity program. It
`#include`s the module under test and the mocks from `test/mocks/`. Unity is
vendored at `test/unity/`. The Python tools' suites run in the same sweep, and
`test_gen_ha_dashboard` runs under `uv`.

## Releasing firmware (OTA)

Devices poll an OTA manifest, a static JSON file that names the version each
device should run. [ota_manifest.md](ota_manifest.md) covers its format and
the rules devices follow.

1. **Tag the release.** The image's version is
   `git describe --always --tags --dirty`, and the manifest must name that
   exact string. Build from a clean, tagged commit, so the version reads
   `1.5.6` and not `1.5.6-dirty` or `1.5.5-3-g1a2b3c4`.
2. **Build:** `idf.py build`.
3. **Update your manifest file** (by default `moes-timer.json` in the
   current directory; `-m` points elsewhere). Set `version` to the new
   version and `url` to `…/magtag_timer-<version>.bin`, on the `default`
   entry, or on one device's entry for a staged rollout.
4. **Publish** with `tools/push_firmware.sh`, dry run (`-N`) first. The script
   reads the version out of the built image and uploads
   `magtag_timer-<version>.bin` and `.elf`, then the manifest under its own
   name. The image goes up first, so a failed upload never advertises a
   missing file. If `jq` is installed, the script also refuses a manifest
   that doesn't match the image (`-f` overrides).

`push_firmware.sh` targets a Kubernetes pod through `kubectl`. It defaults to
the owner's setup: namespace `firmware`, selector `app=firmware`, destination
`/srv/firmware`. Override these with `-n`/`-l`/`-d`. Any other HTTPS host
works too: copy the same three files there under the same names. The device
must trust the host's certificate, which means its root CA is the one built
into the image ([OTA Setup](ota_manifest.md)).

## Pre-commit hooks

Install both hook types once, inside the container:

```bash
pre-commit install
pre-commit install --hook-type commit-msg
```

The second one enables the commit-message check. Messages follow
`<type>[(<scope>)][!]: <description>`, where the type is one of `feat`, `fix`,
`docs`, `refactor`, `test`, `chore`, `perf`, `style`, `ci`, `build` or
`revert`. The other hooks run `clang-format --dry-run --Werror` and `cppcheck`
on `.c`/`.h` files, plus the repo's own checks in `scripts/`. They also block
commits to `main`. The vendored `test/unity/` and `lib/cJSON/` are exempt. To
fix formatting, run `clang-format -i <file>`.

---

## Tools

| Tool | What it is for |
|------|----------------|
| `tools/monitor.sh` | Watch the serial log across deep sleep without resetting the device. |
| `tools/logcat.py` | The reader behind `monitor.sh`. Its header explains the USB reset trap. |
| `tools/flash_assets.sh` | Write or erase the custom alert WAV in the `assets` partition. |
| `tools/push_firmware.sh` | Publish a built image, its ELF and the OTA manifest. |
| `tools/check_slot_size.py` | The build-size guard. Every build runs it. |
| `tools/gen_ha_dashboard.py` | Print the HA setup steps, the config-publishing automation and the dashboard YAML. Run with `uv run`. Usage: [Running it](home_assistant/dashboard.md#running-it). |
| `tools/ha_devices.example.yaml` | Template for `tools/ha_devices.yaml` (gitignored), the device list `gen_ha_dashboard.py` reads. |
| `tools/ha/magtag_publish_config.yaml` | The HA automation that publishes each device's config document. Install: [Setup step 4](home_assistant/setup.md#4-install-the-config-publishing-automation). |

## Project layout

The repo root is the ESP-IDF project.

| Path | Role |
|------|------|
| `CMakeLists.txt` | Project entry point and the build-size guard. |
| `sdkconfig.defaults` | Committed Kconfig defaults. `sdkconfig` itself is gitignored. |
| `partitions.csv` | The flash layout. The app slot size is frozen. |
| `main/` | The application component: every firmware `.c` module and `Kconfig.projbuild`. |
| `main/idf_component.yml` | Managed dependencies (LVGL 9, esp-mqtt). |
| `include/` | Headers shared across components, plus `credentials.local.h`. |
| `components/ssd1680/` | The in-repo e-ink panel driver. |
| `lib/cJSON/` | Vendored cJSON. |
| `test/` | Host tests (CMake, ctest, vendored Unity). `test/difftest/` is the refactor differential sweep. |
| `tools/` | Developer tools (see [Tools](#tools)). |
| `scripts/` | Checks the pre-commit hooks run. |
| `docs/` | The human docs, including the [hardware checklist](hardware_checklist.md). |
| `docs/agent_notes/` | Notes for coding agents. Humans can skip them. |
| `docs/planning/` | Plans and records. `implemented/` holds the finished ones. |

For how the firmware is put together, start at
[architecture.md](architecture.md).
