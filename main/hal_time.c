#include "hal_time.h"

#include <time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

time_t hal_time_now(void) {
    return time(NULL);
}

void hal_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t hal_time_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}
