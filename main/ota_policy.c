/* OTA decision logic — pure, host-tested. No clock, no NVS, no ESP
   includes: everything it decides from arrives as an argument. */
#include "ota_policy.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config_validate.h" /* config_is_https_url */

/* ---- reason codes ----

   One table, indexed by the enum. Note what this does NOT buy: a code
   added before OTA_REASON_COUNT simply grows the array and leaves a NULL
   slot, and the compiler says nothing about it. The lookup below
   therefore reads NULL as "", and the real backstop is
   test_every_reason_has_a_distinct_string, which walks the whole enum at
   runtime. ota_policy_reason_is_persistable's switch is the only part of
   this module where a new code cannot pass silently. */
static const char *const REASON_STR[OTA_REASON_COUNT] = {
    [OTA_REASON_NONE] = "",
    [OTA_REASON_NO_URL] = "no_url",
    [OTA_REASON_NO_TIME] = "no_time",
    [OTA_REASON_LOCKED] = "locked",
    [OTA_REASON_LOW_BATT] = "low_batt",
    [OTA_REASON_LOW_HEAP] = "low_heap",
    [OTA_REASON_UP_TO_DATE] = "up_to_date",
    [OTA_REASON_PINNED] = "pinned",
    [OTA_REASON_NO_ENTRY] = "no_entry",
    [OTA_REASON_BAD_MANIFEST] = "bad_manifest",
    [OTA_REASON_NO_SCHEMA] = "no_schema",
    [OTA_REASON_BAD_VERSION] = "bad_version",
    [OTA_REASON_BAD_URL] = "bad_url",
    [OTA_REASON_GAVE_UP] = "gave_up",
    [OTA_REASON_HTTP] = "http",
    [OTA_REASON_NET] = "net",
    [OTA_REASON_TLS] = "tls",
    [OTA_REASON_TLS_CERT] = "tls_cert",
    [OTA_REASON_TIMEOUT] = "timeout",
    [OTA_REASON_BAD_IMAGE] = "bad_image",
};

const char *ota_policy_reason_str(ota_reason_t reason) {
    /* One cast, not a `reason < 0 ||` half-guard: the enum has no
       negative enumerators so GCC gives it an unsigned type, which makes
       that test dead. The cast also covers a signed-enum toolchain, where
       a negative value wraps above the count onto this same arm. */
    if ((unsigned)reason >= (unsigned)OTA_REASON_COUNT)
        return "";
    /* A missing table entry is a NULL, not a "" — passing it to the %s
       below would be undefined behaviour and would put "(null)" in front
       of Home Assistant. */
    const char *s = REASON_STR[reason];
    return (s != NULL) ? s : "";
}

void ota_policy_reason_text(ota_reason_t reason, int http_status, char *out, size_t out_len) {
    if (out == NULL || out_len == 0)
        return;
    if (reason == OTA_REASON_HTTP && http_status > 0) {
        snprintf(out, out_len, "http_%d", http_status);
        return;
    }
    snprintf(out, out_len, "%s", ota_policy_reason_str(reason));
}

bool ota_policy_reason_is_persistable(ota_reason_t reason) {
    /* Every arm spelled out, and deliberately NO default: -Wswitch then
       makes a newly added reason a diagnostic here (the firmware build
       runs -Werror), which is the compile-time check the string table
       above cannot give. Adding a code forces a decision about whether it
       may overwrite ota_result.

       The rule: record an outcome only when a check or a download
       actually produced one. "No check ran", and "the check ran and
       there was correctly nothing to do", both leave ota_result alone —
       which is what lets a download failure from the previous window
       survive to reach the next window's stat payload, the only moment
       it is ever published. */
    switch (reason) {
        case OTA_REASON_NONE:       /* nothing happened */
        case OTA_REASON_NO_URL:     /* OTA switched off; a setting, not an outcome */
        case OTA_REASON_NO_TIME:    /* the plan's "(not set - no check ran)" row */
        case OTA_REASON_LOCKED:     /* charge lock, reported on its own channel */
        case OTA_REASON_UP_TO_DATE: /* ran, and there was nothing to do */
        case OTA_REASON_PINNED:     /* deliberately frozen, not a fault */
        case OTA_REASON_COUNT:
            return false;
        /* low_batt and low_heap skip the work but the plan's failure
           table still names an ota_result for both: they are the two
           skips a human acts on. */
        case OTA_REASON_LOW_BATT:
        case OTA_REASON_LOW_HEAP:
        case OTA_REASON_GAVE_UP:
        case OTA_REASON_BAD_MANIFEST:
        case OTA_REASON_NO_SCHEMA:
        case OTA_REASON_NO_ENTRY:
        case OTA_REASON_BAD_VERSION:
        case OTA_REASON_BAD_URL:
        case OTA_REASON_HTTP:
        case OTA_REASON_NET:
        case OTA_REASON_TLS:
        case OTA_REASON_TLS_CERT:
        case OTA_REASON_TIMEOUT:
        case OTA_REASON_BAD_IMAGE:
            return true;
    }
    return false; /* not reachable for a valid enum value */
}

ota_reason_t ota_policy_reason(const ota_error_facts_t *facts) {
    if (facts == NULL)
        return OTA_REASON_NONE;
    /* Our own deadline first: when the budget aborts a transfer the
       transport reports whatever a torn-down connection reports, and
       that symptom would otherwise mask the cause. */
    if (facts->deadline_hit)
        return OTA_REASON_TIMEOUT;
    /* The split that earns its keep: a rejected chain means the pinned
       root is wrong or expired and needs a serial visit; a TLS failure
       with a clean chain is the network flaking and fixes itself. */
    if (facts->tls_cert_flags != 0)
        return OTA_REASON_TLS_CERT;
    if (facts->tls_failed)
        return OTA_REASON_TLS;
    if (facts->image_rejected)
        return OTA_REASON_BAD_IMAGE;
    if (facts->http_status >= 400)
        return OTA_REASON_HTTP;
    if (facts->transport_failed)
        return OTA_REASON_NET;
    return OTA_REASON_NONE;
}

/* ---- preconditions ---- */

ota_reason_t ota_policy_check_gate(const ota_gate_in_t *in) {
    /* A NULL input is a caller bug, but the safe reading of "I know
       nothing" is "OTA is off", not "go ahead". */
    if (in == NULL || !in->url_set)
        return OTA_REASON_NO_URL;
    if (!in->time_valid)
        return OTA_REASON_NO_TIME;
    /* Ahead of the battery floor on purpose: the lock is the more
       specific statement, and it implies the floor anyway. */
    if (in->charge_locked)
        return OTA_REASON_LOCKED;
    if (in->batt_pct >= 0 && in->batt_pct < in->min_batt_pct)
        return OTA_REASON_LOW_BATT;
    return OTA_REASON_NONE;
}

ota_reason_t ota_policy_download_gate(const ota_gate_in_t *in) {
    /* Repeated rather than delegated to check_gate: the heap test below
       dereferences `in` here, so this function has to establish that
       itself. Leaning on check_gate's NULL handling would make a local
       dereference safe only by remote control. No test can distinguish
       its removal — check_gate would still answer NO_URL first — so it
       is hygiene for the reader and the analyser, not behaviour. */
    if (in == NULL)
        return OTA_REASON_NO_URL;
    ota_reason_t r = ota_policy_check_gate(in);
    if (r != OTA_REASON_NONE)
        return r;
    if (in->free_heap < in->min_free_heap)
        return OTA_REASON_LOW_HEAP;
    return OTA_REASON_NONE;
}

/* ---- retry budget ---- */

bool ota_policy_budget_exhausted(const char *target, const char *counted_target, uint16_t fails, uint16_t max_fails) {
    /* 0 is not "disabled", it is "the caller forgot the field". The
       Kconfig range is 1..10, so menuconfig cannot produce a 0; the only
       realistic source is a designated initialiser in ota_flow.c that
       omits .max_fails. Reading that as "no budget" would restore the
       exact sad loop the budget exists to stop — silently, and forever -
       so it falls back to the compiled default instead. Drift with the
       Kconfig default is harmless here: this value is only ever reached
       on the caller-bug path, where it is a floor against the sad loop
       rather than a configuration surface. */
    if (max_fails == 0)
        max_fails = OTA_MAX_FAILS_DEFAULT;
    if (target == NULL || counted_target == NULL || counted_target[0] == '\0')
        return false; /* nothing has been counted yet */
    if (strcmp(target, counted_target) != 0)
        return false; /* stale count: a new version re-arms the device */
    return fails >= max_fails;
}

uint16_t ota_policy_next_fail_count(const char *target, const char *counted_target, uint16_t fails) {
    if (target == NULL || counted_target == NULL || strcmp(target, counted_target) != 0)
        return 1; /* first failure against a new target */
    if (fails == UINT16_MAX)
        return fails; /* saturate: a wrapped counter would look fresh */
    return (uint16_t)(fails + 1);
}

/* ---- block selection ---- */

/* Highest schema in 1..max_schema, first of any duplicates. A block that
   is not an object, or whose `schema` is missing, not a number, not
   integral or out of range, is skipped rather than fatal — the same
   forward-compatibility rule that applies to unknown keys, one level up.

   max_schema is a parameter rather than OTA_SCHEMA_MAX read directly so
   that "of two SUPPORTED blocks, the highest wins" is testable now: with
   the cap at 1 there cannot be two supported blocks, and that rule is
   the whole reason the manifest is an array. The test drives it with a
   cap of 2; ota_policy_decide passes OTA_SCHEMA_MAX. */
static const cJSON *select_block(const cJSON *root, int max_schema, int *out_schema, bool *out_duplicate) {
    const cJSON *best = NULL;
    int best_schema = -1;
    bool duplicate = false;
    const cJSON *el = NULL;
    cJSON_ArrayForEach(el, root) {
        if (!cJSON_IsObject(el))
            continue;
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(el, "schema");
        if (!cJSON_IsNumber(schema))
            continue;
        int v = schema->valueint;
        /* Integral values only. cJSON derives valueint from valuedouble
           by saturating truncation, so {"schema":1.9} would otherwise
           read as 1 and {"schema":1e300} as INT_MAX — a malformed value
           silently reinterpreted as a schema this build claims to
           understand. Skipping keeps the "ignore, never fail" rule while
           refusing to guess what the publisher meant. */
        if ((double)v != schema->valuedouble)
            continue;
        /* Schemas are numbered from 1. The upper bound is the forward
           compatibility rule; the lower bound is a correctness one, and
           it is NOT redundant with the best_schema seed below. That seed
           is -1, so it excludes negatives only: without this test a
           {"schema":0} block — a typo, or 0 reserved to mean "do not
           read this" — would be selected and then handed to the schema-1
           resolver below. */
        if (v < 1 || v > max_schema)
            continue;
        if (v > best_schema) { /* strict >, so the FIRST of a duplicate pair wins */
            best_schema = v;
            best = el;
            duplicate = false; /* a new winner: any earlier repeat was not of this schema */
        } else if (best != NULL && v == best_schema) {
            duplicate = true; /* malformed, but not fatal — the caller logs it */
        }
    }
    *out_schema = best_schema;
    *out_duplicate = duplicate;
    return best;
}

/* ---- resolution within the selected block (schema 1) ----

   Once an OBJECT entry has been found for this device it is BINDING:
   every rejection below returns a reason rather than falling back to
   `default`. Installing the fleet default because the device's own entry
   was mistyped would ship a version the publisher explicitly did not
   target here, and the whole point of the devices map is to keep one
   device off the fleet version. Not updating is the safe failure.

   The single exception is an entry that is not an object at all: that
   cannot express a targeting decision, so there is nothing to be bound
   by and the default applies. Both directions are pinned by tests. */

static ota_reason_t resolve(const cJSON *block, const ota_decide_in_t *in, ota_decision_t *out) {
    const char *dev = (in->device_id != NULL) ? in->device_id : "";
    const cJSON *entry = NULL;

    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(block, "devices");
    if (cJSON_IsObject(devices) && dev[0] != '\0') {
        const cJSON *mine = cJSON_GetObjectItemCaseSensitive(devices, dev);
        if (cJSON_IsObject(mine))
            entry = mine; /* binding from here on — see the note above */
    }
    if (entry == NULL) {
        const cJSON *def = cJSON_GetObjectItemCaseSensitive(block, "default");
        if (cJSON_IsObject(def))
            entry = def;
    }
    if (entry == NULL)
        return OTA_REASON_NO_ENTRY;

    /* Absent or null version = pinned. This is how a device is frozen
       without deleting its entry, so it must not read as an error. */
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(entry, "version");
    if (ver == NULL || cJSON_IsNull(ver))
        return OTA_REASON_PINNED;
    if (!cJSON_IsString(ver) || ver->valuestring == NULL)
        return OTA_REASON_BAD_VERSION;
    const char *version = ver->valuestring;
    if (version[0] == '\0' || strlen(version) >= OTA_VERSION_MAX)
        return OTA_REASON_BAD_VERSION;
    snprintf(out->version, sizeof(out->version), "%s", version);

    /* "Different, not newer" — deliberately not semver ordering. A
       downgrade is how a rollback is published; turning this into a
       greater-than test would require lying about version numbers to
       recover a fleet. */
    const char *running = (in->running_version != NULL) ? in->running_version : "";
    if (strcmp(version, running) == 0)
        return OTA_REASON_UP_TO_DATE;

    /* Checked after the version compare, so a device that is already on
       the published version does not report a URL problem it will never
       act on. */
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(entry, "url");
    if (!cJSON_IsString(url) || url->valuestring == NULL)
        return OTA_REASON_BAD_URL;
    /* Same rule as the HA-set endpoint, from one definition: a plaintext
       firmware URL is an arbitrary-code-execution channel. Length is a
       rejection and never a truncation — half a URL fails in a way
       nobody can diagnose. */
    if (!config_is_https_url(url->valuestring) || strlen(url->valuestring) >= OTA_URL_MAX)
        return OTA_REASON_BAD_URL;
    snprintf(out->url, sizeof(out->url), "%s", url->valuestring);

    /* Last, so gave_up is only ever reported for something we would
       otherwise have downloaded. */
    if (ota_policy_budget_exhausted(out->version, in->counted_target, in->fails, in->max_fails))
        return OTA_REASON_GAVE_UP;

    out->update = true;
    return OTA_REASON_NONE;
}

void ota_policy_decide(const ota_decide_in_t *in, ota_decision_t *out) {
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->schema = -1;
    out->reason = OTA_REASON_BAD_MANIFEST;
    if (in == NULL || in->manifest == NULL || in->manifest_len == 0)
        return;

    /* ParseWithLength, not Parse: the buffer comes from a transport that
       hands over a byte count, and a truncated body is the realistic
       failure. Trusting NUL termination here would read past the end of
       exactly that buffer. */
    cJSON *root = cJSON_ParseWithLength(in->manifest, in->manifest_len);
    if (root == NULL)
        return;
    /* The format IS an array — an object at the top level is a manifest
       written against no schema at all, which is a shape violation
       (bad_manifest) rather than "no block I can read" (no_schema). */
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    int schema = -1;
    bool duplicate = false;
    const cJSON *block = select_block(root, OTA_SCHEMA_MAX, &schema, &duplicate);
    if (block == NULL) {
        out->reason = OTA_REASON_NO_SCHEMA;
        cJSON_Delete(root);
        return;
    }
    /* Not distinguishable from `out->schema = 1` today, and a mutation
       test will say so: decide() caps selection at OTA_SCHEMA_MAX, which
       is 1, and the filter is 1..cap, so a selected block's schema can
       only ever BE 1. It is written this way because that stops being
       true the moment the cap is bumped;
       test_highest_supported_schema_wins already pins the value the
       selector reports, at a cap of 2, so only this copy is riding on
       the equivalence. */
    out->schema = schema;
    out->duplicate_schema = duplicate;
    out->reason = resolve(block, in, out);
    cJSON_Delete(root);
}
