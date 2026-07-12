#pragma once
#include <stdio.h>
#include <time.h>

/* ISO date "YYYY-MM-DD" — the one format compared lexicographically
   everywhere (holiday blob, last_date, school-year bounds, snapshot
   date). buf must hold at least 11 bytes. Header-only so host tests
   that mock hal_time still link. */
static inline void date_fmt_iso(char *buf, size_t len, const struct tm *tm) {
    snprintf(buf, len, "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}
