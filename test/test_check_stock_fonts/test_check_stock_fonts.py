#!/usr/bin/env python3
"""Host tests for tools/check_stock_fonts.py -- the stock-font guard.

Drives the guard with synthetic linker maps, because the failing case (a
Montserrat symbol in the image) cannot be produced by the real build without
undoing the LV_FONT_DEFAULT definition. Registered in test/CMakeLists.txt
with add_test(), like test_check_slot_size.
"""

import os
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOL = os.path.normpath(os.path.join(_HERE, "..", "..", "tools", "check_stock_fonts.py"))

HEAD = "Discarded input sections\n\n"
MARK = "\nMemory Configuration\n\nLinker script and memory map\n\n"
CLEAN = " .rodata.ui_font_term_12  0x3c000000  0x500 liblvgl.a(ui_font_term_12.c.obj)\n"
MONT = " .rodata.lv_font_montserrat_12  0x3c000500  0x900 liblvgl.a(lv_font_montserrat_12.c.obj)\n"
UNSCII = " .rodata.lv_font_unscii_8  0x3c000500  0x900 liblvgl.a(lv_font_unscii_8.c.obj)\n"


class StockFontGuard(unittest.TestCase):
    def _run(self, text):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "app.map")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            return subprocess.run(
                [sys.executable, _TOOL, "--map", path], capture_output=True, text=True
            )

    def test_clean_map_passes(self):
        self.assertEqual(self._run(HEAD + MARK + CLEAN).returncode, 0)

    def test_montserrat_in_image_fails_and_is_named(self):
        r = self._run(HEAD + MARK + CLEAN + MONT)
        self.assertEqual(r.returncode, 1)
        self.assertIn("lv_font_montserrat_12", r.stderr)

    def test_unscii_in_image_fails(self):
        self.assertEqual(self._run(HEAD + MARK + UNSCII).returncode, 1)

    def test_discarded_sections_do_not_count(self):
        # Compiled but gc'd: listed before the memory map, not in the image.
        self.assertEqual(self._run(HEAD + MONT + UNSCII + MARK + CLEAN).returncode, 0)

    def test_map_without_marker_is_an_error_not_a_pass(self):
        self.assertEqual(self._run(CLEAN).returncode, 2)

    def test_empty_map_is_an_error_not_a_pass(self):
        self.assertEqual(self._run("").returncode, 2)

    def test_missing_map_is_an_error_not_a_pass(self):
        r = subprocess.run(
            [sys.executable, _TOOL, "--map", "/nonexistent/app.map"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
