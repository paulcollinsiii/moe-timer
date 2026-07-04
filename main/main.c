#include "esp_log.h"
#include "esp_sleep.h"
#include "neopixel.h"

static const char *TAG = "main";

void app_main(void) {
    /* MUST be first: ensures GPIO 21 power gate is HIGH (NeoPixels off) */
    neopixel_init();
    ESP_LOGI(TAG, "MagTag Screen Timer boot");
    /* stub: full implementation in Stream 4 */
}
