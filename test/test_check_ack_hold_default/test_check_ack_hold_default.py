#!/usr/bin/env python3
"""Host tests for scripts/check-ack-hold-default.py.

Why this suite is Python while most suites in test/ are C/Unity: the code
under test is a pre-commit tool, not firmware -- exactly like
tools/check_slot_size.py, scripts/check-log-args.py and
scripts/check-status-led-wrapper.py. See the comments above their add_test()
calls in test/CMakeLists.txt for the rest of the argument, and for why a gate
whose tests live somewhere separate is a gate whose tests stop being run.

Why it exists at all: the gate it covers replaces a comment that ASSERTED an
invariant nothing held -- and the two gates before it both shipped with
silent holes that review found. A gate that passes when it cannot see its
subject converts a known hazard into a believed-solved one, which is the same
failure one level up and the exact failure this gate was written about. So
the cases below are the ways this one could pass silently: the two figures it
must compare hidden among the many other numbers in both files (both carry
long prose quoting 250, 400 and 170 by name), a `default` belonging to a
neighbouring config symbol, the #ifdef arm's own #define mistaken for the
fallback, and every flavour of "the thing I am about is not there".
"""

import os
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, "..", ".."))
_SCANNER = os.path.join(_ROOT, "scripts", "check-ack-hold-default.py")
_KCONFIG = os.path.join("main", "Kconfig.projbuild")
_HEADER = os.path.join("include", "status_led.h")


def kconfig(default="400", *, symbol="MAGTAG_STATUS_LED_ACK_HOLD_MS", extra=""):
    """A Kconfig file shaped like the real one: neighbours on both sides, and
    help prose that quotes other figures the gate must not read."""
    return f"""\
menu "MagTag Timer"

    config MAGTAG_STATUS_LED_BRIGHTNESS
        int "Status NeoPixel brightness (percent)"
        default 100
        range 5 100
        help
            A neighbour with its own default, above the symbol.

    config {symbol}
        int "NeoPixel ack hold / chore-ack coalescing window (ms)"
        default {default}
        range 0 3000
{extra}        help
            The 250 that shipped before was too short; test_wake_flow is
            built at 170, and 400 is design 2.5's prose estimate. None of
            these three numbers is the default and none may be read as it.

    config MAGTAG_QUIET_START_HHMM
        int "NeoPixel quiet hours start (HHMM, 24h)"
        default 2230
        range 0 2359
        help
            A neighbour with its own default, below the symbol.

endmenu
"""


def header(fallback="400", *, guard="CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS", body=None):
    """A header shaped like the real one: prose quoting other figures, the
    #ifdef arm aliasing the CONFIG symbol, and the literal in the #else."""
    if body is None:
        body = f"""\
#ifdef {guard}
#define STATUS_LED_ACK_HOLD_MS {guard}
#else
#define STATUS_LED_ACK_HOLD_MS {fallback}
#endif
"""
    return f"""\
#pragma once
#include <stdint.h>

/* This line carried 250 first. test_wake_flow is built at 170 so no case
   there can encode today's figure as a literal. 400 is the estimate.
   #define STATUS_LED_ACK_HOLD_MS 999 -- and not even a #define in a comment
   may be mistaken for the fallback. */
{body}
#define SOMETHING_ELSE 250
"""


class GateCase(unittest.TestCase):
    def scan(self, kc, hd, *, write_kconfig=True, write_header=True):
        """Run the gate against a synthetic two-file tree."""
        with tempfile.TemporaryDirectory() as d:
            for rel, text, write in ((_KCONFIG, kc, write_kconfig), (_HEADER, hd, write_header)):
                if not write:
                    continue
                path = os.path.join(d, rel)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w") as fh:
                    fh.write(text)
            r = subprocess.run([sys.executable, _SCANNER], cwd=d, capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def assertClean(self, kc, hd, **kw):
        rc, out = self.scan(kc, hd, **kw)
        self.assertEqual(rc, 0, "expected the gate to pass, got:\n" + out)

    def assertFails(self, kc, hd, *needles, **kw):
        rc, out = self.scan(kc, hd, **kw)
        self.assertEqual(rc, 1, "expected the gate to fail, it passed:\n" + out)
        for n in needles:
            self.assertIn(n, out)
        return out


class TestTheAgreeingPair(GateCase):
    """What must not be flagged, including the shapes that look alike."""

    def test_the_real_shape_passes(self):
        self.assertClean(kconfig("400"), header("400"))

    def test_the_value_it_used_to_carry_also_passes_when_both_moved(self):
        # The gate is about agreement, not about 400: a deliberate sweep that
        # lands both sides on a new figure is the supported operation.
        self.assertClean(kconfig("250"), header("250"))
        self.assertClean(kconfig("600"), header("600"))

    def test_zero_agrees_with_zero(self):
        # 0 is inside the Kconfig range and is falsy -- a comparison written
        # with `if not default` rather than `is None` would misread it.
        self.assertClean(kconfig("0"), header("0"))

    def test_neighbouring_defaults_are_not_read_as_this_one(self):
        # Both neighbours carry defaults (100 above, 2230 below). If the gate
        # read either, no pairing of 400/400 could be clean.
        out = self.assertFails(kconfig("400"), header("100"), "default 400")
        self.assertIn("100", out)


class TestTheDivergence(GateCase):
    """The defect itself: one side raised, the other left behind."""

    def test_the_half_landed_raise_fails(self):
        out = self.assertFails(kconfig("400"), header("250"))
        self.assertIn(_KCONFIG, out)
        self.assertIn(_HEADER, out)
        self.assertIn("400", out)
        self.assertIn("250", out)

    def test_it_fails_in_the_other_direction_too(self):
        # Not "the header is stale": either side may be the one that moved.
        self.assertFails(kconfig("250"), header("400"))

    def test_the_message_names_both_files_and_lines(self):
        out = self.assertFails(kconfig("400"), header("170"))
        self.assertIn(_KCONFIG + ":", out)
        self.assertIn(_HEADER + ":", out)
        self.assertIn("EXTRA_DEFS", out)  # says WHY it matters, not just that


class TestProseCannotFoolIt(GateCase):
    """Both real files discuss 250, 400 and 170 at length. None may be read."""

    def test_figures_quoted_in_help_text_are_not_the_default(self):
        # The synthetic help text names 250, 170 and 400 while the default is
        # 300. A looser parse would pick one of the three and pass or fail at
        # random.
        self.assertClean(kconfig("300"), header("300"))

    def test_a_define_inside_a_comment_is_not_the_fallback(self):
        # header() plants `#define STATUS_LED_ACK_HOLD_MS 999` inside a block
        # comment above the guard. It is outside the #else arm, so the
        # structural parse never reaches it -- which is the point of keying on
        # #ifdef/#else/#endif rather than on "the last #define".
        self.assertClean(kconfig("400"), header("400"))

    def test_the_ifdef_arm_alias_is_not_the_fallback(self):
        # The #ifdef arm defines the macro to the CONFIG symbol, not to an
        # integer. A parse that took the FIRST #define would hit that and
        # report it as unparseable on a perfectly correct file.
        self.assertClean(kconfig("400"), header("400"))

    def test_an_unrelated_trailing_define_is_not_the_fallback(self):
        # `#define SOMETHING_ELSE 250` sits after the #endif in every header
        # this suite builds; 400/400 staying clean is what proves it unread.
        self.assertClean(kconfig("400"), header("400"))


class TestItCannotGoGreenBlind(GateCase):
    """A gate that cannot see its subject must FAIL. Both predecessors of this
    gate shipped with a hole of exactly this shape."""

    def test_a_missing_kconfig_fails(self):
        self.assertFails(kconfig(), header(), "cannot read", write_kconfig=False)

    def test_a_missing_header_fails(self):
        self.assertFails(kconfig(), header(), "cannot read", write_header=False)

    def test_a_renamed_kconfig_symbol_fails(self):
        self.assertFails(kconfig(symbol="MAGTAG_STATUS_LED_ACK_HOLD_MSEC"), header(), "renamed or removed")

    def test_a_kconfig_block_with_no_default_fails(self):
        kc = kconfig().replace("        default 400\n", "", 1)
        self.assertFails(kc, header(), "no unconditional `default`")

    def test_a_conditional_default_is_reported_not_guessed(self):
        # `default 400 if FOO` is not understood. Guessing 400 from it would
        # be the gate inventing an answer; there is no such default today and
        # adding one is a design change that should be seen.
        kc = kconfig().replace("        default 400\n", "        default 400 if FOO\n", 1)
        self.assertFails(kc, header(), "no unconditional `default`")

    def test_two_defaults_in_the_block_fail(self):
        kc = kconfig(extra="        default 500\n")
        self.assertFails(kc, header(), "2 `default` lines")

    def test_a_non_integer_default_fails(self):
        self.assertFails(kconfig('"400"'), header(), "not a plain integer")

    def test_a_missing_ifdef_guard_fails(self):
        hd = header(body="#define STATUS_LED_ACK_HOLD_MS 400\n")
        self.assertFails(kconfig(), hd, "host fallback's guard is gone")

    def test_a_renamed_config_symbol_in_the_guard_fails(self):
        self.assertFails(kconfig(), header(guard="CONFIG_MAGTAG_ACK_HOLD_MS"), "host fallback's guard is gone")

    def test_a_guard_with_no_else_fails(self):
        hd = header(
            body="#ifdef CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS\n"
            "#define STATUS_LED_ACK_HOLD_MS CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS\n"
            "#endif\n"
        )
        self.assertFails(kconfig(), hd, "no `#else`/`#endif` pair")

    def test_an_else_arm_that_defines_nothing_fails(self):
        hd = header(
            body="#ifdef CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS\n"
            "#define STATUS_LED_ACK_HOLD_MS CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS\n"
            "#else\n"
            "#define SOMETHING_ELSE 400\n"
            "#endif\n"
        )
        self.assertFails(kconfig(), hd, "the `#else` arm does not `#define STATUS_LED_ACK_HOLD_MS`")

    def test_a_non_integer_fallback_fails(self):
        hd = header(
            body="#ifdef CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS\n"
            "#define STATUS_LED_ACK_HOLD_MS CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS\n"
            "#else\n"
            "#define STATUS_LED_ACK_HOLD_MS (400)\n"
            "#endif\n"
        )
        self.assertFails(kconfig(), hd, "not a plain integer")

    def test_a_nested_conditional_does_not_swallow_the_else(self):
        # A nested #if inside the #ifdef arm has its own #endif. A depth-blind
        # scan would take that #endif as the guard's and never find the #else,
        # failing a file that is actually fine.
        hd = header(
            body="#ifdef CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS\n"
            "#if CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS > 3000\n"
            "#error too long\n"
            "#endif\n"
            "#define STATUS_LED_ACK_HOLD_MS CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS\n"
            "#else\n"
            "#define STATUS_LED_ACK_HOLD_MS 400\n"
            "#endif\n"
        )
        self.assertClean(kconfig("400"), hd)


class TestTheRealTree(GateCase):
    """The committed tree must pass its own gate."""

    def test_the_repository_passes(self):
        r = subprocess.run([sys.executable, _SCANNER], cwd=_ROOT, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
