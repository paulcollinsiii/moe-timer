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

/* Seconds until a refresh is allowed (0 = go now). Lets the caller absorb
   the remaining interval instead of having a frame silently rejected —
   two renders in one wake (countdown step → pause/break/alert) can land
   inside the minimum interval. */
int32_t ssd1680_refresh_wait_sec(int64_t now_sec, int64_t last_refresh_sec, int32_t min_interval_sec) {
    if (ssd1680_refresh_allowed(now_sec, last_refresh_sec, min_interval_sec))
        return 0;
    return (int32_t)(min_interval_sec - (now_sec - last_refresh_sec));
}

/* A partial refresh diffs against the controller's previous-frame RAM.
   After panel power loss that RAM is garbage, so partial must promote to
   full until a refresh has written it. Modes: 0 = FULL, 1 = PARTIAL. */
int ssd1680_resolve_refresh_mode(int requested_mode, bool prev_frame_valid) {
    if (!prev_frame_valid)
        return 0; /* SSD1680_REFRESH_FULL */
    return requested_mode;
}
