#pragma once

/* The OTA manifest URL fallback lives in credentials.local.h (gitignored
   via *.local.h — safe from accidental commits). Copy
   credentials.local.h.example to include/credentials.local.h and fill it
   in. WiFi and the MQTT broker are NOT set here any more: the device is
   provisioned on-device (SoftAP setup mode), and the owner enters both
   there. */
#if defined(__has_include)
#if __has_include("credentials.local.h")
#include "credentials.local.h"
#endif
#endif

/* An owner whose credentials.local.h still defines one of the old
   WiFi/MQTT fallbacks gets a build-time nudge: these macros are no longer
   read by anything (see above). Plain #warning becomes a hard error under
   this build's -Werror (GCC promotes #warning via -Werror=cpp), so this
   uses #pragma message instead, which -Werror does not elevate. */
#if defined(NVS_DEFAULT_WIFI_SSID) || defined(NVS_DEFAULT_WIFI_PASS) || defined(NVS_DEFAULT_MQTT_URI) || \
    defined(NVS_DEFAULT_MQTT_USER) || defined(NVS_DEFAULT_MQTT_PASS)
#pragma message \
    "credentials.local.h still defines a WiFi/MQTT NVS_DEFAULT_* macro; it is no longer read. Enter WiFi and MQTT on the device in setup mode instead."
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

/* Chore gate: the free slice of each day's allocation, in minutes, one
   per day type, mirroring the four allocations above (design 3.3).
   0 = fully gated, nothing free until the chores are acked — and 0 is
   also what every device in the field reads today, which is exactly why
   it is the default: shipping this feature changes no behaviour until
   someone configures chores (design row C1 makes a chore-less device
   inert regardless).

   DELIBERATELY NOT IN THE SEEDED-DEFAULTS REGISTRY BELOW, for the reason
   spelled out at NVS_DEFAULT_OTA_URL: these are HA-editable at runtime,
   the registry drives the defaults fingerprint, and a row here would mean
   that editing any unrelated allocation in menuconfig changes the
   fingerprint, reseeds NVS and silently reverts an operator's HA-set
   chore_free values on the next boot. Nothing is given up by leaving them
   out — schedule_get_chore_free_sec() passes these as the fallback for an
   absent key, exactly as schedule_get_allocation_sec() does, so an
   unseeded key simply reads as 0. No menuconfig knob either: HA is the
   only editor these ever need. */
#define NVS_DEFAULT_CHORE_FREE_WD 0
#define NVS_DEFAULT_CHORE_FREE_WE 0
#define NVS_DEFAULT_CHORE_FREE_HOL 0
#define NVS_DEFAULT_CHORE_FREE_SUM 0

/* OTA manifest endpoint and the check-on-sync flag. Set the URL in
   credentials.local.h (preferred — gitignored) or via menuconfig; empty
   disables OTA entirely.

   DELIBERATELY NOT IN THE SEEDED-DEFAULTS REGISTRY BELOW. Both keys are
   HA-editable at runtime, and the registry drives the defaults
   fingerprint: a row here would mean that editing any unrelated
   allocation in menuconfig changes the fingerprint, reseeds NVS, and
   silently reverts an operator's HA-set endpoint and check-on-sync flag
   on the next boot — the opposite of what a runtime override is for. The
   getters supply these lazily instead, via the same with-default helpers
   that tz, the quiet-hours pair and bedtime already use. Pinned by
   test_reseed_does_not_revert_ha_set_ota_values. */
#ifndef NVS_DEFAULT_OTA_URL
#ifdef CONFIG_MAGTAG_OTA_URL
#define NVS_DEFAULT_OTA_URL CONFIG_MAGTAG_OTA_URL
#else
#define NVS_DEFAULT_OTA_URL ""
#endif
#endif
/* Kconfig bool: defined (to 1) only when y, so #ifdef is the test. */
#ifdef CONFIG_MAGTAG_OTA_CHECK_ON_SYNC
#define NVS_DEFAULT_OTA_ON_SYNC 1
#else
#define NVS_DEFAULT_OTA_ON_SYNC 0
#endif

/* HA config-in defaults (phase 2): consumed by nvs_config getters when the
   key was never written; menuconfig values on firmware, fixed on host. */
#define NVS_DEFAULT_TZ "EST5EDT,M3.2.0,M11.1.0"
#ifdef CONFIG_MAGTAG_QUIET_START_HHMM
#define NVS_DEFAULT_QUIET_START CONFIG_MAGTAG_QUIET_START_HHMM
#define NVS_DEFAULT_QUIET_END CONFIG_MAGTAG_QUIET_END_HHMM
#define NVS_DEFAULT_BREAK_INTERVAL_MIN CONFIG_MAGTAG_BREAK_INTERVAL_MIN
#define NVS_DEFAULT_BREAK_DURATION_MIN CONFIG_MAGTAG_BREAK_DURATION_MIN
#else
#define NVS_DEFAULT_QUIET_START 2230
#define NVS_DEFAULT_QUIET_END 800
#define NVS_DEFAULT_BREAK_INTERVAL_MIN 30
#define NVS_DEFAULT_BREAK_DURATION_MIN 15
#endif
/* Bed Time lockout (0 = disabled, else 1800-2359; see bedtime.h). */
#ifdef CONFIG_MAGTAG_BEDTIME_HHMM
#define NVS_DEFAULT_BEDTIME CONFIG_MAGTAG_BEDTIME_HHMM
#else
#define NVS_DEFAULT_BEDTIME 2200
#endif

/* Alert tone selections (tone_id_t indices; HA select entities). No
   menuconfig for these — an enum index is opaque there; HA is the knob. */
#include "tones.h"
#define NVS_DEFAULT_TONE_EXPIRY TONE_MARIMBA
#define NVS_DEFAULT_TONE_BREAK TONE_CHIME
#define NVS_DEFAULT_TONE_BED TONE_GRANVALS

/* Alert volume percent (0-TONES_VOLUME_MAX; >100 = clipping boost). The
   default is the loud end — the DAC sine at reference level is much
   quieter than the old LEDC square wave was; HA is the turn-it-down knob. */
#ifdef CONFIG_MAGTAG_ALERT_VOLUME_PCT
#define NVS_DEFAULT_ALERT_VOLUME CONFIG_MAGTAG_ALERT_VOLUME_PCT
#else
#define NVS_DEFAULT_ALERT_VOLUME 200
#endif

/* Dublin City Schools (Grizzell MS) 2026-27 school year.
   Source: dublinschools.net 2026-27 school calendar. Update yearly. */
#define NVS_DEFAULT_SUMMER_START "2026-05-29" /* ~day after the 2025-26 last day */
#define NVS_DEFAULT_SCHOOL_START "2026-08-20" /* first day for students K-12 */
#define NVS_DEFAULT_SCHOOL_END "2027-05-28"   /* last day; summer resumes next day */

/* ---- seeded-defaults registry ------------------------------------------
   One row per key that nvs_config_init_defaults() seeds. Drives all three
   consumers from one list: the full reseed, the init-if-missing repair,
   and the defaults fingerprint — adding a seeded key here is the whole
   change. ROW ORDER IS LOAD-BEARING: the fingerprint folds rows in this
   order, and a changed fingerprint reseeds every deployed device
   (reverting HA-managed keys). The holidays blob is seeded separately and
   deliberately NOT fingerprinted, and neither are the OTA keys — see the
   NVS_DEFAULT_OTA_URL comment above before adding a row for them. */
#include "nvs_keys.h"

#define NVS_SEEDED_U16S(X)                          \
    X(NVS_KEY_WEEKDAY_MIN, NVS_DEFAULT_WEEKDAY_MIN) \
    X(NVS_KEY_WEEKEND_MIN, NVS_DEFAULT_WEEKEND_MIN) \
    X(NVS_KEY_HOLIDAY_MIN, NVS_DEFAULT_HOLIDAY_MIN) \
    X(NVS_KEY_SUMMER_MIN, NVS_DEFAULT_SUMMER_MIN)

/* No NVS_SEEDED_STRS any more: wifi_ssid/wifi_pass/mqtt_uri/mqtt_user/
   mqtt_pass were its only rows, and build-time credentials are gone (see
   the on-device setup mode). The getters' own empty-default fallback
   (nvs_config.c:get_str_empty_default) already reads an unwritten key as
   "" — the unprovisioned signal — with no registry row needed. If a future
   string default needs seeding, copy the NVS_SEEDED_U16S/FOLD_U16/SEED_U16/
   INIT_U16 pattern in nvs_config.c rather than resurrecting an empty
   macro. */

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
