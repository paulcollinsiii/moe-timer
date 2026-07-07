#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    DAY_WEEKDAY = 0,
    DAY_WEEKEND,
    DAY_HOLIDAY,
    DAY_SUMMER, /* school summer break; precedence: holiday > weekend > summer > weekday */
} day_type_t;

#ifdef __cplusplus
extern "C" {
#endif

day_type_t schedule_get_day_type(time_t now);
uint32_t schedule_get_allocation_sec(day_type_t day_type);
bool schedule_is_holiday(const char *date_str, const char *blob, size_t blob_len);
/* True when date_str (YYYY-MM-DD) falls outside the school year defined by
   NVS_DEFAULT_SCHOOL_START/END in nvs_defaults.h. */
bool schedule_is_summer(const char *date_str);

#ifdef __cplusplus
}
#endif
