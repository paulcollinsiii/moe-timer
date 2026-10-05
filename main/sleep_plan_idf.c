/* The one device call the sleep planner's decisions end in. Kept out of
   sleep_plan.c so that file stays free of ESP-IDF and host-testable; the
   decision this acts on (sleep_plan_timer_armed) is tested there. */
#include "esp_sleep.h"
#include "sleep_plan.h"

void sleep_plan_arm_timer(uint32_t seconds) {
    if (sleep_plan_timer_armed(seconds)) {
        esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    }
}
