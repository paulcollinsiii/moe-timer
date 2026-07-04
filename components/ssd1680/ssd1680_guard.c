/* Pure panel-protection logic — no ESP-IDF dependencies so it is
   host-testable. Policy (partial vs full cadence) lives in display.c;
   this guard only prevents physically harmful refresh rates. */
#include <stdbool.h>
#include <stdint.h>

bool ssd1680_refresh_allowed(int64_t now_sec, int64_t last_refresh_sec, int32_t min_interval_sec) {
    if (last_refresh_sec <= 0)
        return true; /* never refreshed (cold boot) */
    if (now_sec < last_refresh_sec)
        return true; /* clock stepped backwards (NTP correction) */
    return (now_sec - last_refresh_sec) >= min_interval_sec;
}
