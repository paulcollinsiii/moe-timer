#!/usr/bin/env python3
"""Host tests for scripts/check-status-led-wrapper.py.

Why this suite is Python while most suites in test/ are C/Unity: the code
under test is a pre-commit tool, not firmware -- exactly like
tools/check_slot_size.py and scripts/check-log-args.py. See the comments above
their add_test() calls in test/CMakeLists.txt for the rest of the argument,
and for why a gate whose tests live somewhere separate is a gate whose tests
stop being run.

Why it exists at all: the gate this covers was written BECAUSE a wrapper
invariant that everyone believed was checked turned out to be checked by
nothing -- ten of twelve call sites survived being reverted. A gate that
passes silently converts a known hazard into a believed-solved one, which is
the same failure one level up. The cases below are the ways this one could
pass silently: a call hidden in a comment (the file under check mentions the
function by name a dozen times in prose), a call hidden in a string, a call
the pattern is too loose or too tight to see, and a missing target file.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, "..", ".."))
_SCANNER = os.path.join(_ROOT, "scripts", "check-status-led-wrapper.py")
_TARGET = os.path.join("main", "wake_flow.c")

WRAPPER = """\
static void wake_flow_show_status_leds(void) {
    if (s_chore_strip_lit) {
        wake_flow_paint_chore_strip();
        return;
    }
    status_led_show_timer_state();
}
"""


class GateCase(unittest.TestCase):
    def scan(self, body):
        """Run the gate against a synthetic main/wake_flow.c."""
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, _TARGET)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as fh:
                fh.write(body)
            r = subprocess.run([sys.executable, _SCANNER], cwd=d, capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def assertClean(self, body):
        rc, out = self.scan(body)
        self.assertEqual(rc, 0, "expected the gate to pass, got:\n" + out)

    def assertFails(self, body, *needles):
        rc, out = self.scan(body)
        self.assertEqual(rc, 1, "expected the gate to fail, it passed:\n" + out)
        for n in needles:
            self.assertIn(n, out)
        return out


class TestTheOnePermittedCall(GateCase):
    def test_the_wrapper_alone_passes(self):
        self.assertClean(WRAPPER)

    def test_a_second_call_anywhere_fails(self):
        self.assertFails(
            WRAPPER + "static void elsewhere(void) { status_led_show_timer_state(); }\n",
            "found 2",
            "wake_flow_show_status_leds",
        )

    def test_the_reverted_site_is_reported_by_line(self):
        body = WRAPPER + "\n\n\nvoid f(void) { status_led_show_timer_state(); }\n"
        out = self.assertFails(body)
        self.assertIn(":11", out)

    def test_zero_calls_fails_too(self):
        """A wrapper whose body no longer paints is not 'even safer'.

        It is the enforcement point gone: every other site still calls the
        wrapper, so the timer pixel silently stops being painted at all.
        """
        self.assertFails(WRAPPER.replace("    status_led_show_timer_state();\n", ""), "found 0")


class TestWhatDoesNotCount(GateCase):
    def test_a_prose_mention_in_a_block_comment_is_not_a_call(self):
        """The real file mentions the function by name a dozen times.

        A gate that counted those would be permanently red, and a gate
        'fixed' by loosening it until they passed would stop seeing calls.
        """
        self.assertClean(
            "/* THE ONLY CALLER of status_led_show_timer_state() in this file,\n"
            "   and status_led.h states 'not alongside\n"
            "   status_led_show_timer_state()' as a caller contract. */\n" + WRAPPER
        )

    def test_a_prose_mention_in_a_line_comment_is_not_a_call(self):
        self.assertClean(WRAPPER + "// see status_led_show_timer_state() for why\n")

    def test_a_call_hidden_in_a_comment_does_not_count_either(self):
        """The same rule in the direction that could hide a real call.

        Commented-out code is not code; if it comes back, the gate sees it.
        """
        self.assertClean(WRAPPER + "void f(void) { /* status_led_show_timer_state(); */ }\n")

    def test_the_name_inside_a_string_literal_is_not_a_call(self):
        self.assertClean(WRAPPER + 'void f(void) { ESP_LOGI(TAG, "status_led_show_timer_state()"); }\n')

    def test_a_longer_name_that_merely_contains_it_is_not_a_call(self):
        self.assertClean(WRAPPER + "void f(void) { status_led_show_timer_state_twice(); }\n")

    def test_the_name_without_parentheses_is_not_a_call(self):
        """A function pointer taken by name paints nothing by itself."""
        self.assertClean(WRAPPER + "void (*fp)(void) = status_led_show_timer_state;\n")


GESTURE = """\
static swap_gesture_t wake_flow_swap_count_gesture(void) {
    status_led_swap_count_show(2);
    while (more()) {
        status_led_swap_count_show(3);
    }
    neopixel_stop();
    wake_flow_show_status_leds();
    return SWAP_GESTURE_HOME;
}
"""


class TestTheCountDisplayClaimant(GateCase):
    """The sixth claimant: Button C's count display (status_led.h)."""

    def test_a_gesture_that_releases_the_strip_passes(self):
        self.assertClean(WRAPPER + GESTURE)

    def test_the_count_display_outside_the_gesture_fails(self):
        out = self.assertFails(
            WRAPPER + GESTURE + "void elsewhere(void) { status_led_swap_count_show(1); }\n",
            "outside wake_flow_swap_count_gesture",
        )
        self.assertIn("line", out)

    def test_a_gesture_that_never_wipes_the_strip_fails(self):
        self.assertFails(WRAPPER + GESTURE.replace("    neopixel_stop();\n", ""), "neopixel_stop", "never released")

    def test_a_gesture_that_never_repaints_the_state_pixel_fails(self):
        self.assertFails(
            WRAPPER + GESTURE.replace("    wake_flow_show_status_leds();\n", ""),
            "wake_flow_show_status_leds",
            "never released",
        )

    def test_a_release_before_the_last_show_does_not_count(self):
        """The release has to come AFTER the last frame, or it wipes nothing."""
        body = GESTURE.replace(
            "    neopixel_stop();\n    wake_flow_show_status_leds();\n    return",
            "    return",
        ).replace(
            "    status_led_swap_count_show(2);\n",
            "    neopixel_stop();\n    wake_flow_show_status_leds();\n    status_led_swap_count_show(2);\n",
        )
        self.assertFails(WRAPPER + body, "never released")

    def test_an_early_return_between_the_last_show_and_the_release_fails(self):
        """The release names appear after the last show, so a name-presence
        check passes; the path that returns before them leaves magenta lit."""
        body = GESTURE.replace(
            "    neopixel_stop();\n",
            "    if (bail()) {\n        return SWAP_GESTURE_HOME;\n    }\n    neopixel_stop();\n",
        )
        self.assertFails(WRAPPER + body, "a return between the last", "leaves the strip lit")

    def test_the_wrapper_before_the_wipe_fails(self):
        """The wipe clears pixels the wrapper has just painted, so the state
        pixel is dark: the order is part of the contract."""
        body = GESTURE.replace(
            "    neopixel_stop();\n    wake_flow_show_status_leds();\n",
            "    wake_flow_show_status_leds();\n    neopixel_stop();\n",
        )
        self.assertFails(WRAPPER + body, "wake_flow_show_status_leds", "before neopixel_stop")

    def test_a_release_inside_a_dead_branch_fails(self):
        body = GESTURE.replace(
            "    neopixel_stop();\n    wake_flow_show_status_leds();\n",
            "    if (0) {\n        neopixel_stop();\n        wake_flow_show_status_leds();\n    }\n",
        )
        self.assertFails(WRAPPER + body, "not an unconditional statement")

    def test_a_release_guarded_without_braces_fails(self):
        body = GESTURE.replace("    neopixel_stop();\n", "    if (committed) neopixel_stop();\n")
        self.assertFails(WRAPPER + body, "neopixel_stop", "not an unconditional statement")

    def test_a_return_after_the_release_is_fine(self):
        """The ordinary tail: release, log, return."""
        self.assertClean(WRAPPER + GESTURE.replace("    return", "    ESP_LOGI(TAG, \"done\");\n    return"))

    def test_a_show_with_no_gesture_function_at_all_fails(self):
        self.assertFails(WRAPPER + "void f(void) { status_led_swap_count_show(1); }\n", "has no owner")

    def test_a_prose_mention_is_not_a_show(self):
        self.assertClean(WRAPPER + "/* status_led_swap_count_show() paints the count */\n")

    def test_a_file_that_never_shows_the_count_is_fine(self):
        self.assertClean(WRAPPER)


class TestSpellings(GateCase):
    def test_whitespace_before_the_paren_still_counts(self):
        self.assertFails(WRAPPER + "void f(void) { status_led_show_timer_state (); }\n", "found 2")

    def test_a_call_split_across_lines_still_counts(self):
        self.assertFails(WRAPPER + "void f(void) {\n    status_led_show_timer_state\n        ();\n}\n", "found 2")


class TestTheTargetItself(GateCase):
    def test_a_missing_target_is_a_failure_not_a_pass(self):
        """Renaming or deleting the file must not quietly retire the gate."""
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run([sys.executable, _SCANNER], cwd=d, capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("cannot read", r.stdout + r.stderr)

    def test_the_real_file_in_the_tree_passes(self):
        """The gate is green on the tree it ships in.

        Run from a copy so the case cannot depend on the working directory
        pre-commit happens to use.
        """
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "main"))
            shutil.copy(os.path.join(_ROOT, _TARGET), os.path.join(d, _TARGET))
            r = subprocess.run([sys.executable, _SCANNER], cwd=d, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
