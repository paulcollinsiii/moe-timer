#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    DAY_WEEKDAY = 0,
    DAY_WEEKEND,
    DAY_HOLIDAY,
} day_type_t;

day_type_t schedule_get_day_type(time_t now);
uint32_t schedule_get_allocation_sec(day_type_t day_type);
bool schedule_is_holiday(const char *date_str, const char *blob, size_t blob_len);
