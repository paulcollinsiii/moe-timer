#pragma once
#include <stdbool.h>

/* Shared config field validators — pure, host-tested. Used by both the
   bulk config-document applier (config_apply.c) and the per-field HA
   editable-entity applier (ha_config.c). */

#ifdef __cplusplus
extern "C" {
#endif

/* True when s is a real "YYYY-MM-DD" date: strict shape plus calendar
   validity (month lengths, leap years) via a mktime round trip. */
bool config_is_iso_date(const char *s);

/* True when s is an "https://<host>..." URL: the scheme (case-insensitive,
   per RFC 3986) plus a non-empty remainder, and no spaces, control chars,
   quotes or backslashes anywhere. Plain http is rejected on purpose — an
   unauthenticated firmware endpoint is an arbitrary-code-execution
   channel, and TLS is what makes the manifest trustworthy. Empty is NOT
   accepted here; use config_is_ota_url for the field rule. */
bool config_is_https_url(const char *s);

/* The rule both OTA-URL apply paths share (ha_config.c's set/<key> and
   config_apply.c's bulk document): empty (= OTA disabled, the only way to
   turn it off from HA) or a valid https URL. One definition so the two
   paths cannot drift — a non-https URL must be rejected by both. */
bool config_is_ota_url(const char *s);

/* Shared field bounds: ha_config.c advertises them in HA discovery
   (number entity min/max) and config_apply.c enforces them on the
   retained config document — one definition so they cannot drift. */
#define CFG_BOUND_ALLOC_LO 1
#define CFG_BOUND_ALLOC_HI 1440
#define CFG_BOUND_BREAK_INT_LO 0 /* 0 = breaks disabled */
#define CFG_BOUND_BREAK_INT_HI 480
#define CFG_BOUND_BREAK_DUR_LO 1
#define CFG_BOUND_BREAK_DUR_HI 120
#define CFG_BOUND_TIMER_MIN_LO 1
#define CFG_BOUND_TIMER_MIN_HI 1440
#define CFG_BOUND_NAME_MAX 32
#define CFG_BOUND_TZ_MAX 48
/* OTA manifest endpoint buffer (127 usable chars). Sized for a real
   hosting path — a GitHub raw URL with owner/repo/branch runs to ~80 —
   because being refused by a length limit is a poor failure mode for the
   one field that turns updates on. This is the longest CFG_STR in the
   registry; ha_config.c's state builder sizes its scratch from it. */
#define CFG_BOUND_OTA_URL_MAX 128

#ifdef __cplusplus
}
#endif
