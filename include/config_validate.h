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

#ifdef __cplusplus
}
#endif
