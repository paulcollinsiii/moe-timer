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
    same name, must match its size, must name a slot, and never changes the
    generated header.
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
      DIMS    (w, h) of the PNG
      INK     the PNG's ink count, hard-coded in the C suites
      IDENT   the C stem: <ident>_rle / SPRITE_<IDENT>
      GOLDEN  the screen golden (stem, no .bin) that holds it
      BOX     (x, y, w, h) where the golden holds the sprite (w, h scaled)
      C_SITES a message naming every C test that asserts INK, and the golden
              to regenerate, shown when the ink count drifts
    and define placement(src), which reads the sprite's (x, y) out of
    display_screens.c's source text (asserting the build_sprite call, so a
    moved call fails here) and returns it.
    """

    PNG = DIMS = INK = IDENT = GOLDEN = BOX = C_SITES = None
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
    DIMS = (105, 122)
    INK = 3034
    IDENT = "charge"
    GOLDEN = "charge_me"
    BOX = (8, 3, 105, 122)
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
            r"build_sprite\(\s*scr,\s*&SPRITE_CHARGE,\s*CHARGE_ART_X,\s*\(DISP_VER\s*-\s*SPRITE_CHARGE\.h\)\s*/\s*2,\s*1,\s*lv_color_black\(\)\s*\)",
        )
        return x, (128 - self.DIMS[1]) // 2


class TestTimesUp(SpriteChecks, unittest.TestCase):
    __test__ = True
    PNG = TIMESUP_PNG
    DIMS = (178, 124)
    INK = 5048
    IDENT = "timesup"
    GOLDEN = "timesup"
    BOX = (2, 4, 178, 124)
    BLEED = 12  # ink pixels on the art's last row (= the panel's bottom row)
    C_SITES = (
        "test/test_sprite/test_sprite.c (test_the_generated_timesup_art_decodes_to_the_png_dimensions_and_ink_count), "
        "test/test_display_render/test_display_render.c "
        "(test_the_timesup_screen_paints_the_art_at_the_left_and_the_words_at_the_right; the bottom-row bleed "
        "count is test_the_timesup_screen_fits_the_panel_but_for_the_art_bottom_bleed), and this test; "
        "then regenerate the golden timesup.bin"
    )

    def placement(self, src):
        # TIMESUP_ART_X, flush with the bottom edge (the template is cropped by it on purpose)
        x = int(re.search(r"#define TIMESUP_ART_X (\d+)", src).group(1))
        self.assertRegex(
            src,
            r"build_sprite\(\s*scr,\s*&SPRITE_TIMESUP,\s*TIMESUP_ART_X,\s*"
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
    DIMS = (46, 52)
    INK = 1105
    RLE_LEN = 205
    IDENT = "break"
    GOLDEN = "break_screen"
    SCALE = 2
    INVERTED = True
    BOX = (4, 4, 92, 104)
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
            r"build_sprite\(\s*scr,\s*&SPRITE_BREAK,\s*BREAK_ART_X,\s*BREAK_ART_Y,\s*2,\s*lv_color_white\(\)\s*\)",
        )
        return x, y


class TestBedtime(SpriteChecks, unittest.TestCase):
    __test__ = True
    PNG = BEDTIME_PNG
    DIMS = (29, 23)
    INK = 222
    RLE_LEN = 60
    IDENT = "bedtime"
    GOLDEN = "bedtime"
    SCALE = 4
    INVERTED = True
    BOX = (4, 4, 116, 92)
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
            r"build_sprite\(\s*scr,\s*&SPRITE_BEDTIME,\s*BEDTIME_ART_X,\s*BEDTIME_ART_Y,\s*BEDTIME_ART_SCALE,\s*lv_color_white\(\)\s*\)",
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
    slot and match its template's size exactly; everything else is refused
    with the file, the expected and the actual size.
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

    def test_a_size_mismatch_is_refused_naming_expected_and_actual(self):
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
