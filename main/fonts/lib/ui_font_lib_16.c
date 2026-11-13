/*******************************************************************************
 * Size: 16 px
 * Bpp: 1
 * Opts: --no-compress --no-prefilter --bpp 1 --size 16 --font LiberationSans-Regular.ttf --autohint-off -r 0x20-0x7E,0xB0,0x2022 --font FontAwesome5-Solid+Brands+Regular.woff -r 0xF00C,0xF021,0xF054 --format lvgl --lv-font-name ui_font_lib_16 -o ui_font_lib_16.c --force-fast-kern-format
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef UI_FONT_LIB_16
#define UI_FONT_LIB_16 1
#endif

#if UI_FONT_LIB_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0xb0,

    /* U+0022 "\"" */
    0x99, 0x90,

    /* U+0023 "#" */
    0x11, 0x8, 0x84, 0x9f, 0xf2, 0x21, 0x10, 0x88,
    0x44, 0xff, 0xa2, 0x11, 0x8, 0x80,

    /* U+0024 "$" */
    0x8, 0x1f, 0x1a, 0x49, 0x4, 0x83, 0x40, 0xf0,
    0x1e, 0x9, 0x84, 0xc2, 0x79, 0x27, 0xe0, 0x40,

    /* U+0025 "%" */
    0xf0, 0x49, 0x8, 0x91, 0x89, 0x10, 0x93, 0xf,
    0x20, 0x4, 0xf0, 0x49, 0x8, 0x91, 0x89, 0x10,
    0x92, 0xf,

    /* U+0026 "&" */
    0x1c, 0x8, 0x82, 0x20, 0x98, 0x3c, 0x1c, 0x2d,
    0x92, 0x34, 0x86, 0x31, 0xc7, 0x9c,

    /* U+0027 "'" */
    0xe0,

    /* U+0028 "(" */
    0x36, 0x44, 0xc8, 0x88, 0x88, 0xc4, 0x46, 0x30,

    /* U+0029 ")" */
    0xc6, 0x22, 0x31, 0x11, 0x11, 0x32, 0x26, 0xc0,

    /* U+002A "*" */
    0x10, 0x4f, 0xcc, 0x31, 0x20,

    /* U+002B "+" */
    0x10, 0x20, 0x47, 0xf1, 0x2, 0x4, 0x0,

    /* U+002C "," */
    0xf5,

    /* U+002D "-" */
    0xe0,

    /* U+002E "." */
    0xf0,

    /* U+002F "/" */
    0x11, 0x22, 0x22, 0x44, 0x44, 0x88,

    /* U+0030 "0" */
    0x38, 0x8a, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x8d, 0x11, 0xc0,

    /* U+0031 "1" */
    0x30, 0xe2, 0x40, 0x81, 0x2, 0x4, 0x8, 0x10,
    0x20, 0x47, 0xf0,

    /* U+0032 "2" */
    0x7d, 0x8e, 0x18, 0x10, 0x60, 0x82, 0x8, 0x20,
    0x83, 0x7, 0xf0,

    /* U+0033 "3" */
    0x7d, 0x8e, 0x8, 0x30, 0xc7, 0x81, 0x81, 0x3,
    0x7, 0x1b, 0xe0,

    /* U+0034 "4" */
    0x6, 0x7, 0x2, 0x83, 0x41, 0x21, 0x11, 0x88,
    0x84, 0xff, 0x81, 0x0, 0x80, 0x40,

    /* U+0035 "5" */
    0xff, 0x83, 0x4, 0xb, 0xd8, 0xc0, 0x81, 0x2,
    0x7, 0x1b, 0xc0,

    /* U+0036 "6" */
    0x3c, 0x8f, 0x4, 0xb, 0xd8, 0xf0, 0xc1, 0x83,
    0x85, 0x19, 0xe0,

    /* U+0037 "7" */
    0xfe, 0x4, 0x10, 0x60, 0x83, 0x4, 0x8, 0x30,
    0x60, 0x81, 0x0,

    /* U+0038 "8" */
    0x7d, 0x8e, 0xc, 0x1c, 0x67, 0x31, 0xc1, 0x83,
    0x7, 0x1b, 0xe0,

    /* U+0039 "9" */
    0x79, 0x8a, 0x1c, 0x18, 0x30, 0x71, 0xbd, 0x2,
    0xf, 0x13, 0xc0,

    /* U+003A ":" */
    0xf0, 0x3, 0xc0,

    /* U+003B ";" */
    0xf0, 0x3, 0xd4,

    /* U+003C "<" */
    0x2, 0x3b, 0xc4, 0xf, 0x3, 0x80, 0x80,

    /* U+003D "=" */
    0xfe, 0x0, 0x0, 0xf, 0xe0,

    /* U+003E ">" */
    0x80, 0xe0, 0x78, 0x11, 0xee, 0x20, 0x0,

    /* U+003F "?" */
    0x7d, 0x8e, 0x8, 0x10, 0x20, 0xc3, 0x8, 0x10,
    0x0, 0xc1, 0x80,

    /* U+0040 "@" */
    0x7, 0xc0, 0x60, 0xc2, 0x1, 0x99, 0xea, 0x4c,
    0xed, 0x21, 0x19, 0x84, 0x64, 0x12, 0x90, 0xcb,
    0x65, 0x64, 0xe7, 0x18, 0x0, 0x30, 0x60, 0x3e,
    0x0,

    /* U+0041 "A" */
    0xe, 0x1, 0x40, 0x28, 0xd, 0x81, 0x10, 0x22,
    0xc, 0x61, 0xfc, 0x60, 0xc8, 0x9, 0x1, 0x60,
    0x30,

    /* U+0042 "B" */
    0xfe, 0x61, 0xb0, 0x58, 0x2c, 0x37, 0xf3, 0x7,
    0x81, 0xc0, 0xe0, 0xf0, 0x7f, 0xc0,

    /* U+0043 "C" */
    0x1f, 0x18, 0x34, 0x7, 0x0, 0x80, 0x20, 0x8,
    0x3, 0x0, 0xc0, 0x10, 0x16, 0xc, 0x7c,

    /* U+0044 "D" */
    0xfe, 0x30, 0x6c, 0xb, 0x3, 0xc0, 0x70, 0x1c,
    0x7, 0x1, 0xc0, 0xf0, 0x2c, 0x3b, 0xf8,

    /* U+0045 "E" */
    0xff, 0xe0, 0x30, 0x18, 0xc, 0x7, 0xfb, 0x1,
    0x80, 0xc0, 0x60, 0x30, 0x1f, 0xf0,

    /* U+0046 "F" */
    0xff, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xff, 0xc0,
    0xc0, 0xc0, 0xc0, 0xc0,

    /* U+0047 "G" */
    0x1f, 0x18, 0x64, 0xf, 0x0, 0x80, 0x20, 0x8,
    0x7e, 0x3, 0xc0, 0xd0, 0x36, 0x18, 0x7c,

    /* U+0048 "H" */
    0xc1, 0xe0, 0xf0, 0x78, 0x3c, 0x1f, 0xff, 0x7,
    0x83, 0xc1, 0xe0, 0xf0, 0x78, 0x30,

    /* U+0049 "I" */
    0xff, 0xf0,

    /* U+004A "J" */
    0x1e, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x83, 0x7,
    0x8d, 0x11, 0xc0,

    /* U+004B "K" */
    0xc1, 0xb0, 0xcc, 0x63, 0x30, 0xd8, 0x3e, 0xe,
    0x83, 0x30, 0xc6, 0x30, 0xcc, 0x1b, 0x2,

    /* U+004C "L" */
    0xc1, 0x83, 0x6, 0xc, 0x18, 0x30, 0x60, 0xc1,
    0x83, 0x7, 0xf0,

    /* U+004D "M" */
    0xc0, 0x7c, 0x1f, 0x83, 0xf0, 0x7f, 0x1f, 0xa2,
    0xf4, 0x5e, 0x5b, 0xca, 0x79, 0x4f, 0x19, 0xe2,
    0x30,

    /* U+004E "N" */
    0xc1, 0xf0, 0xf8, 0x7e, 0x3d, 0x1e, 0x4f, 0x27,
    0x8b, 0xc5, 0xe1, 0xf0, 0xf8, 0x30,

    /* U+004F "O" */
    0x1e, 0x18, 0x6c, 0xb, 0x3, 0x80, 0x60, 0x18,
    0x6, 0x1, 0xc0, 0xd0, 0x26, 0x18, 0x78,

    /* U+0050 "P" */
    0xfe, 0x60, 0xf0, 0x78, 0x1c, 0x1e, 0xf, 0xf9,
    0x80, 0xc0, 0x60, 0x30, 0x18, 0x0,

    /* U+0051 "Q" */
    0x1e, 0x18, 0x6c, 0xb, 0x3, 0x80, 0x60, 0x18,
    0x6, 0x1, 0xc0, 0xd0, 0x26, 0x18, 0x78, 0x4,
    0x1, 0x80, 0x38,

    /* U+0052 "R" */
    0xfe, 0x60, 0xb0, 0x78, 0x3c, 0x1e, 0xb, 0xf9,
    0x8c, 0xc2, 0x61, 0xb0, 0x58, 0x30,

    /* U+0053 "S" */
    0x3e, 0x30, 0xf0, 0x78, 0x6, 0x1, 0xe0, 0x1c,
    0x3, 0x0, 0xc0, 0x70, 0x67, 0xc0,

    /* U+0054 "T" */
    0xff, 0x84, 0x2, 0x1, 0x0, 0x80, 0x40, 0x20,
    0x10, 0x8, 0x4, 0x2, 0x1, 0x0,

    /* U+0055 "U" */
    0xc1, 0xe0, 0xf0, 0x78, 0x3c, 0x1e, 0xf, 0x7,
    0x83, 0xc1, 0xa0, 0x98, 0xc7, 0xc0,

    /* U+0056 "V" */
    0xc0, 0x68, 0x9, 0x83, 0x30, 0x62, 0x8, 0x63,
    0x4, 0x40, 0x88, 0x1b, 0x1, 0x40, 0x28, 0x2,
    0x0,

    /* U+0057 "W" */
    0xc1, 0x7, 0x87, 0xd, 0xa, 0x13, 0x14, 0x26,
    0x28, 0xc4, 0xd9, 0x9, 0x12, 0x1a, 0x24, 0x34,
    0x58, 0x38, 0xe0, 0x60, 0xc0, 0xc1, 0x80,

    /* U+0058 "X" */
    0x60, 0xc4, 0x10, 0xc6, 0xd, 0x80, 0xa0, 0xc,
    0x3, 0x80, 0xd8, 0x11, 0x6, 0x31, 0x83, 0x20,
    0x20,

    /* U+0059 "Y" */
    0x41, 0x31, 0x88, 0x86, 0xc1, 0x40, 0xe0, 0x20,
    0x10, 0x8, 0x4, 0x2, 0x1, 0x0,

    /* U+005A "Z" */
    0x7f, 0x1, 0x80, 0x80, 0xc0, 0xc0, 0x40, 0x60,
    0x20, 0x30, 0x30, 0x10, 0x1f, 0xf0,

    /* U+005B "[" */
    0xf2, 0x49, 0x24, 0x92, 0x49, 0x38,

    /* U+005C "\\" */
    0x88, 0x44, 0x44, 0x22, 0x22, 0x11,

    /* U+005D "]" */
    0xe4, 0x92, 0x49, 0x24, 0x92, 0x78,

    /* U+005E "^" */
    0x38, 0x50, 0xa2, 0x24, 0x58, 0xc0,

    /* U+005F "_" */
    0xff, 0x80,

    /* U+0060 "`" */
    0xc8, 0x80,

    /* U+0061 "a" */
    0x7c, 0xc4, 0x6, 0x6, 0x7e, 0xc6, 0x86, 0x8e,
    0xf3,

    /* U+0062 "b" */
    0xc1, 0x83, 0x5, 0xec, 0x78, 0x70, 0xe1, 0xc3,
    0x87, 0x1d, 0xe0,

    /* U+0063 "c" */
    0x79, 0x8a, 0x4, 0x8, 0x10, 0x20, 0x62, 0x78,

    /* U+0064 "d" */
    0x6, 0xc, 0x1b, 0xfc, 0x70, 0xe1, 0xc3, 0x87,
    0xf, 0x1b, 0xd0,

    /* U+0065 "e" */
    0x7d, 0x8e, 0xc, 0x1f, 0xf0, 0x20, 0x63, 0x7c,

    /* U+0066 "f" */
    0x36, 0x6f, 0x66, 0x66, 0x66, 0x66,

    /* U+0067 "g" */
    0x7b, 0x96, 0x1c, 0x38, 0x70, 0xe1, 0xe7, 0x76,
    0xf, 0x13, 0xc0,

    /* U+0068 "h" */
    0x82, 0x8, 0x2e, 0xcf, 0x18, 0x61, 0x86, 0x18,
    0x61,

    /* U+0069 "i" */
    0xc3, 0xff, 0xff,

    /* U+006A "j" */
    0x41, 0x55, 0x55, 0x5c,

    /* U+006B "k" */
    0x81, 0x2, 0x4, 0x28, 0xd3, 0x2c, 0x78, 0xd9,
    0x12, 0x34, 0x30,

    /* U+006C "l" */
    0xff, 0xf0,

    /* U+006D "m" */
    0xb9, 0xd8, 0xcf, 0x10, 0xc2, 0x18, 0x43, 0x8,
    0x61, 0xc, 0x21, 0x84, 0x20,

    /* U+006E "n" */
    0xbb, 0x3c, 0x61, 0x86, 0x18, 0x61, 0x84,

    /* U+006F "o" */
    0x7d, 0x8e, 0xc, 0x18, 0x30, 0x60, 0xe3, 0x7c,

    /* U+0070 "p" */
    0xbd, 0x8f, 0xe, 0x1c, 0x38, 0x70, 0xe3, 0xbd,
    0x83, 0x6, 0x0,

    /* U+0071 "q" */
    0x7b, 0x8e, 0x1c, 0x38, 0x70, 0xe1, 0xe3, 0x7a,
    0xc, 0x18, 0x30,

    /* U+0072 "r" */
    0xbc, 0xc8, 0x88, 0x88, 0x80,

    /* U+0073 "s" */
    0x3c, 0x85, 0x3, 0x3, 0xc0, 0xc0, 0xa1, 0x3c,

    /* U+0074 "t" */
    0x66, 0xf6, 0x66, 0x66, 0x66, 0x70,

    /* U+0075 "u" */
    0x86, 0x18, 0x61, 0x86, 0x18, 0xf3, 0x74,

    /* U+0076 "v" */
    0xc7, 0x8d, 0x12, 0x26, 0xc5, 0xa, 0x14, 0x30,

    /* U+0077 "w" */
    0x84, 0x39, 0x4d, 0x29, 0x25, 0x24, 0xa4, 0xf5,
    0x8c, 0x61, 0x8c, 0x31, 0x80,

    /* U+0078 "x" */
    0x44, 0xd8, 0xa1, 0xc1, 0x5, 0xa, 0x22, 0xc6,

    /* U+0079 "y" */
    0xc6, 0x8d, 0x13, 0x26, 0xc5, 0xa, 0x1c, 0x10,
    0x20, 0xc7, 0x0,

    /* U+007A "z" */
    0xf8, 0xc4, 0x62, 0x33, 0x10, 0xf8,

    /* U+007B "{" */
    0x39, 0x8, 0x42, 0x11, 0x98, 0x61, 0x8, 0x42,
    0x10, 0xe0,

    /* U+007C "|" */
    0xff, 0xfe,

    /* U+007D "}" */
    0xe1, 0x8, 0x42, 0x10, 0xc3, 0x31, 0x8, 0x42,
    0x13, 0x80,

    /* U+007E "~" */
    0xe1, 0x24, 0x38,

    /* U+00B0 "°" */
    0x69, 0x96,

    /* U+2022 "•" */
    0xff, 0xff,

    /* U+F00C "" */
    0x0, 0x6, 0x0, 0xf, 0x0, 0x1f, 0x0, 0x3e,
    0x60, 0x7c, 0xf0, 0xf8, 0xf9, 0xf0, 0x7f, 0xe0,
    0x3f, 0xc0, 0x1f, 0x80, 0xf, 0x0, 0x6, 0x0,

    /* U+F021 "" */
    0x7, 0xe3, 0xf, 0xfb, 0x1c, 0x3f, 0x38, 0x1f,
    0x70, 0xf, 0x60, 0x7f, 0x60, 0x7f, 0x0, 0x0,
    0x0, 0x0, 0xfe, 0x6, 0xfe, 0x6, 0xf0, 0xe,
    0xf0, 0x1c, 0xfc, 0x38, 0xdf, 0xf0, 0xc7, 0xe0,

    /* U+F054 "" */
    0xc0, 0xe0, 0x70, 0x38, 0x1c, 0xe, 0x7, 0xe,
    0x1c, 0x38, 0x70, 0xe0, 0xc0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 71, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 71, .box_w = 1, .box_h = 12, .ofs_x = 2, .ofs_y = 0},
    {.bitmap_index = 3, .adv_w = 91, .box_w = 4, .box_h = 3, .ofs_x = 1, .ofs_y = 9},
    {.bitmap_index = 5, .adv_w = 142, .box_w = 9, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 19, .adv_w = 142, .box_w = 9, .box_h = 14, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 35, .adv_w = 228, .box_w = 12, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 53, .adv_w = 171, .box_w = 10, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 67, .adv_w = 49, .box_w = 1, .box_h = 3, .ofs_x = 1, .ofs_y = 9},
    {.bitmap_index = 68, .adv_w = 85, .box_w = 4, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 76, .adv_w = 85, .box_w = 4, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 84, .adv_w = 100, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 89, .adv_w = 150, .box_w = 7, .box_h = 7, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 96, .adv_w = 71, .box_w = 2, .box_h = 4, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 97, .adv_w = 85, .box_w = 3, .box_h = 1, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 98, .adv_w = 71, .box_w = 2, .box_h = 2, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 99, .adv_w = 71, .box_w = 4, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 105, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 116, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 127, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 138, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 149, .adv_w = 142, .box_w = 9, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 163, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 174, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 185, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 196, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 207, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 218, .adv_w = 71, .box_w = 2, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 221, .adv_w = 71, .box_w = 2, .box_h = 11, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 224, .adv_w = 150, .box_w = 7, .box_h = 7, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 231, .adv_w = 150, .box_w = 7, .box_h = 5, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 236, .adv_w = 150, .box_w = 7, .box_h = 7, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 243, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 254, .adv_w = 260, .box_w = 14, .box_h = 14, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 279, .adv_w = 171, .box_w = 11, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 296, .adv_w = 171, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 310, .adv_w = 185, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 325, .adv_w = 185, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 340, .adv_w = 171, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 354, .adv_w = 156, .box_w = 8, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 366, .adv_w = 199, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 381, .adv_w = 185, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 395, .adv_w = 71, .box_w = 1, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 397, .adv_w = 128, .box_w = 7, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 408, .adv_w = 171, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 423, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 434, .adv_w = 213, .box_w = 11, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 451, .adv_w = 185, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 465, .adv_w = 199, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 480, .adv_w = 171, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 494, .adv_w = 199, .box_w = 10, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 513, .adv_w = 185, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 527, .adv_w = 171, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 541, .adv_w = 156, .box_w = 9, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 555, .adv_w = 185, .box_w = 9, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 569, .adv_w = 171, .box_w = 11, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 586, .adv_w = 242, .box_w = 15, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 609, .adv_w = 171, .box_w = 11, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 626, .adv_w = 171, .box_w = 9, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 640, .adv_w = 156, .box_w = 9, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 654, .adv_w = 71, .box_w = 3, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 660, .adv_w = 71, .box_w = 4, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 666, .adv_w = 71, .box_w = 3, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 672, .adv_w = 120, .box_w = 7, .box_h = 6, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 678, .adv_w = 142, .box_w = 9, .box_h = 1, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 680, .adv_w = 85, .box_w = 3, .box_h = 3, .ofs_x = 1, .ofs_y = 10},
    {.bitmap_index = 682, .adv_w = 142, .box_w = 8, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 691, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 702, .adv_w = 128, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 710, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 721, .adv_w = 142, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 729, .adv_w = 71, .box_w = 4, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 735, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 746, .adv_w = 142, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 755, .adv_w = 57, .box_w = 2, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 758, .adv_w = 57, .box_w = 2, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 762, .adv_w = 128, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 773, .adv_w = 57, .box_w = 1, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 775, .adv_w = 213, .box_w = 11, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 788, .adv_w = 142, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 795, .adv_w = 142, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 803, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 814, .adv_w = 142, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 825, .adv_w = 85, .box_w = 4, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 830, .adv_w = 128, .box_w = 7, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 838, .adv_w = 71, .box_w = 4, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 844, .adv_w = 142, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 851, .adv_w = 128, .box_w = 7, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 859, .adv_w = 185, .box_w = 11, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 872, .adv_w = 128, .box_w = 7, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 880, .adv_w = 128, .box_w = 7, .box_h = 12, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 891, .adv_w = 128, .box_w = 5, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 897, .adv_w = 86, .box_w = 5, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 907, .adv_w = 67, .box_w = 1, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 909, .adv_w = 86, .box_w = 5, .box_h = 15, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 919, .adv_w = 150, .box_w = 7, .box_h = 3, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 922, .adv_w = 102, .box_w = 4, .box_h = 4, .ofs_x = 1, .ofs_y = 8},
    {.bitmap_index = 924, .adv_w = 90, .box_w = 4, .box_h = 4, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 926, .adv_w = 256, .box_w = 16, .box_h = 12, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 950, .adv_w = 256, .box_w = 16, .box_h = 16, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 982, .adv_w = 160, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_1[] = {
    0x0, 0x1f72, 0xef5c, 0xef71, 0xefa4
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 176, .range_length = 61349, .glyph_id_start = 96,
        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = 5, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 2,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t ui_font_lib_16 = {
#else
lv_font_t ui_font_lib_16 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 17,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if UI_FONT_LIB_16*/

