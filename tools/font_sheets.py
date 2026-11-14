#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pillow>=10.1",
# ]
# ///
"""Contact sheets for judging a UI font set by eye.

    uv run tools/font_sheets.py                 # stock + every generated set
    uv run tools/font_sheets.py term lib        # chosen sets
    uv run tools/font_sheets.py --scale 4

For each set it builds test_display_render for the host with that font set
(-DMAGTAG_FONT_SET), runs it with MAGTAG_DUMP_DIR so every screen is written
as the raw 1-bit framebuffer the panel would be sent (LVGL's own I1 render,
thresholded exactly as on the device), and tiles the frames into
docs/planning/fonts/<set>.png, nearest-neighbour scaled by an integer so
pixels stay visible. A last image, compare.png, lays a few representative
screens side by side across the sets.

The committed goldens are not read or written. Tests that assert stock
metrics may fail under another set; that is expected and ignored here, only
the frames are collected. Frames a failing test never reached are missing
from that set's sheet and the script says which.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "docs" / "planning" / "fonts"
W, H = 296, 128
SETS = ["stock", "mont1", "term", "lib"]
COMPARE = [
    "main_running_meditation_x2",
    "main_chore_gated",
    "chores_one_acked",
    "break_screen_chores",
    "setup",
    "sheet_main_long_timer_name",
    "sheet_main_widest_digits",
    "sheet_chores_long_names",
]
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


def run_set(name, dump_dir):
    build = ROOT / "test" / f"build-fonts-{name}"
    subprocess.run(["cmake", "-S", str(ROOT / "test"), "-B", str(build),
                    f"-DMAGTAG_FONT_SET={name}"], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(build), "--target", "test_display_render", "-j8"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dump_dir.mkdir(parents=True, exist_ok=True)
    env = {"MAGTAG_DUMP_DIR": str(dump_dir), "PATH": "/usr/bin:/bin"}
    res = subprocess.run([str(build / "test_display_render")], env=env,
                         capture_output=True, text=True)
    failed = [ln for ln in res.stdout.splitlines() if ":FAIL" in ln]
    return len(failed)


def label_font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def tile_sheet(name, frames, scale, notes):
    font = label_font(14 * max(1, scale // 2))
    lab_h = 22 * max(1, scale // 2)
    names = sorted(frames)
    rows = (len(names) + COLUMNS - 1) // COLUMNS
    cw, ch = W * scale, H * scale + lab_h
    sheet = Image.new("RGB", (COLUMNS * cw + (COLUMNS + 1) * PAD,
                              rows * ch + (rows + 1) * PAD + lab_h), BG)
    d = ImageDraw.Draw(sheet)
    d.text((PAD, PAD // 2), f"font set: {name}   {notes}", fill=INK, font=font)
    for i, n in enumerate(names):
        x = PAD + (i % COLUMNS) * (cw + PAD)
        y = lab_h + PAD + (i // COLUMNS) * (ch + PAD)
        d.text((x, y), n, fill=INK, font=font)
        sheet.paste(frames[n], (x, y + lab_h))
        d.rectangle((x - 1, y + lab_h - 1, x + W * scale, y + lab_h + H * scale), outline=(120, 120, 120))
    return sheet


def compare_sheet(sets, all_frames, scale):
    font = label_font(14 * max(1, scale // 2))
    lab_h = 22 * max(1, scale // 2)
    cw, ch = W * scale, H * scale + lab_h
    shown = [n for n in COMPARE if any(n in all_frames[s] for s in sets)]
    sheet = Image.new("RGB", (len(sets) * cw + (len(sets) + 1) * PAD,
                              len(shown) * (ch + lab_h) + (len(shown) + 1) * PAD), BG)
    d = ImageDraw.Draw(sheet)
    for r, n in enumerate(shown):
        y = PAD + r * (ch + lab_h + PAD)
        d.text((PAD, y), n, fill=INK, font=font)
        for c, s in enumerate(sets):
            x = PAD + c * (cw + PAD)
            d.text((x, y + lab_h), s, fill=INK, font=font)
            if n in all_frames[s]:
                sheet.paste(all_frames[s][n], (x, y + 2 * lab_h))
    return sheet


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sets", nargs="*", default=SETS)
    ap.add_argument("--scale", type=int, default=3, choices=[3, 4])
    args = ap.parse_args()
    sets = args.sets or SETS
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    all_frames = {}
    reference = None
    for name in sets:
        with tempfile.TemporaryDirectory() as tmp:
            dump = Path(tmp)
            failed = run_set(name, dump)
            frames = {p.stem: frame_image(p, args.scale) for p in dump.glob("*.bin")}
        if reference is None:
            reference = set(frames)
        missing = sorted(reference - set(frames))
        notes = f"{len(frames)} frames, {failed} stock-metric tests failed"
        if missing:
            notes += f", missing: {', '.join(missing)}"
        print(name, notes)
        all_frames[name] = frames
        tile_sheet(name, frames, args.scale, notes).save(OUT_DIR / f"{name}.png", optimize=True)
    if len(sets) > 1:
        compare_sheet(sets, all_frames, args.scale).save(OUT_DIR / "compare.png", optimize=True)


if __name__ == "__main__":
    main()
