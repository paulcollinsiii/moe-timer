#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chores.h"
#include "esp_compat.h"
#ifndef NATIVE
#include "nvs.h" /* ESP_ERR_NVS_NOT_FOUND, for chore_store_names_known() */
#endif

/* Durable storage for the chore checklist: the configured NAMES and the
   day-stamped ACK RECORD, in two NVS blobs.

   Layer 3 (driver), over hal_nvs. It owns bytes and nothing else: the
   meaning of an ack mask lives one layer down in chores.c (layer 1, pure)
   and the decision of when to load or store lives one layer up (the wake
   flow). What is here is the record shape, its validation, and the two
   rules that can only be applied at the moment the record comes back out
   of flash.

   IT DOES NOT READ THE CLOCK. "Today" arrives as a `const char *today` in
   YYYY-MM-DD, never from time() or hal_time, for two reasons: the date
   comparison is then a total function of its arguments and every rollover
   case is host-testable without moving a clock, and the policy question
   "which local day is it?" stays with the caller that already answers it
   for the timer (timer_current_date()), instead of being answered a
   second time, differently, down here.

   WHY FLASH AND NOT RTC MEMORY (design row C14): power cycle, panic and
   OTA reboot must all leave today's acks standing — a kid must never redo
   a chore because the firmware updated. RTC_DATA_ATTR survives deep sleep
   ONLY; an esp_restart() zeroes every RTC variable, so the one event this
   record exists for is exactly the one RTC memory cannot survive.

   FLASH WEAR is deliberately not managed here. timer_persist.c reads the
   stored blob back and skips the write when nothing changed, because it
   saves on every wake; this record is written on a button toggle, a
   handful of times a day, so the same care would buy nothing and cost a
   read per press. Do not add change-detection here. */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the names blob (NVS key "chores") --------------------------------- */

/* Written by config_apply from the HA config document; read by the wake
   flow and by whatever renders the list. Version-stamped and
   length-checked exactly like nvs_timer_defs_blob_t, and for the same
   reason: a blob written by a different firmware layout must read as
   ABSENT (fall back to "no chores configured", the inert C1 default)
   rather than be reinterpreted field by field.

   Bump the version ONLY for a change that moves or reinterprets an
   existing byte. A new field that consumes `rsvd` keeps every offset and
   every existing blob valid, and must NOT bump — a bump makes
   chore_store_load_names() reject every blob in the field, which throws
   away the very list it is protecting. That is safe only because
   chore_store_save_names() memsets the whole struct before filling it, so
   an untouched reserve byte reads 0 on an old blob; any future writer MUST
   memset too or the reserve stops being trustworthy. */
#define CHORE_NAMES_BLOB_VERSION 1
typedef struct {
    uint8_t version;
    /* Stored, not derived from the non-empty rows: the count is what
       chores.c bounds every ack bit by, and "3 chores, the middle one
       named empty" has to stay distinguishable from "1 chore". */
    uint8_t n;
    /* Reserve, and honestly labelled: unlike nvs_timer_defs_blob_t's
       rsvd[3] this is NOT recovered implicit padding — every member here
       is a byte, so the struct has alignment 1 and no holes to recover.
       It is two bytes of deliberate headroom that keep the header 4 bytes
       wide like the timer blob's, so a future flag can be added without
       moving `names` or changing sizeof. Named so nothing can hide in it. */
    uint8_t rsvd[2];
    char names[CHORE_MAX][CHORE_NAME_BUF];
} nvs_chore_names_blob_t;
/* Measured, not assumed. The sizeof assert alone would not be enough: the
   point of asserting EVERY offset is that the next field must either grow
   the struct (caught by sizeof) or visibly consume `rsvd` — an edit that
   sits directly under CHORE_NAMES_BLOB_VERSION where the bump rule is. */
_Static_assert(sizeof(nvs_chore_names_blob_t) == 67, "layout grew: migrate or bump CHORE_NAMES_BLOB_VERSION");
_Static_assert(offsetof(nvs_chore_names_blob_t, version) == 0, "fields reordered");
_Static_assert(offsetof(nvs_chore_names_blob_t, n) == 1, "fields reordered");
_Static_assert(offsetof(nvs_chore_names_blob_t, rsvd) == 2, "fields reordered");
_Static_assert(offsetof(nvs_chore_names_blob_t, names) == 4, "fields reordered");
_Static_assert(1 + 1 + 2 + CHORE_MAX * CHORE_NAME_BUF == sizeof(nvs_chore_names_blob_t),
               "implicit padding appeared: a new field could hide in it");

/* ---- the ack record (NVS key "chore_ack") ------------------------------- */

/* Design §5.4 classifies this one as STATE, like ota_result: device-owned,
   not in the settings registry and not in the bulk config document. The
   names blob above is the opposite — document-only, no accessor pair, no
   HA entity of its own. Neither gets one here.

   `date` is the day stamp the whole record hangs on and `list_hash` is
   chores_list_hash() of the list the mask was acked against. Both are
   stored rather than inferred because both questions ("is this still
   today?", "is this still the same list?") have to be answerable from the
   record alone, on a boot that has no memory of either. */
/* THE VERSION BYTE IS FREE, and the reserve is what it would have cost.
   The design's verbatim shape — date[11], list_hash, acked, released — is
   16 bytes with one byte of implicit padding at offset 11, because
   list_hash has to land on an even offset. Putting `version` at offset 0
   pushes `date` into 1..11 and fills that hole exactly: still 16 bytes,
   now with no padding anywhere. Measured with offsetof, not reasoned
   about — the asserts below are that measurement.

   And there is NO reserve here, which is the one deliberate difference
   from nvs_timer_defs_blob_t and nvs_chore_names_blob_t above. A reserve
   buys the right to add a field without a version bump, and what a bump
   costs is whatever the blob holds. Those two are CONFIG pushed down from
   HA, so a bump loses the table until HA pushes it again. This one is
   DEVICE STATE rewritten several times a day: a bump costs at most the
   acks of the single day it lands on, and the next button press writes a
   fresh record. Four bytes of struct to insure one day's acks is not a
   trade worth making. Bump the version freely; do not add a reserve. */
#define CHORE_ACK_BLOB_VERSION 1
typedef struct {
    uint8_t version;
    char date[11];      /* "YYYY-MM-DD" + NUL; "" = never written */
    uint16_t list_hash; /* even offset by construction — the reason version goes first */
    uint8_t acked;      /* chore_ack_t.acked: bit i = chore i acked */
    uint8_t released;   /* chore_ack_t.released, widened to a byte for a fixed layout */
} nvs_chore_ack_blob_t;
_Static_assert(sizeof(nvs_chore_ack_blob_t) == 16, "layout grew: migrate or bump CHORE_ACK_BLOB_VERSION");
_Static_assert(offsetof(nvs_chore_ack_blob_t, version) == 0, "fields reordered");
_Static_assert(offsetof(nvs_chore_ack_blob_t, date) == 1, "fields reordered");
_Static_assert(offsetof(nvs_chore_ack_blob_t, list_hash) == 12, "fields reordered");
_Static_assert(offsetof(nvs_chore_ack_blob_t, acked) == 14, "fields reordered");
_Static_assert(offsetof(nvs_chore_ack_blob_t, released) == 15, "fields reordered");
_Static_assert(1 + 11 + 2 + 1 + 1 == sizeof(nvs_chore_ack_blob_t),
               "implicit padding appeared: a new field could hide in it");

/* The exact length of a YYYY-MM-DD date, not counting its NUL. Every
   `today` argument below must be a NUL-TERMINATED string of exactly this
   many characters — the terminator is part of the contract, not an
   afterthought, because reading it is how these functions tell a
   ten-character date from a longer one. They cannot see your buffer's
   size, so an unterminated `char[CHORE_DATE_LEN]` is a read past its end.
   A string literal, a `char[CHORE_DATE_LEN + 1]`, or timer_current_date()
   all satisfy it. */
#define CHORE_DATE_LEN 10

/* ---- names ------------------------------------------------------------- */

/* Load the configured chore names.

   `names` must point at CHORE_MAX rows of CHORE_NAME_BUF bytes; `n_out`
   receives the configured count, always <= CHORE_MAX. Both are written on
   EVERY path: a missing key, a stale version, a wrong-length blob and an
   out-of-range count all leave `names` all-empty and *n_out == 0, which is
   the inert "no chores configured" default (design row C1) that every
   device in the field is in today. So a caller that ignores the return
   code still gets a safe, fully-initialised answer. The return code
   says which of three things happened:

     ESP_OK / ESP_ERR_NVS_NOT_FOUND  the list, or "never configured";
     ESP_ERR_INVALID_VERSION         the stored bytes were REJECTED (wrong
                                     version, length or count) — as
                                     permanent as the blob, and the whole
                                     device runs on n = 0 until a new list
                                     is written;
     anything else                   the READ itself failed (the raw
                                     hal_nvs error): the list is UNKNOWN.

   Most callers want n = 0 in all three failure cases and ignore the code.
   The one that must not is a caller that PUBLISHES the count somewhere
   that outlives the wake — mqtt_ha.c's discovery pass retires HA entities
   past the count and then stamps the pass as done — and it asks
   chore_store_names_known() below.

   Every returned row is NUL-terminated here, at index CHORE_NAME_MAX if
   the stored row filled all 20 bytes without one. The rows come straight
   back from flash and chores_list_hash() tolerates an unterminated row by
   design, but str* in a caller would not. */
esp_err_t chore_store_load_names(char names[][CHORE_NAME_BUF], uint8_t *n_out);

/* True when chore_store_load_names() returned an AUTHORITATIVE count: the
   list, "never configured", or a rejected blob (which the rest of the
   device also reads as no chores, so n = 0 is the truth about this wake).
   False only when the read failed and the stored list is unknown — act on
   n = 0 then and a transient flash error becomes a durable fact. */
static inline bool chore_store_names_known(esp_err_t load_ret) {
    return load_ret == ESP_OK || load_ret == ESP_ERR_NVS_NOT_FOUND || load_ret == ESP_ERR_INVALID_VERSION;
}

/* Store the configured chore names. Builds the blob itself — memset
   first, then the fields — so the reserve bytes are 0 by construction and
   the bump rule above stays true no matter who calls this.

   n > CHORE_MAX is refused with ESP_ERR_INVALID_SIZE and nothing is
   written: there are only CHORE_MAX rows to store, and silently clamping
   would persist a list that does not match the count the caller believes
   it saved. Rows at and above `n` are stored empty regardless of what the
   caller's array holds there.

   PRECONDITION on `names`, identical to chores_list_hash()'s: min(n,
   CHORE_MAX) readable rows of CHORE_NAME_BUF bytes, and NULL is legal with
   n == 0 and only with n == 0. Unterminated rows are accepted and stored
   truncated to CHORE_NAME_MAX bytes. */
esp_err_t chore_store_save_names(const char names[][CHORE_NAME_BUF], uint8_t n);

/* ---- the day-stamped acks ---------------------------------------------- */

/* Load today's acks, applying the two rules that can only be applied here.

   `today` is a NUL-terminated YYYY-MM-DD date of CHORE_DATE_LEN
   characters (see CHORE_DATE_LEN above, and WHERE `today` MUST COME FROM
   below); `current_hash` is chores_list_hash() of the list as it is NOW.
   `*out` is written on every path, zeroed ({0, false}) whenever nothing
   usable came back, so an ignored return code cannot leave a caller acting
   on stack garbage.

   `out->acked` comes back exactly as it was stored, NOT masked to the
   configured count: a record written when four chores were configured can
   return bits at or above today's `n`. That is correct at this layer —
   masking here would silently destroy acks that a restored list would
   have made meaningful again — and chores.c bounds every bit it reads.
   So read the mask through chores_is_acked(mask, i, n) and never test a
   bit raw; a renderer that walked the bits directly would draw ticks for
   chores that are not on the list.

   The two rules, and the asymmetry between them is the whole point:

     - stored date != today (row C13, day rollover): acks AND the release
       flag are both stale. A new day starts locked, with nothing acked.
     - stored date == today, hash mismatch (row C10, the list was edited):
       chores_reconcile() clears the acks, because positional bits stop
       meaning anything once the list moves, but PRESERVES `released` — a
       list edit must never re-lock a day that has already released.

   Note which one is consulted first: the date. A record from last week
   whose hash still matches is not a partially-valid record, it is a
   different day.

   The reconcile half is delegated to chores_reconcile() rather than
   restated here; the rule belongs to layer 1 and a second copy of it would
   be a second thing to get wrong.

   WHERE `today` MUST COME FROM — and this is the C14 hazard, on the one
   row this record exists for ("a kid must never redo a chore because the
   firmware updated"):

   timer_current_date() reads g_rtc_state.last_date, which is
   RTC_DATA_ATTR. RTC memory survives deep sleep but NOT esp_restart(), so
   on the wake after an OTA reboot — before timer_persist_try_restore()
   has put the date back — timer_current_date() returns "". "" is not a
   date. Hand it to this function and you get ESP_ERR_INVALID_SIZE with
   *out cleared.

   The cleared output is not the harm, and the difference matters: THE
   RECORD IN FLASH IS UNTOUCHED. This function only ever reads NVS, and
   the date check returns before even the read, so a failed load cannot
   mutate anything. The harm is one step further on — read the cleared
   output as "nothing acked today" and then chore_store_save_ack() on top
   of it, and a good record is overwritten with an empty one. Today's acks
   are then genuinely lost to a reboot, which is exactly the outcome C14
   forbids.

   So ESP_ERR_INVALID_SIZE means DEFER, not "nothing acked": do not treat
   the cleared output as authoritative, and above all do not save over it.
   Wait for a date you trust and load again. Because the record is
   untouched, deferring is always safe and costs nothing but a wake.

   This is a clock-trust question and the codebase already has the notion
   — ota_policy.h's ota_gate_in_t carries a `bool time_valid` field,
   documented there as "NTP has set the clock this session", and the OTA
   path gates on it for exactly this reason. A caller that knows the clock
   is trustworthy this session has a real date to pass; one that does not
   has nothing to pass but "", and should not call. */
esp_err_t chore_store_load_ack(const char *today, uint16_t current_hash, chore_ack_t *out);

/* Store today's acks, stamped with `today` and `list_hash`.

   Called on every ack toggle and on the release latch. Unconditional: it
   does not read the stored blob first and does not compare — see the flash
   note at the top of this file.

   `today` must be a NUL-terminated CHORE_DATE_LEN-character date or the
   call is refused with ESP_ERR_INVALID_SIZE and nothing is written; a
   short or overlong date would be stamped into the record and then either
   never match again (the acks are lost every wake) or match the wrong
   day. memsets the whole struct before filling it, like the names writer.

   The refusal is a backstop, not the contract. Never reach this function
   with a date the load side already refused — see WHERE `today` MUST COME
   FROM above: saving on top of a deferred load is the one sequence that
   can destroy a good record. */
esp_err_t chore_store_save_ack(const char *today, uint16_t list_hash, chore_ack_t ack);

#ifdef __cplusplus
}
#endif
