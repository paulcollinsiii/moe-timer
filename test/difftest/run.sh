#!/usr/bin/env bash
# Build and run one cycle's differential sweep.
#
#   ./run.sh cycle09_rollover
#
# Regenerates the harnesses from the CURRENT worktree plus the baseline git
# rev named inside the cycle script, builds each, and runs it. Prints one line
# per harness: the case count and the divergence count.
#
# The control MUST report 0 divergences: that is the behaviour-preservation
# claim. Every perturbation MUST report a NON-ZERO count: that is the harness
# proving it can actually see a change. A perturbation reporting 0 means the
# sweep is blind along that axis and the control's 0 proves nothing.
set -uo pipefail

cycle="${1:?usage: run.sh <cycle-script-basename>}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
dir="$here/cycles"
work="${TMPDIR:-/tmp}/difftest.$cycle.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# Pure modules both sides call identically are linked for real rather than
# stubbed — a stub here would be a second implementation to keep in sync, and
# the sweep is comparing OLD against NEW, not against a model.
EXTRA_SRC="${EXTRA_SRC:-$repo/main/bedtime.c $repo/main/quiet_hours.c}"

python3 "$dir/$cycle.py" >/dev/null || { echo "generate FAILED"; exit 1; }

status=0
control_seen=0
for src in "$dir"/harness_*.c; do
    name="$(basename "$src" .c)"
    bin="$work/$name"
    if ! gcc -std=gnu11 -Wall -O1 -DNATIVE \
            -I "$repo/include" -I "$repo/main" -I "$repo/test/mocks" \
            -include "$repo/test/mocks/esp_compat.h" \
            -o "$bin" "$src" $EXTRA_SRC 2> "$work/$name.log"; then
        echo "BUILD FAILED  $name"; sed -n "1,25p" "$work/$name.log"; status=1; continue
    fi
    out="$("$bin")"
    cases="$(sed -n 's/.*cases=\([0-9]*\).*/\1/p'      <<< "$out" | tail -1)"
    div="$(  sed -n 's/.*diverg[a-z]*=\([0-9]*\).*/\1/p' <<< "$out" | tail -1)"
    [ -n "$cases" ] || { echo "UNPARSEABLE   $name: $out"; status=1; continue; }

    if [ "$name" = "harness_control" ]; then
        control_seen=1
        if [ "$div" -eq 0 ]; then verdict="OK    (identical)"
        else verdict="REGRESSION"; status=1; fi
    else
        if [ "$div" -gt 0 ]; then verdict="OK    (perturbation seen)"
        else verdict="BLIND — sweep cannot see this axis"; status=1; fi
    fi
    printf '%-42s cases=%-8s divergences=%-8s %s\n' "$name" "$cases" "$div" "$verdict"
done

# A control that never ran is not a pass; without it nothing was compared.
if [ "$control_seen" -eq 0 ]; then echo "NO CONTROL HARNESS — nothing proven"; status=1; fi
rm -f "$dir"/harness_*.c
exit $status
