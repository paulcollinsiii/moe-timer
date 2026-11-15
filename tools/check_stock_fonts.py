#!/usr/bin/env python3
"""Fail the build when LVGL's stock fonts are linked into the firmware.

Wired from the project root CMakeLists.txt. The firmware draws every glyph
from the 1 bpp ui_font_term_* faces; main/CMakeLists.txt makes that the LVGL
default font with a compile definition (LV_FONT_DEFAULT) on the lvgl
component. If that definition stops taking effect, LVGL silently falls back
to the Kconfig choice, links lv_font_montserrat_* or lv_font_unscii_*, and
every label renders in a 4 bpp face the panel was never tuned for. Nothing
else notices: the build still passes and the image only grows.

The check reads the linker map, past the "Linker script and memory map"
header. Sections the linker discards (--gc-sections) are listed earlier
under "Discarded input sections" and do not count: an object that was
compiled but is unreferenced is not in the image.

Exit codes:
    0  no stock-font symbol is linked
    1  at least one is linked (the build must fail)
    2  the guard could not run -- missing or empty map, or a map without the
       "Linker script and memory map" header. Never a pass.
"""

import argparse
import re
import sys

LINKED_MARKER = "Linker script and memory map"
STOCK_FONT = re.compile(r"lv_font_(?:montserrat|unscii)\w*")


class GuardError(Exception):
    """The guard could not reach a verdict. Always fatal, never a pass."""


def linked_stock_fonts(map_text):
    """Return the sorted stock-font symbol names in the linked part of a map."""
    head, marker, linked = map_text.partition(LINKED_MARKER)
    if not marker:
        raise GuardError(f"map has no '{LINKED_MARKER}' section")
    return sorted(set(STOCK_FONT.findall(linked)))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--map", required=True, metavar="FILE")
    args = parser.parse_args(argv)

    try:
        try:
            with open(args.map, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as exc:
            raise GuardError(f"cannot read map {args.map}: {exc}") from exc
        if not text:
            raise GuardError(f"map {args.map} is empty")
        found = linked_stock_fonts(text)
    except GuardError as exc:
        print(f"error: stock-font guard could not run: {exc}", file=sys.stderr)
        return 2

    if found:
        print(
            "\n  ================ STOCK-FONT GUARD: FAIL ================\n\n"
            "  LVGL stock fonts are linked into the image:\n\n"
            + "".join(f"    {name}\n" for name in found)
            + "\n  Every screen is meant to use ui_font_term_*. Check that\n"
            "  main/CMakeLists.txt still defines LV_FONT_DEFAULT on the lvgl\n"
            "  component and that no code names a lv_font_montserrat_* or\n"
            "  lv_font_unscii_* font.\n\n"
            "  ========================================================\n",
            file=sys.stderr,
        )
        return 1
    print("Stock-font guard: no lv_font_montserrat or lv_font_unscii symbol linked.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
