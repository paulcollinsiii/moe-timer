#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);
void audio_beep_sequence(void);    /* 3 beeps x 5 cycles = 15 s expiry alert */
void audio_break_alarm(void);      /* 2 beeps x 3 cycles (~6 s), stop-flag aware */
void audio_break_over_chime(void); /* short double-beep, fire-and-forget */
void audio_stop(void);

#ifdef __cplusplus
}
#endif
