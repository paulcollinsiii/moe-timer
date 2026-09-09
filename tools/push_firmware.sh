#!/usr/bin/env bash
# Publish a built firmware image, its ELF and the OTA manifest to the pod that
# serves https://firmware.kaffi.internal/.
#
# Usage:
#   tools/push_firmware.sh [options]
#
#   -b DIR    build directory            (default: build)
#   -m FILE   OTA manifest               (default: moes-timer.json)
#   -n NS     pod namespace              (default: firmware)
#   -l SEL    pod label selector         (default: app=firmware)
#   -d DIR    destination in the pod     (default: /srv/firmware)
#   -f        push even if the manifest and the image disagree
#   -N        dry run: resolve and check everything, copy nothing
#
# Naming. The bin and the ELF are published under the version the running
# build baked into them -- magtag_timer-1.5.4.bin -- so several releases can
# sit in the pod at once and a device that is slow to wake can still fetch the
# image its manifest entry names. That version is read out of the image's own
# esp_app_desc_t rather than from git or build/project_description.json,
# because the descriptor string is the one the device actually compares
# against the manifest in ota_check(): a stale build directory, a dirty tree
# or a tag moved after the build all make git disagree with the artifact, and
# it is the artifact that gets served.
#
# The manifest keeps its own name. CONFIG_MAGTAG_OTA_URL is a fixed URL
# ending in moes-timer.json, so it is the one file here that must NOT be
# versioned -- a device polls that exact path to find out which version it
# should be running. Renaming it publishes a release that nothing can
# discover.
#
# Requires kubectl with a working context. jq is optional and only powers the
# manifest cross-check.
set -euo pipefail

BUILD_DIR=build
MANIFEST=moes-timer.json
NAMESPACE=firmware
SELECTOR=app=firmware
DEST=/srv/firmware
FORCE=0
DRY_RUN=0

usage() { sed -n '2,/copy nothing/p' "$0" | sed 's/^# \?//'; exit "${1:-0}"; }

while getopts ":b:m:n:l:d:fNh" opt; do
    case "$opt" in
        b) BUILD_DIR=$OPTARG ;;
        m) MANIFEST=$OPTARG ;;
        n) NAMESPACE=$OPTARG ;;
        l) SELECTOR=$OPTARG ;;
        d) DEST=${OPTARG%/} ;;
        f) FORCE=1 ;;
        N) DRY_RUN=1 ;;
        h) usage 0 ;;
        :) echo "$0: -$OPTARG needs an argument" >&2; usage 2 ;;
        \?) echo "$0: unknown option -$OPTARG" >&2; usage 2 ;;
    esac
done
shift $((OPTIND - 1))
if [ $# -ne 0 ]; then
    echo "$0: unexpected argument '$1'" >&2
    usage 2
fi

die() { echo "$0: $*" >&2; exit 1; }

command -v kubectl >/dev/null || die "kubectl not found in PATH"

# ---- Identify the artifact ------------------------------------------------
#
# esp_app_desc_t sits immediately after the 24-byte image header and the
# 8-byte header of its first segment, so the struct starts at 0x20: magic
# word at 0x20, char version[32] at 0x30, char project_name[32] at 0x50.
# Checking the magic first turns "you pointed at a partition table / an
# ota_data blob / a truncated file" into one line instead of a filename built
# out of binary garbage.
read_desc() { dd if="$1" bs=1 skip="$2" count="$3" 2>/dev/null | tr -d '\0'; }

BIN=$BUILD_DIR/magtag_timer.bin
ELF=$BUILD_DIR/magtag_timer.elf
for f in "$BIN" "$ELF" "$MANIFEST"; do
    [ -f "$f" ] || die "$f not found (build first, or pass -b/-m)"
done

magic=$(dd if="$BIN" bs=1 skip=32 count=4 2>/dev/null | od -An -tx4 | tr -d ' \n')
[ "$magic" = "abcd5432" ] ||
    die "$BIN has no app descriptor (magic $magic, want abcd5432) - not an IDF app image?"

VERSION=$(read_desc "$BIN" 48 32)
PROJECT=$(read_desc "$BIN" 80 32)
[ -n "$VERSION" ] || die "$BIN carries an empty version string"
[ -n "$PROJECT" ] || die "$BIN carries an empty project name"

BIN_NAME=$PROJECT-$VERSION.bin
ELF_NAME=$PROJECT-$VERSION.elf
MANIFEST_NAME=$(basename "$MANIFEST")

echo "image    $BIN ($(du -h "$BIN" | cut -f1)) -> $BIN_NAME"
echo "symbols  $ELF ($(du -h "$ELF" | cut -f1)) -> $ELF_NAME"
echo "manifest $MANIFEST -> $MANIFEST_NAME (never versioned: the OTA URL is fixed)"

# ---- Cross-check the manifest against the image ---------------------------
#
# The two failures this catches are the whole reason the manifest is pushed
# from the same script as the image: a manifest whose url does not name the
# file we are about to upload points every device at a 404, and a manifest
# that names no entry at this version means the upload is invisible. Both
# leave the pod looking perfectly healthy.
if command -v jq >/dev/null; then
    bad_urls=$(jq -r --arg p "$PROJECT" '
        [ .[] | (.default // empty), (.devices // {} | .[]) ]
        | map(select(.url != null and .version != null))
        | map(select((.url | sub(".*/"; "")) != ($p + "-" + .version + ".bin")))
        | .[] | "  " + .version + " -> " + .url' "$MANIFEST")
    if [ -n "$bad_urls" ]; then
        echo "$0: manifest urls do not match their own version fields:" >&2
        echo "$bad_urls" >&2
        [ "$FORCE" = 1 ] || die "refusing to publish a manifest that points at the wrong file (-f overrides)"
    fi

    if ! jq -e --arg v "$VERSION" '
        [ .[] | (.default // empty), (.devices // {} | .[]) ]
        | any(.version == $v)' "$MANIFEST" >/dev/null; then
        echo "$0: no manifest entry asks for $VERSION - nothing would install this image" >&2
        [ "$FORCE" = 1 ] || die "manifest and image disagree (-f overrides)"
    fi
else
    echo "$0: jq not found - skipping the manifest/image cross-check" >&2
fi

# ---- Resolve the pod ------------------------------------------------------
#
# Phase is checked because kubectl cp into a Pending or CrashLoopBackOff pod
# fails deep inside tar with a message that reads like a path problem.
read -r POD PHASE <<<"$(kubectl get pods -n "$NAMESPACE" -l "$SELECTOR" \
    -o jsonpath='{.items[0].metadata.name} {.items[0].status.phase}')"
[ -n "$POD" ] || die "no pod matches -l $SELECTOR in namespace $NAMESPACE"
[ "$PHASE" = Running ] || die "pod $POD is $PHASE, not Running"
echo "target   $NAMESPACE/$POD:$DEST/"

if [ "$DRY_RUN" = 1 ]; then
    echo "dry run - nothing copied"
    exit 0
fi

# ---- Publish --------------------------------------------------------------
#
# The image goes up before the manifest that advertises it, so a failure
# part-way through leaves devices on the old release rather than chasing a
# URL that is not there yet.
srcs=("$BIN" "$ELF" "$MANIFEST")
dsts=("$BIN_NAME" "$ELF_NAME" "$MANIFEST_NAME")
for i in "${!srcs[@]}"; do
    echo "copying  ${dsts[$i]}"
    kubectl cp "${srcs[$i]}" "$NAMESPACE/$POD:$DEST/${dsts[$i]}"
done

echo "published:"
kubectl exec -n "$NAMESPACE" "$POD" -- \
    ls -l "$DEST/$BIN_NAME" "$DEST/$ELF_NAME" "$DEST/$MANIFEST_NAME"
