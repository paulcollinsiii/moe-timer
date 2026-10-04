# qrcodegen

Source: Project Nayuki's QR Code generator library (C), MIT License.
https://www.nayuki.io/page/qr-code-generator-library

Vendored from the copy already present in this tree's LVGL dependency
(`managed_components/lvgl__lvgl/src/libs/qrcode/`, LVGL 9.5.0), which is
itself Nayuki's library with a thin LVGL wrapper around it. That wrapper —
the `#include "lvgl.h"` / `#ifdef LV_USE_QRCODE` guard, the
`LV_STD{BOOL,DEF,INT}_INCLUDE` macro indirection, and `LV_ASSERT` — is
stripped here so the two files are plain C with no LVGL dependency:
`qrcodegen.h` includes `<stdbool.h>`/`<stddef.h>`/`<stdint.h>` directly, and
`qrcodegen.c` uses `<assert.h>`'s `assert()`. No other line changed. This
project's own QR drawing never enables `LV_USE_QRCODE` and never links
LVGL's copy — see `main/qr_render.c`.

Two functions near the end of both files (`qrcodegen_version2size`,
`qrcodegen_getMinFitVersion`) are LVGL-fork additions, not part of
upstream Nayuki's API. They have no LVGL dependency of their own, so they
were left in rather than deleted along with the wrapper; this project does
not call either.

`LICENSE.txt` is Nayuki's own, copied unchanged.
