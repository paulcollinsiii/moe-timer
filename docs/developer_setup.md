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

**macOS note**: Docker Desktop for macOS does not support USB device passthrough. You cannot flash the MagTag directly from a macOS-hosted container. Options: use a Linux VM, or flash from the macOS host with `pio run -t upload` after installing Platform.io locally.

---

## Getting Started

```bash
git clone <repo-url>
cd magtag-espidf
code .
```

VS Code will detect `.devcontainer/devcontainer.json` and prompt **"Reopen in Container"** — click it. The first build:

1. Pulls `mcr.microsoft.com/devcontainers/cpp:1-ubuntu-22.04` (~1.5 GB).
2. Installs Platform.io CLI into the image layer.
3. Creates a `pio-packages` Docker volume for the ESP32-S2 toolchain.

On the **first `PlatformIO: Build`** (`Ctrl+Alt+B`), Platform.io downloads the Espressif toolchain into the `pio-packages` volume (~1 GB, one-time). Subsequent container rebuilds skip this download.

VS Code opens with `project/` as the workspace root — this is where `platformio.ini` lives and where all firmware code will go.

---

## Flashing & Monitoring

1. Connect the MagTag to your machine via USB-C.
2. Verify the device is visible inside the container: open a terminal and run `ls /dev/ttyACM*` (should show `/dev/ttyACM0` or similar).
3. **Upload**: `PlatformIO: Upload` (`Ctrl+Alt+U`) or `pio run -t upload` in the terminal.
4. **Serial monitor**: `PlatformIO: Monitor` (`Ctrl+Alt+M`) or `pio device monitor`.

Default baud rate is `115200` (set in `platformio.ini`).

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

The named Docker volume `pio-packages` is mounted at `/home/vscode/.platformio` inside the container. It persists across container rebuilds so the ~1 GB toolchain is only downloaded once.

To inspect or prune:
```bash
# List volumes
docker volume ls | grep pio

# Remove (forces full re-download on next build)
docker volume rm pio-packages
```

---

## Project Layout

All firmware source lives under `project/`. The workspace opens directly to this directory.

| Path | Role |
|------|------|
| `project/platformio.ini` | Build configuration (board, framework, monitor speed) |
| `project/sdkconfig.defaults` | ESP-IDF Kconfig defaults committed to the repo |
| `project/src/` | Main application component (Platform.io maps this to ESP-IDF `main/`) |
| `project/include/` | Shared headers visible across components |
| `project/lib/` | Platform.io-managed private libraries |
| `project/components/ssd1680/` | SSD1680 e-ink SPI driver component |
| `project/test/` | Unit tests (`pio test`) |

See [ProductOverview.md](ProductOverview.md) for the full module breakdown and architecture.

---

## Running Tests

```bash
# From the container terminal (inside project/)
pio test -e magtag
```

Or use the **PlatformIO: Test** command from the VS Code command palette. Tests live in `project/test/` and follow Platform.io's Unity-based test framework conventions.
