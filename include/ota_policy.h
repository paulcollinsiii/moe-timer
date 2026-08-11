#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_validate.h" /* CFG_BOUND_OTA_RESULT_MAX / _TARGET_MAX */

/* OTA decision logic — layer 1, pure. Every function here is a total
   function over its arguments: no clock, no NVS, no ESP-IDF includes, no
   I/O. It parses the manifest (with cJSON, as config_apply.c does),
   selects a schema block, resolves this device's entry, applies the
   preconditions and the retry budget, and maps a failed attempt to a
   reason code. ota_flow.c sequences it; ota.c does the transport.
   Host-tested end to end in test_ota_policy.

   Deliberately NOT here: the transport. The manifest arrives as a
   caller-supplied buffer, so HTTPS today and MQTT later reuse this module
   and its whole test suite unchanged.

   There are several entry points rather than one, because the facts a
   decision needs arrive at different moments in a wake: the gate runs
   before any network is spent, the decision needs the manifest bytes, the
   download gate re-samples what can only be known at the second window,
   and the reason mapping only exists once something has failed. A single
   entry point would force the caller to invent placeholder values for
   facts it cannot yet have. */

#ifdef __cplusplus
extern "C" {
#endif

/* Highest manifest schema this build understands. Blocks above it are
   ignored, which is what lets a schema-2 block ship alongside schema 1
   without stranding firmware that predates it. Bump when the parser
   learns a new format — and only then. */
#define OTA_SCHEMA_MAX 1

/* Version strings are bounded by esp_app_desc_t.version, which is a
   32-byte field: 31 chars is the longest value that can ever match a
   running version, so a longer one in the manifest is a publishing
   mistake and is rejected rather than stored truncated.

   The bound is on LENGTH only. Comparison is by C string, so a version
   carrying an escaped NUL ("1.5.0\u0000x") compares as "1.5.0" and reads
   as up_to_date. That is memory-safe, and it is not worth parser surgery
   to reject: the app descriptor cannot hold such a string either, so no
   device could ever be running the version it appears to name. */
#define OTA_VERSION_MAX 32

/* Binary-image URL from the manifest. More generous than
   CFG_BOUND_OTA_URL_MAX (the endpoint, which is stored in NVS and
   republished in the cfg state document) because this one is transient,
   is never echoed back, and carries a filename on top of the host path. */
#define OTA_URL_MAX 192

/* Retry budget used when max_fails arrives as 0. Mirrors
   CONFIG_MAGTAG_OTA_MAX_FAILS's Kconfig default; see
   ota_policy_budget_exhausted for why 0 is treated as "the caller forgot
   the field" rather than "no budget". */
#define OTA_MAX_FAILS_DEFAULT 3

/* Longest reason text, including the "http_<status>" form.

   This is a REQUIREMENT ON CONSUMERS, not a description of an existing
   constraint. Whatever buffer ota_result is read into must be at least
   this wide, and ota_target's must be at least OTA_VERSION_MAX.
   hal_nvs_read_str returns ESP_ERR_NVS_INVALID_LENGTH on a short buffer
   and writes NOTHING, so a narrow one leaves the caller's buffer
   uninitialised rather than truncating; for ota_target a failed read
   also means the budget comparison never matches and the device retries
   that version forever. */
#define OTA_REASON_TEXT_MAX 16

/* The reciprocal of that requirement, now that the storage widths exist.
   Asserting against this module's own strings would only prove they fit
   their own constant, so these tie the two independent declarations
   together instead: config_validate.h owns how wide the stored NVS values
   are, ota_policy.h owns how wide the text can get, and neither can be
   narrowed past the other without failing the build. Direction matters --
   storage must be >= text, not equal, so RESULT_MAX 24 carrying a 16-byte
   maximum is headroom, not a violation. */
_Static_assert(OTA_REASON_TEXT_MAX <= CFG_BOUND_OTA_RESULT_MAX,
               "ota_result storage is narrower than the longest reason text");
_Static_assert(OTA_VERSION_MAX <= CFG_BOUND_OTA_TARGET_MAX,
               "ota_target storage is narrower than the longest manifest version");

/* Why nothing happened — or, after an attempt, what went wrong. The
   failure-table codes (docs/planning/ota.plan.md) plus the "we decided
   not to" codes, which are equally worth publishing: "pinned" and
   "up_to_date" are the difference between a device that is behaving and
   a device that is silently broken.

   Every outcome gets a code here; deciding which ones reach NVS is
   ota_flow.c's job, not this module's. The plan's table wants ota_result
   left ALONE for the "no check ran" rows (no NTP in particular), so that
   a genuine failure from an earlier window is not overwritten by a
   routine skip. Returning the specific reason anyway costs nothing and
   gives the flow something to log — discarding it here would not. */
typedef enum {
    OTA_REASON_NONE = 0,     /* proceed / nothing to report */
    OTA_REASON_NO_URL,       /* endpoint empty = OTA disabled */
    OTA_REASON_NO_TIME,      /* no NTP: TLS cannot validate a cert without a clock */
    OTA_REASON_LOCKED,       /* charge lock engaged */
    OTA_REASON_LOW_BATT,     /* below the battery floor */
    OTA_REASON_LOW_HEAP,     /* not enough heap for the TLS session */
    OTA_REASON_UP_TO_DATE,   /* resolved version == running version */
    OTA_REASON_PINNED,       /* entry present, version absent/null = frozen */
    OTA_REASON_NO_ENTRY,     /* block has no default and does not list us */
    OTA_REASON_BAD_MANIFEST, /* unparseable, or the top level is not an array */
    OTA_REASON_NO_SCHEMA,    /* array parsed, no block this build can read */
    OTA_REASON_BAD_VERSION,  /* version wrong type, empty, or over the bound */
    OTA_REASON_BAD_URL,      /* url missing, wrong type, not https, or too long */
    OTA_REASON_GAVE_UP,      /* retry budget exhausted for this target */
    OTA_REASON_HTTP,         /* HTTP status >= 400; the status rides alongside */
    OTA_REASON_NET,          /* transport failed with no more detail */
    OTA_REASON_TLS,          /* TLS failed, but the chain verified */
    OTA_REASON_TLS_CERT,     /* the chain was REJECTED — a different fix entirely */
    OTA_REASON_TIMEOUT,      /* our own deadline aborted the download */
    OTA_REASON_BAD_IMAGE,    /* image header invalid / wrong chip */
    OTA_REASON_COUNT,
} ota_reason_t;

/* Stable short code for the reason, for logs, NVS and the HA stat
   payload. "" for OTA_REASON_NONE and for anything out of range. */
const char *ota_policy_reason_str(ota_reason_t reason);

/* Same, but folds an HTTP status into the code ("http_404"), because
   http_404 and http_500 are different answers with different fixes.
   http_status is ignored for every other reason. Always NUL-terminates. */
void ota_policy_reason_text(ota_reason_t reason, int http_status, char *out, size_t out_len);

/* May this reason be written to ota_result?

   The plan's failure table wants ota_result LEFT ALONE for the rows
   where no check ran, so that a download failure from the previous
   window is still there to be published in the next one — a download
   failure cannot publish itself, because MQTT is already closed when it
   happens. That rule is behaviour, so it lives here and is host-tested,
   rather than becoming an unwritten convention in the orchestrator.

   False for: nothing happened, no check ran (no_url / no_time / locked),
   and the check ran with correctly nothing to do (up_to_date / pinned).
   True for every genuine outcome, including the low_batt and low_heap
   skips, which the failure table does name an ota_result for. */
bool ota_policy_reason_is_persistable(ota_reason_t reason);

/* Everything a precondition can be decided from. Sampled by the caller;
   nothing in here is read by this module from anywhere else.
   batt_pct < 0 means "unreadable" and does not gate — the charge lock is
   the real low-battery defence, and a flaky ADC must not become a
   permanent OTA block. */
typedef struct {
    bool url_set;           /* the configured endpoint is non-empty */
    bool time_valid;        /* NTP has set the clock this session */
    bool charge_locked;     /* lock_gate_charge_locked() */
    int batt_pct;           /* state of charge, or < 0 if unknown */
    int min_batt_pct;       /* CONFIG_MAGTAG_OTA_MIN_BATT_PCT */
    uint32_t free_heap;     /* esp_get_free_heap_size() at the download point */
    uint32_t min_free_heap; /* headroom the TLS session needs */
} ota_gate_in_t;

/* May the manifest check run at all? OTA_REASON_NONE = yes. Cheap facts
   only: this runs before any network is spent. */
ota_reason_t ota_policy_check_gate(const ota_gate_in_t *in);

/* May the download run? The check gate's questions again — the two
   windows are minutes and a panel repaint apart, so the earlier answer is
   not evidence — plus the heap headroom, which is only knowable here. */
ota_reason_t ota_policy_download_gate(const ota_gate_in_t *in);

/* Inputs to the manifest decision. manifest_len is authoritative: the
   buffer need not be NUL-terminated, because a transport hands over a
   length and a truncated body is the realistic failure. A NULL device_id
   or running_version is read as "" rather than dereferenced. */
typedef struct {
    const char *manifest;
    size_t manifest_len;
    const char *device_id;       /* device_id(), e.g. "magtag-a1b2c3" */
    const char *running_version; /* esp_app_get_description()->version */
    const char *counted_target;  /* NVS ota_target: what ota_fails counts against */
    uint16_t fails;              /* NVS ota_fails */
    uint16_t max_fails;          /* CONFIG_MAGTAG_OTA_MAX_FAILS; 0 disables the budget */
} ota_decide_in_t;

/* version/url are filled as soon as they validate, including on the
   no-update outcomes — the caller reports the target it gave up on, and
   the retry counter is keyed on it. schema is the block that was read,
   or -1 if none was.

   duplicate_schema: more than one block carried the schema that was
   selected. The manifest is malformed (the schemas are meant to be
   non-overlapping), the first block wins, and the run continues. It is
   reported rather than logged here because this module cannot log —
   ota_flow.c owns the ESP_LOGW, which is the plan's "take the first and
   log it" split across the layer boundary. Only meaningful when
   schema >= 0: on the bad_manifest and no_schema paths no block was
   selected, so there is nothing for it to be about and it is always
   false. */
typedef struct {
    bool update;
    ota_reason_t reason;
    int schema;
    bool duplicate_schema;
    char version[OTA_VERSION_MAX];
    char url[OTA_URL_MAX];
} ota_decision_t;

/* Manifest bytes in, "download this / do nothing because X" out.
   Never writes past `out`, never keeps a pointer into the manifest. */
void ota_policy_decide(const ota_decide_in_t *in, ota_decision_t *out);

/* The retry budget, exposed separately from the decision because the
   caller also has to MAINTAIN the counter after an attempt, which is a
   different question from "may I attempt".

   exhausted: true once `fails` has reached `max_fails` for this exact
   target. A different target, or a counter that has never been armed,
   means the count is stale and does not apply — that is the escape from
   the sad loop: publishing a new version re-arms the device with no
   reset from anywhere else.

   max_fails == 0 falls back to OTA_MAX_FAILS_DEFAULT. It does NOT mean
   "no budget": the Kconfig range is 1..10 so menuconfig cannot produce a
   0, which leaves a caller that forgot the field as the only realistic
   source — and reading that as "unlimited retries" would silently
   reinstate the daily sad loop the budget exists to prevent. */
bool ota_policy_budget_exhausted(const char *target, const char *counted_target, uint16_t fails, uint16_t max_fails);

/* What ota_fails should become after a failed attempt against `target`:
   one more for the same target, back to 1 for a different one. Saturates
   rather than wrapping, so a wrapped counter can never look fresh. */
uint16_t ota_policy_next_fail_count(const char *target, const char *counted_target, uint16_t fails);

/* What layer 3 could observe about a failure, in transport-neutral terms
   so the classification stays here and testable rather than hiding in the
   driver. tls_cert_flags is the mbedtls certificate-verify bitmask that
   esp_tls_get_and_clear_last_error() reports; non-zero means the chain
   was rejected, which is the whole point of the tls_cert / tls split. */
typedef struct {
    bool deadline_hit;     /* our own budget aborted the transfer */
    bool image_rejected;   /* the image header or chip id was refused */
    bool tls_failed;       /* the TLS layer reported a failure */
    int tls_cert_flags;    /* mbedtls X509 verify flags; 0 = chain was fine */
    int http_status;       /* last HTTP status seen, 0 if none */
    bool transport_failed; /* any other transport error */
} ota_error_facts_t;

/* Facts to reason code. Precedence: our deadline first (the abort is the
   cause, whatever the socket says on the way out), then cert, then TLS,
   then the image, then HTTP, then a bare transport error. */
ota_reason_t ota_policy_reason(const ota_error_facts_t *facts);

#ifdef __cplusplus
}
#endif
