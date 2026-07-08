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
#define NVS_DEFAULTS_VERSION 3 /* v3: Dublin 2026-27 school calendar */

/* Allocations come from menuconfig (MagTag Timer menu) on firmware builds;
   host tests have no sdkconfig and use the fixed fallbacks. */
#ifndef NATIVE
#include "sdkconfig.h"
#endif
#ifdef CONFIG_MAGTAG_WEEKDAY_MIN
#define NVS_DEFAULT_WEEKDAY_MIN CONFIG_MAGTAG_WEEKDAY_MIN
#define NVS_DEFAULT_WEEKEND_MIN CONFIG_MAGTAG_WEEKEND_MIN
#define NVS_DEFAULT_HOLIDAY_MIN CONFIG_MAGTAG_HOLIDAY_MIN
#define NVS_DEFAULT_SUMMER_MIN CONFIG_MAGTAG_SUMMER_MIN
#else
#define NVS_DEFAULT_WEEKDAY_MIN 60
#define NVS_DEFAULT_WEEKEND_MIN 120
#define NVS_DEFAULT_HOLIDAY_MIN 120
#define NVS_DEFAULT_SUMMER_MIN 120
#endif
#ifndef NVS_DEFAULT_WIFI_SSID
#define NVS_DEFAULT_WIFI_SSID ""
#endif
#ifndef NVS_DEFAULT_WIFI_PASS
#define NVS_DEFAULT_WIFI_PASS ""
#endif

/* MQTT broker for the Home Assistant integration. Set in
   credentials.local.h (preferred — gitignored, like WiFi) or via
   menuconfig; empty URI disables MQTT entirely. */
#ifndef NVS_DEFAULT_MQTT_URI
#ifdef CONFIG_MAGTAG_MQTT_URI
#define NVS_DEFAULT_MQTT_URI CONFIG_MAGTAG_MQTT_URI
#else
#define NVS_DEFAULT_MQTT_URI ""
#endif
#endif
#ifndef NVS_DEFAULT_MQTT_USER
#ifdef CONFIG_MAGTAG_MQTT_USER
#define NVS_DEFAULT_MQTT_USER CONFIG_MAGTAG_MQTT_USER
#else
#define NVS_DEFAULT_MQTT_USER ""
#endif
#endif
#ifndef NVS_DEFAULT_MQTT_PASS
#ifdef CONFIG_MAGTAG_MQTT_PASS
#define NVS_DEFAULT_MQTT_PASS CONFIG_MAGTAG_MQTT_PASS
#else
#define NVS_DEFAULT_MQTT_PASS ""
#endif
#endif

/* Dublin City Schools (Grizzell MS) 2026-27 school year.
   Source: dublinschools.net 2026-27 school calendar. Update yearly. */
#define NVS_DEFAULT_SUMMER_START "2026-05-29" /* ~day after the 2025-26 last day */
#define NVS_DEFAULT_SCHOOL_START "2026-08-20" /* first day for students K-12 */
#define NVS_DEFAULT_SCHOOL_END "2027-05-28"   /* last day; summer resumes next day */

/* Weekday no-school days during the 2026-27 school year (weekends are
   their own category; summer break is the summer category). Newline-
   separated YYYY-MM-DD. */
#define NVS_DEFAULT_HOLIDAYS \
    "2026-09-07\n"           \
    "2026-10-16\n"           \
    "2026-11-03\n"           \
    "2026-11-25\n"           \
    "2026-11-26\n"           \
    "2026-11-27\n"           \
    "2026-12-21\n"           \
    "2026-12-22\n"           \
    "2026-12-23\n"           \
    "2026-12-24\n"           \
    "2026-12-25\n"           \
    "2026-12-28\n"           \
    "2026-12-29\n"           \
    "2026-12-30\n"           \
    "2026-12-31\n"           \
    "2027-01-01\n"           \
    "2027-01-04\n"           \
    "2027-01-18\n"           \
    "2027-02-12\n"           \
    "2027-02-15\n"           \
    "2027-03-29\n"           \
    "2027-03-30\n"           \
    "2027-03-31\n"           \
    "2027-04-01\n"           \
    "2027-04-02\n"           \
    "2027-04-23\n"
