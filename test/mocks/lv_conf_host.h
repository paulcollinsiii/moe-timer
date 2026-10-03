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
   in the firmware's sdkconfig.defaults. Mirrored here so the goldens stay
   the layout contract under the same compiled-in widget/format set. */
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
   stays 16 (host-only; untouched per task instructions) but no SW-draw unit
   besides I1 is compiled in, matching the firmware. */
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

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

#endif /* LV_CONF_HOST_H */
