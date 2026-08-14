#!/usr/bin/env python3
"""Host tests for tools/check_slot_size.py -- the build-size guard.

Why this suite is Python and every other suite in test/ is C/Unity:
the code under test is a build-system tool, not firmware. A C suite
cannot exercise it. The alternative -- testing the guard only by
building the firmware -- can never reach the failure branch, because
the only way to trip it would be to actually grow the image by ~60 KB.
The arithmetic therefore lives in a script that takes synthetic inputs,
and this suite drives the boundary from both sides.

Registered in test/CMakeLists.txt with add_test(), so it runs in the
normal `ctest` sweep alongside the C suites.

No ESP-IDF import: the guard hand-parses the partition-table binary
precisely so that both it and this suite stay runnable in the plain
host build, which has no IDF on its path.
"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOL = os.path.normpath(os.path.join(_HERE, "..", "..", "tools", "check_slot_size.py"))

sys.path.insert(0, os.path.dirname(_TOOL))
import check_slot_size as g  # noqa: E402

# The real numbers this project is calibrated against. partitions.csv
# freezes both app slots at 0x1C0000; 85 % of that is 1,559,756.8 B, so
# the largest image that may pass is 1,559,756 B.
SLOT = 0x1C0000  # 1,835,008
PCT = 85
LIMIT = 1_559_756


def _entry(ptype, subtype, offset, size, name):
    """One 32-byte partition-table record, same layout gen_esp32part.py writes."""
    return struct.pack(
        b"<2sBBLL16sL", b"\xaa\x50", ptype, subtype, offset, size, name.encode(), 0
    )


def _table(entries, pad_to=0x1000):
    """A partition-table image: entries, the md5 record, then 0xFF padding."""
    blob = b"".join(entries)
    blob += b"\xeb\xeb" + b"\xff" * 14 + b"\x00" * 16  # md5 record (digest unchecked)
    return blob + b"\xff" * (pad_to - len(blob))


REAL_TABLE = _table(
    [
        _entry(g.TYPE_DATA, 0x02, 0x009000, 0x6000, "nvs"),
        _entry(g.TYPE_DATA, 0x01, 0x00F000, 0x1000, "phy_init"),
        _entry(g.TYPE_APP, 0x10, 0x010000, SLOT, "ota_0"),
        _entry(g.TYPE_APP, 0x11, 0x1D0000, SLOT, "ota_1"),
        _entry(g.TYPE_DATA, 0x00, 0x390000, 0x2000, "otadata"),
        _entry(0x40, 0x00, 0x392000, 0x6E000, "assets"),
    ]
)


class TestThresholdArithmetic(unittest.TestCase):
    """The decision itself: integer-only, and inclusive at the threshold."""

    def test_well_under_passes(self):
        self.assertFalse(g.exceeds(1_000_000, SLOT, PCT))

    def test_one_byte_under_the_limit_passes(self):
        self.assertFalse(g.exceeds(LIMIT - 1, SLOT, PCT))

    def test_exactly_at_the_limit_passes(self):
        # 1,559,756 B is 84.99996 % -- at the guard, not over it.
        self.assertFalse(g.exceeds(LIMIT, SLOT, PCT))

    def test_one_byte_over_the_limit_fails(self):
        self.assertTrue(g.exceeds(LIMIT + 1, SLOT, PCT))

    def test_grossly_over_fails(self):
        self.assertTrue(g.exceeds(1_800_000, SLOT, PCT))

    def test_image_exactly_filling_the_slot_fails(self):
        self.assertTrue(g.exceeds(SLOT, SLOT, PCT))

    def test_image_overflowing_the_slot_fails(self):
        self.assertTrue(g.exceeds(SLOT + 1, SLOT, PCT))

    def test_empty_image_passes(self):
        self.assertFalse(g.exceeds(0, SLOT, PCT))

    def test_exact_percentage_boundary_is_inclusive(self):
        # A slot where 85 % lands on a whole byte, so "exactly at the
        # threshold" is reachable and its treatment is pinned rather than
        # inferred: at is allowed, one over is not.
        slot = 2000
        self.assertEqual(g.guard_limit(slot, PCT), 1700)
        self.assertFalse(g.exceeds(1699, slot, PCT))
        self.assertFalse(g.exceeds(1700, slot, PCT))
        self.assertTrue(g.exceeds(1701, slot, PCT))

    def test_float_arithmetic_would_get_the_boundary_wrong(self):
        # Why exceeds() cross-multiplies instead of comparing percentages.
        # The natural-looking `image / slot * 100.0 > pct` says True for
        # every case below -- 70000/1000000*100.0 is 7.000000000000001 in
        # binary floating point -- and would fail a build sitting exactly
        # on its threshold. Integer arithmetic has no such boundary.
        for slot, pct, image in ((1_000_000, 7, 70_000), (100, 7, 7), (100, 56, 56)):
            self.assertGreater(image / slot * 100.0, float(pct))  # the trap
            self.assertFalse(g.exceeds(image, slot, pct))  # the guard

    def test_hundred_percent_override_still_rejects_overflow(self):
        # 100 is the highest MAGTAG_MAX_SLOT_PCT that means anything: an
        # image cannot be more than 100 % of the slot and still boot.
        self.assertFalse(g.exceeds(SLOT, SLOT, 100))
        self.assertTrue(g.exceeds(SLOT + 1, SLOT, 100))


class TestGuardLimitAgreesWithDecision(unittest.TestCase):
    """guard_limit() is what the message quotes; exceeds() is what decides.

    They are computed differently (floor-division vs cross-multiplication),
    so a message that says "the guard allows N bytes" could drift one byte
    away from the branch that actually fires. Sweep the boundary and the
    percentage range to hold them together.
    """

    def test_agreement_across_the_boundary(self):
        for pct in range(1, 101):
            limit = g.guard_limit(SLOT, pct)
            for image in range(max(0, limit - 3), limit + 4):
                self.assertEqual(
                    g.exceeds(image, SLOT, pct),
                    image > limit,
                    f"disagreement at pct={pct} image={image} limit={limit}",
                )

    def test_agreement_across_awkward_slot_sizes(self):
        # Every percentage, not a sample of them: the int-vs-float
        # disagreements cluster at whichever pct makes slot*pct/100 land on
        # a value binary floating point cannot represent (7, 14, 28, 55,
        # 56 for a 1,000,000 B slot), which a sampled sweep walks straight
        # past.
        for slot in (1, 2, 3, 7, 99, 100, 101, 1023, 1024, 1_000_000, 0x1C0000, 0x1C0001):
            for pct in range(1, 101):
                limit = g.guard_limit(slot, pct)
                for image in range(max(0, limit - 2), limit + 3):
                    self.assertEqual(
                        g.exceeds(image, slot, pct),
                        image > limit,
                        f"disagreement at slot={slot} pct={pct} image={image}",
                    )


class TestPartitionTableParsing(unittest.TestCase):
    """The slot size is read from the flashed artifact, never hardcoded."""

    def test_finds_the_app_slot_in_the_real_table(self):
        name, size = g.smallest_app_partition(REAL_TABLE)
        self.assertEqual(size, SLOT)
        self.assertIn(name, ("ota_0", "ota_1"))

    def test_picks_the_smallest_app_partition(self):
        # The binding constraint is the smallest slot the image must fit,
        # which is what IDF's own check_sizes.py uses.
        table = _table(
            [
                _entry(g.TYPE_APP, 0x10, 0x010000, 0x200000, "ota_0"),
                _entry(g.TYPE_APP, 0x11, 0x210000, 0x180000, "ota_1"),
            ]
        )
        self.assertEqual(g.smallest_app_partition(table), ("ota_1", 0x180000))

    def test_ignores_data_partitions(self):
        table = _table(
            [
                _entry(g.TYPE_DATA, 0x02, 0x009000, 0x100, "nvs"),
                _entry(g.TYPE_APP, 0x10, 0x010000, 0x180000, "ota_0"),
            ]
        )
        self.assertEqual(g.smallest_app_partition(table), ("ota_0", 0x180000))

    def test_table_with_no_app_partition_is_an_error(self):
        table = _table([_entry(g.TYPE_DATA, 0x02, 0x009000, 0x6000, "nvs")])
        with self.assertRaises(g.GuardError):
            g.smallest_app_partition(table)

    def test_garbage_table_is_an_error(self):
        with self.assertRaises(g.GuardError):
            g.smallest_app_partition(b"not a partition table" * 8)

    def test_empty_table_is_an_error(self):
        with self.assertRaises(g.GuardError):
            g.smallest_app_partition(b"\xff" * 0x1000)


class TestMessages(unittest.TestCase):
    def test_failure_message_names_the_override_and_the_freeze(self):
        msg = g.format_failure(1_662_976, SLOT, PCT, "ota_0")
        self.assertIn("MAGTAG_MAX_SLOT_PCT", msg)
        self.assertIn("FROZEN", msg)
        # Not just the word: the reason the word matters, and what to do
        # about it. A guard whose message a reader can only obey, never
        # decide on, sends them looking for the flag that silences it.
        self.assertIn("deployed devices keep it forever", msg)
        self.assertIn("raise MAGTAG_MAX_SLOT_PCT in CMakeLists.txt", msg)
        self.assertIn("own commit", msg)
        self.assertIn("reason", msg)
        self.assertIn("90.6", msg)  # the measured percentage
        self.assertIn("1,835,008", msg)  # the slot, with separators
        self.assertIn("1,662,976", msg)  # the image
        self.assertIn("1,559,756", msg)  # what the guard allows
        self.assertIn("103,220", msg)  # over by

    def test_failure_message_stays_coherent_at_a_rounding_collision(self):
        # 1,559,757 B rounds to "85.0 %", which alone would read as though
        # it were not over the 85 % guard. The byte figures are what make
        # the message unambiguous, so they must always be present.
        msg = g.format_failure(LIMIT + 1, SLOT, PCT, "ota_0")
        self.assertIn("85.0", msg)
        self.assertIn("1,559,757", msg)
        self.assertIn("over by 1 B", msg)

    def test_pass_message_reports_the_percentage_and_the_headroom(self):
        msg = g.format_pass(1_495_232, SLOT, PCT, "ota_0")
        self.assertIn("81.5", msg)
        self.assertIn("1,835,008", msg)
        self.assertIn("64,524", msg)  # LIMIT - 1,495,232 bytes of headroom
        self.assertIn("ota_0", msg)


class TestCommandLine(unittest.TestCase):
    """End-to-end: real files, real exit codes. This is the half that
    proves the build actually fails rather than printing red text."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.table = os.path.join(self.dir.name, "partition-table.bin")
        with open(self.table, "wb") as f:
            f.write(REAL_TABLE)

    def tearDown(self):
        self.dir.cleanup()

    def _image(self, size):
        path = os.path.join(self.dir.name, f"app{size}.bin")
        with open(path, "wb") as f:
            f.truncate(size)  # sparse: no bytes actually written
        return path

    def _run(self, image, pct=PCT, table=None):
        return subprocess.run(
            [
                sys.executable,
                _TOOL,
                "--partition-table",
                table if table is not None else self.table,
                "--image",
                image,
                "--max-pct",
                str(pct),
            ],
            capture_output=True,
            text=True,
        )

    def test_at_the_limit_exits_zero_and_reports_on_stdout(self):
        r = self._run(self._image(LIMIT))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("1,559,756", r.stdout)
        self.assertIn("ota_0", r.stdout)

    def test_one_byte_over_exits_one_and_reports_on_stderr(self):
        r = self._run(self._image(LIMIT + 1))
        self.assertEqual(r.returncode, 1)
        self.assertIn("MAGTAG_MAX_SLOT_PCT", r.stderr)
        self.assertIn("FROZEN", r.stderr)

    def test_raising_the_override_lets_the_same_image_through(self):
        # The documented escape hatch has to actually work, or the guard
        # is a wall rather than a decision point.
        image = self._image(LIMIT + 1)
        self.assertEqual(self._run(image, pct=PCT).returncode, 1)
        self.assertEqual(self._run(image, pct=90).returncode, 0)

    def test_missing_image_is_an_error_not_a_pass(self):
        # The worst failure mode a size guard has: silently succeeding
        # because it never found anything to measure.
        r = self._run(os.path.join(self.dir.name, "does-not-exist.bin"))
        self.assertEqual(r.returncode, 2)

    def test_missing_partition_table_is_an_error_not_a_pass(self):
        r = self._run(
            self._image(1_000_000), table=os.path.join(self.dir.name, "nope.bin")
        )
        self.assertEqual(r.returncode, 2)

    def test_empty_image_file_is_an_error_not_a_pass(self):
        # A zero-byte .bin is 0 % of the slot and would sail through the
        # arithmetic; it means the build produced nothing.
        r = self._run(self._image(0))
        self.assertEqual(r.returncode, 2)

    def test_nonsense_thresholds_are_rejected(self):
        image = self._image(1_000_000)
        for pct in ("0", "-5", "101", "1000"):
            self.assertEqual(self._run(image, pct=pct).returncode, 2, f"pct={pct}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
