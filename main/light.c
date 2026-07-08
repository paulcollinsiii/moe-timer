/* MagTag ambient light: ALS-PT19 -> GPIO3 = ADC1_CH2 (Adafruit MagTag
   schematic; CircuitPython board.LIGHT). Shares battery.c's ADC1 unit —
   adc_oneshot units are single-owner. */
#include "light.h"

#include "battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "light";

#define LIGHT_ADC_CHANNEL ADC_CHANNEL_2 /* GPIO3 on ESP32-S2 */
#define LIGHT_SAMPLES 4

static bool s_configured;

void light_init(void) {
    adc_oneshot_unit_handle_t adc = (adc_oneshot_unit_handle_t)battery_adc_unit();
    if (adc == NULL) {
        ESP_LOGW(TAG, "ADC unit unavailable (battery_init failed?)");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_oneshot_config_channel(adc, LIGHT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc channel config failed: %s", esp_err_to_name(ret));
        return;
    }
    s_configured = true;
}

int light_read_mv(void) {
    adc_oneshot_unit_handle_t adc = (adc_oneshot_unit_handle_t)battery_adc_unit();
    if (!s_configured || adc == NULL)
        return -1;
    int sum = 0, samples = 0;
    for (int i = 0; i < LIGHT_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(adc, LIGHT_ADC_CHANNEL, &raw) != ESP_OK)
            continue;
        /* Uncalibrated: 12 dB full-scale ~= 2500 mV / 4095. Trend data,
           not lux — good enough for HA graphs. */
        sum += raw * 2500 / 4095;
        samples++;
    }
    return (samples > 0) ? sum / samples : -1;
}
