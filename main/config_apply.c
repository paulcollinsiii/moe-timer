/* Apply an HA config document to NVS. Pure over nvs_config and the shared
   validators, plus one read of the compile-time timer table
   (timer_defs_compiled) for the bottom rung of apply_timers()' optional-key
   ladder. Host-tested. */
#include "config_apply.h"

#include <stdio.h>
#include <string.h>

#include "bedtime.h" /* bedtime_hhmm_valid */
#include "cJSON.h"
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
    apply_timers(root, &e);

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
