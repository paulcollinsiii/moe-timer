/* RIFF/WAVE header validation for the assets partition (pure C,
   host-tested). Accepts only what the DAC playback path can stream:
   PCM, mono, 16-bit, 8-22.05 kHz. */
#ifndef WAV_HEADER_H
#define WAV_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t data_offset; /* first PCM byte, relative to file start */
    uint32_t data_len;    /* bytes of PCM (caller clamps to partition) */
    uint32_t sample_rate;
} wav_info_t;

/* Parse from a prefix of the file (512 bytes is plenty for real WAVs).
   The data chunk may extend past the prefix — only its header must fit. */
bool wav_header_parse(const uint8_t *buf, size_t len, wav_info_t *out);

#endif /* WAV_HEADER_H */
