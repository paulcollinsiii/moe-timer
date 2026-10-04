#pragma once
#include <stdbool.h>

/* WiFi + MQTT provisioning: the QR module matrix behind the setup screen's
   code. Pure — no LVGL, no ESP-IDF — so it host-tests directly; the
   drawing itself (turning this matrix into panel pixels) lives in
   display_screens.c, the same file that turns every other piece of state
   in this tree into widgets.

   Wraps the vendored qrcodegen library (lib/qrcodegen) rather than LVGL's
   own QR widget: LV_USE_QRCODE draws through an indexed-image canvas,
   which blends through ARGB8888 before reaching the panel's I1 format, and
   this project's sdkconfig.defaults turns ARGB8888 support off (see the
   plan's "QR rendering trap"). Drawing filled rectangles straight onto an
   lv_obj — the path every bar on this panel already uses — needs only the
   module matrix, which is all this header hands over. */

#ifdef __cplusplus
extern "C" {
#endif

/* The largest QR version this module will ever produce, and why 7 is
   enough: setup_session.h's QR payload maxes out at 150 bytes (a 31-byte
   SSID plus the fixed 10-char AP password used twice — see
   SETUP_SESSION_QR_MAX's own comment for the exact skeleton), and that
   payload is never alphanumeric-only (it is JSON — braces, colons,
   quotes, a lowercase "ver"/"name"/etc.), so qrcodegen always falls back
   to byte mode for it. Measured directly against this library: byte mode
   at error-correction LOW fits up to 154 bytes at version 7 (45x45
   modules), and the worst case this project ever hands it is 150 — 4
   bytes of headroom, not a coincidence to be shrunk. Capping maxVersion
   here is what makes a too-long payload fail cleanly instead of growing
   into a QR the panel has no room for: see qr_render_encode()'s doc
   comment and the panel-fit arithmetic in main/qr_render.c. */
#define QR_RENDER_MAX_VERSION 7

/* Modules per side at QR_RENDER_MAX_VERSION, the QR Code Model 2 formula
   (4 * version + 17). Odd, and the ceiling qr_render_module()'s valid
   coordinate range ever reaches. */
#define QR_RENDER_MAX_MODULES (4 * QR_RENDER_MAX_VERSION + 17)

/* Encodes `payload` (a NUL-terminated string) at error-correction LOW,
   choosing the smallest version from 1 to QR_RENDER_MAX_VERSION that
   fits — never a higher error-correction level even if one would still
   fit at the same version (this module always reports the version it
   actually used via *size_out, and a silently-boosted ECC would still
   report the same version, which is the only thing a caller sizing a
   fixed on-panel box can use).

   On success, returns true and sets *size_out to the modules-per-side
   count (odd, 21..QR_RENDER_MAX_MODULES); qr_render_module() then answers
   for this code until the next call here. On failure — the payload does
   not fit any version up to QR_RENDER_MAX_VERSION at ECC LOW — returns
   false and sets *size_out to 0, and qr_render_module() answers false
   (white) for every coordinate until the next successful encode: the
   screen that calls this is expected to fall back to rendering the
   payload's fields as plain text instead of a QR, never to draw a blank
   or stale code. size_out must not be NULL. */
bool qr_render_encode(const char *payload, int *size_out);

/* The color of module (x, y) of the code the last successful
   qr_render_encode() produced — false for white, true for black — with
   (0, 0) at the top left, matching qrcodegen_getModule()'s own
   convention (this is a thin wrapper over it). Out-of-range coordinates,
   and every coordinate when the last encode failed or none has run yet,
   answer false. */
bool qr_render_module(int x, int y);

#ifdef __cplusplus
}
#endif
