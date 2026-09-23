/* The chore checklist's NVS records: the name list and the day-stamped
   ack record. Layer 3 (driver) — see include/chore_store.h for the
   contracts, the layout rules and why this file never reads a clock. */
#include "chore_store.h"

#include <string.h>

#include "hal_nvs.h"
#ifndef NATIVE
#include "nvs.h" /* ESP_ERR_NVS_NOT_FOUND */
#endif
#include "nvs_keys.h"

#ifndef NATIVE
#include "esp_log.h"
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

static const char *TAG = "chore_store";

/* The CALLER's `today` is usable only if it is a NUL-terminated string of
   exactly CHORE_DATE_LEN characters. The FORMAT is deliberately not
   validated: the only thing this file does with a date is compare it for
   equality, so a well-formed-but-nonsensical date simply never matches and
   reads as a day rollover — the safe direction. What the length check
   buys is the case equality alone cannot survive: a `today` that is not a
   date at all, which would be stamped into a saved record and then never
   match again, or on the load side would compare unequal against a
   perfectly good record and read as a rollover that never happened.

   This reads up to today[CHORE_DATE_LEN], so `today` must be terminated
   within CHORE_DATE_LEN + 1 bytes — the header states that as a
   precondition, because nothing here can see the caller's buffer size.
   Stored dates are NOT checked with this; see chore_store_load_ack(). */
static bool today_len_ok(const char *today) {
    return strnlen(today, CHORE_DATE_LEN + 1) == CHORE_DATE_LEN;
}

esp_err_t chore_store_load_names(char names[][CHORE_NAME_BUF], uint8_t *n_out) {
    /* Written before anything can fail, so every error path leaves the
       caller with the inert "no chores configured" default (C1) rather
       than whatever was on its stack. */
    memset(names, 0, (size_t)CHORE_MAX * CHORE_NAME_BUF);
    *n_out = 0;

    /* Read into raw and require the returned length to match exactly, so
       the struct is only materialised once the length is right and a
       short blob is never partially consumed. An OVERSIZED stored blob
       does not reach the length test at all — nvs_get_blob refuses to
       write a value that does not fit and returns
       ESP_ERR_NVS_INVALID_LENGTH — but that is a verdict on the stored
       BYTES exactly like a short blob, so it joins the same rejection
       below and both shapes are rejected identically. */
    uint8_t raw[sizeof(nvs_chore_names_blob_t)];
    size_t len = sizeof(raw);
    esp_err_t ret = hal_nvs_read_blob(NVS_KEY_CHORES, raw, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ret; /* never configured — not an error, just the default */
    }
    /* Any other error is the READ failing, not the record: the list is
       UNKNOWN, which is a different answer from "rejected" and is passed
       through unchanged so a caller can tell them apart (see
       chore_store_names_known() in the header). */
    if (ret != ESP_OK && ret != ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "chores blob unreadable: ret=%d", (int)ret);
        return ret;
    }
    if (ret != ESP_OK || len != sizeof(nvs_chore_names_blob_t)) {
        ESP_LOGW(TAG, "chores blob rejected: ret=%d len=%u", (int)ret, (unsigned)len);
        return ESP_ERR_INVALID_VERSION;
    }

    nvs_chore_names_blob_t blob;
    memcpy(&blob, raw, sizeof(blob));
    /* The count is checked here and not clamped: a count the hardware
       cannot have means the bytes are not what this firmware wrote, and
       the rows behind it are no more trustworthy than the count. */
    if (blob.version != CHORE_NAMES_BLOB_VERSION || blob.n > CHORE_MAX) {
        ESP_LOGW(TAG, "chores blob rejected: ver=%u n=%u", (unsigned)blob.version, (unsigned)blob.n);
        return ESP_ERR_INVALID_VERSION;
    }

    for (uint8_t i = 0; i < CHORE_MAX && i < blob.n; i++) {
        /* CHORE_NAME_MAX bytes, then a terminator this loader supplies:
           a stored row that filled all 20 bytes has no NUL of its own,
           and the 21st byte of the destination exists for exactly this. */
        memcpy(names[i], blob.names[i], CHORE_NAME_MAX);
        names[i][CHORE_NAME_MAX] = '\0';
    }
    *n_out = blob.n;
    return ESP_OK;
}

esp_err_t chore_store_save_names(const char names[][CHORE_NAME_BUF], uint8_t n) {
    if (n > CHORE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_chore_names_blob_t blob;
    /* The whole struct, before any field: the reserve bytes reading 0 is
       what lets a future field consume them without a version bump. */
    memset(&blob, 0, sizeof(blob));
    blob.version = CHORE_NAMES_BLOB_VERSION;
    blob.n = n;
    for (uint8_t i = 0; i < CHORE_MAX && i < n; i++) {
        /* strnlen, not strlen: a source row need not be terminated — the
           same tolerance chores_list_hash() documents. The destination is
           already zeroed, so a short name carries no tail from whatever
           the previous list had there. */
        memcpy(blob.names[i], names[i], strnlen(names[i], CHORE_NAME_MAX));
    }
    return hal_nvs_write_blob(NVS_KEY_CHORES, &blob, sizeof(blob));
}

esp_err_t chore_store_load_ack(const char *today, uint16_t current_hash, chore_ack_t *out) {
    /* Same rule as the names loader: a cleared day on every failure path,
       so an ignored return code cannot leave a caller on stack garbage.
       Clearing is NOT a safe default in itself — against C14 a lost
       re-ack IS the harm — and the only reason it is tolerable here is
       that nothing below writes: the record in flash is untouched on
       every one of these paths, so a caller that DEFERS on the error can
       always load again and get the real answer. A caller that instead
       saves on top of the cleared output destroys the record. The header
       states that as the contract; see WHERE `today` MUST COME FROM. */
    out->acked = 0;
    out->released = false;
    if (!today_len_ok(today)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t raw[sizeof(nvs_chore_ack_blob_t)];
    size_t len = sizeof(raw);
    esp_err_t ret = hal_nvs_read_blob(NVS_KEY_CHORE_ACK, raw, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ret; /* nothing acked yet today, or ever */
    }
    if (ret != ESP_OK || len != sizeof(nvs_chore_ack_blob_t)) {
        ESP_LOGW(TAG, "chore_ack blob rejected: ret=%d len=%u", (int)ret, (unsigned)len);
        return ESP_ERR_INVALID_VERSION;
    }

    nvs_chore_ack_blob_t rec;
    memcpy(&rec, raw, sizeof(rec));
    if (rec.version != CHORE_ACK_BLOB_VERSION) {
        ESP_LOGW(TAG, "chore_ack blob rejected: ver=%u", (unsigned)rec.version);
        return ESP_ERR_INVALID_VERSION;
    }

    /* The DATE rule (C13), and it is consulted first on purpose: a record
       from another day is not a partially-valid record whose hash might
       rescue it, it is a different day. Both the acks and the release flag
       go with it — a new day starts locked, whatever yesterday achieved.
       The record stays in flash; the next save overwrites it.

       strcmp alone, with no length check on the stored date, and it cannot
       run off the field: `today` is already known to be terminated at
       index CHORE_DATE_LEN, so the comparison differs at or before index
       CHORE_DATE_LEN and stops inside rec.date's 11 bytes even when the
       stored field holds no NUL at all. A stored date that is short, long
       or unterminated therefore just compares unequal and reads as a day
       rollover, which is the answer a length check would have given too.
       A guard here was tried and removed: it changed no outcome. */
    if (strcmp(rec.date, today) != 0) {
        return ESP_OK;
    }

    /* The HASH rule (C10), delegated rather than restated: it clears the
       acks but PRESERVES `released`, because a list edit must never
       re-lock a day that has already released. */
    const chore_ack_t stored = {.acked = rec.acked, .released = rec.released != 0};
    *out = chores_reconcile(stored, rec.list_hash, current_hash);
    return ESP_OK;
}

esp_err_t chore_store_save_ack(const char *today, uint16_t list_hash, chore_ack_t ack) {
    if (!today_len_ok(today)) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_chore_ack_blob_t rec;
    memset(&rec, 0, sizeof(rec)); /* the date's NUL, and its unused tail */
    rec.version = CHORE_ACK_BLOB_VERSION;
    memcpy(rec.date, today, CHORE_DATE_LEN);
    rec.list_hash = list_hash;
    rec.acked = ack.acked;
    rec.released = ack.released ? 1 : 0;
    /* Unconditional: no read-back, no comparison. A handful of writes a
       day does not need timer_persist's flash-wear care, and buying it
       would cost a flash read on every button press. */
    return hal_nvs_write_blob(NVS_KEY_CHORE_ACK, &rec, sizeof(rec));
}
