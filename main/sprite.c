/* Run-length sprite decoder; the encoding is described in sprite.h. */
#include "sprite.h"

void sprite_iter_init(sprite_iter_t *it, const sprite_t *s) {
    it->s = s;
    it->pos = 0;
    it->left = 0;
    it->i = 0;
    it->cur_ink = false;
    it->next_ink = false; /* the first run is always background */
}

bool sprite_iter_next(sprite_iter_t *it, uint16_t *x, uint16_t *y, uint16_t *len) {
    const sprite_t *s = it->s;
    const uint32_t total = (uint32_t)s->w * s->h;

    for (;;) {
        if (it->pos >= total)
            return false; /* the image is complete; any further bytes are ignored */
        if (it->left == 0) {
            if (it->i >= s->rle_len)
                return false; /* truncated stream: the rest stays background */
            it->left = s->rle[it->i++];
            it->cur_ink = it->next_ink;
            it->next_ink = !it->next_ink;
            /* A run that would pass the last pixel is clamped, never painted
               beyond it. */
            if (it->left > total - it->pos)
                it->left = total - it->pos;
            continue; /* a zero-length run (the 255,0 continuation) loops here */
        }
        /* Split at the row end so the caller only ever sees one row. */
        uint32_t col = it->pos % s->w;
        uint32_t n = s->w - col;
        if (n > it->left)
            n = it->left;
        bool ink = it->cur_ink;
        uint32_t at = it->pos;
        it->pos += n;
        it->left -= n;
        if (ink) {
            *x = (uint16_t)col;
            *y = (uint16_t)(at / s->w);
            *len = (uint16_t)n;
            return true;
        }
    }
}
