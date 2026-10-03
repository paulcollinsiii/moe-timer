/* LVGL configuration for the HOST golden-render tests (test_display_render).
   Everything not set here falls back to lv_conf_internal.h defaults; the
   options below mirror what the firmware build sets via Kconfig so host
   renders match the panel: clib stdlib, the montserrat sizes the layouts
   use, and I1 software-render support for the 1bpp e-ink framebuffer. */
#ifndef LV_CONF_HOST_H
#define LV_CONF_HOST_H

#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_COLOR_DEPTH 16
#define LV_DRAW_SW_SUPPORT_I1 1

/* LVGL size trim mirror (size-spike task 0,
   docs/planning/20261003.wifi-provisioning.plan.md): the firmware only ever
   creates lv_label and lv_bar widgets and renders 1-bit, so every other
   widget, layout engine and non-I1 software-draw pixel format is disabled
   in the firmware's sdkconfig.defaults. What is mirrored here is exactly
   that — the compiled-in widget set and the compiled-in software-draw
   pixel-format set — and nothing else about the render configuration: the
   theme and LV_COLOR_DEPTH below are deliberately NOT mirrored (see the
   divergence note below). Goldens stay the layout contract under the same
   widget/format set; they are not proof the two builds render identically
   in every respect. */
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 0
#define LV_USE_ARCLABEL 0
#define LV_USE_BUTTON 0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CALENDAR_HEADER_ARROW 0
#define LV_USE_CALENDAR_HEADER_DROPDOWN 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGE 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_FLEX 0
#define LV_USE_GRID 0
#define LV_USE_OBSERVER 0

/* Pixel formats: I1 only (see LV_DRAW_SW_SUPPORT_I1 above). LV_COLOR_DEPTH
   stays 16 because nothing here depends on it being 1: the render surface
   itself is set explicitly and independently of LV_COLOR_DEPTH —
   test_display_render.c calls lv_display_set_color_format(s_disp,
   LV_COLOR_FORMAT_I1) on the display object, so every golden already
   renders through the I1 path no matter what LV_COLOR_DEPTH says. What
   LV_COLOR_DEPTH actually controls is LV_COLOR_FORMAT_NATIVE (lv_color.h),
   LVGL's fallback format for things with no explicit format of their own —
   lv_color_white()/lv_color_black(), and the render layer LVGL allocates
   for an opacity-layered or transformed widget when that layer does not
   need per-pixel alpha (lv_refr.c:565). None of that is exercised by this
   UI's plain lv_label/lv_bar widgets today. Changing LV_COLOR_DEPTH to 1 is
   therefore not needed to mirror the firmware's trim above, and was left
   alone rather than made unnecessarily to match firmware depth — see the
   divergence note right after the pixel-format list below.

   DO NOT change this to match the firmware's depth. It could move
   goldens, and nothing requires the change (above). */
#define LV_DRAW_SW_SUPPORT_RGB565 0
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 0
#define LV_DRAW_SW_SUPPORT_RGB565A8 0
#define LV_DRAW_SW_SUPPORT_RGB888 0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0
#define LV_DRAW_SW_SUPPORT_L8 0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_A8 0

/* Known host/firmware divergence, left as-is (do not "fix" by matching the
   firmware — either change below could move goldens):
   - Theme: this file sets no LV_USE_THEME_*, so the host falls back to
     lv_conf_internal.h's defaults — LV_USE_THEME_DEFAULT enabled,
     LV_USE_THEME_MONO disabled. The firmware's sdkconfig.defaults does the
     opposite (CONFIG_LV_USE_THEME_MONO=y, THEME_DEFAULT off). The host
     therefore renders under LVGL's default theme, not the mono theme the
     panel actually uses.
   - Native format: with LV_COLOR_DEPTH 16 (above), the host's
     LV_COLOR_FORMAT_NATIVE is RGB565, but LV_DRAW_SW_SUPPORT_RGB565 is off
     above. Today nothing asks for that native format (plain lv_label/
     lv_bar don't), so this is latent. The day a screen gets an opacity
     (opa < COVER), a transform or clip_corner on a part without per-pixel
     alpha, LVGL would render that layer as RGB565 on the host and as I1 on
     the device (lv_refr.c:565) — an unsupported format draws blank with
     LV_USE_LOG off (same failure mode as the ARGB8888/QR trap in
     sdkconfig.defaults), so the host render could come out blank while the
     firmware's stays correct. A golden failure that only reproduces on
     host, right after such a style change, is more likely this divergence
     than a real bug. */

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

#endif /* LV_CONF_HOST_H */
