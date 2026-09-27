# Developer Setup

This guide covers everything needed to get a working build environment for the MagTag Screen Timer firmware. All development happens inside a Docker dev container — no local toolchain installation required.

---

## Prerequisites

| Tool | Purpose |
|------|---------|
| [Docker Desktop](https://www.docker.com/products/docker-desktop/) (Linux/Windows) or Docker Engine (Linux) | Container runtime |
| [VS Code](https://code.visualstudio.com/) | Editor |
| [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) (`ms-vscode-remote.remote-containers`) | Opens the repo inside the container |

**Windows only**: USB device passthrough from WSL2 requires [`usbipd-win`](https://github.com/dorssel/usbipd-win). Install it on the Windows host, then see [USB Device Access](#usb-device-access) below.

**macOS note**: Docker Desktop for macOS does not support USB device passthrough. You cannot flash the MagTag directly from a macOS-hosted container. Options: use a Linux VM, or install ESP-IDF locally on the macOS host and flash with `idf.py -p <port> flash`.

---

## Getting Started

```bash
git clone <repo-url>
cd magtag-espidf
code .
```

VS Code will detect `.devcontainer/devcontainer.json` and prompt **"Reopen in Container"** — click it. The first image build:

1. Pulls `mcr.microsoft.com/devcontainers/cpp:1-ubuntu-22.04` (~1.5 GB).
2. Installs the ESP-IDF prerequisites (cmake, ninja, dfu-util, libusb, ...).
3. **Bakes ESP-IDF v6.0.1 + the ESP32-S2 toolchain into the image** (~2 GB download, one-time). Container rebuilds reuse the cached Docker layer and start with a working environment.

---

## Pre-Commit Hooks

Run once after cloning (inside the devcontainer):

```bash
pre-commit install
pre-commit install --hook-type commit-msg
```

Both commands are required — the second activates the commit-msg stage for the Conventional Commits validator.

**Commit message format** (enforced by hook):
```
<type>[(<scope>)][!]: <description>
```
Valid types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `perf`, `style`, `ci`, `build`, `revert`

**C formatting**: `.c`/`.h` files must pass `clang-format --dry-run --Werror` (vendored `test/unity/` is excluded). To auto-fix:
```bash
clang-format -i <file.c>
```

---

## Building

Activate the toolchain in each new terminal, then use `idf.py`:

```bash
source ~/esp/esp-idf/export.sh

idf.py build            # compile the firmware
idf.py fullclean        # wipe the build directory
```

`sdkconfig` is generated from `sdkconfig.defaults` (committed) and is gitignored — delete `sdkconfig` and rebuild to pick up changed defaults.

### Build-size guard

Every build runs `tools/check_slot_size.py` (hung off the `app` target from the
project-root `CMakeLists.txt`) and **fails the build** — it does not warn — once
the app image passes `MAGTAG_MAX_SLOT_PCT` percent of the `0x1C0000` app slot.
The threshold is currently **85**. A passing build prints the live figure
(`Build-size guard: image N B is X.X % of the … slot; M B left under the 85 %
guard`). A failing one prints image, guard limit and slot size instead of the
headroom, plus how far over you are — and it has **two** branches: over the
guard but still inside the slot, or over the slot itself, which says the image
*does not fit* and that raising the guard cannot help (the tool refuses a
`--max-pct` above 100, and 100 % is the slot).

It fails rather than warns on purpose. This repo has no CI, so a printed warning
has exactly one reader — whoever happens to be watching that build — and the
headroom it protects is **one-way**: an OTA writes an app slot and cannot
rewrite the partition table, so the slot size shipped with the first OTA-capable
firmware is the size those devices keep, short of a serial cable per device.
ESP-IDF's own check only speaks up at 5 % free, which is after the decision that
mattered.

`-D MAGTAG_MAX_SLOT_PCT=…` on the command line is deliberately **refused** with
a `FATAL_ERROR`. The threshold is a plain (non-cache) variable that shadows any
cache entry, so the flag would otherwise be silently ignored and the build would
fail at the same number with no hint why. Raising the guard is legitimate while
the image still fits the slot — it accepts a new floor — but it costs an edit
to `set(MAGTAG_MAX_SLOT_PCT …)` in `CMakeLists.txt`, in its own commit whose
message says what the bytes bought.
Nothing to clean up after a refusal **under `idf.py`**: it deletes
`CMakeCache.txt` whenever a configure fails, so just re-run without the flag.
Driving `cmake` directly the entry *does* persist — clear it with
`cmake -U MAGTAG_MAX_SLOT_PCT -B <build dir>`.

The arithmetic is covered from both sides by `test/test_check_slot_size/`, which
can drive the failure branch with synthetic sizes — something no real build can
do without first growing the image by tens of KB.

---

## Flashing & Monitoring

1. Connect the MagTag to your machine via USB-C.
2. Verify the device is visible inside the container: `ls /dev/ttyACM*` (should show `/dev/ttyACM0` or similar).
3. **Flash + monitor**:

```bash
source ~/esp/esp-idf/export.sh
idf.py -p /dev/ttyACM0 flash monitor    # Ctrl+] exits the monitor
```

**For ongoing log watching use `tools/monitor.sh` instead of `idf.py
monitor`** — the device deep-sleeps between wakes, and both a plain monitor
and ModemManager toggle DTR/RTS in ways the S2 native-USB console treats as
a reset (see the header comments in `tools/logcat.py`).

Console-over-native-USB caveats (inherent, not bugs):

- Output printed before the host opens the port is **dropped**. Short wakes
  (no WiFi sync, ~1.5 s of logging) often show few or no lines — the wake
  still happened; judge by the e-ink/LEDs. Wakes that sync WiFi log long
  enough to be captured.
- Lines can appear **truncated/merged** (`main_task: Calling aI (618)
  wifi:...`): the ROM CDC TX buffer is tiny and overflow bytes are silently
  discarded until the host starts draining. Lossy transport, not corruption.
- For complete boot logs, the fallback is a UART adapter on the debug pads.

See [hardware_smoke_test.md](hardware_smoke_test.md) for the on-device validation checklist.

### Custom alert WAV (assets partition)

The partition table reserves a raw 440 KB `assets` partition for an optional
alert sound, selectable in Home Assistant as the "Custom WAV" tone. The file
must be 16-bit mono PCM WAV at 8–22.05 kHz (440 KB ≈ 14 s at 16 kHz, ≈ 28 s
at 8 kHz):

```bash
source ~/esp/esp-idf/export.sh
tools/flash_assets.sh -p /dev/ttyACM0 path/to/tone.wav   # write
tools/flash_assets.sh -p /dev/ttyACM0 --erase            # wipe (falls back to chime)
```

An empty or invalid partition is harmless — the firmware plays the Gentle
chime instead.

**Re-partitioning invalidates a flashed WAV.** `assets` is addressed by name,
but its *offset* changed when the OTA app slots grew to 0x1C0000, and any
future table change can move it again. Flashing a new partition table leaves
whatever bytes were there at the old offset, so the partition reads as
empty/invalid and the tone silently falls back to the chime. Re-run
`tools/flash_assets.sh` on every device that has a custom alert tone after
reflashing the table.

---

## USB Device Access

The dev container runs with `--privileged` and mounts `/dev/bus/usb` from the host, giving it access to all USB devices attached to the host at runtime.

**Verify inside the container:**
```bash
ls /dev/ttyACM*   # native USB CDC (ESP32-S2 default)
ls /dev/ttyUSB*   # UART bridge (fallback)
```

**Linux host**: Works out of the box. If you get a permissions error, confirm your host user is in the `dialout` group (`sudo usermod -aG dialout $USER`, then log out and back in).

**Windows host (WSL2)**: Before opening the devcontainer, attach the MagTag USB device to WSL2:
```powershell
# In an elevated PowerShell on Windows
usbipd list                          # find the MagTag bus ID
usbipd attach --wsl --busid <ID>     # attach to WSL2
```
Then open the devcontainer normally; the device will appear as `/dev/ttyACM0` inside the container.

**macOS host**: Not supported — see [Prerequisites](#prerequisites).

---

## Toolchain Cache

ESP-IDF and the Xtensa toolchain live in the container **image** (`~/esp/esp-idf` and `~/.espressif`), not in a volume. Rebuilding the container reuses the cached Docker layer; only a `--no-cache` image rebuild re-downloads them. `docker builder prune` reclaims disk from stale build layers.

---

## Project Layout

The repo root is the ESP-IDF project.

| Path | Role |
|------|------|
| `CMakeLists.txt` | ESP-IDF project entry point |
| `sdkconfig.defaults` | ESP-IDF Kconfig defaults committed to the repo |
| `main/` | Main application component (all `.c` modules) |
| `main/idf_component.yml` | Managed dependencies (LVGL 9) |
| `include/` | Shared headers visible across components |
| `components/ssd1680/` | SSD1680 e-ink SPI driver component (custom, in-repo) |
| `test/` | Host-side unit tests (CMake + ctest + vendored Unity) |
| `docs/hardware_smoke_test.md` | On-device validation checklist |

See [architecture.md](architecture.md) for the module map, hardware target, and subsystem design notes; [ProductOverview.md](ProductOverview.md) for the product-level breakdown; and `docs/superpowers/specs/2026-07-03-display-rework-and-idf6-remediation-design.md` for the display-stack architecture (deltas from the original overview are tabled there).

---

## Running Tests

Host-side unit tests need no ESP-IDF toolchain — plain gcc + cmake:

```bash
cmake -S test -B test/build
cmake --build test/build
ctest --test-dir test/build --output-on-failure
```

Each suite in `test/test_*/` is a single-TU Unity program that `#include`s the module under test plus mocks from `test/mocks/`. Unity itself is vendored at `test/unity/` (v2.6.0, MIT).
