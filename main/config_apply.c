/* Apply an HA config document to NVS. Pure over nvs_config and the shared
   validators, plus one read of the compile-time timer table
   (timer_defs_compiled) for the bottom rung of apply_timers()' optional-key
   ladder. Host-tested.

   ONE EXCEPTION to "pure over nvs_config", and it is deliberate:
   apply_chores() also reads and writes the LIVE RTC chore acks, through
   timer_chore_acked()/timer_chore_set_acked(). Editing the chore list
   invalidates positional ack bits that this wake may still repaint from,
   and nothing else reconciles them before the next boot. The reasoning is
   at apply_chores(). */
#include "config_apply.h"

#include <stdio.h>
#include <string.h>

#include "bedtime.h" /* bedtime_hhmm_valid */
#include "cJSON.h"
#include "chore_store.h"
#include "chores.h"
#include "config_validate.h" /* config_is_iso_date */
#include "nvs_config.h"
#include "quiet_hours.h" /* quiet_hhmm_valid */
#include "timer.h"       /* TIMER_EXTRA_SLOTS, timer_defs_compiled */
#include "tones.h"       /* tones_names for the tone selects */

/* ---- error accumulator: builds the ack "errors" list ---- */

/* Capacity budget: the finished ack must fit CONFIG_ACK_MIN whole, or the
   ack itself is truncated into unparseable JSON — the very failure this
   accumulator exists to prevent, one level up. Worst case is
   {"ver":"<=23"},"ok":false,"errors":[<errors>],"errors_truncated":true}
   = 80 B of envelope, so the list gets the rest with margin. */
#define ERR_LIST_CAP 160

typedef struct {
    char errors[ERR_LIST_CAP];
    int count;
    bool truncated;
} err_acc_t;

/* Append a field name to the ack's "errors" array.
   snprintf alone was not enough: it truncates mid-name and still reports
   the would-be length, so `count` kept incrementing and the list ended in
   an unterminated JSON string (a document with every field wrong-typed
   produced ...,"tone_bed","alert_v]}). HA then fails to parse the whole
   ack, so EVERY error is lost — including the ones that fitted, and
   including the OTA fields, which sort last and were dropped first.
   An entry that does not fit WHOLE is therefore dropped and flagged. */
static void err_add(err_acc_t *e, const char *field) {
    size_t used = strlen(e->errors);
    size_t need = strlen(field) + 2 + (e->count ? 1u : 0u); /* quotes + separator */
    if (used + need + 1 > sizeof(e->errors)) {
        e->truncated = true;
        return;
    }
    int n = snprintf(e->errors + used, sizeof(e->errors) - used, "%s\"%s\"", e->count ? "," : "", field);
    if (n > 0)
        e->count++;
}

/* Apply a bounded integer field to a u16 setter; records the field name on
   an out-of-range or wrong-typed value. Absent field = no-op. */
static void apply_u16(const cJSON *root, const char *field, int lo, int hi, esp_err_t (*setter)(uint16_t),
                      err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsNumber(item) || item->valuedouble < lo || item->valuedouble > hi) {
        err_add(e, field);
        return;
    }
    if (setter((uint16_t)item->valueint) != ESP_OK)
        err_add(e, field);
}

/* HHMM time-of-day field: real-time validity (hour<=23, minute<=59) via
   the field's `valid` rule, not a plain numeric range. Absent = no-op. */
static void apply_hhmm(const cJSON *root, const char *field, bool (*valid)(int), esp_err_t (*setter)(uint16_t),
                       err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsNumber(item) || !valid(item->valueint)) {
        err_add(e, field);
        return;
    }
    if (setter((uint16_t)item->valueint) != ESP_OK)
        err_add(e, field);
}

static void apply_date(const cJSON *root, const char *field, esp_err_t (*setter)(const char *), err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsString(item) || !config_is_iso_date(item->valuestring)) {
        err_add(e, field);
        return;
    }
    if (setter(item->valuestring) != ESP_OK)
        err_add(e, field);
}

/* Enum-as-option-string field (mirrors the HA select): the string must
   match one of `options` exactly; stored as its index. Absent = no-op. */
static void apply_enum(const cJSON *root, const char *field, const char *const *options, int n_options,
                       esp_err_t (*setter)(uint16_t), err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (cJSON_IsString(item)) {
        for (int i = 0; i < n_options; i++) {
            if (strcmp(item->valuestring, options[i]) == 0) {
                if (setter((uint16_t)i) != ESP_OK)
                    err_add(e, field);
                return;
            }
        }
    }
    err_add(e, field);
}

/* `valid` is the optional field-specific content rule — the same predicate
   ha_config.c's registry attaches to the field, so the bulk document and
   the per-entity set path accept exactly the same values. NULL = length is
   the whole rule. Absent field = no-op. */
static void apply_str(const cJSON *root, const char *field, size_t maxlen, bool (*valid)(const char *),
                      esp_err_t (*setter)(const char *), err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= maxlen || (valid != NULL && !valid(item->valuestring))) {
        err_add(e, field);
        return;
    }
    if (setter(item->valuestring) != ESP_OK)
        err_add(e, field);
}

/* JSON boolean -> u16 0/1 (the CFG_BOOL entities). Strictly a boolean:
   `1` and `"ON"` are named as errors rather than guessed at, so a
   mistyped document is visible in the ack instead of half-applying.
   Absent = no-op, which is what keeps a document that says nothing about
   the field from clearing it. */
static void apply_bool(const cJSON *root, const char *field, esp_err_t (*setter)(uint16_t), err_acc_t *e) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (item == NULL)
        return;
    if (!cJSON_IsBool(item)) {
        err_add(e, field);
        return;
    }
    if (setter(cJSON_IsTrue(item) ? 1 : 0) != ESP_OK)
        err_add(e, field);
}

#define HOLIDAY_BLOB_CAP 512

static void apply_holidays(const cJSON *root, err_acc_t *e) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "holidays");
    if (arr == NULL)
        return;
    if (!cJSON_IsArray(arr)) {
        err_add(e, "holidays");
        return;
    }
    char blob[HOLIDAY_BLOB_CAP];
    size_t pos = 0;
    bool had_bad = false;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsString(item) || !config_is_iso_date(item->valuestring)) {
            had_bad = true;
            continue;
        }
        if (pos + 11 > sizeof(blob))
            break; /* cap: whole dates only, never a partial trailing line */
        memcpy(blob + pos, item->valuestring, 10);
        blob[pos + 10] = '\n';
        pos += 11;
    }
    if (had_bad)
        err_add(e, "holidays");
    if (nvs_config_set_holidays(blob, pos) != ESP_OK)
        err_add(e, "holidays");
}

/* ---- the chore checklist (design 1.2; rows C1, C10, C11, C12) ---- */

/* The chore list. Document-only, exactly like `holidays` above: no
   accessor pair, no HA entity of its own, CHORE_MAX entries of at most
   CHORE_NAME_MAX bytes.

   IT DOES NOT TRUNCATE, and that is the one place it departs from
   apply_holidays() above, which `break`s when its blob fills and flags
   NOTHING — a dropped holiday is invisible in the ack. Design 1.2
   requires the opposite here ("enforced with a named error in the
   config_ack, never truncated: a fourth chore is an `errors` entry"), so
   an over-cap count, an over-long name, an empty name and a non-string
   entry are all named. Copying the holidays template verbatim would have
   inherited the silent drop.

   AND IT WRITES NOTHING on any of those, rather than keeping the entries
   that were fine. Same reasoning as apply_timers()' whole-array rejection:
   a list that is half the operator's intent is worse than the list that
   was already there, because the acks are positional against it and a kid
   would be ticking rows nobody asked for.

   ABSENT AND EMPTY ARE DIFFERENT STATEMENTS. Absent = the document says
   nothing about the list, so the stored list stands — the durability rule
   every optional field in this file obeys, and what stops a document that
   predates the feature from wiping a configured list. `"chores": []` = the
   document says there are none, which is the documented way to turn the
   feature off (row C1: no chores configured, the gate is inert), so it
   writes the empty list. Refusing `[]` would leave no way to switch the
   feature off from the bulk document at all.

   config_is_clean_str() IS HYGIENE AT THE POINT OF ENTRY, and it is
   explicitly NOT justified by a downstream escaper this function does not
   control. Two facts, because an earlier version of this comment got both
   wrong: (1) no chore name reaches a JSON builder today at all —
   chore_store_load_names() has two callers, this file and chore_store.c,
   and nothing in stats_json.c, mqtt_ha.c or ha_config.c touches a name;
   (2) when one does, it will be escaped, not corrupting, because
   stats_json.c runs every embedded string through jesc() and states the
   contract in as many words — "NO string reaching it can break the JSON,
   and a field that is safe only because of what some other module
   currently does is a field that stops being safe the day that module
   changes".

   What the check buys, beside the 20-BYTE storage cap, is a name that is
   RENDERABLE and a rejection the operator can see: no control byte to
   smear a panel row or a log line, and a bad name named in the ack rather
   than stored and discovered later on the device. Keeping it also keeps
   this function's contract independent of stats_json.c's, which is the
   direction that file's own comment argues for.

   apply_timers() above does NOT hold timer names to this rule. That is
   pre-existing, and with jesc() in the path closing it would be
   belt-on-braces — so it is neither a precedent to copy nor a gap this
   function is compensating for. */
static void apply_chores(const cJSON *root, err_acc_t *e) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "chores");
    if (arr == NULL)
        return;
    if (!cJSON_IsArray(arr)) {
        err_add(e, "chores");
        return;
    }
    char names[CHORE_MAX][CHORE_NAME_BUF];
    memset(names, 0, sizeof(names));
    uint8_t n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        /* n >= CHORE_MAX is FIRST, so a fourth entry is refused before
           anything indexes names[3]; the rest is the per-entry contract.
           CHORE_NAME_MAX is a BYTE count (chores.h) and strlen counts
           bytes, so a 16-character German name in 17 bytes is measured as
           17 — which is the storage question the cap answers. */
        if (n >= CHORE_MAX || !cJSON_IsString(item) || item->valuestring[0] == '\0' ||
            strlen(item->valuestring) > CHORE_NAME_MAX || !config_is_clean_str(item->valuestring)) {
            err_add(e, "chores");
            return;
        }
        /* memcpy, not snprintf: snprintf would silently truncate, which is
           the one behaviour this function exists to avoid. The length was
           just bounded at CHORE_NAME_MAX, so the terminator fits inside
           CHORE_NAME_BUF by construction. */
        memcpy(names[n], item->valuestring, strlen(item->valuestring) + 1);
        n++;
    }

    /* The list that is about to be replaced, for the ack reconcile below.
       chore_store_load_names() writes both outputs on every path, so a
       missing or rejected blob reads as the empty list rather than
       leaving stack garbage to hash. */
    char prev[CHORE_MAX][CHORE_NAME_BUF];
    uint8_t prev_n = 0;
    chore_store_load_names(prev, &prev_n);
    uint16_t prev_hash = chores_list_hash(prev, prev_n);

    if (chore_store_save_names(names, n) != ESP_OK) {
        err_add(e, "chores");
        return; /* the stored list did not move, so the acks still mean what they meant */
    }

    /* THE LIVE ACKS, and this is the half that is easy to miss. Ack bits
       are POSITIONAL (chores.h) and this apply runs inside the network
       window, so from here on the RTC holds a mask positional to the list
       that just went away while the panel may still repaint in this same
       wake — it would tick the wrong rows.
       chore_store_load_ack() applies row C10's rule on the next boot, from
       the hash stored beside the record, and the record in flash is left
       alone here precisely because that reconcile is what heals it. What
       nothing else heals is RTC, so it is healed here.
       The rule itself is chores_reconcile()'s and is not restated: acks
       cleared because the bits stop meaning anything, `released`
       PRESERVED because a list edit must never re-lock a day that has
       already released. An unchanged list hashes the same and comes back
       untouched, so this needs no "did it change?" test of its own.

       THE LAYERING HERE IS A KNOWN TRADE-OFF, recorded rather than
       hidden. net_apply_finish() (main/net_apply.c) is arguably the
       better home: it already holds reconcile_defs(), whose job is
       exactly "the MQTT window changed persisted config, reconcile the
       derived live state, and tell the caller whether to re-render", and
       this placement makes config_apply.c the SECOND writer of the RTC
       chore acks (timer_persist.c is the first). The cost is real and
       paid: config_apply.c now depends on chores.c and chore_store.c, so
       every single-TU host suite that includes it inherits both.

       WHAT MAKES THE MOVE NON-TRIVIAL, and the reason it was not made:
       net_apply_finish() runs AFTER this function, by which point the old
       list is gone from NVS, so it cannot compute prev_hash the way this
       function does (read-before-write). The only other source is the ack
       record's stored list_hash, whose provenance is DIFFERENT — that is
       the hash as of the last TOGGLE, not the last list EDIT, so two list
       edits with no toggle between them leave it stale in a way the
       read-before-write here cannot be. An apparently-cleaner design with
       a bug waiting in it.

       Why RTC and only RTC: a repaint genuinely can follow this apply in
       the same wake (wake_flow.c opens the network window; net_apply.c
       joins it and can drive a re-render), and timer_persist.c is
       boot-time only, so nothing else reconciles RTC before the next
       boot. The flash ack record is deliberately left alone because
       chore_store_load_ack() re-derives from its stored hash at boot. */
    chore_ack_t live = {.acked = timer_chore_acked(), .released = timer_chore_released()};
    chore_ack_t next = chores_reconcile(live, prev_hash, chores_list_hash(names, n));
    timer_chore_set_acked(next.acked);
    timer_chore_set_released(next.released);
}

/* The four chore_free_* minute keys, each with the allocation it is paired
   with. ONE table rather than two hand-written lists of four, because the
   apply pass and the cross-field pass below have to agree on the pairing —
   separate lists would be two chances to pair summer with the weekend, and
   a mispairing is silent. */
typedef struct {
    const char *free_field;
    const char *alloc_field;
    esp_err_t (*set_free)(uint16_t);
    esp_err_t (*get_free)(uint16_t *);
    esp_err_t (*get_alloc)(uint16_t *);
} chore_free_pair_t;

static const chore_free_pair_t k_chore_free_pairs[] = {
    {"chore_free_wd", "weekday_min", nvs_config_set_chore_free_wd, nvs_config_get_chore_free_wd,
     nvs_config_get_weekday_min},
    {"chore_free_we", "weekend_min", nvs_config_set_chore_free_we, nvs_config_get_chore_free_we,
     nvs_config_get_weekend_min},
    {"chore_free_hol", "holiday_min", nvs_config_set_chore_free_hol, nvs_config_get_chore_free_hol,
     nvs_config_get_holiday_min},
    {"chore_free_sum", "summer_min", nvs_config_set_chore_free_sum, nvs_config_get_chore_free_sum,
     nvs_config_get_summer_min},
};
#define CHORE_FREE_PAIRS (sizeof(k_chore_free_pairs) / sizeof(k_chore_free_pairs[0]))

/* Range only. The CROSS-FIELD rule is a separate pass on purpose — see
   check_chore_free_pairs(). LO is 0 and that is not a typo: 0 is fully
   gated, the default and the state every device in the field is in. */
static void apply_chore_free(const cJSON *root, err_acc_t *e) {
    for (size_t i = 0; i < CHORE_FREE_PAIRS; i++) {
        apply_u16(root, k_chore_free_pairs[i].free_field, CFG_BOUND_CHORE_FREE_LO, CFG_BOUND_CHORE_FREE_HI,
                  k_chore_free_pairs[i].set_free, e);
    }
}

/* Is `field` already in the errors list? The entries are whole quoted
   names, so match the quotes. */
static bool err_has(const err_acc_t *e, const char *field) {
    char quoted[32];
    snprintf(quoted, sizeof(quoted), "\"%s\"", field);
    return strstr(e->errors, quoted) != NULL;
}

/* The cross-field rule `chore_free <= allocation`, for all four pairs.

   THE ORDERING REQUIREMENT, stated exactly: it must run after the four
   ALLOCATIONS and the four chore_free_* have been applied — those eight
   fields, and no more. It is CALLED last in the apply pass only because
   that is the simplest place satisfying the requirement; nothing between
   apply_chore_free() and the call site touches either side of any pair,
   and moving the call up to sit directly after apply_chore_free() changes
   no behaviour (measured, not assumed). What DOES break it is running it
   before the allocations land, which is the mutation the key-order test
   below kills. Do not read "called last" as "must be last".

   WHY NOT AS THE FIELD IS APPLIED: a single document can carry both
   `weekday_min` and `chore_free_wd`, cJSON preserves key order, and a
   check performed at apply time compares against whichever allocation
   happens to be in NVS at that instant. {"weekday_min":1440,
   "chore_free_wd":1000} would then be VALID with the allocation first and
   INVALID with the free slice first — the same document, two answers,
   decided by the order HA happened to serialise its keys in. Running the
   check once, at the end, over the values that actually landed makes the
   answer a function of the document's CONTENT. Both orders are pinned in
   test_config_apply.

   ALL FOUR PAIRS, whatever day it is (row C12). A broken summer pair in
   December is still a named error, it is just dormant: "the device does
   not block in December over a broken summer setting" is the BLOCKING
   gate's rule (M2's config-error screen, which reads today's day type),
   not this ack's. This function has no clock and needs none.

   C12's "NAMED BUT DORMANT" IS NOT DURABLE, and that is a design-level
   gap for M2/M3 rather than a defect here. The config_ack is published
   RETAINED (mqtt_ha.c), so the naming does persist in MQTT — but only
   until the NEXT document overwrites that topic, typically with
   ok:true. After that a broken NON-TODAY pair is invisible everywhere:
   this ack is gone, and C11's blocking screen only reads TODAY's day
   type. So "named in the config_ack; dormant" is satisfied at the moment
   of the edit, not durably. If a broken summer pair should still be
   discoverable in December, the place for it is the stat payload (a
   config-warning field), which is M2/M3's call — deliberately not made
   here, because inventing a second channel for it would be scope this
   task does not own.

   WHAT IT DOES NOT DO: it does not reject the write and it does not clamp.
   The values stay exactly as the document set them and the pair is left
   standing in NVS, because rows C11 and C12 both describe a device that
   HAS the broken pair — C11's config-error screen has to have something
   to fire on, and C12's "dormant" is only meaningful if the broken
   setting persists. The per-entity setter path (ha_config.c) is the one
   the design gives the reject/clamp asymmetry to; see config_validate.h.

   THAT IS A DELIBERATE DIVERGENCE FROM apply_str()'s stated principle
   above — "the bulk document and the per-entity set path accept exactly
   the same values" — and M1-T8 must not "fix" it by unifying them.
   Concretely, for `weekday_min: 30` arriving while chore_free_wd holds
   120: the per-entity allocation setter CLAMPS chore_free_wd to 30 and
   NVS ends consistent, while this path leaves chore_free_wd at 120,
   names it in the ack, and NVS holds the invalid pair. Two routes, two
   outcomes, on purpose — config_validate.h assigns the reject/clamp
   asymmetry to the per-entity setters by name and never to the bulk
   applier, and C11/C12 need the broken pair to survive. The divergence
   is recorded at both ends: here, and in config_validate.h.

   ONLY PAIRS THE DOCUMENT SPEAKS TO. Nothing re-validates a value sitting
   in NVS (config_validate.h says so in as many words), and errors[] is a
   statement about THIS document — re-reporting a pre-existing broken pair
   would make every unrelated document ack ok:false forever. EITHER key
   counts, because the document can break a pair from either side: by
   raising the free slice, or by lowering the allocation under it. */
static void check_chore_free_pairs(const cJSON *root, err_acc_t *e) {
    for (size_t i = 0; i < CHORE_FREE_PAIRS; i++) {
        const chore_free_pair_t *p = &k_chore_free_pairs[i];
        if (cJSON_GetObjectItemCaseSensitive(root, p->free_field) == NULL &&
            cJSON_GetObjectItemCaseSensitive(root, p->alloc_field) == NULL)
            continue;
        /* The STORED pair, so a value the range check refused is judged on
           what is actually in NVS. Both getters fill *out on every path
           (nvs_config.c's get_u16_with_default), so an unwritten or
           unreadable key reads as its default rather than as garbage —
           which is why neither return code is checked, the same
           conflation apply_timers() makes below.
           The cost, stated so it is not a surprise: on an NVS READ FAULT
           the pair reads as free=0 against the allocation default, which
           is VALID, so a genuinely broken pair goes UNNAMED that window.
           Silent-and-permissive is the right failure direction here (the
           alternative is naming a field the document may not even have
           mentioned), and the next document re-runs the check. */
        uint16_t free_min = 0;
        uint16_t alloc_min = 0;
        p->get_free(&free_min);
        p->get_alloc(&alloc_min);
        /* Argument order copied verbatim from config_validate.h: the free
           slice is the SUBJECT and goes FIRST. Both parameters are
           uint16_t, so a swap compiles silently and inverts the answer. */
        if (config_is_valid_chore_free_min(free_min, alloc_min))
            continue;
        /* Named once. apply_chore_free() may already have named this field
           for being out of range, in which case the stored value it left
           behind can also break the pair; two entries for one key read as
           two faults and cost the 160-byte list twice. */
        if (!err_has(e, p->free_field))
            err_add(e, p->free_field);
    }
}

static void apply_timers(const cJSON *root, err_acc_t *e) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "timers");
    if (arr == NULL)
        return;
    if (!cJSON_IsArray(arr)) {
        err_add(e, "timers");
        return;
    }
    /* The stored table, for the optional-key rule below. A read failure
       (never written, or version drift) just means "no slot existed", which
       is the same answer as an empty table — so no error path is needed.

       That conflation has a cost, and it is deliberate rather than
       overlooked. On version drift the bytes are still physically there;
       treating them as absent skips tier 2 for EVERY slot, so the
       menuconfig rung answers and the result is then persisted over the
       data that was still present. For `break_eligible` on this project's
       shipping sdkconfig (slots 1 and 2 are =y) that turns an operator's
       deliberate break=OFF back ON — the unsafe direction for a safety
       gate, since a break-eligible activity may run during, and drain, a
       Screen Break. It is accepted because the design's whole premise is
       that "unreadable" cannot be distinguished from "absent" without a
       migration, and a migration is what BUG-5 defers. The operator-side
       mitigation is the same one BUG-6 has: state `"break": false` in the
       document, which is durable across any blob loss. Recorded in
       docs/home_assistant.md so it is not folklore. */
    nvs_timer_defs_blob_t prev;
    bool have_prev = (nvs_config_get_timer_defs(&prev) == ESP_OK);

    nvs_timer_defs_blob_t defs;
    memset(&defs, 0, sizeof(defs));
    defs.version = TIMER_DEFS_BLOB_VERSION;
    int slot = 0;
    const cJSON *entry;
    /* One bad entry rejects the whole array, so the error has to name the
       entry: "timers" alone reads as "none of your timers applied" with
       no clue which one to fix, and an over-long name is the easy way to
       land here. */
    char where[16];
    cJSON_ArrayForEach(entry, arr) {
        if (slot >= TIMER_EXTRA_SLOTS)
            break;
        snprintf(where, sizeof(where), "timers[%d]", slot);
        if (!cJSON_IsObject(entry)) {
            err_add(e, where);
            return;
        }
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        const cJSON *min = cJSON_GetObjectItemCaseSensitive(entry, "min");
        const cJSON *reload = cJSON_GetObjectItemCaseSensitive(entry, "reload");
        const cJSON *brk = cJSON_GetObjectItemCaseSensitive(entry, "break");
        if (name == NULL) {
            slot++; /* {} = disabled slot */
            continue;
        }
        if (!cJSON_IsString(name) || strlen(name->valuestring) >= sizeof(defs.defs[slot].name)) {
            err_add(e, where);
            return; /* whole array rejected — a half-written table is worse */
        }
        if (min == NULL || !cJSON_IsNumber(min) || min->valuedouble < CFG_BOUND_TIMER_MIN_LO ||
            min->valuedouble > CFG_BOUND_TIMER_MIN_HI) {
            err_add(e, where);
            return;
        }
        snprintf(defs.defs[slot].name, sizeof(defs.defs[slot].name), "%s", name->valuestring);
        defs.defs[slot].min = min->valueint;
        /* Optional keys (`reload`, `break`) resolve down a three-tier
           ladder, evaluated per FIELD, not per slot:

               this document  >  the stored blob  >  menuconfig

           the document's key if it carries one; otherwise the stored value
           for a slot that already HAS a definition; otherwise the
           compile-time value for this slot index. There is no fourth tier:
           a slot menuconfig does not configure has 0 in both flags anyway,
           so "empty defaults" is what the bottom rung already yields there.

           Tier 1 over tier 2 is BUG-6. It used to be "absent means false"
           unconditionally, which silently cleared a flag the operator had
           set from the per-timer switch every time a document that did not
           mention it was applied — and the retained set/ command that would
           have restored it is consumed on apply (mqtt_ha.c), so nothing
           healed it. (When BUG-6 was written `break` was missing from the
           documented `timers` schema, which is why documentation-shaped
           documents omitted it. docs/home_assistant.md documents both keys
           now, so an operator who states `break` explicitly gets durable
           intent — it survives an NVS erase and tier 3 never applies to
           that slot. Tier 3 is the answer for slots the document does not
           speak to.) See docs/planning/refactor.bugdiscoveries.md.

           Tier 2 over tier 3 keeps a value someone chose in HA above one
           chosen at build time.

           The menuconfig tier is the one that keeps getting lost, so, twice
           over: it is a DEFAULT CONSULTED IN PLACE, never a value written
           to flash to earn standing. Before BUG-8 the ladder appeared to
           work only because timer_defs_install() wrote the Kconfig table to
           NVS at boot, which made `existed` true and put the compile-time
           value in `prev` — laundering a build-time default into tier-2
           storage, where it was indistinguishable from an operator's choice
           and got published to HA as one. THAT is BUG-8. ad62dff removed
           the laundering and took the tier with it, collapsing the ladder
           to `document > empty` and dropping a deliberate menuconfig
           break-eligibility on the first document applied after an NVS
           erase. The timer_defs_compiled() reads below restore the tier
           without the forgery. Reinstating the boot write to get it back
           would reopen BUG-8; deleting these reads would flatten the ladder
           again.

           A menuconfig value that reaches flash THROUGH this function is
           not that masquerade: the document is authoritative, and a later
           document omitting the key then preserving it via tier 2 is the
           intended outcome, not a regression.

           Caveat, and it now has teeth: "already has a definition" is by
           SLOT INDEX, not by name, so a document that repurposes a slot
           carries the previous activity's flags over — and on a first
           definition it carries MENUCONFIG's flags over. Slots 1 and 2 ship
           break-eligible in this project's sdkconfig, so repurposing either
           to a screen activity while omitting `break` now inherits `true`
           and lets that activity run during (and drain) a Screen Break,
           where it used to resolve to false. Repurposing a slot index to a
           different KIND of activity must say `"break": false` explicitly
           rather than rely on omission. Same for `reload`, whose menuconfig
           help warns that a non-break-eligible chore should not be
           reloadable.

           Accepted consequence, decided deliberately rather than stumbled
           into: `break` is settable from the per-timer switch, and that
           retained set/ command is consumed on apply. So a "break off" that
           exists ONLY as a switch flip — never written into the retained
           document — does not survive an NVS erase: the blob is gone, the
           command was eaten, the document says nothing, and menuconfig's
           `y` wins. The operator accepted that price for ranking menuconfig
           above empty; the fix if it bites is `"break": false` in the
           document, not deleting the tier. */
        /* "Already has a definition", widened by the `defined` provenance
           bit. The name test alone missed a slot an HA per-timer SWITCH had
           touched before the slot was ever named — reload/break set, name
           still "" — and handed that operator's choice back to menuconfig
           the moment a document named the slot. The two tests are OR'd, not
           swapped: `defined` reads 0 on every blob written before the field
           existed, so on a device in the field the name test is the one
           that answers and behaviour is bit-for-bit what it is today.

           Consequence of the OR, stated so it is not discovered later: a
           blob that ha_config.c's fallback wrote (an HA control edit on a
           device with no table) carries MENUCONFIG-seeded names for the
           slots the edit did not touch, and a non-empty name reads as
           `existed`. That is harmless at the moment of the write — the
           seed came from timer_defs_compiled(), so tier 2 and tier 3 hold
           the same value — but it freezes those slots against a LATER
           menuconfig change. In other words the first per-timer control
           edit provisions the whole table, not just the slot it stamps.
           Per-slot `defined` granularity was chosen over per-field; this is
           the edge it does not cover. */
        bool existed = have_prev && (prev.defs[slot].defined || prev.defs[slot].name[0] != '\0');
        /* Tier 3. NULL only if TIMER_EXTRA_SLOTS outgrew the Kconfig table. */
        const timer_def_t *ct = timer_defs_compiled(slot + 1);
        uint8_t ct_reload = (ct != NULL && ct->reloadable) ? 1 : 0;
        uint8_t ct_break = (ct != NULL && ct->break_eligible) ? 1 : 0;
        defs.defs[slot].reload = (reload != NULL) ? (cJSON_IsTrue(reload) ? 1 : 0)
                                 : existed        ? prev.defs[slot].reload
                                                  : ct_reload;
        defs.defs[slot].break_eligible = (brk != NULL) ? (cJSON_IsTrue(brk) ? 1 : 0)
                                         : existed     ? prev.defs[slot].break_eligible
                                                       : ct_break;
        /* The document is an authority, so a slot it names is defined by
           it. Redundant with the name test for THIS slot; recorded anyway
           so the bit means one thing everywhere ("an authority set this")
           rather than "an authority set this, except when a name happens
           to be present". A `{}` entry takes the `continue` above and stays
           0, which is the same answer the name test gives it today. */
        defs.defs[slot].defined = 1;
        slot++;
    }
    if (nvs_config_set_timer_defs(&defs) != ESP_OK)
        err_add(e, "timers");
}

int config_ack_too_long(char *ack, size_t ack_len, int len, int max) {
    return snprintf(ack, ack_len, "{\"ok\":false,\"err\":\"too_long\",\"len\":%d,\"max\":%d}", len, max);
}

config_result_t config_apply(const char *json, char *ack, size_t ack_len) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        snprintf(ack, ack_len, "{\"ok\":false,\"err\":\"parse\"}");
        return CONFIG_INVALID;
    }

    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "ver");
    /* ver may arrive as a JSON number or string; normalize to text */
    char ver_str[24] = {0};
    if (cJSON_IsString(ver) && ver->valuestring[0] != '\0') {
        snprintf(ver_str, sizeof(ver_str), "%s", ver->valuestring);
    } else if (cJSON_IsNumber(ver)) {
        snprintf(ver_str, sizeof(ver_str), "%lld", (long long)ver->valuedouble);
    } else {
        snprintf(ack, ack_len, "{\"ok\":false,\"err\":\"no_ver\"}");
        cJSON_Delete(root);
        return CONFIG_INVALID;
    }
    /* ver is interpolated raw into ALL THREE ack emissions below, so a
       quote, backslash or control character in it produces unparseable
       JSON ({"ver":"a"b","ok":true}) — the same defect class the errors[]
       cap fixes, but reachable with one mistyped ver instead of twelve
       simultaneously invalid fields. Rejected rather than escaped: ver is
       also strcmp'd against and stored in NVS as cfg_ver, and one
       representation is the only way those two stay in agreement. */
    if (!config_is_clean_str(ver_str)) {
        snprintf(ack, ack_len, "{\"ok\":false,\"err\":\"ver\"}");
        cJSON_Delete(root);
        return CONFIG_INVALID;
    }

    char stored[24];
    nvs_config_get_cfg_ver(stored, sizeof(stored));
    if (strcmp(stored, ver_str) == 0) {
        /* Skipped — with one exception, and it is the difference between a
           recoverable device and a bricked config.

           `cfg_ver` and the timer-defs blob are independent NVS keys. Lose
           the blob (version drift, a bad read, a partial erase) while
           `cfg_ver` survives and this early return fires on every window
           forever: the document that would rebuild the table is never
           looked at, ha_config.c's per-timer writes have no readable table
           to modify, and there is nothing the operator can do from HA
           except notice the timers are wrong. Bumping `ver` fixes it, but
           only if you already know that — which is precisely the knowledge
           a stuck device denies you.

           So: on a `ver` match, still rebuild the timer table if it is
           unreadable. Deliberately narrow. No other NVS KEY is re-applied
           (everything else has its own key and is not at risk of this
           coupling), the result is still CONFIG_SKIPPED because no new
           configuration was accepted, and apply_timers() returns
           immediately for a document with no `timers` array. Errors go to
           the accumulator and are dropped: the ack for a skipped document
           says "skipped", and inventing an error field here would make a
           document that is fine look broken. BUG-5.

           Three consequences that "narrow" does NOT cover. None is a bug to
           fix here; they are recorded because each was rediscovered once.

           1. It DESTROYS the unreadable bytes. apply_timers() sees
              have_prev == false, so tier 2 is skipped for every slot,
              menuconfig answers, and the result is written over data that
              was still physically intact. The runtime answer for this wake
              was already menuconfig's (timer_defs_install() ran on the
              compile-time table anyway); what this adds is persistence, with
              no operator action and an ack of ok:true. The governing
              consequence: any TIMER_DEFS_BLOB_VERSION bump must ship its
              migration in the SAME image, because the first window after
              the bump leaves the migration nothing to read. See BUG-5 in
              docs/planning/refactor.bugdiscoveries.md.
           2. "Nothing else is re-applied" is about NVS keys, not about
              running timers. net_apply.c's reconcile_defs() re-installs the
              table at window close; if this branch rewrote it mid-window,
              timer_reconcile_def() may RESET a running slot and chirp.
              Traced, not executed.
           3. The retry is unbounded and silent. If the write keeps failing
              this runs every window forever, always answering
              ok:true,skipped:true. Not a flash-wear problem — a failing
              write does not program, and the healthy path writes exactly
              once — but it is permanently invisible. */
        nvs_timer_defs_blob_t probe;
        if (nvs_config_get_timer_defs(&probe) != ESP_OK) {
            err_acc_t rebuild = {.errors = {0}, .count = 0, .truncated = false};
            apply_timers(root, &rebuild);
        }
        snprintf(ack, ack_len, "{\"ver\":\"%s\",\"ok\":true,\"skipped\":true}", ver_str);
        cJSON_Delete(root);
        return CONFIG_SKIPPED;
    }

    err_acc_t e = {.errors = {0}, .count = 0, .truncated = false};

    apply_str(root, "name", CFG_BOUND_NAME_MAX, NULL, nvs_config_set_dev_name, &e);
    apply_str(root, "tz", CFG_BOUND_TZ_MAX, NULL, nvs_config_set_tz, &e);
    apply_u16(root, "weekday_min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, nvs_config_set_weekday_min, &e);
    apply_u16(root, "weekend_min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, nvs_config_set_weekend_min, &e);
    apply_u16(root, "holiday_min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, nvs_config_set_holiday_min, &e);
    apply_u16(root, "summer_min", CFG_BOUND_ALLOC_LO, CFG_BOUND_ALLOC_HI, nvs_config_set_summer_min, &e);
    apply_chore_free(root, &e);
    apply_hhmm(root, "quiet_start", quiet_hhmm_valid, nvs_config_set_quiet_start, &e);
    apply_hhmm(root, "quiet_end", quiet_hhmm_valid, nvs_config_set_quiet_end, &e);
    apply_hhmm(root, "bedtime", bedtime_hhmm_valid, nvs_config_set_bedtime, &e);
    apply_u16(root, "break_interval_min", CFG_BOUND_BREAK_INT_LO, CFG_BOUND_BREAK_INT_HI,
              nvs_config_set_break_interval_min, &e);
    apply_u16(root, "break_duration_min", CFG_BOUND_BREAK_DUR_LO, CFG_BOUND_BREAK_DUR_HI,
              nvs_config_set_break_duration_min, &e);
    apply_enum(root, "tone_expiry", tones_names, TONE_COUNT, nvs_config_set_tone_expiry, &e);
    apply_enum(root, "tone_break", tones_names, TONE_COUNT, nvs_config_set_tone_break, &e);
    apply_enum(root, "tone_bed", tones_names, TONE_COUNT, nvs_config_set_tone_bed, &e);
    apply_u16(root, "alert_volume", 0, TONES_VOLUME_MAX, nvs_config_set_alert_volume, &e);
    apply_date(root, "summer_start", nvs_config_set_summer_start, &e);
    apply_date(root, "school_start", nvs_config_set_school_start, &e);
    apply_date(root, "school_end", nvs_config_set_school_end, &e);
    /* OTA. These are HA-settable, so they MUST be parsed here as well as
       in ha_config.c's registry — a field with an entity but no
       bulk-document key is silently cleared by every application of the
       retained document. That is exactly how break_eligible was lost
       (BUG-6 in docs/planning/refactor.bugdiscoveries.md); the third
       place is docs/home_assistant.md. Absent = unchanged, so a document
       that predates OTA leaves an HA-set endpoint alone.
       config_is_ota_url is the shared rule: empty (OTA off) or https —
       plain http would make the update channel unauthenticated. */
    apply_str(root, "ota_url", CFG_BOUND_OTA_URL_MAX, config_is_ota_url, nvs_config_set_ota_url, &e);
    apply_bool(root, "ota_on_sync", nvs_config_set_ota_on_sync, &e);
    apply_holidays(root, &e);
    apply_chores(root, &e);
    apply_timers(root, &e);
    /* Called last because that is the simplest place that satisfies the
       ordering rule, which is NARROWER than "last": after the four
       allocations and the four chore_free_* above, and nothing else.
       Directly after apply_chore_free() would also be correct. See
       check_chore_free_pairs(). */
    check_chore_free_pairs(root, &e);

    /* Record the version last: a power cut mid-apply leaves cfg_ver stale,
       so the (idempotent) document simply re-applies next window. */
    nvs_config_set_cfg_ver(ver_str);

    if (e.count > 0 || e.truncated) {
        /* errors_truncated says "there were more" — without it a dropped
           entry is indistinguishable from a field that applied cleanly. */
        snprintf(ack, ack_len, "{\"ver\":\"%s\",\"ok\":false,\"errors\":[%s]%s}", ver_str, e.errors,
                 e.truncated ? ",\"errors_truncated\":true" : "");
    } else {
        snprintf(ack, ack_len, "{\"ver\":\"%s\",\"ok\":true}", ver_str);
    }
    cJSON_Delete(root);
    return CONFIG_APPLIED;
}
