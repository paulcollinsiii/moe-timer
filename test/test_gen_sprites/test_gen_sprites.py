#!/usr/bin/env python3
"""Host tests for tools/gen_sprites.py -- the sprite RLE generator.

Runs under uv, not the bare interpreter: the tool reads PNGs with pillow,
declared in its PEP 723 header. test/CMakeLists.txt registers it as

    uv run --no-project --with pillow python3 test_gen_sprites.py

and fails the test loudly (rather than skipping it) when uv is missing.

What is pinned, and why each matters:
  - the encoding: background-first alternating runs, a run over 255 as
    255,0,rest, decoding back to the same pixels (the C decoder in
    main/sprite.c implements the same rules; test_sprite covers it);
  - THE ROUND TRIP: each shipped TEMPLATE (tools/sprites/charge.png,
    timesup.png, break.png, bedtime.png; the last two drawn at 2x and 4x,
    white on black) encodes and decodes to exactly itself, its screen golden
    holds it pixel for pixel at the placement display_screens.c names, and
    the committed main/sprite_data.c holds exactly that stream (the C suite
    cannot read PNGs, so this is where "the sprite in flash is the art" is
    proven). The ink counts below are the SINGLE SOURCE for the C suites
    (test_sprite, test_display_render), which cite this file;
  - --check passes on the committed files and fails on a stale or missing
    copy, so a PNG edited without regenerating cannot ship; it compares
    against the templates only, never a private sprites.local/;
  - --override-dir (TestOverride): an override replaces the template of the
    same name, must name a slot, and never changes the generated header;
  - THE SLOT SIZE (TestSmallerImages, TestSlotAnchors, TestBlankTemplates):
    each template is padded to its slot's MAX size, which is the size every
    generated sprite has. An image (override or variant) may be smaller in
    either dimension and is padded with background, placed by the slot's
    vertical anchor and, for charge and break, inset by the room their slots
    grew past the old art (SLOT_INSET), so art of an old slot size lands
    where it always did; one larger than its slot is refused. tools/sprites/templates/<slot>.png is a blank image of each
    slot's exact size, and the template art still lands on the same panel
    pixels it always did (INK_BOX).
"""

from __future__ import annotations

import contextlib
import io
import os
import re
import shutil
import sys
import tempfile
import unittest
from unittest import mock

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gen_sprites as g  # noqa: E402

CHARGE_PNG = os.path.join(ROOT, "tools", "sprites", "charge.png")
TIMESUP_PNG = os.path.join(ROOT, "tools", "sprites", "timesup.png")
BREAK_PNG = os.path.join(ROOT, "tools", "sprites", "break.png")
BEDTIME_PNG = os.path.join(ROOT, "tools", "sprites", "bedtime.png")
BLANK_DIR = os.path.join(ROOT, "tools", "sprites", "templates")
PANEL_H = 128  # DISP_VER


def runs_of(rle):
    """Expand 255,0,rest continuations into the colour runs they mean."""
    out = []
    i = 0
    while i < len(rle):
        n = rle[i]
        i += 1
        total = n
        while n == 255 and i + 1 < len(rle) and rle[i] == 0:
            n = rle[i + 1]
            total += n
            i += 2
        out.append(total)
    return out


class TestEncoding(unittest.TestCase):
    def roundtrip(self, w, h, px):
        rle = g.encode(w, h, px)
        self.assertEqual(g.decode(w, h, rle), px)
        return rle

    def test_background_first_alternating_runs(self):
        # . # # . / # . . #
        self.assertEqual(g.encode(4, 2, [0, 1, 1, 0, 1, 0, 0, 1]), [1, 2, 1, 1, 2, 1])

    def test_an_image_starting_with_ink_leads_with_a_zero_run(self):
        self.assertEqual(g.encode(3, 1, [1, 1, 0]), [0, 2, 1])

    def test_runs_wrap_across_rows(self):
        px = [0, 0, 1, 1, 1, 1, 1, 0, 0]
        self.assertEqual(self.roundtrip(3, 3, px), [2, 5, 2])

    def test_all_background_is_one_run(self):
        self.assertEqual(self.roundtrip(4, 3, [0] * 12), [12])

    def test_a_run_of_exactly_255_has_no_continuation(self):
        rle = self.roundtrip(255, 2, [0] * 255 + [1] * 255)
        self.assertEqual(rle, [255, 255])

    def test_a_run_over_255_is_written_255_0_rest(self):
        rle = self.roundtrip(300, 1, [0] * 10 + [1] * 280 + [0] * 10)
        self.assertEqual(rle, [10, 255, 0, 25, 10])

    def test_a_run_over_510_chains_the_continuation(self):
        rle = self.roundtrip(600, 1, [1] * 600)
        self.assertEqual(rle, [0, 255, 0, 255, 0, 90])
        self.assertEqual(runs_of(rle), [0, 600])

    def test_a_long_background_run_is_continued_too(self):
        rle = self.roundtrip(300, 1, [0] * 295 + [1] * 5)
        self.assertEqual(rle, [255, 0, 40, 5])

    def test_decode_follows_the_c_decoder_on_bad_streams(self):
        # truncated: the rest is background; overrun: clamped to w*h
        self.assertEqual(g.decode(4, 1, [1, 1]), [0, 1, 0, 0])
        self.assertEqual(g.decode(3, 1, [1, 9, 9]), [0, 1, 1])
        self.assertEqual(g.decode(2, 1, []), [0, 0])

    def test_every_byte_fits_a_byte(self):
        px = [(i // 7) % 2 for i in range(2000)]
        self.assertTrue(all(0 <= b <= 255 for b in g.encode(40, 50, px)))


class SpriteChecks:
    """What is pinned for every shipped sprite; a subclass names one.

    A subclass must set:
      PNG     path of the source art
      DIMS    (w, h) of the PNG: the slot's max size (the template is padded
              to it with background)
      INK     the PNG's ink count, hard-coded in the C suites
      IDENT   the C stem: <ident>_rle / SPRITE_<IDENT>
      GOLDEN  the screen golden (stem, no .bin) that holds it
      BOX     (x, y, w, h) where the golden holds the sprite (w, h scaled)
      INK_BOX (x0, y0, x1, y1) inclusive: the panel pixels the template's ink
              spans, pinned from before the slots were padded to their max,
              so padding can never move the art
      OLD_DIMS, OLD_ORIGIN  the template's size and panel origin before the
              padding: an override of that size (the owner's existing art)
              must still land at OLD_ORIGIN, which for charge and break is
              inside the grown slot (the slot's inset, gen_sprites.SLOT_INSET)
      C_SITES a message naming every C test that asserts INK, and the golden
              to regenerate, shown when the ink count drifts
    and define placement(src), which reads the sprite's (x, y) out of
    display_screens.c's source text (asserting the build_sprite call, so a
    moved call fails here) and returns it.
    """

    PNG = DIMS = INK = IDENT = GOLDEN = BOX = INK_BOX = OLD_DIMS = OLD_ORIGIN = C_SITES = None
    RLE_LEN = None  # when set: the encoded stream length, pinned in the C suite too
    SCALE = 1  # integer draw scale: each art pixel is SCALE x SCALE on the panel
    INVERTED = False  # True: white ink on a black screen (the break screen)
    __test__ = False

    def placement(self, src):
        raise NotImplementedError

    def test_it_round_trips_exactly(self):
        w, h, px = g.load_png(self.PNG)
        self.assertEqual((w, h), self.DIMS)
        rle = g.encode(w, h, px)
        self.assertEqual(g.decode(w, h, rle), px)
        # also against the pixels read independently of the generator: raw
        # bytes of the 1-bit image, not load_png's getpixel expression
        with Image.open(self.PNG) as im:
            self.assertEqual(im.mode, "1")
            raw = im.tobytes()
        stride = (w + 7) // 8
        indep = [1 - ((raw[y * stride + x // 8] >> (7 - (x & 7))) & 1) for y in range(h) for x in range(w)]
        self.assertEqual(g.decode(w, h, rle), indep)

    def test_the_stream_length_is_the_one_the_c_suite_asserts(self):
        # RLE_LEN is hard-coded in test_sprite.c (SPRITE_*.rle_len); a
        # re-encode that changes the stream must fail here first
        if self.RLE_LEN is None:
            self.skipTest("no rle_len pinned for this sprite")
        w, h, px = g.load_png(self.PNG)
        self.assertEqual(len(g.encode(w, h, px)), self.RLE_LEN, "update test/test_sprite/test_sprite.c too")

    def test_the_ink_count_is_the_one_the_c_suites_assert(self):
        # INK is hard-coded in the C suites, which cannot read a PNG. If the
        # art changes, update it in every site the subclass lists (C_SITES),
        # and here; then regenerate the golden.
        _w, _h, px = g.load_png(self.PNG)
        self.assertEqual(sum(px), self.INK, "PNG edited: update every site in %s" % (self.C_SITES,))

    def test_the_golden_holds_the_png_at_the_placement_pixel_for_pixel(self):
        # panel 296x128, 1 bit/px MSB first, 1 = white, 0 = ink; the sprite
        # sits where main/display_screens.c puts it
        with open(os.path.join(ROOT, "main", "display_screens.c")) as f:
            src = f.read()
        sx, sy = self.placement(src)
        with open(os.path.join(ROOT, "test", "test_display_render", "golden", self.GOLDEN + ".bin"), "rb") as f:
            gold = f.read()
        ph, stride = 128, 37
        self.assertEqual(len(gold), stride * ph)
        with Image.open(self.PNG) as im:
            w, h = im.size
            png = [[im.getpixel((x, y)) == 0 for x in range(w)] for y in range(h)]
        k = self.SCALE
        bad = []
        for y in range(h):
            for x in range(w):
                ink = png[y][x]
                # each art pixel is a k x k block on the panel
                for dy in range(k):
                    for dx in range(k):
                        gx, gy = sx + x * k + dx, sy + y * k + dy
                        bit = (gold[gy * stride + gx // 8] >> (7 - (gx & 7))) & 1
                        # upright screens: ink is a 0 bit; the inverted
                        # break screen draws white (1) ink on black
                        gold_ink = (bit == 1) if self.INVERTED else (bit == 0)
                        if ink != gold_ink:
                            bad.append((x, y))
        self.assertEqual(bad[:5], [], "golden differs from the PNG at (x, y) above")
        self.assertEqual((sx, sy, w * k, h * k), self.BOX)

    def test_the_template_ink_lands_on_the_panel_pixels_it_always_did(self):
        # the slot was padded to its max with background on its grow sides,
        # never by moving the art: the ink's panel extent is pinned
        with open(os.path.join(ROOT, "main", "display_screens.c")) as f:
            sx, sy = self.placement(f.read())
        w, _h, px = g.load_png(self.PNG)
        k = self.SCALE
        xs = [i % w for i, v in enumerate(px) if v]
        ys = [i // w for i, v in enumerate(px) if v]
        got = (sx + min(xs) * k, sy + min(ys) * k, sx + max(xs) * k + k - 1, sy + max(ys) * k + k - 1)
        self.assertEqual(got, self.INK_BOX)

    def test_an_override_of_the_old_size_lands_at_the_old_origin(self):
        # gen_sprites.pad_offset() places a smaller image inside the slot;
        # the template is the art padded exactly as an override of its old
        # size would be, so the owner's existing art does not move
        w, h, _px = g.load_png(self.PNG)
        dx, dy = g.pad_offset(self.IDENT, w, h, self.OLD_DIMS[0], self.OLD_DIMS[1])
        with open(os.path.join(ROOT, "main", "display_screens.c")) as f:
            sx, sy = self.placement(f.read())
        k = self.SCALE
        self.assertEqual((sx + dx * k, sy + dy * k), self.OLD_ORIGIN)

    def test_the_committed_c_holds_that_stream(self):
        w, h, px = g.load_png(self.PNG)
        rle = g.encode(w, h, px)
        with open(os.path.join(ROOT, "main", "sprite_data.c")) as f:
            text = f.read()
        m = re.search(r"%s_rle\[\] = \{(.*?)\};" % self.IDENT, text, re.S)
        self.assertIsNotNone(m, "%s_rle[] not found in main/sprite_data.c" % self.IDENT)
        committed = [int(t) for t in re.findall(r"\d+", m.group(1))]
        self.assertEqual(committed, rle)
        self.assertIn("{%d, %d, %s_rle, %d}" % (w, h, self.IDENT, len(rle)), text)

    def test_the_header_names_the_sprite(self):
        with open(os.path.join(ROOT, "include", "sprite_data.h")) as f:
            self.assertIn("extern const sprite_t SPRITE_%s;" % self.IDENT.upper(), f.read())


class TestCharge(SpriteChecks, unittest.TestCase):
    __test__ = True
    PNG = CHARGE_PNG
    DIMS = (154, 124)
    INK = 3034
    IDENT = "charge"
    GOLDEN = "charge_me"
    BOX = (2, 2, 154, 124)
    INK_BOX = (20, 5, 112, 124)
    OLD_DIMS, OLD_ORIGIN = (105, 122), (8, 3)
    C_SITES = (
        "test/test_sprite/test_sprite.c (test_the_generated_charge_art_decodes_to_the_png_dimensions_and_ink_count), "
        "test/test_display_render/test_display_render.c "
        "(test_the_charge_me_screen_paints_the_art_at_the_left), and this test; "
        "then regenerate the golden charge_me.bin"
    )

    def placement(self, src):
        # CHARGE_ART_X, vertically centred
        x = int(re.search(r"#define CHARGE_ART_X (\d+)", src).group(1))
        self.assertRegex(
            src,
            r"build_sprite\(\s*scr,\s*art\s*\?\s*art\s*:\s*&SPRITE_CHARGE,\s*CHARGE_ART_X,\s*\(DISP_VER\s*-\s*SPRITE_CHARGE\.h\)\s*/\s*2,\s*1,\s*lv_color_black\(\)\s*\)",
        )
        return x, (128 - self.DIMS[1]) // 2


class TestTimesUp(SpriteChecks, unittest.TestCase):
    __test__ = True
    PNG = TIMESUP_PNG
    DIMS = (187, 126)
    INK = 5048
    IDENT = "timesup"
    GOLDEN = "timesup"
    BOX = (2, 2, 187, 126)
    INK_BOX = (4, 10, 178, 127)
    OLD_DIMS, OLD_ORIGIN = (178, 124), (2, 4)
    BLEED = 12  # ink pixels on the art's last row (= the panel's bottom row)
    C_SITES = (
        "test/test_sprite/test_sprite.c (test_the_generated_timesup_art_decodes_to_the_png_dimensions_and_ink_count), "
        "test/test_display_render/test_display_render.c "
        "(test_the_timesup_screen_paints_the_art_at_the_left_and_the_words_at_the_right; the bottom-row bleed "
        "count is test_the_timesup_screen_fits_the_panel_but_for_the_art_bottom_bleed), and this test; "
        "then regenerate the golden timesup.bin"
    )

    def placement(self, src):
        # TIMESUP_ART_X, flush with the bottom edge (the template's art is cut off by it on purpose)
        x = int(re.search(r"#define TIMESUP_ART_X (\d+)", src).group(1))
        self.assertRegex(
            src,
            r"build_sprite\(\s*scr,\s*art\s*\?\s*art\s*:\s*&SPRITE_TIMESUP,\s*TIMESUP_ART_X,\s*"
            r"DISP_VER\s*-\s*SPRITE_TIMESUP\.h,\s*1,\s*lv_color_black\(\)\s*\)",
        )
        return x, 128 - self.DIMS[1]

    def test_the_last_row_bleed_is_the_pinned_pixel_count(self):
        # the sprite sits flush with the bottom edge, so its last row IS the
        # panel's bottom row: the ink pixels there are the intentional bleed
        # test_display_render.c asserts (..._but_for_the_art_bottom_bleed)
        w, h, px = g.load_png(self.PNG)
        self.assertEqual(
            sum(px[(h - 1) * w :]), self.BLEED, "PNG edited: update the bleed count in test_display_render.c"
        )


class TestBreak(SpriteChecks, unittest.TestCase):
    __test__ = True
    PNG = BREAK_PNG
    DIMS = (47, 53)
    INK = 1105
    RLE_LEN = 205
    IDENT = "break"
    GOLDEN = "break_screen"
    SCALE = 2
    INVERTED = True
    BOX = (2, 2, 94, 106)
    INK_BOX = (8, 8, 91, 105)
    OLD_DIMS, OLD_ORIGIN = (46, 52), (4, 4)
    C_SITES = (
        "test/test_sprite/test_sprite.c (test_the_generated_break_art_decodes_to_the_png_dimensions_and_ink_count), "
        "test/test_display_render/test_display_render.c "
        "(test_the_break_screen_paints_the_art_at_the_left_at_twice_its_size), and this test; "
        "then regenerate the six break_screen*.bin goldens"
    )

    def placement(self, src):
        # BREAK_ART_X / BREAK_ART_Y, drawn at 2x, WHITE ink (the screen is inverted)
        x = int(re.search(r"#define BREAK_ART_X (\d+)", src).group(1))
        y = int(re.search(r"#define BREAK_ART_Y (\d+)", src).group(1))
        self.assertRegex(
            src,
            r"build_sprite\(\s*scr,\s*art\s*\?\s*art\s*:\s*&SPRITE_BREAK,\s*BREAK_ART_X,\s*BREAK_ART_Y,\s*2,\s*lv_color_white\(\)\s*\)",
        )
        return x, y


class TestBedtime(SpriteChecks, unittest.TestCase):
    __test__ = True
    PNG = BEDTIME_PNG
    DIMS = (32, 25)
    INK = 222
    RLE_LEN = 73
    IDENT = "bedtime"
    GOLDEN = "bedtime"
    SCALE = 4
    INVERTED = True
    BOX = (4, 4, 128, 100)
    INK_BOX = (4, 4, 119, 95)
    OLD_DIMS, OLD_ORIGIN = (29, 23), (4, 4)
    C_SITES = (
        "test/test_sprite/test_sprite.c (test_the_generated_bedtime_art_decodes_to_the_png_dimensions_and_ink_count), "
        "test/test_display_render/test_display_render.c "
        "(test_the_bedtime_screen_paints_the_art_at_the_left_at_four_times_its_size), and this test; "
        "then regenerate the golden bedtime.bin"
    )

    def placement(self, src):
        # BEDTIME_ART_X / BEDTIME_ART_Y, drawn at 4x, WHITE ink (the screen is inverted)
        x = int(re.search(r"#define BEDTIME_ART_X (\d+)", src).group(1))
        y = int(re.search(r"#define BEDTIME_ART_Y (\d+)", src).group(1))
        self.assertEqual(4, int(re.search(r"#define BEDTIME_ART_SCALE (\d+)", src).group(1)))
        self.assertRegex(
            src,
            r"build_sprite\(\s*scr,\s*art\s*\?\s*art\s*:\s*&SPRITE_BEDTIME,\s*BEDTIME_ART_X,\s*BEDTIME_ART_Y,\s*BEDTIME_ART_SCALE,\s*lv_color_white\(\)\s*\)",
        )
        return x, y


class TestGenerated(unittest.TestCase):
    def test_generated_files_carry_the_banner(self):
        for rel in ("main/sprite_data.c", "include/sprite_data.h"):
            with open(os.path.join(ROOT, rel)) as f:
                self.assertIn("Generated by tools/gen_sprites.py", f.read())


class TestCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.sprites = os.path.join(self.tmp, "sprites")
        os.mkdir(self.sprites)
        im = Image.new("1", (8, 4), 1)
        for x in range(2, 6):
            im.putpixel((x, 1), 0)
        im.save(os.path.join(self.sprites, "tiny_thing.png"))
        self.c = os.path.join(self.tmp, "sprite_data.c")
        self.h = os.path.join(self.tmp, "sprite_data.h")
        self.args = ["--sprites-dir", self.sprites, "--c-out", self.c, "--h-out", self.h]

    def run_main(self, extra):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = g.main(self.args + extra)
        return rc, err.getvalue()

    def test_generate_then_check_passes(self):
        self.assertEqual(self.run_main([])[0], 0)
        self.assertEqual(self.run_main(["--check"])[0], 0)
        with open(self.h) as f:
            self.assertIn("SPRITE_TINY_THING", f.read())

    def test_check_does_not_write(self):
        rc, _ = self.run_main(["--check"])
        self.assertNotEqual(rc, 0)
        self.assertFalse(os.path.exists(self.c))

    def test_check_fails_when_a_png_changes(self):
        self.run_main([])
        im = Image.new("1", (8, 4), 1)
        im.putpixel((0, 0), 0)
        im.save(os.path.join(self.sprites, "tiny_thing.png"))
        rc, err = self.run_main(["--check"])
        self.assertNotEqual(rc, 0)
        self.assertIn("sprite_data.c", err)

    def test_check_fails_when_the_c_is_edited_by_hand(self):
        self.run_main([])
        with open(self.c, "a") as f:
            f.write("/* hand edit */\n")
        self.assertNotEqual(self.run_main(["--check"])[0], 0)

    def test_the_committed_files_pass_check(self):
        rc = g.main(["--check"])
        self.assertEqual(rc, 0)

    def test_a_non_1bit_png_is_refused_with_a_name(self):
        Image.new("RGB", (4, 4), (128, 128, 128)).save(os.path.join(self.sprites, "grey.png"))
        with self.assertRaises(SystemExit) as cm:
            g.main(self.args)
        # the full path, not just the basename the template and an override share
        self.assertIn(os.path.join(self.sprites, "grey.png"), str(cm.exception.code))

    def test_a_bad_file_name_is_refused_with_a_name(self):
        for bad in ("Slot.png", "my slot.png", "slot-2.png"):
            im = Image.new("1", (4, 4), 1)
            path = os.path.join(self.sprites, bad)
            im.save(path)
            with self.assertRaises(SystemExit, msg=bad) as cm:
                g.main(self.args)
            self.assertIn(bad, str(cm.exception.code))
            os.remove(path)

    def test_dimensions_and_stream_over_the_uint16_limits_are_refused(self):
        for attr, limit in (("MAX_DIM", 7), ("MAX_RLE_LEN", 2)):
            with mock.patch.object(g, attr, limit):
                with self.assertRaises(SystemExit, msg=attr) as cm:
                    g.main(self.args)  # tiny_thing.png is 8x4, 3 runs
                self.assertIn("tiny_thing.png", str(cm.exception.code))

    def test_the_banner_names_the_sprites_directory(self):
        self.run_main([])
        with open(self.c) as f:
            first = f.readline()
        self.assertIn(g.banner_dir(self.sprites), first)
        with open(os.path.join(ROOT, "main", "sprite_data.c")) as f:
            self.assertIn("from the PNGs in tools/sprites --", f.readline())

    def test_a_sprite_too_big_for_the_stream_length_is_refused(self):
        # rle_len is a uint16: a checkerboard far past 65535 runs cannot fit
        im = Image.new("1", (400, 400), 1)
        for y in range(400):
            for x in range(400):
                if (x + y) % 2:
                    im.putpixel((x, y), 0)
        im.save(os.path.join(self.sprites, "huge.png"))
        with self.assertRaises(SystemExit) as cm:
            g.main(self.args)
        self.assertIn("huge", str(cm.exception.code))


class TestOverride(unittest.TestCase):
    """--override-dir: private art merged over the templates, slot by slot.

    Two templates (alpha 8x4, beta 6x6). An override must name an existing
    slot and be no larger than its template in either dimension; a larger
    one is refused with the file, the slot's size and its own (the padding of
    a smaller one is TestSmallerImages').
    """

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.sprites = os.path.join(self.tmp, "sprites")
        self.over = os.path.join(self.tmp, "over")
        os.mkdir(self.sprites)
        os.mkdir(self.over)
        self.save(self.sprites, "alpha", (8, 4), [(1, 1)])
        self.save(self.sprites, "beta", (6, 6), [(0, 0), (5, 5)])
        self.c = os.path.join(self.tmp, "sprite_data.c")
        self.h = os.path.join(self.tmp, "sprite_data.h")
        self.args = ["--sprites-dir", self.sprites, "--c-out", self.c, "--h-out", self.h]

    @staticmethod
    def save(d, name, size, ink):
        im = Image.new("1", size, 1)
        for p in ink:
            im.putpixel(p, 0)
        im.save(os.path.join(d, name + ".png"))

    def run_main(self, extra):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = g.main(self.args + extra)
        return rc, err.getvalue()

    def text(self, path):
        with open(path) as f:
            return f.read()

    def test_an_override_replaces_its_template_and_the_others_stay(self):
        self.save(self.over, "alpha", (8, 4), [(3, 2), (4, 2), (5, 2)])
        self.assertEqual(self.run_main(["--override-dir", self.over])[0], 0)
        c = self.text(self.c)
        alpha = g.encode(8, 4, [1 if (x, y) in {(3, 2), (4, 2), (5, 2)} else 0 for y in range(4) for x in range(8)])
        beta = g.encode(6, 6, [1 if (x, y) in {(0, 0), (5, 5)} else 0 for y in range(6) for x in range(6)])
        m = re.search(r"alpha_rle\[\] = \{(.*?)\};", c, re.S)
        self.assertEqual([int(t) for t in re.findall(r"\d+", m.group(1))], alpha)
        m = re.search(r"beta_rle\[\] = \{(.*?)\};", c, re.S)
        self.assertEqual([int(t) for t in re.findall(r"\d+", m.group(1))], beta)

    def test_the_c_differs_from_the_templates_only_in_the_overridden_slot(self):
        self.run_main([])
        plain = self.text(self.c)
        self.save(self.over, "beta", (6, 6), [(2, 2)])
        self.run_main(["--override-dir", self.over])
        over = self.text(self.c)
        self.assertNotEqual(plain, over)
        a = re.search(r"static const uint8_t alpha_rle.*?\};", plain, re.S).group(0)
        self.assertIn(a, over)

    def test_the_header_is_identical_with_and_without_overrides(self):
        self.run_main([])
        plain = self.text(self.h)
        self.save(self.over, "alpha", (8, 4), [(3, 2)])
        self.save(self.over, "beta", (6, 6), [(2, 2)])
        self.run_main(["--override-dir", self.over])
        self.assertEqual(self.text(self.h), plain)

    def test_an_empty_override_dir_changes_nothing(self):
        self.run_main([])
        plain = self.text(self.c)
        os.remove(self.c)
        self.assertEqual(self.run_main(["--override-dir", self.over])[0], 0)
        self.assertEqual(self.text(self.c), plain)

    def test_an_unknown_slot_name_is_refused_naming_the_file_and_the_slots(self):
        self.save(self.over, "gamma", (8, 4), [])
        with self.assertRaises(SystemExit) as cm:
            g.main(self.args + ["--override-dir", self.over])
        msg = str(cm.exception.code)
        self.assertIn("gamma.png", msg)
        self.assertIn("alpha", msg)
        self.assertIn("beta", msg)
        self.assertFalse(os.path.exists(self.c))

    def test_an_override_larger_than_its_slot_is_refused_naming_both_sizes(self):
        self.save(self.over, "alpha", (9, 4), [])
        with self.assertRaises(SystemExit) as cm:
            g.main(self.args + ["--override-dir", self.over])
        msg = str(cm.exception.code)
        self.assertIn("alpha.png", msg)
        self.assertIn("8x4", msg)
        self.assertIn("9x4", msg)
        self.assertFalse(os.path.exists(self.c))

    def test_an_override_that_is_not_1bit_is_refused_with_its_name(self):
        Image.new("RGB", (8, 4), (128, 128, 128)).save(os.path.join(self.over, "alpha.png"))
        with self.assertRaises(SystemExit) as cm:
            g.main(self.args + ["--override-dir", self.over])
        self.assertIn("alpha.png", str(cm.exception.code))

    def test_the_mode_error_names_the_full_path_of_the_override(self):
        Image.new("RGB", (8, 4), (128, 128, 128)).save(os.path.join(self.over, "alpha.png"))
        with self.assertRaises(SystemExit) as cm:
            g.main(self.args + ["--override-dir", self.over])
        self.assertIn(os.path.join(self.over, "alpha.png"), str(cm.exception.code))

    def test_an_upper_case_extension_is_refused_not_skipped(self):
        for bad in ("alpha.PNG", "alpha.Png"):
            path = os.path.join(self.over, bad)
            Image.new("1", (8, 4), 1).save(path, format="PNG")
            with self.assertRaises(SystemExit, msg=bad) as cm:
                g.main(self.args + ["--override-dir", self.over])
            self.assertIn(path, str(cm.exception.code))
            os.remove(path)

    def test_a_directory_named_like_a_png_is_ignored(self):
        os.mkdir(os.path.join(self.over, "alpha.png"))
        self.assertEqual(self.run_main(["--override-dir", self.over])[0], 0)

    def test_the_c_banner_names_the_override_dir_only_when_one_is_merged(self):
        self.run_main([])
        self.assertNotIn("overrides", self.text(self.c).splitlines()[0])
        self.assertEqual(self.run_main(["--override-dir", self.over])[0], 0)  # empty dir
        self.assertNotIn("overrides", self.text(self.c).splitlines()[0])
        self.save(self.over, "alpha", (8, 4), [(3, 2)])
        self.run_main(["--override-dir", self.over])
        self.assertIn(" + %s overrides" % g.banner_dir(self.over), self.text(self.c).splitlines()[0])
        self.assertNotIn("overrides", self.text(self.h).splitlines()[0])

    def test_no_header_writes_only_the_c(self):
        self.save(self.over, "alpha", (8, 4), [(3, 2)])
        rc, _err = self.run_main(["--override-dir", self.over, "--no-header"])
        self.assertEqual(rc, 0)
        self.assertTrue(os.path.exists(self.c))
        self.assertFalse(os.path.exists(self.h))

    def test_an_override_generation_with_no_header_never_touches_the_source_tree_header(self):
        # what main/CMakeLists.txt runs: default --h-out (the source tree's
        # include/sprite_data.h), but --no-header
        real_h = os.path.join(ROOT, "include", "sprite_data.h")
        with open(real_h, "rb") as f:
            before = f.read()
        mtime = os.stat(real_h).st_mtime_ns
        self.save(self.over, "alpha", (8, 4), [(3, 2)])
        args = ["--sprites-dir", self.sprites, "--c-out", self.c, "--override-dir", self.over, "--no-header"]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(g.main(args), 0)
        self.assertEqual(os.stat(real_h).st_mtime_ns, mtime)
        with open(real_h, "rb") as f:
            self.assertEqual(f.read(), before)

    def test_a_missing_override_dir_is_refused(self):
        with self.assertRaises(SystemExit) as cm:
            g.main(self.args + ["--override-dir", os.path.join(self.tmp, "nope")])
        self.assertIn("nope", str(cm.exception.code))

    def test_non_png_files_in_the_override_dir_are_ignored(self):
        with open(os.path.join(self.over, "notes.txt"), "w") as f:
            f.write("not art\n")
        self.assertEqual(self.run_main(["--override-dir", self.over])[0], 0)

    def test_check_ignores_overrides(self):
        self.run_main([])
        self.save(self.over, "alpha", (8, 4), [(3, 2)])
        self.assertEqual(self.run_main(["--check", "--override-dir", self.over])[0], 0)

    def test_check_still_fails_on_a_stale_c_with_overrides_present(self):
        self.run_main([])
        self.save(self.over, "alpha", (8, 4), [(3, 2)])
        self.run_main(["--override-dir", self.over])  # C now holds override art
        self.assertNotEqual(self.run_main(["--check", "--override-dir", self.over])[0], 0)

    def test_the_overridden_slots_are_listed_for_the_build_log(self):
        self.save(self.over, "beta", (6, 6), [(2, 2)])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            g.main(self.args + ["--override-dir", self.over])
        self.assertIn("overridden slots: beta", out.getvalue())


class TestVariants(unittest.TestCase):
    """<slot>.<n>.png variants, in the templates and in the override directory.

    Rules pinned here: a variant may not be larger than its slot's base size
    (a smaller one is padded: TestSmallerImages); the
    numbering is 1, 2, ... without a gap (variant 0 is <slot>.png); an
    override file REPLACES the template file of the same slot and number, and
    an override number the template lacks EXTENDS the slot; the emitted
    table's order is the number; the header never depends on overrides.
    """

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.sprites = os.path.join(self.tmp, "sprites")
        self.over = os.path.join(self.tmp, "over")
        os.mkdir(self.sprites)
        os.mkdir(self.over)
        self.c = os.path.join(self.tmp, "sprite_data.c")
        self.h = os.path.join(self.tmp, "sprite_data.h")
        self.args = ["--sprites-dir", self.sprites, "--c-out", self.c, "--h-out", self.h]
        self.save(self.sprites, "alpha.png", (8, 4), [(1, 1)])
        self.save(self.sprites, "beta.png", (6, 6), [(0, 0)])

    @staticmethod
    def save(d, fname, size, ink):
        im = Image.new("1", size, 1)
        for p in ink:
            im.putpixel(p, 0)
        im.save(os.path.join(d, fname))

    def text(self, path):
        with open(path) as f:
            return f.read()

    def run_main(self, extra=()):
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            return g.main(self.args + list(extra))

    def refuse(self, extra=()):
        with self.assertRaises(SystemExit) as cm:
            self.run_main(extra)
        return str(cm.exception.code)

    def rle_of(self, name, w, h, ink):
        return g.encode(w, h, [1 if (x, y) in ink else 0 for y in range(h) for x in range(w)])

    def stream(self, c, ident):
        m = re.search(r"static const uint8_t %s_rle\[\] = \{(.*?)\};" % ident, c, re.S)
        self.assertIsNotNone(m, "%s_rle not emitted" % ident)
        return [int(t) for t in re.findall(r"\d+", m.group(1))]

    def table(self, c, slot):
        m = re.search(r"const sprite_t \*const SPRITE_%s_VARIANTS\[\] = \{(.*?)\};" % slot.upper(), c, re.S)
        self.assertIsNotNone(m, "SPRITE_%s_VARIANTS not emitted" % slot.upper())
        return [t.strip() for t in m.group(1).split(",") if t.strip()]

    def count(self, c, slot):
        m = re.search(r"const unsigned SPRITE_%s_VARIANT_COUNT = (\d+);" % slot.upper(), c)
        self.assertIsNotNone(m, "SPRITE_%s_VARIANT_COUNT not emitted" % slot.upper())
        return int(m.group(1))

    def test_a_slot_with_no_variants_has_a_one_entry_table_holding_the_base(self):
        self.assertEqual(self.run_main(), 0)
        c = self.text(self.c)
        self.assertEqual(self.table(c, "alpha"), ["&SPRITE_ALPHA"])
        self.assertEqual(self.count(c, "alpha"), 1)
        self.assertEqual(self.count(c, "beta"), 1)

    def test_the_header_declares_the_table_and_the_count_for_every_slot(self):
        self.run_main()
        h = self.text(self.h)
        for slot in ("ALPHA", "BETA"):
            self.assertIn("extern const sprite_t *const SPRITE_%s_VARIANTS[];" % slot, h)
            self.assertIn("extern const unsigned SPRITE_%s_VARIANT_COUNT;" % slot, h)
            self.assertIn("extern const sprite_t SPRITE_%s;" % slot, h)

    def test_template_variants_are_emitted_in_number_order_after_the_base(self):
        self.save(self.sprites, "alpha.2.png", (8, 4), [(2, 2)])
        self.save(self.sprites, "alpha.1.png", (8, 4), [(3, 3)])
        self.assertEqual(self.run_main(), 0)
        c = self.text(self.c)
        self.assertEqual(self.table(c, "alpha"), ["&SPRITE_ALPHA", "&SPRITE_ALPHA_VARIANT_1", "&SPRITE_ALPHA_VARIANT_2"])
        self.assertEqual(self.count(c, "alpha"), 3)
        self.assertEqual(self.stream(c, "alpha_1"), self.rle_of("alpha", 8, 4, {(3, 3)}))
        self.assertEqual(self.stream(c, "alpha_2"), self.rle_of("alpha", 8, 4, {(2, 2)}))
        self.assertIn("const sprite_t SPRITE_ALPHA_VARIANT_1 = {8, 4, alpha_1_rle,", c)

    def test_numbers_are_numeric_not_lexical(self):
        for n in range(1, 11):
            self.save(self.sprites, "alpha.%d.png" % n, (8, 4), [(n % 8, 0)])
        self.run_main()
        c = self.text(self.c)
        self.assertEqual(self.count(c, "alpha"), 11)
        self.assertEqual(self.table(c, "alpha")[10], "&SPRITE_ALPHA_VARIANT_10")
        self.assertEqual(self.table(c, "alpha")[2], "&SPRITE_ALPHA_VARIANT_2")

    def test_variant_symbols_are_not_in_the_header(self):
        self.save(self.sprites, "alpha.1.png", (8, 4), [(3, 3)])
        self.run_main()
        self.assertNotIn("VARIANT_1", self.text(self.h))

    def test_a_variant_larger_than_its_slot_is_refused_naming_the_file_and_both_sizes(self):
        self.save(self.sprites, "alpha.1.png", (9, 4), [])
        msg = self.refuse()
        self.assertIn(os.path.join(self.sprites, "alpha.1.png"), msg)
        self.assertIn("8x4", msg)
        self.assertIn("9x4", msg)
        self.assertFalse(os.path.exists(self.c))

    def test_an_override_variant_larger_than_its_slot_is_refused_too(self):
        self.save(self.over, "beta.1.png", (6, 7), [])
        msg = self.refuse(["--override-dir", self.over])
        self.assertIn(os.path.join(self.over, "beta.1.png"), msg)
        self.assertIn("6x6", msg)
        self.assertIn("6x7", msg)

    def test_the_header_says_the_count_is_not_a_constant_expression(self):
        self.run_main()
        h = self.text(self.h)
        self.assertIn("not a\n   constant expression", h)
        self.assertLess(h.index("constant expression"), h.index("extern const unsigned"))

    def test_an_override_variant_larger_than_its_slot_is_refused_at_every_number(self):
        # the size check is not a first-variant check: .2 and .3 are judged too
        self.save(self.sprites, "alpha.1.png", (8, 4), [])
        for n in (1, 2, 3):
            self.save(self.over, "alpha.%d.png" % n, (8, 4), [])
        for n in (1, 2, 3):
            bad = "alpha.%d.png" % n
            self.save(self.over, bad, (8, 5), [])
            msg = self.refuse(["--override-dir", self.over])
            self.assertIn(os.path.join(self.over, bad), msg, bad)
            self.assertIn("8x4", msg, bad)
            self.assertIn("8x5", msg, bad)
            self.save(self.over, bad, (8, 4), [])
        self.assertEqual(self.run_main(["--override-dir", self.over]), 0)

    def test_a_gap_in_the_numbering_is_refused_naming_the_slot_and_the_missing_number(self):
        self.save(self.sprites, "alpha.1.png", (8, 4), [])
        self.save(self.sprites, "alpha.3.png", (8, 4), [])
        msg = self.refuse()
        self.assertIn("alpha", msg)
        self.assertIn("alpha.2.png", msg)

    def test_a_variant_numbered_from_2_is_a_gap_at_1(self):
        self.save(self.sprites, "alpha.2.png", (8, 4), [])
        self.assertIn("alpha.1.png", self.refuse())

    def test_a_gap_is_judged_after_the_merge_so_an_override_can_fill_it(self):
        self.save(self.sprites, "alpha.2.png", (8, 4), [(2, 2)])
        self.save(self.over, "alpha.1.png", (8, 4), [(3, 3)])
        self.assertEqual(self.run_main(["--override-dir", self.over]), 0)
        self.assertEqual(self.count(self.text(self.c), "alpha"), 3)

    def test_variant_numbers_that_are_not_plain_positive_integers_are_refused(self):
        for bad in ("alpha.0.png", "alpha.01.png", "alpha.-1.png", "alpha.x.png", "alpha.1.1.png"):
            path = os.path.join(self.sprites, bad)
            self.save(self.sprites, bad, (8, 4), [])
            msg = self.refuse()
            self.assertIn(bad, msg, bad)
            os.remove(path)

    def test_a_variant_of_a_slot_that_does_not_exist_is_refused(self):
        self.save(self.over, "gamma.1.png", (8, 4), [])
        msg = self.refuse(["--override-dir", self.over])
        self.assertIn("gamma.1.png", msg)
        self.assertIn("alpha", msg)

    def test_an_override_variant_extends_the_templates_variants(self):
        self.save(self.sprites, "alpha.1.png", (8, 4), [(3, 3)])
        self.save(self.over, "alpha.2.png", (8, 4), [(4, 2)])
        self.assertEqual(self.run_main(["--override-dir", self.over]), 0)
        c = self.text(self.c)
        self.assertEqual(self.count(c, "alpha"), 3)
        self.assertEqual(self.stream(c, "alpha"), self.rle_of("alpha", 8, 4, {(1, 1)}))
        self.assertEqual(self.stream(c, "alpha_1"), self.rle_of("alpha", 8, 4, {(3, 3)}))
        self.assertEqual(self.stream(c, "alpha_2"), self.rle_of("alpha", 8, 4, {(4, 2)}))

    def test_an_override_variant_replaces_the_template_variant_of_the_same_number(self):
        self.save(self.sprites, "alpha.1.png", (8, 4), [(3, 3)])
        self.save(self.over, "alpha.1.png", (8, 4), [(5, 0)])
        self.run_main(["--override-dir", self.over])
        c = self.text(self.c)
        self.assertEqual(self.count(c, "alpha"), 2)
        self.assertEqual(self.stream(c, "alpha_1"), self.rle_of("alpha", 8, 4, {(5, 0)}))

    def test_an_override_variant_alone_adds_a_second_sprite_beside_the_template(self):
        # the owner's case: drop <slot>.1.png into sprites.local/ and the
        # template stays variant 0
        self.save(self.over, "alpha.1.png", (8, 4), [(5, 0)])
        self.run_main(["--override-dir", self.over])
        c = self.text(self.c)
        self.assertEqual(self.count(c, "alpha"), 2)
        self.assertEqual(self.stream(c, "alpha"), self.rle_of("alpha", 8, 4, {(1, 1)}))
        self.assertEqual(self.count(c, "beta"), 1)

    def test_an_override_base_replaces_only_variant_0(self):
        self.save(self.sprites, "alpha.1.png", (8, 4), [(3, 3)])
        self.save(self.over, "alpha.png", (8, 4), [(6, 2)])
        self.run_main(["--override-dir", self.over])
        c = self.text(self.c)
        self.assertEqual(self.count(c, "alpha"), 2)
        self.assertEqual(self.stream(c, "alpha"), self.rle_of("alpha", 8, 4, {(6, 2)}))
        self.assertEqual(self.stream(c, "alpha_1"), self.rle_of("alpha", 8, 4, {(3, 3)}))

    def test_the_header_is_identical_with_and_without_variants_in_the_override_dir(self):
        self.run_main()
        plain = self.text(self.h)
        self.save(self.over, "alpha.1.png", (8, 4), [(5, 0)])
        self.run_main(["--override-dir", self.over])
        self.assertEqual(self.text(self.h), plain)

    def test_the_overridden_slots_list_includes_a_slot_that_only_gained_a_variant(self):
        self.save(self.over, "beta.1.png", (6, 6), [(2, 2)])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            g.main(self.args + ["--override-dir", self.over])
        self.assertIn("overridden slots: beta", out.getvalue())

    def test_the_banner_names_the_override_dir_when_only_a_variant_is_merged(self):
        self.save(self.over, "beta.1.png", (6, 6), [(2, 2)])
        self.run_main(["--override-dir", self.over])
        self.assertIn("overrides", self.text(self.c).splitlines()[0])

    def test_check_ignores_override_variants(self):
        self.run_main()
        self.save(self.over, "alpha.1.png", (8, 4), [(5, 0)])
        self.assertEqual(self.run_main(["--check", "--override-dir", self.over]), 0)

    def test_a_template_variant_makes_check_fail_until_regenerated(self):
        self.run_main()
        self.save(self.sprites, "alpha.1.png", (8, 4), [(5, 0)])
        self.assertNotEqual(self.run_main(["--check"]), 0)
        self.run_main()
        self.assertEqual(self.run_main(["--check"]), 0)

    def test_upper_case_variant_files_are_refused_not_skipped(self):
        path = os.path.join(self.over, "alpha.1.PNG")
        Image.new("1", (8, 4), 1).save(path, format="PNG")
        self.assertIn(path, self.refuse(["--override-dir", self.over]))

    def test_a_generated_name_that_collides_is_refused(self):
        # slot "alpha_variant_1" would emit the same SPRITE_ALPHA_VARIANT_1
        self.save(self.sprites, "alpha.1.png", (8, 4), [])
        self.save(self.sprites, "alpha_variant_1.png", (8, 4), [])
        msg = self.refuse()
        self.assertIn("SPRITE_ALPHA_VARIANT_1", msg)

    def test_the_committed_c_has_one_variant_per_slot(self):
        c = self.text(os.path.join(ROOT, "main", "sprite_data.c"))
        for slot in ("charge", "timesup", "break", "bedtime"):
            self.assertEqual(self.count(c, slot), 1, slot)
            self.assertEqual(self.table(c, slot), ["&SPRITE_%s" % slot.upper()])


class TestSmallerImages(unittest.TestCase):
    """An image smaller than its slot is padded to the slot's size.

    Every slot is 8x6 here, named after the real ones so each placement is
    exercised: bedtime is anchored top-left (padded right and below); break
    top-left inset by (1, 1) (one column left and one row above, as room
    allows, the rest right and below); charge inset 6 columns from the left
    as room allows and vertically centred (the odd row going below); timesup
    bottom-left (padded right and above). A slot the generator has no
    placement for (alpha) is top-left. The generated sprite always has the
    slot's size, so the layouts never see an override's own size.
    """

    SW, SH = 8, 6
    # where a 5x3 image lands in an 8x6 slot: (columns left of it, rows above)
    OFFS = {"break": (1, 1), "bedtime": (0, 0), "charge": (3, 1), "timesup": (0, 3), "alpha": (0, 0)}

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.sprites = os.path.join(self.tmp, "sprites")
        self.over = os.path.join(self.tmp, "over")
        os.mkdir(self.sprites)
        os.mkdir(self.over)
        for slot in self.OFFS:
            self.save(self.sprites, slot + ".png", (self.SW, self.SH), [(7, 5)])
        self.c = os.path.join(self.tmp, "sprite_data.c")
        self.h = os.path.join(self.tmp, "sprite_data.h")
        self.args = ["--sprites-dir", self.sprites, "--c-out", self.c, "--h-out", self.h]

    @staticmethod
    def save(d, fname, size, ink):
        im = Image.new("1", size, 1)
        for p in ink:
            im.putpixel(p, 0)
        im.save(os.path.join(d, fname))

    def run_main(self, extra=()):
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            return g.main(self.args + list(extra))

    def refuse(self, extra=()):
        with self.assertRaises(SystemExit) as cm:
            self.run_main(extra)
        return str(cm.exception.code)

    def stream(self, ident):
        with open(self.c) as f:
            c = f.read()
        m = re.search(r"static const uint8_t %s_rle\[\] = \{(.*?)\};" % ident, c, re.S)
        self.assertIsNotNone(m, "%s_rle not emitted" % ident)
        return c, [int(t) for t in re.findall(r"\d+", m.group(1))]

    def slot_rle(self, ink):
        return g.encode(self.SW, self.SH, [1 if (x, y) in ink else 0 for y in range(self.SH) for x in range(self.SW)])

    def test_a_smaller_override_is_placed_per_slot(self):
        art = [(0, 0), (4, 2)]  # corners of a 5x3 image
        for slot in self.OFFS:
            self.save(self.over, slot + ".png", (5, 3), art)
        self.assertEqual(self.run_main(["--override-dir", self.over]), 0)
        for slot, (left, top) in self.OFFS.items():
            c, got = self.stream(slot)
            self.assertEqual(got, self.slot_rle({(x + left, y + top) for x, y in art}), slot)
            self.assertIn("{8, 6, %s_rle," % slot, c, slot)

    def test_only_the_smaller_dimension_is_padded(self):
        # full width, short: padded vertically only; full height, narrow:
        # padded horizontally only, never moved down
        self.save(self.over, "timesup.png", (8, 2), [(7, 1)])
        self.save(self.over, "bedtime.png", (3, 6), [(2, 5)])
        self.save(self.over, "break.png", (8, 4), [(7, 3)])
        self.run_main(["--override-dir", self.over])
        self.assertEqual(self.stream("timesup")[1], self.slot_rle({(7, 5)}))
        self.assertEqual(self.stream("bedtime")[1], self.slot_rle({(2, 5)}))
        # full width: no room for break's inset column, but its inset row
        self.assertEqual(self.stream("break")[1], self.slot_rle({(7, 4)}))

    def test_an_inset_is_clamped_to_the_room_the_image_leaves(self):
        # charge's 6-column inset in an 8-wide slot: min(6, 8 - w)
        for w, left in ((1, 6), (2, 6), (3, 5), (7, 1), (8, 0)):
            self.save(self.over, "charge.png", (w, 6), [(0, 0)])
            self.run_main(["--override-dir", self.over])
            self.assertEqual(self.stream("charge")[1], self.slot_rle({(left, 0)}), "w=%d" % w)
        # break's one-row inset: min(1, 6 - h)
        for h, top in ((5, 1), (6, 0)):
            self.save(self.over, "break.png", (8, h), [(0, 0)])
            self.run_main(["--override-dir", self.over])
            self.assertEqual(self.stream("break")[1], self.slot_rle({(0, top)}), "h=%d" % h)

    def test_the_centred_slot_puts_the_odd_row_below(self):
        # (H - h) // 2 above: the layout centres with (DISP_VER - h) / 2,
        # which rounds the same way
        for h, top in ((5, 0), (4, 1), (3, 1), (2, 2), (1, 2)):
            self.save(self.over, "charge.png", (8, h), [(0, 0)])
            self.run_main(["--override-dir", self.over])
            self.assertEqual(self.stream("charge")[1], self.slot_rle({(0, top)}), "h=%d" % h)

    def test_a_smaller_template_variant_is_padded_too(self):
        self.save(self.sprites, "timesup.1.png", (2, 2), [(1, 1)])
        self.assertEqual(self.run_main(), 0)
        c, got = self.stream("timesup_1")
        self.assertEqual(got, self.slot_rle({(1, 5)}))
        self.assertIn("SPRITE_TIMESUP_VARIANT_1 = {8, 6,", c)

    def test_a_smaller_override_variant_is_padded_too(self):
        self.save(self.over, "bedtime.1.png", (2, 2), [(1, 1)])
        self.assertEqual(self.run_main(["--override-dir", self.over]), 0)
        self.assertEqual(self.stream("bedtime_1")[1], self.slot_rle({(1, 1)}))

    def test_an_image_larger_than_its_slot_in_either_dimension_is_refused(self):
        for size in ((9, 6), (8, 7), (9, 3), (2, 7)):
            path = os.path.join(self.over, "charge.png")
            self.save(self.over, "charge.png", size, [])
            msg = self.refuse(["--override-dir", self.over])
            self.assertIn(path, msg, size)
            self.assertIn("%dx%d" % size, msg, size)
            self.assertIn("8x6", msg, size)
            os.remove(path)
        self.save(self.sprites, "break.1.png", (8, 7), [])
        msg = self.refuse()
        self.assertIn(os.path.join(self.sprites, "break.1.png"), msg)
        self.assertIn("8x7", msg)
        self.assertIn("8x6", msg)

    def test_pad_offset_is_the_placement(self):
        for slot, off in self.OFFS.items():
            self.assertEqual(g.pad_offset(slot, 8, 6, 5, 3), off, slot)


class TestSlotAnchors(unittest.TestCase):
    """The generator's placement per slot agrees with the layout's.

    display_screens.c puts the charge art at y = (DISP_VER - h) / 2 and the
    time's-up art at y = DISP_VER - h, with h the slot's height. A smaller
    image padded by pad_offset() must put its own top row exactly where the
    layout would have put an image of its height, so the owner's art at the
    old size lands on the same pixels as before. Horizontally (and for break
    vertically too) the charge and break slots grew past where their art
    used to start, so pad_offset() insets a smaller image by the growth
    (gen_sprites.SLOT_INSET), as far as the image leaves room. (The
    placement() regexes in TestCharge / TestTimesUp pin the layout's
    formulas.)
    """

    def slot(self, png):
        w, h, _px = g.load_png(png)
        return w, h

    def test_every_shipped_slot_has_an_anchor(self):
        slots = {g.parse_name(f)[0] for f in g.list_pngs(g.DEFAULT_SPRITES)}
        self.assertEqual(slots, set(g.SLOT_ANCHOR))

    def test_the_charge_anchor_matches_the_centred_placement_at_every_height(self):
        sw, sh = self.slot(CHARGE_PNG)
        for h in range(1, sh + 1):
            _dx, dy = g.pad_offset("charge", sw, sh, sw, h)
            self.assertEqual((PANEL_H - sh) // 2 + dy, (PANEL_H - h) // 2, "h=%d" % h)

    def test_the_timesup_anchor_keeps_the_art_flush_with_the_bottom_at_every_height(self):
        sw, sh = self.slot(TIMESUP_PNG)
        for h in range(1, sh + 1):
            _dx, dy = g.pad_offset("timesup", sw, sh, sw, h)
            self.assertEqual(PANEL_H - sh + dy, PANEL_H - h, "h=%d" % h)

    def test_the_bedtime_anchor_is_the_top_left_corner(self):
        sw, sh = self.slot(BEDTIME_PNG)
        self.assertEqual(g.pad_offset("bedtime", sw, sh, 1, 1), (0, 0))

    @staticmethod
    def define(name):
        with open(os.path.join(ROOT, "main", "display_screens.c")) as f:
            return int(re.search(r"#define %s (\d+)" % name, f.read()).group(1))

    def test_a_charge_image_up_to_148_wide_keeps_the_old_x8(self):
        # the slot grew 6 columns LEFT (x 8 -> 2) to the panel's 2 px edge
        # margin; the inset keeps narrower art where it always was, and only
        # an image wider than 148 reaches into the new columns
        sw, sh = self.slot(CHARGE_PNG)
        x0 = self.define("CHARGE_ART_X")
        for w in range(1, sw + 1):
            dx, _dy = g.pad_offset("charge", sw, sh, w, sh)
            self.assertEqual(x0 + dx, 8 if w <= 148 else 2 + sw - w, "w=%d" % w)

    def test_a_break_image_up_to_46x52_keeps_the_old_4_4(self):
        # the slot grew one art pixel (2 panel px) left and up, to (2, 2);
        # each inset is one art pixel, clamped to the room the image leaves
        sw, sh = self.slot(BREAK_PNG)
        x0, y0 = self.define("BREAK_ART_X"), self.define("BREAK_ART_Y")
        for w in range(1, sw + 1):
            for h in range(1, sh + 1):
                dx, dy = g.pad_offset("break", sw, sh, w, h)
                want = (4 if w <= 46 else 2, 4 if h <= 52 else 2)
                self.assertEqual((x0 + 2 * dx, y0 + 2 * dy), want, "%dx%d" % (w, h))

    def test_every_inset_names_a_shipped_slot(self):
        self.assertLessEqual(set(g.SLOT_INSET), set(g.SLOT_ANCHOR))


class TestBlankTemplates(unittest.TestCase):
    """tools/sprites/templates/<slot>.png: a blank canvas per slot, exactly
    the slot's max size, for drawing private art on. It is not a sprite:
    the generator reads only the files directly in tools/sprites/."""

    SLOTS = ("charge", "timesup", "break", "bedtime")

    def test_there_is_one_blank_template_per_slot_and_nothing_else(self):
        self.assertEqual(sorted(os.listdir(BLANK_DIR)), sorted(s + ".png" for s in self.SLOTS))

    def test_each_is_a_blank_1bit_image_of_its_slots_exact_size(self):
        for slot in self.SLOTS:
            path = os.path.join(BLANK_DIR, slot + ".png")
            with Image.open(path) as im:
                self.assertEqual(im.mode, "1", slot)
            w, h, px = g.load_png(path)
            sw, sh, _ = g.load_png(os.path.join(g.DEFAULT_SPRITES, slot + ".png"))
            self.assertEqual((w, h), (sw, sh), slot)
            self.assertEqual(sum(px), 0, "%s: the template has ink" % slot)

    def test_the_generator_does_not_read_a_subdirectory_as_sprites(self):
        self.assertNotIn("templates", g.list_pngs(g.DEFAULT_SPRITES))
        with tempfile.TemporaryDirectory() as tmp:
            sprites = os.path.join(tmp, "sprites")
            os.makedirs(os.path.join(sprites, "templates"))
            Image.new("1", (4, 4), 1).save(os.path.join(sprites, "alpha.png"))
            Image.new("1", (4, 4), 1).save(os.path.join(sprites, "templates", "zeta.png"))
            c, h, _ = g.build(sprites)
            self.assertNotIn("ZETA", c + h)

    def test_a_blank_template_copied_as_an_override_builds_a_blank_slot(self):
        with tempfile.TemporaryDirectory() as over:
            shutil.copy(os.path.join(BLANK_DIR, "timesup.png"), over)
            c, _h, replaced = g.build(g.DEFAULT_SPRITES, over)
        self.assertEqual(replaced, ["timesup"])
        sw, sh, _ = g.load_png(TIMESUP_PNG)
        m = re.search(r"timesup_rle\[\] = \{(.*?)\};", c, re.S)
        self.assertEqual([int(t) for t in re.findall(r"\d+", m.group(1))], g.encode(sw, sh, [0] * (sw * sh)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
