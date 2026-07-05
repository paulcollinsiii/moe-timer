#pragma once

/* Real WiFi credentials live in credentials.local.h (gitignored via
   *.local.h — safe from accidental commits). Copy credentials.local.h.example
   to include/credentials.local.h and fill it in. */
#if defined(__has_include)
#if __has_include("credentials.local.h")
#include "credentials.local.h"
#endif
#endif

/* Bump this whenever any default below changes: on the next boot,
   nvs_config_init_defaults() re-seeds ALL defaults over whatever is in
   NVS (no erase-flash needed). While there is no runtime settings UI,
   overwriting everything is the honest behaviour. */
#define NVS_DEFAULTS_VERSION 2 /* v2: production allocations after smoke testing */

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
