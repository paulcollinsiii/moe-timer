#include "config_cache.h"

#include <stdint.h>

#include "bedtime.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "quiet_hours.h"
#include "schedule.h"
#include "time_util.h"

/* Status pixels stay dark during configured quiet hours (alert pulses are
   exempt — they accompany an audible, dismissable alarm). The window is
   read from NVS once per wake — the quiet callback fires from the LED
   task on every pixel update. */
static bool s_quiet_cfg_loaded;
static uint16_t s_quiet_start_cfg;
static uint16_t s_quiet_end_cfg;

/* Bed time follows the same wake-scoped cache pattern; both caches are
   dropped by config_cache_invalidate() after a network window so an HA
   edit applies within the same wake. */
static bool s_bedtime_cfg_loaded;
static uint16_t s_bedtime_cfg;

bool config_cache_quiet_active(time_t now) {
    int now_min = time_util_minutes_of_day(now);
    if (!s_quiet_cfg_loaded) {
        /* Belt-and-braces seeding, not a load-bearing step: the getters
           already write the same default into *out unconditionally before
           touching flash (get_u16_with_default, nvs_config.c), which is
           why their return can be ignored here. The seeds keep this call
           site readable without having to know that. */
        s_quiet_start_cfg = NVS_DEFAULT_QUIET_START;
        s_quiet_end_cfg = NVS_DEFAULT_QUIET_END;
        nvs_config_get_quiet_start(&s_quiet_start_cfg);
        nvs_config_get_quiet_end(&s_quiet_end_cfg);
        s_quiet_cfg_loaded = true;
    }
    return quiet_hours_active(now_min, quiet_hhmm_to_minutes(s_quiet_start_cfg),
                              quiet_hhmm_to_minutes(s_quiet_end_cfg));
}

int config_cache_bedtime_minutes(void) {
    if (!s_bedtime_cfg_loaded) {
        s_bedtime_cfg = NVS_DEFAULT_BEDTIME;
        nvs_config_get_bedtime(&s_bedtime_cfg);
        s_bedtime_cfg_loaded = true;
    }
    /* The cache holds the raw HHMM and the fold is redone per call, so
       the fallback below cannot leak into the cached value: a later
       reader of the store still sees the operator's edit. */
    int m = bedtime_minutes((int)s_bedtime_cfg);
    /* bedtime_minutes() answers -1 for both "disabled" and "unusable", so
       the stored value has to break the tie. 0 was chosen deliberately by
       whoever provisioned the device and stays disabled; anything else
       that failed validation is a bad edit, and falling back to the
       compile-time default beats day-locking the screen (see the header
       for why the two must not be merged). */
    if (m < 0 && s_bedtime_cfg != 0) {
        m = bedtime_minutes(NVS_DEFAULT_BEDTIME);
    }
    return m;
}

void config_cache_invalidate(void) {
    schedule_cache_invalidate();
    s_quiet_cfg_loaded = false;
    s_bedtime_cfg_loaded = false;
}
