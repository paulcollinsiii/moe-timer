#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);
void audio_beep_sequence(void); /* 3 beeps x 5 cycles = 15 s alert */
void audio_stop(void);

#ifdef __cplusplus
}
#endif
