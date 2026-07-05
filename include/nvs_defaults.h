#pragma once

/* Real WiFi credentials live in credentials.local.h (gitignored via
   *.local.h — safe from accidental commits). Copy credentials.local.h.example
   to include/credentials.local.h and fill it in. */
#if defined(__has_include)
#if __has_include("credentials.local.h")
#include "credentials.local.h"
#endif
#endif

#define NVS_DEFAULT_WEEKDAY_MIN 60
#define NVS_DEFAULT_WEEKEND_MIN 120
#define NVS_DEFAULT_HOLIDAY_MIN 120
#ifndef NVS_DEFAULT_WIFI_SSID
#define NVS_DEFAULT_WIFI_SSID ""
#endif
#ifndef NVS_DEFAULT_WIFI_PASS
#define NVS_DEFAULT_WIFI_PASS ""
#endif

/* US Federal Holidays 2026 — newline-separated YYYY-MM-DD */
#define NVS_DEFAULT_HOLIDAYS \
    "2026-01-01\n"           \
    "2026-01-19\n"           \
    "2026-02-16\n"           \
    "2026-05-25\n"           \
    "2026-06-19\n"           \
    "2026-07-03\n"           \
    "2026-07-04\n"           \
    "2026-09-07\n"           \
    "2026-10-12\n"           \
    "2026-11-11\n"           \
    "2026-11-26\n"           \
    "2026-12-25\n"
