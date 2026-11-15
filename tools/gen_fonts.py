#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Generate the 1 bpp UI fonts under main/fonts/ with lv_font_conv.

    uv run tools/gen_fonts.py                 # all five sizes
    uv run tools/gen_fonts.py --list          # what each size uses

One C file per size, ui_font_term_<px>.c, defining `ui_font_term_<px>`.
The code points of each size are the UI_RANGE_<px> / UI_SYMS_<px> macros of
include/ui_font_ranges.h (the same header test_ui_fonts checks the output
against). The generated C is committed and the firmware and host builds never
run this; only a font or range change does, and it needs Node (npx).

lv_font_conv is pinned. At --bpp 1 it renders with FreeType's monochrome
target, which has no anti-aliasing and ignores --autohint-strong; what
matters is the hinting source: the default forces FreeType's autohinter, and
--autohint-off uses the font's own TrueType instructions (Liberation Sans is
hand-hinted for exactly this). Bitmap strikes are used when the face carries
one at the requested size (Terminus), because FreeType loads them. Inputs are
TTF/OTF/WOFF only: BDF and PCF are rejected.

The FontAwesome source is the one LVGL builds its own symbols from; it is
read from the managed LVGL component (fetched by the first `idf.py build` /
`idf.py reconfigure`) and is not committed here. Terminus and Liberation Sans
are committed under tools/fonts/ with their licences.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

LV_FONT_CONV = "lv_font_conv@1.5.3"
SIZES = (12, 16, 18, 28, 48)

ROOT = Path(__file__).resolve().parent.parent
RANGES_H = ROOT / "include" / "ui_font_ranges.h"
OUT_DIR = ROOT / "main" / "fonts"
LVGL_SCRIPTS = ROOT / "managed_components" / "lvgl__lvgl" / "scripts" / "built_in_font"

AWESOME = LVGL_SCRIPTS / "FontAwesome5-Solid+Brands+Regular.woff"
TERMINUS = ROOT / "tools" / "fonts" / "terminus" / "TerminusTTF-4.49.3.ttf"
LIBERATION = ROOT / "tools" / "fonts" / "liberation" / "LiberationSans-Regular.ttf"

# A face for one size: (source file, extra per-font flags).
AUTOHINT = ()
TT_HINTS = ("--autohint-off",)

# A face drawn on the pixel grid for the text sizes (Terminus' native 12, 16
# and 18 px bitmaps), a hinted outline face for the big sizes.
FACES = {
    12: (TERMINUS, AUTOHINT),
    16: (TERMINUS, AUTOHINT),
    18: (TERMINUS, AUTOHINT),
    28: (LIBERATION, TT_HINTS),
    48: (LIBERATION, TT_HINTS),
}
SET_NAME = "term"


def read_ranges():
    text = RANGES_H.read_text()
    out = {}
    for kind in ("RANGE", "SYMS"):
        for px in SIZES:
            # One string literal, or several adjacent ones (clang-format
            # wraps a long one over backslash-continued lines).
            m = re.search(rf'^#define UI_{kind}_{px}((?:[ \t]*\\?\n?[ \t]*"[^"\n]*")+)[ \t]*$', text, re.M)
            if m is None:
                sys.exit(f"{RANGES_H}: UI_{kind}_{px} not found")
            out[(kind, px)] = "".join(re.findall(r'"([^"\n]*)"', m.group(1)))
    return out


def command(px, face, flags, ranges, out_name):
    """The lv_font_conv argv, run from a directory holding the sources by
    base name, so the Opts line stamped into the C carries no local paths."""
    argv = [
        "npx", "--yes", LV_FONT_CONV,
        "--no-compress", "--no-prefilter", "--bpp", "1", "--size", str(px),
        "--font", face.name, *flags, "-r", ranges[("RANGE", px)],
    ]
    if ranges[("SYMS", px)]:
        argv += ["--font", AWESOME.name, "-r", ranges[("SYMS", px)]]
    argv += [
        "--format", "lvgl", "--lv-font-name", f"ui_font_{SET_NAME}_{px}",
        "-o", out_name, "--force-fast-kern-format",
    ]
    return argv


def generate(ranges):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for px in SIZES:
            face, flags = FACES[px]
            for src in (face, AWESOME):
                if not src.exists():
                    sys.exit(f"missing source font {src} (run idf.py reconfigure once "
                             "to fetch the LVGL component)" if "managed_components" in str(src)
                             else f"missing source font {src}")
                shutil.copy(src, work / src.name)
            out_name = f"ui_font_{SET_NAME}_{px}.c"
            argv = command(px, face, flags, ranges, out_name)
            print(" ".join(argv))
            subprocess.run(argv, cwd=work, check=True)
            shutil.copy(work / out_name, OUT_DIR / out_name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--list", action="store_true", help="print the face of each size and exit")
    args = ap.parse_args()
    if args.list:
        print({px: f"{f.name} {' '.join(h)}".strip() for px, (f, h) in FACES.items()})
        return
    generate(read_ranges())


if __name__ == "__main__":
    main()
