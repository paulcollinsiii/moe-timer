/* Snapshot save/restore policy, moved out of main.c. Policy only: the
   snapshot's shape and its validation belong to timer.c, the blob's
   storage to nvs_config.c. What lives here is the pair of decisions —
   when a write is worth the flash wear, and when a stored day may
   overrule the live RTC state. */
#include "timer_persist.h"

#include <string.h>

#include "nvs_config.h"
#include "timer.h"

#ifndef NATIVE
#include "esp_log.h"
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

static const char *TAG = "timer_persist";

void timer_persist_save(void) {
    timer_snapshot_t snap, stored;
    timer_make_snapshot(&snap);
    /* Short-circuit order is load-bearing: a load that fails (missing,
       truncated, stale layout) leaves `stored` partly or wholly
       uninitialised, so the memcmp must not run — comparing against
       unspecified stack bytes would decide "unchanged" at random. */
    if (nvs_config_load_timer_snapshot(&stored) == ESP_OK && memcmp(&snap, &stored, sizeof(snap)) == 0) {
        return;
    }
    esp_err_t ret = nvs_config_save_timer_snapshot(&snap);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "snapshot save failed: %s", esp_err_to_name(ret));
    }
}

bool timer_persist_try_restore(time_t now) {
    if (timer_current_date()[0] != '\0')
        return false; /* RTC state intact — normal deep-sleep wake */
    timer_snapshot_t snap;
    if (nvs_config_load_timer_snapshot(&snap) != ESP_OK)
        return false;
    if (!timer_restore_snapshot(&snap, now))
        return false;
    /* Hoisted out of the log argument (HAZ-1). The (void) is not
       decoration: the host stub above discards its varargs, which would
       leave this set-but-unused under -Wall. */
    const timer_state_t st = timer_get_state();
    (void)st;
    ESP_LOGW(TAG, "Timer state restored from NVS snapshot, state=%d", (int)st);
    return true;
}
