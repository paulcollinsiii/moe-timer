#include "display.h"

#include "esp_log.h"

static const char *TAG = "display";

/* Temporary stub so the firmware links while the SSD1680/LVGL stack is
   built (Tasks 6-9). Replaced wholesale by the LVGL implementation. */
void display_init(void) {
    ESP_LOGI(TAG, "display_init (stub)");
}

void display_update(const display_state_t *state) {
    ESP_LOGI(TAG, "update: %ld s left (stub)", (long)state->remaining_sec);
}

void display_full_refresh(const display_state_t *state) {
    ESP_LOGI(TAG, "full refresh: %ld s left (stub)", (long)state->remaining_sec);
}

void display_timesup(void) {
    ESP_LOGI(TAG, "TIME'S UP (stub)");
}

void display_sync_failed(void) {
    ESP_LOGI(TAG, "sync failed screen (stub)");
}
