#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pillow>=10.1",
# ]
# ///
"""Contact sheet of every screen, for judging the UI fonts by eye.

    uv run tools/font_sheets.py
    uv run tools/font_sheets.py --scale 4 --out /tmp/screens.png

It builds test_display_render for the host, runs it with MAGTAG_DUMP_DIR so
every screen is written as the raw 1-bit framebuffer the panel would be sent
(LVGL's own I1 render, thresholded exactly as on the device), and tiles the
frames into one PNG, nearest-neighbour scaled by an integer so pixels stay
visible. The committed goldens are not read or written. The sheets under
docs/planning/fonts/ are the record of how the font set was picked; this
tool does not overwrite them unless asked to.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "test" / "build-fonts-sheets"
DEFAULT_OUT = BUILD_DIR / "screens.png"
W, H = 296, 128
COLUMNS = 3
PAD = 12
BG = (214, 214, 214)
INK = (20, 20, 20)


def frame_image(path, scale):
    data = path.read_bytes()
    if len(data) != W * H // 8:
        sys.exit(f"{path}: {len(data)} bytes, expected {W * H // 8}")
    # I1 framebuffer: rows of 37 bytes, MSB first, 1 = white (the panel's
    # background is the all-ones byte).
    img = Image.frombytes("1", (W, H), data).convert("RGB")
    return img.resize((W * scale, H * scale), Image.NEAREST)


def render_frames(dump_dir):
    build = BUILD_DIR
    subprocess.run(["cmake", "-S", str(ROOT / "test"), "-B", str(build)],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(build), "--target", "test_display_render", "-j8"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dump_dir.mkdir(parents=True, exist_ok=True)
    env = {"MAGTAG_DUMP_DIR": str(dump_dir), "PATH": "/usr/bin:/bin"}
    res = subprocess.run([str(build / "test_display_render")], env=env,
                         capture_output=True, text=True)
    return sum(1 for ln in res.stdout.splitlines() if ":FAIL" in ln)


def label_font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def tile_sheet(frames, scale, notes):
    font = label_font(14 * max(1, scale // 2))
    lab_h = 22 * max(1, scale // 2)
    names = sorted(frames)
    rows = (len(names) + COLUMNS - 1) // COLUMNS
    cw, ch = W * scale, H * scale + lab_h
    sheet = Image.new("RGB", (COLUMNS * cw + (COLUMNS + 1) * PAD,
                              rows * ch + (rows + 1) * PAD + lab_h), BG)
    d = ImageDraw.Draw(sheet)
    d.text((PAD, PAD // 2), notes, fill=INK, font=font)
    for i, n in enumerate(names):
        x = PAD + (i % COLUMNS) * (cw + PAD)
        y = lab_h + PAD + (i // COLUMNS) * (ch + PAD)
        d.text((x, y), n, fill=INK, font=font)
        sheet.paste(frames[n], (x, y + lab_h))
        d.rectangle((x - 1, y + lab_h - 1, x + W * scale, y + lab_h + H * scale), outline=(120, 120, 120))
    return sheet


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--scale", type=int, default=3, choices=[3, 4])
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"output PNG (default {DEFAULT_OUT})")
    args = ap.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        dump = Path(tmp)
        failed = render_frames(dump)
        frames = {p.stem: frame_image(p, args.scale) for p in dump.glob("*.bin")}
    notes = f"{len(frames)} frames, {failed} failed tests"
    print(notes)
    tile_sheet(frames, args.scale, notes).save(args.out, optimize=True)


if __name__ == "__main__":
    main()
