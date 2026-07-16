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

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

#endif /* LV_CONF_HOST_H */
