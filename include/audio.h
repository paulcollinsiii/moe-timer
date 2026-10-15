#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Optional — every audio call self-initializes on first use per wake. */
void audio_init(void);

/* Play a tone_id_t (see tones.h) for the given number of cycles with a
   short gap between cycles; stop-flag aware. TONE_CUSTOM streams the
   assets-partition WAV and falls back to the chime when absent/invalid. */
void audio_play_tone(int tone_id, int cycles);

/* Alert wrappers: tone comes from the HA-configurable NVS selection. */
void audio_beep_sequence(void);    /* timer expiry alarm */
void audio_break_alarm(void);      /* screen-break alarm */
void audio_bedtime_alarm(void);    /* bed-time alarm */
void audio_break_over_chime(void); /* short fixed chirp, fire-and-forget */
void audio_locate_alarm(void);     /* find-my-timer: classic beeps, max volume */
/* One reward jingle once the last chore is ticked: the HA "Chores done tone"
   selection (its own list, default Off), at the shared alert volume. Off or
   volume 0 returns without touching the DAC or amp. Plays through quiet
   hours — the only audio that does (chore checklist design 9). */
void audio_chores_done_chime(void);
void audio_stop(void);

#ifdef __cplusplus
}
#endif
