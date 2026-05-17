#include "schedule.h"

#include <string.h>
#include <time.h>

#include "hal_nvs.h"
#include "hal_time.h"

day_type_t schedule_get_day_type(time_t now) {
    (void)now;
    return DAY_WEEKDAY;
}
uint32_t schedule_get_allocation_sec(day_type_t day_type) {
    (void)day_type;
    return 3600;
}
bool schedule_is_holiday(const char *d, const char *b, size_t len) {
    (void)d;
    (void)b;
    (void)len;
    return false;
}
