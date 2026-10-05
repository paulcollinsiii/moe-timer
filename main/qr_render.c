/* WiFi + MQTT provisioning: see include/qr_render.h for the contract. */
#include "qr_render.h"

#include <stdint.h>
#include <stdlib.h>

#include "qrcodegen.h"

/* This module's own sizing: buffers sized for exactly QR_RENDER_MAX_VERSION
   (qr_render.h owns why 3 is enough), not qrcodegen's own 40-version
   worst case (qrcodegen_BUFFER_LEN_MAX, almost 4 KB) — this module never
   asks qrcodegen for a version it could not draw on the panel anyway, so
   there is nothing to gain from a bigger buffer. The panel-fit arithmetic
   itself (pixel scale, quiet zone, how the result sits on a 128 px-tall
   screen) belongs to display_screens.c, which owns the screen geometry,
   and is not restated here. */
#define QR_RENDER_BUF_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(QR_RENDER_MAX_VERSION)

/* The code qrcodegen last produced, heap-allocated at encode time rather
   than held in permanent static storage: this screen paints at most a
   handful of times across the device's whole life, and the setup
   session's own heap is tightest exactly while it runs (SoftAP + httpd +
   the SRP6a modexp) — 107 B of .bss sitting idle for the rest of the
   device's life is not a trade this module needs to make. Freed
   explicitly through qr_render_release() (called from display.c's setup
   wrapper right after render() returns) rather than only at the start of
   the next encode, so a caller can guarantee nothing holds it past the
   one render that was ever going to read it. */
static uint8_t *s_qr;
static bool s_have_code;
static int s_size;

bool qr_render_encode(const char *payload, int *size_out) {
    /* qrcodegen's own scratch space: dead the instant this call returns,
       so it lives on THIS function's stack frame — which, on this
       project, is the main task's (7168 B, CONFIG_ESP_MAIN_TASK_STACK_SIZE;
       this is the same task that runs the whole wake and calls this
       function, not the httpd task) — rather than in any static or heap
       storage. */
    uint8_t temp[QR_RENDER_BUF_LEN];

    qr_render_release(); /* whatever a previous call left behind is stale
                            the instant this one starts, win or lose */

    uint8_t *qr = malloc(QR_RENDER_BUF_LEN);
    if (qr == NULL) {
        *size_out = 0;
        return false; /* same contract as any other encode failure: text fallback */
    }

    bool ok = qrcodegen_encodeText(payload, temp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, QR_RENDER_MAX_VERSION,
                                   qrcodegen_Mask_AUTO,
                                   /* boostEcl: left off. qrcodegen's boost only raises
                                      ECC within the SAME version when there is room
                                      for it, so it can never push a payload to a
                                      larger version or turn a success into a
                                      failure — it is not a correctness lever here.
                                      It stays off so the one capacity ceiling this
                                      module documents (53 bytes at version 3, ECC
                                      LOW) is true of every encode this module ever
                                      does, rather than true only of the ones
                                      boosting happened not to touch. */
                                   false);
    if (!ok) {
        free(qr);
        *size_out = 0;
        return false;
    }

    s_qr = qr;
    s_have_code = true;
    s_size = qrcodegen_getSize(s_qr);
    *size_out = s_size;
    return true;
}

bool qr_render_module(int x, int y) {
    if (!s_have_code)
        return false;
    return qrcodegen_getModule(s_qr, x, y);
}

int qr_render_last_size(void) {
    return s_have_code ? s_size : 0;
}

void qr_render_release(void) {
    free(s_qr);
    s_qr = NULL;
    s_have_code = false;
    s_size = 0;
}
