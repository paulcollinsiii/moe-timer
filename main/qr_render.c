/* WiFi + MQTT provisioning: see include/qr_render.h for the contract. */
#include "qr_render.h"

#include <stdint.h>

#include "qrcodegen.h"

/* ---- panel-fit arithmetic (measured, not estimated) ----------------------

   QR_RENDER_MAX_VERSION (7) is qr_render.h's own ceiling; the arithmetic
   for WHY 7 is both sufficient and as large as this module needs lives
   there. What belongs here is why 7 fits the panel at the scale the
   screen builder draws it:

     45 modules (QR_RENDER_MAX_MODULES) at 2 px/module = 90x90 px of code.
     A 4-module quiet zone (the spec's own minimum, not merely "at least
     2") adds 8 px a side at that scale, for a 106x106 px block including
     its margin.
     The panel is 128 px tall: 128 - 106 = 22 px of slack, 11 px top and
     bottom if the block is centred. It fits, with margin to spare — no
     fallback to 1 px/module (illegible on this panel) was needed.

   The buffers below are sized for exactly QR_RENDER_MAX_VERSION, not
   qrcodegen's own 40-version worst case (qrcodegen_BUFFER_LEN_MAX, almost
   4 KB) — this module never asks qrcodegen for a version it could not
   draw on the panel anyway, so there is nothing to gain from a bigger
   buffer and a real static-RAM cost to avoiding it. */
#define QR_RENDER_BUF_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(QR_RENDER_MAX_VERSION)

/* qrcodegen_encodeText's own contract: tempBuffer and qrcode must each be
   at least this size and must not alias each other; tempBuffer holds no
   useful data once the call returns. Static, not stack — this project's
   screens render from the same task that runs the whole wake, and the
   httpd task that runs alongside a setup session has only 4 KB of stack,
   so neither buffer belongs on any caller's frame even though 2 x 255
   bytes is, on its own, a modest ask. Singleton state,
   like display.c's own s_lvbuf/s_panel_fb: one encode is ever in flight,
   synchronously, which is the only way this project renders a screen. */
static uint8_t s_temp[QR_RENDER_BUF_LEN];
static uint8_t s_qr[QR_RENDER_BUF_LEN];
static bool s_have_code;

bool qr_render_encode(const char *payload, int *size_out) {
    s_have_code = qrcodegen_encodeText(payload, s_temp, s_qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                       QR_RENDER_MAX_VERSION, qrcodegen_Mask_AUTO, /* boostEcl */ false);
    *size_out = s_have_code ? qrcodegen_getSize(s_qr) : 0;
    return s_have_code;
}

bool qr_render_module(int x, int y) {
    if (!s_have_code)
        return false;
    return qrcodegen_getModule(s_qr, x, y);
}
