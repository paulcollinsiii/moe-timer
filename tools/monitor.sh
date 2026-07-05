#!/usr/bin/env bash
# Continuous serial log viewer for the MagTag.
#
# The device deep-sleeps between wakes, dropping the native-USB CDC port
# (/dev/ttyACM0) each time. Worse, esp_idf_monitor toggles DTR/RTS whenever
# it (re)opens the port, which the S2 native-USB console interprets as an
# esptool reset request — even with --no-reset — rebooting the device ~1 s
# into every wake and killing the wake handler. logcat.py opens the port
# with pinned control lines instead: no resets, raw log output, automatic
# reconnect across deep-sleep cycles.
#
# Usage: tools/monitor.sh [port]     (default /dev/ttyACM0)
# Quit:  Ctrl+C
set -u
PORT="${1:-/dev/ttyACM0}"
cd "$(dirname "$0")/.."

if ! command -v python >/dev/null || ! python -c 'import serial' 2>/dev/null; then
    # shellcheck disable=SC1090
    source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
fi

exec python tools/logcat.py "$PORT"
