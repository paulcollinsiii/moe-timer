#pragma once
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ISO date "YYYY-MM-DD" — the one format compared lexicographically
   everywhere (holiday blob, last_date, school-year bounds, snapshot
   date). buf must hold at least 11 bytes. Header-only so host tests
   that mock hal_time still link.

   Formatted into a scratch buffer big enough for the widest possible
   output (three 11-character ints and two dashes = 35 + NUL) and then
   copied with the same truncation snprintf applies. The obvious one-line
   snprintf into buf is identical at run time, but at -Os GCC inlines this
   into callers whose buf is exactly 11 bytes, cannot bound an int field,
   and the build's -Werror=format-truncation fails. */
static inline void date_fmt_iso(char *buf, size_t len, const struct tm *tm) {
    char full[40];
    const int n = snprintf(full, sizeof full, "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    if (len == 0 || n < 0)
        return;
    const size_t take = (size_t)n < len - 1 ? (size_t)n : len - 1;
    memcpy(buf, full, take);
    buf[take] = '\0';
}
