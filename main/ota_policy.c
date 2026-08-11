/* OTA decision logic — pure, host-tested. No clock, no NVS, no ESP
   includes: everything it decides from arrives as an argument. */
#include "ota_policy.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config_validate.h" /* config_is_https_url */

/* ---- reason codes ----

   One table, indexed by the enum, so a new code without a string is a
   compile-time array-size mismatch rather than a silently empty field in
   Home Assistant. test_ota_policy asserts the whole table is populated
   and distinct. */
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
    if (reason < 0 || reason >= OTA_REASON_COUNT)
        return "";
    return REASON_STR[reason];
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
       dereference safe only by remote control. */
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
    if (max_fails == 0)
        return false; /* budget disabled — never brick OTA from a config value */
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

/* Highest schema this build understands, first of any duplicates. A
   block that is not an object, or whose `schema` is missing or not a
   number, is skipped rather than fatal — the same forward-compatibility
   rule that applies to unknown keys, one level up.

   There is deliberately no lower bound test: seeding best_schema with the
   -1 "no block" sentinel is already one, since a negative schema can
   never beat it. Writing the bound out as well would be a branch no test
   could reach. */
static const cJSON *select_block(const cJSON *root, int *out_schema, bool *out_duplicate) {
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
        if (v > OTA_SCHEMA_MAX)
            continue;          /* a format this build predates: ignore, never fail */
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

/* ---- resolution within the selected block (schema 1) ---- */

static ota_reason_t resolve(const cJSON *block, const ota_decide_in_t *in, ota_decision_t *out) {
    const char *dev = (in->device_id != NULL) ? in->device_id : "";
    const cJSON *entry = NULL;

    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(block, "devices");
    if (cJSON_IsObject(devices) && dev[0] != '\0') {
        const cJSON *mine = cJSON_GetObjectItemCaseSensitive(devices, dev);
        /* A malformed entry falls through to the default rather than
           taking this device out of the fleet. */
        if (cJSON_IsObject(mine))
            entry = mine;
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
    const cJSON *block = select_block(root, &schema, &duplicate);
    if (block == NULL) {
        out->reason = OTA_REASON_NO_SCHEMA;
        cJSON_Delete(root);
        return;
    }
    out->schema = schema;
    out->duplicate_schema = duplicate;
    out->reason = resolve(block, in, out);
    cJSON_Delete(root);
}
