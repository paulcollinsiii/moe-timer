#!/usr/bin/env bash
# Flash (or erase) the raw "assets" partition holding the custom alert WAV.
#
# The firmware accepts 16-bit mono PCM WAV, 8000-22050 Hz, up to the
# partition size -- currently 376 KB = 385,024 B ~= 12 s at 16 kHz, ~24 s at
# 8 kHz, ~8.7 s at 22.05 kHz. partitions.csv is the source of truth for that
# size; this figure is a convenience copy, so re-check it there if the table
# changes (the flash itself is by name and needs no size). An empty/invalid partition makes the "Custom WAV"
# tone fall back to the built-in chime.
#
# The partition is addressed by name, so this script needs no offsets - but
# re-flashing a changed partition table moves "assets" and destroys whatever
# was written here. Re-run this script after any partition-table reflash.
#
# Usage:
#   tools/flash_assets.sh [-p PORT] path/to/tone.wav   # write
#   tools/flash_assets.sh [-p PORT] --erase            # wipe (test fallback)
#
# Requires an exported ESP-IDF environment (source $IDF_PATH/export.sh).
set -euo pipefail

PORT_ARGS=()
if [ "${1:-}" = "-p" ]; then
    PORT_ARGS=(--port "$2")
    shift 2
fi

if [ $# -ne 1 ]; then
    echo "usage: $0 [-p PORT] <file.wav | --erase>" >&2
    exit 2
fi

if ! command -v parttool.py >/dev/null; then
    echo "parttool.py not found - source \$IDF_PATH/export.sh first" >&2
    exit 1
fi

if [ "$1" = "--erase" ]; then
    parttool.py "${PORT_ARGS[@]}" erase_partition --partition-name=assets
    echo "assets partition erased (Custom WAV will fall back to the chime)"
else
    parttool.py "${PORT_ARGS[@]}" write_partition --partition-name=assets --input "$1"
    echo "assets partition written from $1"
fi
