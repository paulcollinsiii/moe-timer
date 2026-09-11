#pragma once
#include <stdbool.h>
#include <stdint.h>

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

/* The chore gate's cross-field rule: is this `chore_free_*` value usable
   with the allocation it is paired with? Both arguments are MINUTES, the
   unit config is stored and edited in (`weekday_min`, `chore_free_wd`,
   and the CFG_BOUND_* numbers below are all minutes).

   MINUTES, NOT SECONDS — the other half of this feature is a seconds
   domain and both are live at once. schedule_get_chore_free_sec() and
   schedule_get_allocation_sec() return SECONDS; handing their values to
   this predicate compiles (firmware is -Wall -Werror with no -Wconversion,
   host tests are -Wall -Wextra, and uint32_t -> uint16_t is silent in
   both), never trips a bound, and silently answers a different question.
   The 60x does NOT cancel out, and the hazard is present rather than
   hypothetical: (1100, 1000) in minutes is INVALID, but the same pair in
   seconds is (66000, 60000), and 66000 through a uint16_t parameter
   arrives as 464 — VALID, the opposite answer. Nor does the truncation
   announce itself: 1440*60 = 86400 arrives as 20864, an in-range,
   plausible-looking minute count. Convert at the call site, or better,
   call this before anything reaches the seconds domain at all.

   The rule is `chore_free <= allocation`, and the `==` case is VALID:
   nothing is withheld, which is the per-day-type off switch (design 3.3)
   and needs no extra key. Only `chore_free > allocation` is rejected —
   withholding more than a day grants cannot mean anything, so the device
   refuses to guess.

   ONE predicate, TWO consumers that must NOT agree on what to do about a
   false, and that asymmetry is the design, not an oversight:
     - the chore_free_* setter REJECTS the write and names the field in
       the config_ack errors list;
     - the allocation setter CLAMPS the paired chore_free_* down and says
       so in the ack. Blocking a parent who is lowering screen time on
       account of a chore setting they are not thinking about would be the
       wrong answer.
   The clamp target falls out of the boundary above: the largest valid
   chore_free for an allocation is the allocation itself. Both directions
   do the kind thing, which is what makes the invalid state unreachable by
   ordinary operation — the blocking config-error screen exists for the
   config that got there some other way (older firmware, NVS oddity, a bug
   in either setter).

   THE CALL SHAPE FOR EACH SETTER, because both parameters are uint16_t: a
   swapped call compiles silently and inverts the answer, and the two
   setters have opposite habits — the new value is the FIRST argument at
   one site and the SECOND at the other. Copy these, do not infer them:
     - chore_free_* setter:
         config_is_valid_chore_free_min(candidate, current_allocation)
     - allocation setter:
         config_is_valid_chore_free_min(current_chore_free, candidate)
   A swap at the allocation site is the dangerous one: a parent lowering
   the allocation to 30 while chore_free is 60 evaluates (30, 60) -> VALID,
   no clamp fires, and NVS is left holding the invalid pair.

   ONE SHARED PREDICATE IS NOT TOTAL COVERAGE. It does earn its keep — a
   setter hand-written with `<` instead of `<=` would reject the off
   switch, a user-visible design violation — but three gaps are real and
   are NOT closed by sharing this rule:
     - the CLAMP TARGET is prose above, not code. The allocation setter
       hand-writes alloc_min; it is shareable and is not shared.
     - schedule.c's chore-free resolver already encodes this same boundary
       independently, in SECONDS, and cannot call this. Two implementations
       of one rule are in-tree.
     - the config-error gate cannot get its input from the accessor it
       would naturally reach for: schedule_get_chore_free_sec() CLAMPS, and
       the clamp is lossy and irreversible — the legitimate off switch
       (60, 60) and the broken pair (90, 60) both come back as
       free_sec == alloc_sec, so a gate built on it would never fire. The
       gate must read the raw chore_free_* and allocation MINUTE keys and
       call this predicate on the unconverted values. */
bool config_is_valid_chore_free_min(uint16_t chore_free_min, uint16_t alloc_min);

/* Shared field bounds: ha_config.c advertises them in HA discovery
   (number entity min/max) and config_apply.c enforces them on the
   retained config document — one definition so they cannot drift. */
#define CFG_BOUND_ALLOC_LO 1
#define CFG_BOUND_ALLOC_HI 1440
/* The chore-free slice of an allocation. NOTE THE ASYMMETRY WITH THE
   ALLOCATION BOUNDS ABOVE, and do not "tidy" it: CFG_BOUND_ALLOC_LO is 1
   because an allocation of zero is not a thing anyone means, but a
   chore_free of 0 is the DEFAULT (nvs_defaults.h, and deliberately left
   out of the seeded-defaults registry, so an absent key reads 0) and the
   fully-gated setting, so its LO is 0. Copying the allocation LO here
   would NOT retroactively fail anything already stored — nothing
   re-validates a value sitting in NVS — but it would break the default
   two other ways: an incoming config document carrying `chore_free_*: 0`
   would be rejected and the field named in the ack (config_apply.c's
   apply_u16 range-checks the incoming document), and 0 would become
   unselectable in HA (ha_config.c's NUM_U16 advertises lo/hi as the
   number entity's min/max). The one value every device is running would
   become the one value an operator cannot set.

   The HI must EQUAL the allocation HI — `<=` is not enough, which is why
   the assert below is `==`. Above it, chore_free's top values could never
   satisfy config_is_valid_chore_free_min, so HA would advertise numbers
   the setter is bound to reject. Below it, `chore_free == allocation` —
   the per-day-type off switch (design 3.3) — becomes unreachable for
   every allocation above this ceiling, because HA would never offer the
   matching number and the gate could not be turned off for that day type
   at all. Only equality rules out both. */
#define CFG_BOUND_CHORE_FREE_LO 0 /* 0 = fully gated: the default, and every device in the field */
#define CFG_BOUND_CHORE_FREE_HI 1440
_Static_assert(CFG_BOUND_CHORE_FREE_HI == CFG_BOUND_ALLOC_HI,
               "the chore_free and allocation ceilings must be equal: a higher chore_free ceiling advertises top "
               "values that can never validate, and a lower one makes chore_free == allocation (the per-day-type "
               "off switch) unreachable for every allocation above it");
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
