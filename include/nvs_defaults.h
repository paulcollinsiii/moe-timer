#pragma once

/* Real WiFi credentials live in credentials.local.h (gitignored via
   *.local.h — safe from accidental commits). Copy credentials.local.h.example
   to include/credentials.local.h and fill it in. */
#if defined(__has_include)
#if __has_include("credentials.local.h")
#include "credentials.local.h"
#endif
#endif

/* Base salt for the defaults stamp. The stored stamp is a FINGERPRINT of
   this version plus the allocation values below (see
   nvs_config_defaults_fingerprint), so a menuconfig change to any
   allocation re-seeds NVS on the next boot automatically — bump this only
   to force a re-seed for some other reason (e.g. new holiday list). */
#define NVS_DEFAULTS_VERSION 2

/* Allocations come from menuconfig (MagTag Timer menu) on firmware builds;
   host tests have no sdkconfig and use the fixed fallbacks. */
#ifndef NATIVE
#include "sdkconfig.h"
#endif
#ifdef CONFIG_MAGTAG_WEEKDAY_MIN
#define NVS_DEFAULT_WEEKDAY_MIN CONFIG_MAGTAG_WEEKDAY_MIN
#define NVS_DEFAULT_WEEKEND_MIN CONFIG_MAGTAG_WEEKEND_MIN
#define NVS_DEFAULT_HOLIDAY_MIN CONFIG_MAGTAG_HOLIDAY_MIN
#else
#define NVS_DEFAULT_WEEKDAY_MIN 60
#define NVS_DEFAULT_WEEKEND_MIN 120
#define NVS_DEFAULT_HOLIDAY_MIN 120
#endif
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
