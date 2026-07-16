/* Alert audio: DAC-synthesized tones + optional WAV from the assets
   partition. The ESP32-S2 DAC channel 0 IS the MagTag speaker pin
   (GPIO17); the old LEDC square wave drove the same pin digitally, which
   is why the beeps were harsh. Tone tables + sample renderer live in
   tones.c (pure, host-tested); this file is the DAC pump. */
#include "audio.h"

#include <string.h>

#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "sdkconfig.h"
#include "tones.h"
#include "wav_header.h"

static const char *TAG = "audio";

#define AMP_ENABLE_GPIO GPIO_NUM_16
#define SYNTH_RATE_HZ 16000u
#define CYCLE_GAP_MS 1200
/* 4 x 1024 B DMA ring = 256 ms of buffer at 16 kHz: deep enough that the
   render loop never starves it, shallow enough that audio_stop() and the
   end-of-tone amp-off land within a beat. */
#define DAC_DESC_NUM 4
#define DAC_BUF_SIZE 1024
#define ASSETS_SUBTYPE ((esp_partition_subtype_t)0x40)

static volatile bool s_stop_requested = false;
static bool s_initialized;
static dac_continuous_handle_t s_dac;
static uint32_t s_dac_rate;

/* DMA-fed buffers stay in internal RAM (static, not PSRAM/stack). */
static uint8_t s_pcm[DAC_BUF_SIZE];
static uint8_t s_raw[2 * DAC_BUF_SIZE]; /* 16-bit WAV chunk before downmix */

/* Lazy: first play this wake initializes. Most wakes never make a sound —
   configuring the amp pin for them was pure awake-time overhead. Until
   then the deep-sleep hold keeps the amp pin low (amp off), which is
   exactly the state init would set. */
void audio_init(void) {
    if (s_initialized)
        return;
    s_initialized = true;
    /* Release the deep-sleep hold placed by enter_deep_sleep() */
    gpio_hold_dis(AMP_ENABLE_GPIO);
    gpio_config_t amp_cfg = {
        .pin_bit_mask = (1ULL << AMP_ENABLE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&amp_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(amp) failed: %s", esp_err_to_name(ret));
    }
    gpio_set_level(AMP_ENABLE_GPIO, 0);
}

/* (Re)create the continuous-DAC channel at the given sample rate. The
   synth always runs at SYNTH_RATE_HZ; a WAV re-creates at its own rate. */
static bool dac_ensure(uint32_t rate) {
    if (s_dac != NULL && s_dac_rate == rate)
        return true;
    if (s_dac != NULL) {
        dac_continuous_disable(s_dac);
        dac_continuous_del_channels(s_dac);
        s_dac = NULL;
    }
    dac_continuous_config_t cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num = DAC_DESC_NUM,
        .buf_size = DAC_BUF_SIZE,
        .freq_hz = rate,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    esp_err_t ret = dac_continuous_new_channels(&cfg, &s_dac);
    if (ret == ESP_OK)
        ret = dac_continuous_enable(s_dac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dac setup (%lu Hz) failed: %s", (unsigned long)rate, esp_err_to_name(ret));
        if (s_dac != NULL) {
            dac_continuous_del_channels(s_dac);
            s_dac = NULL;
        }
        return false;
    }
    s_dac_rate = rate;
    return true;
}

static void dac_teardown(void) {
    if (s_dac == NULL)
        return;
    dac_continuous_disable(s_dac);
    dac_continuous_del_channels(s_dac);
    s_dac = NULL;
    s_dac_rate = 0;
}

/* Push one rendered chunk into the DMA ring, honoring the stop flag. */
static void dac_push(const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n && !s_stop_requested) {
        size_t loaded = 0;
        esp_err_t ret = dac_continuous_write(s_dac, (uint8_t *)buf + off, n - off, &loaded, 250);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "dac write failed: %s", esp_err_to_name(ret));
            return;
        }
        off += loaded;
    }
}

/* Let the DMA ring play out before silencing the amp, so tone tails are
   not clipped (the ring holds <= 256 ms). */
static void dac_drain(void) {
    if (!s_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(1000 * DAC_DESC_NUM * DAC_BUF_SIZE / (s_dac_rate ? s_dac_rate : SYNTH_RATE_HZ)));
    }
}

static void play_synth(int tone_id, int volume_pct) {
    if (!dac_ensure(SYNTH_RATE_HZ))
        return;
    tone_player_t player;
    tones_player_init(&player, tone_id, SYNTH_RATE_HZ);
    tones_player_set_volume(&player, volume_pct);
    size_t n;
    while (!s_stop_requested && (n = tones_render(&player, s_pcm, sizeof(s_pcm))) > 0) {
        dac_push(s_pcm, n);
    }
}

/* Stream the assets-partition WAV: header-validate, then read 16-bit LE
   chunks and downmix to the DAC's unsigned 8 bits. Returns false when
   there is no playable WAV (caller falls back to a synth tone). */
static bool play_wav(int volume_pct) {
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ASSETS_SUBTYPE, "assets");
    if (part == NULL) {
        ESP_LOGW(TAG, "no assets partition");
        return false;
    }
    uint8_t hdr[512];
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK)
        return false;
    wav_info_t info;
    if (!wav_header_parse(hdr, sizeof(hdr), &info) || info.data_offset >= part->size) {
        ESP_LOGW(TAG, "assets partition holds no playable WAV");
        return false;
    }
    if (info.data_len > part->size - info.data_offset) {
        info.data_len = part->size - info.data_offset; /* clamp to partition */
    }
    if (!dac_ensure(info.sample_rate))
        return false;

    uint32_t pos = 0;
    while (pos < info.data_len && !s_stop_requested) {
        uint32_t n = info.data_len - pos;
        if (n > sizeof(s_raw))
            n = sizeof(s_raw);
        n &= ~1u; /* whole 16-bit samples */
        if (n == 0)
            break;
        if (esp_partition_read(part, info.data_offset + pos, s_raw, n) != ESP_OK)
            break;
        for (uint32_t i = 0; i < n / 2; i++) {
            int16_t s = (int16_t)((uint16_t)s_raw[2 * i] | ((uint16_t)s_raw[2 * i + 1] << 8));
            int32_t v = ((int32_t)s * volume_pct) / 100; /* >100% clips */
            if (v > INT16_MAX)
                v = INT16_MAX;
            else if (v < INT16_MIN)
                v = INT16_MIN;
            s_pcm[i] = (uint8_t)(v / 256 + 128);
        }
        dac_push(s_pcm, n / 2);
        pos += n;
    }
    return true;
}

void audio_play_tone(int tone_id, int cycles) {
    /* HA-configurable volume, one NVS read per alert (not per cycle). */
    uint16_t volume = NVS_DEFAULT_ALERT_VOLUME;
    (void)nvs_config_get_alert_volume(&volume);
    if (volume > TONES_VOLUME_MAX)
        volume = TONES_VOLUME_MAX; /* stored by a future/older firmware */

    s_stop_requested = false;
    audio_init();
    gpio_set_level(AMP_ENABLE_GPIO, 1);
    for (int cycle = 0; cycle < cycles && !s_stop_requested; cycle++) {
        if (tone_id == TONE_CUSTOM) {
            if (!play_wav(volume)) {
                play_synth(TONE_CHIME, volume); /* audible fallback beats silence */
            }
        } else {
            play_synth(tone_id, volume);
        }
        if (!s_stop_requested && cycle + 1 < cycles) {
            dac_drain();
            vTaskDelay(pdMS_TO_TICKS(CYCLE_GAP_MS));
        }
    }
    dac_drain();
    gpio_set_level(AMP_ENABLE_GPIO, 0);
    dac_teardown();
}

/* Per-alert tone from NVS (HA select); clamp shields against a stored
   index from a future/older firmware. */
static int cfg_tone(esp_err_t (*get)(uint16_t *), int fallback) {
    uint16_t v = (uint16_t)fallback;
    (void)get(&v);
    return tones_clamp_id((int)v, fallback);
}

void audio_beep_sequence(void) {
    audio_play_tone(cfg_tone(nvs_config_get_tone_expiry, NVS_DEFAULT_TONE_EXPIRY), CONFIG_MAGTAG_EXPIRY_ALARM_CYCLES);
}

void audio_break_alarm(void) {
    audio_play_tone(cfg_tone(nvs_config_get_tone_break, NVS_DEFAULT_TONE_BREAK), CONFIG_MAGTAG_BREAK_ALARM_CYCLES);
}

void audio_bedtime_alarm(void) {
    audio_play_tone(cfg_tone(nvs_config_get_tone_bed, NVS_DEFAULT_TONE_BED), CONFIG_MAGTAG_BREAK_ALARM_CYCLES);
}

void audio_break_over_chime(void) {
    /* "Your timer changed" chirp: fixed Ding-ding so it stays distinct
       from whatever the configurable alarms are set to. */
    audio_play_tone(TONE_DING, 1);
}

void audio_stop(void) {
    s_stop_requested = true;
    /* Cut the amp immediately (safe cross-task); the playing task exits
       on the flag and tears the DAC down itself. */
    if (s_initialized) {
        gpio_set_level(AMP_ENABLE_GPIO, 0);
    }
}
