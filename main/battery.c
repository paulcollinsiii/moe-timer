/* MagTag battery gauge: VBAT -> 100k/100k divider -> GPIO4 = ADC1_CH3
   (Adafruit MagTag schematic; CircuitPython board.BATTERY). */
#include "battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define BATT_ADC_UNIT ADC_UNIT_1
#define BATT_ADC_CHANNEL ADC_CHANNEL_3 /* GPIO4 on ESP32-S2 */
#define BATT_ADC_ATTEN ADC_ATTEN_DB_12
#define BATT_SAMPLES 4

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

void battery_init(void) {
    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = BATT_ADC_UNIT};
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc unit init failed: %s", esp_err_to_name(ret));
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc channel config failed: %s", esp_err_to_name(ret));
        return;
    }
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = BATT_ADC_UNIT,
        .atten = BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "adc calibration unavailable: %s", esp_err_to_name(ret));
        s_cali = NULL; /* raw-only fallback handled in battery_read_mv */
    }
}

void *battery_adc_unit(void) {
    return s_adc; /* shared with light.c — one owner per ADC unit */
}

int battery_read_mv(void) {
    if (!s_adc)
        return -1;
    int sum_mv = 0, samples = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) != ESP_OK)
            continue;
        int mv;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            sum_mv += mv;
        } else {
            /* No calibration: approximate 12 dB full-scale ~= 2500 mV / 4095 */
            sum_mv += raw * 2500 / 4095;
        }
        samples++;
    }
    if (samples == 0)
        return -1;
    /* x2 for the on-board 100k/100k divider */
    return (sum_mv / samples) * 2;
}
