/* The code points each UI font size is generated with.
   tools/gen_fonts.py reads these macros as the `-r` argument of lv_font_conv
   (UI_RANGE_*: the text face; UI_SYMS_*: the FontAwesome face, empty when the
   size draws no symbol), and test_ui_fonts checks every generated font
   against them. The generator parses this file with a regular expression:
   each macro is `#define NAME "value"`, the value one string literal or
   several adjacent ones (clang-format wraps the long one).

   Why each size holds what it holds (display_screens.c is the only caller):

   12  user text (timer name, status hint, firmware version, AP SSID), so
       full printable ASCII, plus the two non-ASCII code points the stock
       Montserrat builds carry (degree sign, bullet). Symbols: the five
       battery levels, play, pause, right arrow, refresh, tick.
   16  chore names and the swap hint are user text: as 12. Symbols: tick
       (chore rows), right arrow (swap hint), refresh (sync).
   18  AP password, page host, OTA version line, retry line: full printable
       ASCII and the same two extras. No symbol.
   28  string literals only, plus the remaining-time digits and colon:
       exactly the characters of the literals checked by test_ui_fonts.
   48  the break countdown (digits, colon) and "UP!". Nothing else.

   Adding a string to a 28 or 48 label that uses a character outside its
   range fails test_ui_fonts, not the panel. */
#ifndef UI_FONT_RANGES_H
#define UI_FONT_RANGES_H

#define UI_RANGE_12 "0x20-0x7E,0xB0,0x2022"
#define UI_RANGE_16 "0x20-0x7E,0xB0,0x2022"
#define UI_RANGE_18 "0x20-0x7E,0xB0,0x2022"
#define UI_RANGE_28                                                                                                  \
    "0x20-0x21,0x27,0x2D,0x30-0x3A,0x42-0x43,0x45-0x47,0x49,0x4D-0x4E,0x52-0x55,0x57,0x61,0x63-0x69,0x6B-0x70,0x72-" \
    "0x75,0x77,0x79"
#define UI_RANGE_48 "0x20-0x21,0x30-0x3A,0x50,0x55"

#define UI_SYMS_12 "0xF00C,0xF021,0xF04B,0xF04C,0xF054,0xF240-0xF244"
#define UI_SYMS_16 "0xF00C,0xF021,0xF054"
#define UI_SYMS_18 ""
#define UI_SYMS_28 ""
#define UI_SYMS_48 ""

#endif /* UI_FONT_RANGES_H */
