#pragma once
#include <stdbool.h>

/* WiFi + MQTT provisioning: the QR module matrix behind the setup screen's
   code. Pure — no LVGL, no ESP-IDF — so it host-tests directly; the
   drawing itself (turning this matrix into panel pixels) lives in
   display_screens.c, the same file that turns every other piece of state
   in this tree into widgets — and which also owns the panel-fit
   arithmetic (pixel scale, quiet zone, how a version-7 code sits on this
   128 px-tall panel): this header and qr_render.c are about what this
   module asks qrcodegen for, not about the screen, so that arithmetic is
   not restated here.

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

/* The largest QR version this module will ever produce. setup_session.h's
   QR payload is always JSON (braces, colons, quotes, a lowercase
   "ver"/"name"/etc.), so qrcodegen always falls back to byte mode for it.
   Measured directly against this library: byte mode at error-correction
   LOW fits up to 154 bytes at version 7 (45x45 modules). The real payload
   this device ever sends is a fixed 132 bytes (device_id()'s SSID is
   always exactly 13 characters — see setup_session.c), nowhere near that
   ceiling; 7 is sized for the slack setup_session.h's QR buffer reserves
   above that real payload, not for a size this device has ever produced.
   Capping maxVersion here is what makes a too-long payload fail cleanly
   instead of growing into a QR the panel has no room for: see
   qr_render_encode()'s doc comment. */
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
   for this code until the next call to this function or to
   qr_render_release() below, whichever comes first. On failure — the
   payload does not fit any version up to QR_RENDER_MAX_VERSION at ECC
   LOW, or the heap has nothing left to give it — returns false and sets
   *size_out to 0, and qr_render_module() answers false (white) for every
   coordinate until the next successful encode: the screen that calls
   this is expected to fall back to rendering the payload's fields as
   plain text instead of a QR, never to draw a blank or stale code.
   size_out must not be NULL. */
bool qr_render_encode(const char *payload, int *size_out);

/* The color of module (x, y) of the code the last successful
   qr_render_encode() produced — false for white, true for black — with
   (0, 0) at the top left, matching qrcodegen_getModule()'s own
   convention (this is a thin wrapper over it). Out-of-range coordinates,
   and every coordinate when the last encode failed, was released, or
   none has run yet, answer false. */
bool qr_render_module(int x, int y);

/* The modules-per-side of whatever qr_render_module() is currently
   answering from — 0 if the last encode failed, was released, or none
   has run yet. A draw callback reads its loop bound from this AT DRAW
   TIME rather than from a value captured when the on-screen object was
   built, so the size and the matrix it walks always come from the same
   encode — a second qr_render_encode() landing between build and draw
   (not reachable today: this project renders one screen synchronously,
   start to finish, on a single task) cannot leave it drawing a stale
   size against a fresh matrix, or vice versa. */
int qr_render_last_size(void);

/* Frees the heap buffer the last qr_render_encode() allocated. Safe to
   call with no code held (a no-op) or more than once. qr_render_module()
   answers false for every coordinate afterwards, the same as after a
   failed encode, and never dereferences the freed buffer to get there —
   so a caller may free right after the one render that reads this code
   (display.c's setup wrapper does, immediately after render() returns)
   without racing anything that still needs it. qr_render_encode() also
   calls this on itself before it does anything else, so a caller never
   has to call it before encoding again — only after the LAST render that
   will ever read a given code. */
void qr_render_release(void);

#ifdef __cplusplus
}
#endif
