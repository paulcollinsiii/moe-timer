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
   accepted here; use config_is_ota_url for the field rule.

   TWO CONSUMERS, and they need different things from the character rule:
   the HA-editable manifest endpoint (via config_is_ota_url), and the
   firmware-image URL that ota_policy.c reads out of the manifest. The
   rejected characters are excluded because none of them is legal
   unencoded in a URL — NOT because of JSON escaping. That distinction
   matters: only the manifest endpoint is ever republished in the cfg
   state document, so a future loosening argued from "this one is never
   republished" would silently loosen the image URL too. Keep the rule
   about URL legality and it stays correct for both. */
bool config_is_https_url(const char *s);

/* The rule both OTA-URL apply paths share (ha_config.c's set/<key> and
   config_apply.c's bulk document): empty (= OTA disabled, the only way to
   turn it off from HA) or a valid https URL. One definition so the two
   paths cannot drift — a non-https URL must be rejected by both. */
bool config_is_ota_url(const char *s);

/* True when s contains nothing that would corrupt the JSON documents this
   firmware hand-builds (unescaped quote or backslash) or the MQTT/HA layer
   (control characters). Empty is clean — an empty timer name disables a
   slot, and an empty OTA URL disables updates. NULL is not.

   Shared because every string that reaches a hand-built JSON document has
   to pass it: the per-entity set path, and the bulk document's `ver`,
   which is interpolated into all three config_ack emissions. */
bool config_is_clean_str(const char *s);

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
   one field that turns updates on. */
#define CFG_BOUND_OTA_URL_MAX 128

/* ---- string-field ceiling ------------------------------------------
   ONE number that every buffer on a string field's path is sized from:
   the MQTT set/<key> transport slot (mqtt_set_kv_t.value in mqtt_rx.h)
   and ha_config_state_json's render scratch. Every CFG_BOUND_*_MAX below
   is asserted against it, so a new TEXT() field cannot outgrow either the
   transport that has to deliver it or the buffer that has to render it.

   This exists because those three numbers were independent and drifted:
   CFG_BOUND_OTA_URL_MAX 128 advertised "max":127 to HA while the set
   transport still capped a payload at 79, so a real manifest URL was
   dropped in mqtt_rx with only a USB-console warning — no ack, retained
   command never cleared, re-dropped every window, OTA silently never on.
   Deriving the transport from the ceiling makes that unrepresentable.

   AN INDEPENDENT LITERAL, NOT `CFG_BOUND_OTA_URL_MAX`. Four things float
   with this number: the set transport (x HA_CONFIG_SET_SLOTS, on the
   window heap) and ha_config_state_json's raw[]/esc[] scratch (on the
   10 KB net_win task STACK). Defining it as the OTA bound made the assert
   below read `X <= X`, so raising that bound to 512 would have taken the
   render frame from 384 B to 1.5 KB and the sets array to 24 KB with
   nothing firing — the registry test's threshold is the ceiling itself,
   so it moves in lockstep and cannot catch this. Raising the ceiling must
   be a deliberate edit here, against the budget asserts that guard it. */
#define CFG_STR_MAX 128

_Static_assert(CFG_BOUND_NAME_MAX <= CFG_STR_MAX, "device name exceeds the string-field ceiling");
_Static_assert(CFG_BOUND_TZ_MAX <= CFG_STR_MAX, "timezone exceeds the string-field ceiling");
_Static_assert(CFG_BOUND_OTA_URL_MAX <= CFG_STR_MAX, "OTA URL exceeds the string-field ceiling");

/* Device-owned OTA state widths (not config: no HA entity, no bulk-document
   key). Declared here so writer and readers agree on one number —
   hal_nvs_read_str returns ESP_ERR_NVS_INVALID_LENGTH on a short buffer
   and writes NOTHING, and get_str_empty_default maps only NOT_FOUND to "",
   so a reader that guesses low is left holding an uninitialised buffer.
   For ota_target that is not merely cosmetic: the retry-budget comparison
   would never match its stored target and the device would retry a doomed
   version forever. ota_policy.c ties OTA_REASON_TEXT_MAX / OTA_VERSION_MAX
   back to these. */
/* Retained-command id: must match cmd_apply.c's dedup read buffer, or a
   stored id that cannot be read back breaks apply-once dedup. */
#define CFG_BOUND_CMD_ID_MAX 40
#define CFG_BOUND_OTA_RESULT_MAX 24 /* longest reason code is "bad_manifest" (12) */
#define CFG_BOUND_OTA_TARGET_MAX 32 /* manifest version strings cap at 31 chars */

#ifdef __cplusplus
}
#endif
