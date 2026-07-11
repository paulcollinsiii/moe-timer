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

#ifdef __cplusplus
}
#endif
