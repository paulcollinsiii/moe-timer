#!/usr/bin/env python3
"""Fail the build when the app image passes MAGTAG_MAX_SLOT_PCT of its slot.

Wired from the project root CMakeLists.txt; see the comment block there
for why this guard exists and why it fails rather than warns. This file
holds the arithmetic and the message; CMakeLists.txt holds the threshold.

ESP-IDF already checks that the image *fits* (components/partition_table/
check_sizes.py, which errors on overflow and prints -- exit 0 -- a notice
under 5 % free). That is the wrong alarm for a frozen layout: by the time
it fires there is nothing to be done in the field, because an OTA cannot
rewrite the partition table. This guard fires earlier, while giving the
bytes back is still a choice someone can make.

Why a script and not an inline CMake expression: the failure branch can
only be exercised with synthetic inputs -- reaching it for real would mean
growing the image by tens of KB. The comparison lives here so
test/test_check_slot_size/ can drive both sides of the boundary.

Exit codes:
    0  image is at or under the guard
    1  image is over the guard (the build must fail)
    2  the guard could not run -- bad arguments, missing or malformed
       inputs. Never confuse this with a pass: a size guard that cannot
       find the image and exits 0 is worse than no guard at all.
"""

import argparse
import struct
import sys

# Partition-table record layout, as written by ESP-IDF's gen_esp32part.py:
# magic, type, subtype, offset, size, name, flags -- 32 bytes.
ENTRY_FORMAT = b"<2sBBLL16sL"
ENTRY_SIZE = struct.calcsize(ENTRY_FORMAT)
ENTRY_MAGIC = b"\xaa\x50"
MD5_MAGIC = b"\xeb\xeb"
PADDING = b"\xff\xff"

TYPE_APP = 0x00
TYPE_DATA = 0x01


class GuardError(Exception):
    """The guard could not reach a verdict. Always fatal, never a pass."""


def smallest_app_partition(table):
    """Return (name, size) of the smallest type-app partition in a table image.

    The slot size is read from the generated partition-table binary rather
    than hardcoded, so it cannot drift out of step with partitions.csv --
    which is the file that freezes it. Smallest, not first, because the
    image must fit in every slot it may be written to, and the smallest is
    the binding one. (Both of this project's slots are 0x1C0000 today; the
    rule is what keeps that assumption from becoming load-bearing.)
    """
    apps = []
    for pos in range(0, len(table) - ENTRY_SIZE + 1, ENTRY_SIZE):
        raw = table[pos : pos + ENTRY_SIZE]
        head = raw[0:2]
        if head == PADDING:
            break
        if head == MD5_MAGIC:
            continue
        if head != ENTRY_MAGIC:
            raise GuardError(
                f"partition table is malformed at byte {pos}: "
                f"expected magic {ENTRY_MAGIC!r}, found {head!r}"
            )
        _, ptype, _subtype, _offset, size, name, _flags = struct.unpack(
            ENTRY_FORMAT, raw
        )
        if ptype == TYPE_APP:
            apps.append((size, name.rstrip(b"\x00").decode("utf-8", "replace")))
    if not apps:
        raise GuardError("partition table contains no app partition")
    size, name = min(apps)
    return name, size


def guard_limit(slot_bytes, max_pct):
    """Largest image size that is still at or under the guard.

    Floor division, so the returned figure is itself a passing size. This
    is the number the messages quote; exceeds() is the number that decides.
    test_check_slot_size.py sweeps the boundary to hold the two together.
    """
    return slot_bytes * max_pct // 100


def exceeds(image_bytes, slot_bytes, max_pct):
    """True iff the image is strictly over the guard.

    Integer cross-multiplication, never floats: image/slot*100 <= 85.0
    is a comparison whose answer at the boundary depends on binary
    rounding. Rounding belongs in the printed percentage, not in the
    decision. Being exactly at the threshold passes -- the guard is a
    ceiling, not a fence one byte below it.
    """
    return image_bytes * 100 > slot_bytes * max_pct


def _pct(image_bytes, slot_bytes):
    return image_bytes * 100.0 / slot_bytes


def format_pass(image_bytes, slot_bytes, max_pct, slot_name):
    headroom = guard_limit(slot_bytes, max_pct) - image_bytes
    return (
        f"Build-size guard: image {image_bytes:,} B is "
        f"{_pct(image_bytes, slot_bytes):.1f} % of the {slot_bytes:,} B "
        f"{slot_name} slot; {headroom:,} B left under the {max_pct} % guard."
    )


def format_failure(image_bytes, slot_bytes, max_pct, slot_name):
    limit = guard_limit(slot_bytes, max_pct)
    over = image_bytes - limit
    return "\n".join(
        [
            "",
            "  ================ BUILD-SIZE GUARD: FAIL ================",
            "",
            f"  Image is {_pct(image_bytes, slot_bytes):.1f} % of the "
            f"{slot_bytes:,} B {slot_name} slot,",
            f"  over the {max_pct} % guard (MAGTAG_MAX_SLOT_PCT).",
            "",
            f"    image  {image_bytes:>12,} B",
            f"    guard  {limit:>12,} B   ({max_pct} % of the slot)",
            f"    slot   {slot_bytes:>12,} B   FROZEN",
            "",
            f"  That is over by {over:,} B.",
            "",
            "  The slot size is FROZEN: deployed devices keep it forever.",
            "  An OTA cannot rewrite the partition table, so no later",
            "  update can hand this image more room -- only a serial cable",
            "  can, on every device individually.",
            "",
            "  Give the bytes back, or -- if you accept the new floor --",
            "  raise MAGTAG_MAX_SLOT_PCT in CMakeLists.txt (project root)",
            "  as its own commit, stating the reason in the message.",
            "",
            "  ========================================================",
            "",
        ]
    )


def _read(path, what):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        raise GuardError(f"cannot read {what} {path}: {exc}") from exc
    if not data:
        raise GuardError(f"{what} {path} is empty")
    return data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--partition-table", required=True, metavar="BIN")
    parser.add_argument("--image", required=True, metavar="BIN")
    parser.add_argument("--max-pct", required=True, type=int, metavar="PCT")
    args = parser.parse_args(argv)

    try:
        if not 1 <= args.max_pct <= 100:
            # Over 100 would mean "permit an image that does not fit",
            # which no threshold can grant; at or under 0 fails every build.
            raise GuardError(
                f"--max-pct must be between 1 and 100, got {args.max_pct}"
            )
        table = _read(args.partition_table, "partition table")
        image = _read(args.image, "app image")
        slot_name, slot_bytes = smallest_app_partition(table)
    except GuardError as exc:
        print(f"error: build-size guard could not run: {exc}", file=sys.stderr)
        return 2

    image_bytes = len(image)
    if exceeds(image_bytes, slot_bytes, args.max_pct):
        print(
            format_failure(image_bytes, slot_bytes, args.max_pct, slot_name),
            file=sys.stderr,
        )
        return 1
    print(format_pass(image_bytes, slot_bytes, args.max_pct, slot_name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
