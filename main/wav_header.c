/* RIFF/WAVE header walk (see wav_header.h). No ESP includes. */
#include "wav_header.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

bool wav_header_parse(const uint8_t *buf, size_t len, wav_info_t *out) {
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return false;

    bool fmt_ok = false;
    uint32_t sample_rate = 0;
    size_t pos = 12;

    /* Chunk walk: id(4) size(4) payload(size, word-padded). Only the data
       chunk's payload may extend beyond the prefix we were given. */
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        uint32_t size = rd32(buf + pos + 4);
        size_t payload = pos + 8;

        if (memcmp(id, "fmt ", 4) == 0) {
            if (size < 16 || payload + 16 > len)
                return false;
            uint16_t format = rd16(buf + payload);
            uint16_t channels = rd16(buf + payload + 2);
            sample_rate = rd32(buf + payload + 4);
            uint16_t bits = rd16(buf + payload + 14);
            if (format != 1 /* PCM */ || channels != 1 || bits != 16)
                return false;
            if (sample_rate < 8000 || sample_rate > 22050)
                return false;
            fmt_ok = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!fmt_ok || size == 0)
                return false;
            out->data_offset = (uint32_t)payload;
            out->data_len = size;
            out->sample_rate = sample_rate;
            return true;
        }

        /* Only data may extend past the prefix, and data returned above —
           any other chunk claiming more than the prefix is corrupt. */
        uint32_t advance = size + (size & 1u); /* word padding */
        if (advance > len - payload)
            return false;
        pos = payload + advance;
    }
    return false;
}
