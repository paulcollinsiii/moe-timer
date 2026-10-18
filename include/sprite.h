#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Run-length-encoded 1-bit sprites: the decoder behind the four lock-screen
   art slots (charge, time's up, break, bed time). Pure -- no LVGL, no ESP-IDF -- so it host-tests directly; the paint
   (one lv_draw_rect per ink run, the way qr_draw_cb draws the setup QR)
   lives in display_screens.c.

   Encoding (tools/gen_sprites.py writes it, main/sprite_data.c holds it):
   the whole image is ONE row-major scan, split into runs that alternate
   colour starting with BACKGROUND, one byte per run. A run longer than 255
   is written as 255, 0, <rest>: the zero-length opposite run lets the same
   colour continue. Runs wrap across rows; the iterator splits them at row
   ends, so a caller only ever sees a run inside one row.

   The stream is flash data this module did not write, so a malformed one
   must degrade rather than overrun: a run that would pass w*h is clamped
   to the image, and a stream that ends early simply stops (what it never
   covered stays background). Nothing is read past rle_len and nothing is
   yielded outside w*h. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t w, h;
    const uint8_t *rle;
    uint16_t rle_len;
} sprite_t;

/* Iterator state. Opaque to callers: init it, then call next until false. */
typedef struct {
    const sprite_t *s;
    uint32_t pos;  /* next pixel index in the row-major scan */
    uint32_t left; /* pixels remaining in the current run */
    uint16_t i;    /* next byte of s->rle */
    bool cur_ink;  /* colour of the current run */
    bool next_ink; /* colour of the run after it */
} sprite_iter_t;

void sprite_iter_init(sprite_iter_t *it, const sprite_t *s);

/* Yields the next INK run as (x, y, len), len >= 1, entirely inside one
   row. A colour continued across a 255,0 boundary comes out as consecutive
   pieces; they abut, so painting them is identical to painting one run.
   Returns false when the image (or the stream) is exhausted. */
bool sprite_iter_next(sprite_iter_t *it, uint16_t *x, uint16_t *y, uint16_t *len);

#ifdef __cplusplus
}
#endif
