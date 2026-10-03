/* Panic forensics. include/panic_diag.h carries the whole argument —
   why the counter exists, why RTC_NOINIT_ATTR (and why the project note
   against it does not apply here), why the breadcrumb is ALSO copied to
   NVS, and why there are two phase slots rather than one. This file is
   the mechanism.

   Split the way ota_facts.c / ota_flow.c are split: everything that is a
   RULE (the phase table, the slot map, the label, the record guard and
   the record state machine) is pure and host-tested in test_panic_diag;
   everything that reads a clock, a heap, a stack or flash sits below the
   NATIVE fence and is exercised on hardware only. The pure half operates
   on a record PASSED IN, which is what lets the suite drive the whole
   state machine without a scheduler. */
#include "panic_diag.h"

#include <stddef.h>
#include <stdio.h> /* snprintf — the label builder */
#include <string.h>

/* ---- pure layer --------------------------------------------------------- */

/* ---- the phase table, stated once --------------------------------------

   Three things have to agree about every phase: the NAME it publishes,
   the SLOT it owns, and how much of the joined label it can consume.
   They used to be three hand-maintained lists — two switches and a
   _Static_assert over a pair of hardcoded literals — and it was the
   assert that gave way. It compared sizeof("RENDER") + sizeof("OTA_CHECK")
   and so could not see a LONGER phase name being added at all, which is
   exactly what the BOOT_* subdivision is. Adding those phases against a
   guard blind to them would have been adding them against no guard, so
   the lists were collapsed into this table first.

   The enum itself stays hand-written in panic_diag.h rather than being
   generated from here. That is deliberate, and it is the only real cost
   of the arrangement: the header is where the stored-value argument and
   the per-phase notes live, and an enum spelled as a macro invocation
   would hide the numbering that the whole append-don't-renumber rule is
   about. A new enumerator can therefore be forgotten HERE — which is why
   the row count is asserted against PANIC_PHASE__COUNT below, and why
   test_every_phase_has_a_distinct_name catches a missing row as two
   phases both answering "?".

   Columns: enumerator, published label, does it own the net slot. */
#define PANIC_PHASE_TABLE(X)                     \
    X(PANIC_PHASE_NONE, "NONE", false)           \
    X(PANIC_PHASE_BOOT, "BOOT", false)           \
    X(PANIC_PHASE_AWAKE, "AWAKE", false)         \
    X(PANIC_PHASE_RENDER, "RENDER", false)       \
    X(PANIC_PHASE_SLEEP, "SLEEP", false)         \
    X(PANIC_PHASE_NET, "NET", true)              \
    X(PANIC_PHASE_OTA_CHECK, "OTA_CHECK", true)  \
    X(PANIC_PHASE_MQTT, "MQTT", true)            \
    X(PANIC_PHASE_OTA_DL, "OTA_DL", true)        \
    X(PANIC_PHASE_BOOT_NVS, "BOOT_NVS", false)   \
    X(PANIC_PHASE_BOOT_OTA, "BOOT_OTA", false)   \
    X(PANIC_PHASE_BOOT_DISP, "BOOT_DISP", false) \
    X(PANIC_PHASE_BOOT_BATT, "BOOT_BATT", false) \
    X(PANIC_PHASE_BOOT_LOCK, "BOOT_LOCK", false) \
    X(PANIC_PHASE_BOOT_TMR, "BOOT_TMR", false)

#define PANIC_PHASE_X_STR(sym, str, is_net) \
    case sym:                               \
        return str;

#define PANIC_PHASE_X_IS_NET(sym, str, is_net) \
    case sym:                                  \
        return is_net;

/* ALLOWLIST CANDIDATE, and deliberately not one: unlike
   ota_policy_reason_str() this is never called from inside an ESP_LOGx
   argument list (standing rule R3 — a call there stops executing once
   the level is compiled out). Every log site below hoists it to a local
   first. Keep it that way rather than adding a line to
   scripts/check-log-args.py. */
const char *panic_diag_phase_str(panic_phase_t phase) {
    switch (phase) {
        PANIC_PHASE_TABLE(PANIC_PHASE_X_STR)
        case PANIC_PHASE__COUNT:
        default:
            /* A stored value this image does not know. Reported as
               unknown rather than folded into NONE, because "the record
               is from another firmware" and "nothing was running" are
               different findings. */
            return "?";
    }
}

bool panic_diag_phase_is_net(panic_phase_t phase) {
    switch (phase) {
        PANIC_PHASE_TABLE(PANIC_PHASE_X_IS_NET)
        case PANIC_PHASE__COUNT:
        default:
            /* Out of range answers "main slot", the same as NONE. Every
               BOOT_* phase answers false through the table above, and
               that is not a formality: they are marked from the main
               task inside app_main, and a boot phase that landed in the
               net slot would overwrite whatever a window opened from
               INSIDE the boot had put there — which
               lock_gate_check_charge() does. */
            return false;
    }
}

int panic_diag_phase_label(char *buf, size_t len, uint8_t main_phase, uint8_t net_phase) {
    const char *m = panic_diag_phase_str((panic_phase_t)main_phase);
    const char *n = panic_diag_phase_str((panic_phase_t)net_phase);
    const bool have_m = (main_phase != PANIC_PHASE_NONE);
    const bool have_n = (net_phase != PANIC_PHASE_NONE);
    if (buf == NULL || len == 0)
        return 0;
    if (have_m && have_n)
        return snprintf(buf, len, "%s+%s", m, n);
    if (have_n)
        return snprintf(buf, len, "%s", n);
    /* Main-only, and also the both-NONE case: "NONE" is a real reading —
       it says a panic was recorded but neither side was inside a marked
       phase, which points at the unmarked gaps (the ISR/timer tasks, the
       wifi driver's own threads) rather than at any of the phases. */
    return snprintf(buf, len, "%s", m);
}

/* Nine characters per label, and the arithmetic that produces the nine.
   The published field holds main + '+' + net + NUL, so the two labels
   share DIAG_PHASE_MAX - 2 characters between them; an even split gives
   each slot (DIAG_PHASE_MAX - 2) / 2 = 9.

   Checked PER ROW, against the same table the labels themselves come
   from. What stood here before was
   `DIAG_PHASE_MAX >= sizeof("RENDER") + sizeof("OTA_CHECK")` — two
   literals that were the worst case on the day they were written and
   stopped being it the moment a longer name was appended, which is
   exactly what the BOOT_* subdivision did. A guard that cannot see the
   thing it guards against is decoration. This one fires on the row that
   broke it and names the phase in the message, so "BOOT_DISPLAY" is
   rejected at the point where it can still be shortened to "BOOT_DISP".

   SUFFICIENT, NOT EXACT, and that is a deliberate trade. An even split
   rejects a 10-character main-slot label even in a world where every
   net-slot label is short enough to have paid for it. Today the two
   maxima are 9 and 9 ("BOOT_LOCK" and "OTA_CHECK"), so the budget IS the
   real limit and nothing is being given away; if that ever stops being
   true, the fix is to state the two budgets separately rather than to
   loosen this one. The EXACT statement — every pair the label builder
   can actually produce fits — is
   test_every_phase_pair_fits_the_published_field, which walks the whole
   PANIC_PHASE__COUNT^2 cross-product through the real builder. Keep
   both: this one fails fast and points at the row, that one covers the
   joining logic as well as the arithmetic.

   A fold computing the two maxima here was tried first and abandoned:
   a function-like MAX macro cannot be opened by one X-macro pass and
   closed by another, because the preprocessor requires each invocation's
   argument list to be balanced within a single expansion. The variants
   that do work (bit-mask folds, twenty-branch ternary chains) are harder
   to read than the property they check. */
#define PANIC_PHASE_LABEL_BUDGET ((DIAG_PHASE_MAX - 2) / 2)
#define PANIC_PHASE_X_FITS(sym, str, is_net)                             \
    _Static_assert(sizeof(str) - 1u <= PANIC_PHASE_LABEL_BUDGET,         \
                   "phase label " str                                    \
                   " does not fit DIAG_PHASE_MAX: the "                  \
                   "published field is main + '+' + net + NUL, so each " \
                   "slot's label gets (DIAG_PHASE_MAX - 2) / 2 characters");
PANIC_PHASE_TABLE(PANIC_PHASE_X_FITS)

/* The one thing the table cannot check about itself: that it has a row
   for every enumerator. A missing row makes that phase publish "?" AND
   sit on the main slot regardless of where it belongs — both silent, and
   both visible only on a device that has already panicked. */
#define PANIC_PHASE_X_COUNT(sym, str, is_net) +1
_Static_assert((0 PANIC_PHASE_TABLE(PANIC_PHASE_X_COUNT)) == PANIC_PHASE__COUNT,
               "the phase table in panic_diag.c and panic_phase_t in panic_diag.h "
               "disagree on how many phases there are");

/* Layout guard for the checksum below: it hashes RAW BYTES, so an
   implicit padding hole would put whatever the compiler happened to
   leave there inside the hash — and an uninitialised hole makes a
   perfectly good record fail its own guard at random. */
_Static_assert(sizeof(panic_diag_rec_t) == 24,
               "panic_diag_rec_t has grown padding; the byte-wise checksum assumes a "
               "hole-free layout");
_Static_assert(offsetof(panic_diag_rec_t, sum) == 20,
               "the checksum field must stay LAST — it covers everything "
               "before it");

/* FNV-1a over every byte before `sum`. Not a CRC: there is no polynomial
   requirement here, only "a cold boot's uninitialised RTC RAM must not
   pass". Together with the magic word that is 64 bits of agreement to
   find by accident. Written locally rather than pulled from esp_rom_crc
   so the guard is the same code on host and on device — a guard whose
   host test exercises a different implementation is not a test of the
   guard. */
static uint32_t rec_sum(const panic_diag_rec_t *rec) {
    const uint8_t *p = (const uint8_t *)rec;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(panic_diag_rec_t, sum); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

void panic_diag_sample_reset(panic_sample_t *s) {
    if (s == NULL)
        return;
    memset(s, 0, sizeof(*s));
}

void panic_diag_rec_reset(panic_diag_rec_t *rec) {
    if (rec == NULL)
        return;
    memset(rec, 0, sizeof(*rec));
    rec->magic = PANIC_DIAG_MAGIC;
    rec->sum = rec_sum(rec);
}

bool panic_diag_rec_valid(const panic_diag_rec_t *rec) {
    if (rec == NULL)
        return false;
    return rec->magic == PANIC_DIAG_MAGIC && rec->sum == rec_sum(rec);
}

void panic_diag_rec_mark(panic_diag_rec_t *rec, panic_phase_t slot_of, panic_phase_t value,
                         const panic_sample_t *sample) {
    if (rec == NULL)
        return;
    /* A record that never passed its guard is re-armed rather than
       written into. On device this cannot happen after panic_diag_init()
       — it resets the record on every boot — but the pure function has
       to be total, and re-arming is the only answer that leaves the
       record self-consistent. */
    if (!panic_diag_rec_valid(rec))
        panic_diag_rec_reset(rec);

    if (panic_diag_phase_is_net(slot_of)) {
        rec->net_phase = (uint8_t)value;
        if (sample != NULL && !sample->stack_foreign)
            rec->stack_net = sample->stack_free;
    } else {
        rec->main_phase = (uint8_t)value;
        if (sample != NULL && !sample->stack_foreign)
            rec->stack_main = sample->stack_free;
    }
    if (sample != NULL) {
        /* Uptime and heap are global, so the last mark from EITHER side
           carries them — which is what makes "uptime at panic" mean how
           far into the wake it died rather than how far into a phase. */
        rec->uptime_ms = sample->uptime_ms;
        rec->heap_free = sample->heap_free;
    }
    rec->sum = rec_sum(rec);
}

bool panic_diag_rec_exit(panic_diag_rec_t *rec, panic_phase_t phase, panic_phase_t prev, const panic_sample_t *sample) {
    if (rec == NULL)
        return false;
    /* Same totality argument as panic_diag_rec_mark: a record that never
       passed its guard is re-armed rather than written into. Nothing is
       restored on top of it — the phase it claimed to hold is not
       evidence. */
    if (!panic_diag_rec_valid(rec)) {
        panic_diag_rec_reset(rec);
        return false;
    }
    const uint8_t held = panic_diag_phase_is_net(phase) ? rec->net_phase : rec->main_phase;
    if (held != (uint8_t)phase)
        return false;
    panic_diag_rec_mark(rec, phase, prev, sample);
    return true;
}

void panic_diag_fill_stat(diag_stat_t *out, uint32_t panics, const panic_diag_rec_t *last) {
    if (out == NULL)
        return;
    out->panics = panics;
    if (last == NULL || !panic_diag_rec_valid(last)) {
        /* EMPTY, not "NONE". stats_json.h documents the difference and
           it matters at a glance in Home Assistant: blank means no
           breadcrumb is on file (a device that has never panicked, or
           one whose record failed its guard), whereas "NONE" is a
           positive statement that a panic happened outside every marked
           phase. */
        out->panic_phase[0] = '\0';
        out->panic_uptime_s = 0;
        out->panic_heap = 0;
        out->panic_stack_main = 0;
        out->panic_stack_net = 0;
        return;
    }
    panic_diag_phase_label(out->panic_phase, sizeof(out->panic_phase), last->main_phase, last->net_phase);
    out->panic_uptime_s = last->uptime_ms / 1000u;
    out->panic_heap = last->heap_free;
    out->panic_stack_main = last->stack_main;
    out->panic_stack_net = last->stack_net;
}

/* ---- device layer ------------------------------------------------------- */
#ifndef NATIVE

#include "esp_attr.h" /* RTC_NOINIT_ATTR */
#include "esp_log.h"
#include "esp_system.h" /* esp_reset_reason, esp_get_free_heap_size */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_nvs.h"
#include "nvs.h" /* nvs_get_stats */
#include "nvs_keys.h"

static const char *TAG = "panic_diag";

/* THE breadcrumb. RTC_NOINIT_ATTR, not RTC_DATA_ATTR: the latter is
   zeroed by a panic reset and by esp_restart(), which would blank it on
   exactly the boot that needs to read it. The magic + checksum inside
   the record is what pays for "not initialised on a cold power-on" —
   panic_diag.h has the full argument, including why the project's
   standing note against RTC_NOINIT_ATTR (which is about timer state
   across an OTA reboot) does not carry over. */
static RTC_NOINIT_ATTR panic_diag_rec_t s_rec;

/* The record has more than one writing task — the main task marks BOOT /
   AWAKE / RENDER / SLEEP while net_win or ota_dl marks its own slot — and
   although each SLOT has a single writer, the checksum covers the whole
   struct. Two marks that interleaved would leave a sum computed over a
   half-updated record: the record then fails its own guard and STAYS
   failed until the next mark re-seals it, which on a quiet stretch (the
   25 s render-grid wait, say) is seconds of blindness in the one window
   where a panic would matter most. So the seal is atomic.

   A spinlock rather than a mutex: this runs from the display path, the
   sleep funnel (which the awake failsafe enters from esp_timer context)
   and two network tasks, and it must not be able to block any of them.
   The critical section holds only the field writes and the 20-byte hash;
   the expensive sample is taken outside it. */
static portMUX_TYPE s_rec_mux = portMUX_INITIALIZER_UNLOCKED;

/* This boot's findings, latched by init() before the first mark
   overwrites s_rec, and consumed by commit() once NVS is up. Plain
   statics: boot-scoped by construction. */
static bool s_panicked;
static bool s_latched;
static panic_diag_rec_t s_last;

/* Captured by panic_diag_init(), which runs as app_main's first
   statement — so this is the main task by construction. Used only to
   decide whether a mark on the MAIN slot is allowed to write that slot's
   stack figure; see panic_sample_t::stack_foreign. */
static TaskHandle_t s_main_task;

static uint16_t stack_free_bytes(void) {
    /* ESP-IDF's watermark is in BYTES (net_window.c's stack-floor log
       says the same). Saturating rather than wrapping: a 16-bit field is
       plenty for a stack this side of 64 KB, and a wrapped small number
       would read as an imminent overflow that is not happening. */
    const UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    return (hwm > 65535u) ? 65535u : (uint16_t)hwm;
}

static void sample_now(panic_sample_t *out) {
    /* ZEROED FIRST, and this is load-bearing rather than tidiness. Every
       caller declares the sample as a bare local, this function writes
       three of its four fields, and note_stack_ownership() below writes
       the fourth for MAIN-slot phases only — it returns early for the
       net ones. panic_diag_rec_mark() reads stack_foreign on BOTH sides
       to decide whether the stack figure is recorded, so without this
       line whether stack_net lands is decided by whatever the stack
       happened to hold. Fixed here, at the one point every mark goes
       through, rather than at each declaration: a fifth field added
       later is covered by the same line, and a sixth caller cannot
       forget it. */
    panic_diag_sample_reset(out);
    /* esp_timer_get_time() and NOT esp_system_get_time(): the published
       figure has to mean "how far into THIS wake", and only the former
       does. esp_timer_impl_get_time() reads the systimer, which sits in
       the digital domain and therefore restarts at zero on a deep-sleep
       wake and on a panic reset alike; the RTC-derived correction that
       makes time continuous across sleeps is added by
       esp_system_get_time() (esp_timer/src/system_time.c), which is what
       gettimeofday goes through. Truncating to 32 bits wraps at 49.7
       days, four orders of magnitude past the awake failsafe's cap
       (CONFIG_MAGTAG_MAX_AWAKE_SEC), so a wake cannot reach it. */
    out->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    out->heap_free = esp_get_free_heap_size();
    out->stack_free = stack_free_bytes();
}

/* Does the marking task own the slot this phase selects? The net slot is
   shared by every network-window task on purpose, so only the main slot
   is checked. Before init has run there is nothing to compare against,
   which reads as "owned" — the only mark in that window is init's own,
   on the main task.

   The early return for a net phase writes NOTHING, and that stays
   correct only because sample_now() zeroed the field before calling
   here: "owned" is the right reading for a slot with no single owner —
   the window task's own high-water mark IS the net figure — and false is
   what zeroing already left there. Do not turn the early return into a
   write; do not remove the zeroing. */
static void note_stack_ownership(panic_sample_t *s, panic_phase_t phase) {
    if (panic_diag_phase_is_net(phase))
        return;
    s->stack_foreign = (s_main_task != NULL) && (xTaskGetCurrentTaskHandle() != s_main_task);
}

void panic_diag_init(void) {
    s_main_task = xTaskGetCurrentTaskHandle();
    s_panicked = (esp_reset_reason() == ESP_RST_PANIC);
    /* The copy has to happen BEFORE the reset below, and the reset has
       to happen before the first mark — otherwise this boot's BOOT mark
       would overwrite the evidence it is meant to preserve. */
    s_latched = s_panicked && panic_diag_rec_valid(&s_rec);
    if (s_latched)
        s_last = s_rec;
    panic_diag_rec_reset(&s_rec);
    (void)panic_diag_enter(PANIC_PHASE_BOOT);
}

void panic_diag_commit(void) {
    if (!s_panicked)
        return;

    /* Monotonic and never reset by this firmware: the whole point is
       that HA can difference it across any two publishes. A read failure
       leaves 0, which restarts the count rather than skipping it — the
       lesser evil, and visible as a step DOWN in HA rather than as
       silence. */
    uint32_t n = 0;
    (void)hal_nvs_read_u32(NVS_KEY_PANIC_CNT, &n);
    n++;
    (void)hal_nvs_write_u32(NVS_KEY_PANIC_CNT, n);

    if (s_latched) {
        (void)hal_nvs_write_blob(NVS_KEY_PANIC_REC, &s_last, sizeof(s_last));
    } else {
        /* Panicked, but the breadcrumb did not survive its guard (a
           first boot on this firmware, or genuinely corrupt RTC RAM).
           The stale record from an OLDER panic is cleared to a valid
           empty one rather than left in place: leaving it would pair
           this boot's counter increment with a different panic's phase,
           which is the one way this feature could actively mislead. */
        panic_diag_rec_t blank;
        panic_diag_rec_reset(&blank);
        (void)hal_nvs_write_blob(NVS_KEY_PANIC_REC, &blank, sizeof(blank));
    }

    /* R3: every one of these is a local. Nothing but variables inside
       the ESP_LOGx argument list. */
    char label[DIAG_PHASE_MAX];
    panic_diag_phase_label(label, sizeof(label), s_last.main_phase, s_last.net_phase);
    const unsigned long up_ms = (unsigned long)s_last.uptime_ms;
    const unsigned long heap = (unsigned long)s_last.heap_free;
    const unsigned smain = (unsigned)s_last.stack_main;
    const unsigned snet = (unsigned)s_last.stack_net;
    const unsigned long count = (unsigned long)n;
    if (s_latched) {
        ESP_LOGE(TAG, "PANIC #%lu: phase %s, %lu ms into the wake, heap %lu B, stack main %u B / net %u B", count,
                 label, up_ms, heap, smain, snet);
    } else {
        ESP_LOGE(TAG, "PANIC #%lu: no usable breadcrumb (RTC record failed its guard)", count);
    }
}

panic_phase_t panic_diag_enter(panic_phase_t phase) {
    panic_sample_t s;
    /* Sampled OUTSIDE the lock deliberately: uxTaskGetStackHighWaterMark
       walks the whole stack looking for the fill pattern, which is
       thousands of cycles, and holding interrupts off for that on a
       device with a 16 kHz audio DAC and an e-ink BUSY wait would be a
       real cost for a diagnostic. */
    sample_now(&s);
    note_stack_ownership(&s, phase);
    portENTER_CRITICAL(&s_rec_mux);
    const panic_phase_t prev =
        panic_diag_phase_is_net(phase) ? (panic_phase_t)s_rec.net_phase : (panic_phase_t)s_rec.main_phase;
    panic_diag_rec_mark(&s_rec, phase, phase, &s);
    portEXIT_CRITICAL(&s_rec_mux);
    return prev;
}

void panic_diag_exit(panic_phase_t phase, panic_phase_t prev) {
    panic_sample_t s;
    sample_now(&s);
    note_stack_ownership(&s, phase);
    portENTER_CRITICAL(&s_rec_mux);
    /* Conditional restore, and the return is discarded on purpose: a
       refusal means a third task moved this slot while we were inside
       the phase (the awake failsafe is the one that does it), and that
       task's reading is the one worth keeping. panic_diag_rec_exit
       carries the argument. */
    (void)panic_diag_rec_exit(&s_rec, phase, prev, &s);
    portEXIT_CRITICAL(&s_rec_mux);
}

void panic_diag_stat(diag_stat_t *out) {
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));

    uint32_t panics = 0;
    (void)hal_nvs_read_u32(NVS_KEY_PANIC_CNT, &panics);

    panic_diag_rec_t last;
    size_t len = sizeof(last);
    const bool have = (hal_nvs_read_blob(NVS_KEY_PANIC_REC, &last, &len) == ESP_OK) && len == sizeof(last) &&
                      panic_diag_rec_valid(&last);
    panic_diag_fill_stat(out, panics, have ? &last : NULL);

    /* ---- the live half.

       heap_free / heap_min are globals, so they read the same from any
       task. The two stack numbers are not, and they are gathered
       differently on purpose:

         stack_net  a FRESH read, because this function runs ON the
                    net_win task (mqtt_ha.c's publish_states, mid-window)
                    — so uxTaskGetStackHighWaterMark(NULL) is that task's
                    own floor, after wifi association, SNTP and the
                    update check have all been through it. That is the
                    most pessimistic number available and it is free.

         stack_main comes out of the LIVE breadcrumb, where the main task
                    left it at its own last mark. It cannot be read here
                    — a high-water mark belongs to one task and there is
                    no handle for the main task at this layer — and the
                    breadcrumb already samples it on every mark, so no
                    extra plumbing through the snapshot queue is needed.

       One consequence worth stating: on the failsafe path enter_deep_sleep
       runs from the esp_timer task, so the SLEEP mark samples that task's
       stack into stack_main. That value is never published live (the
       window has closed by then) and can only reach HA via a panic latch,
       where the phase label reads SLEEP and says so. */
    out->heap_free = esp_get_free_heap_size();
    out->heap_min = esp_get_minimum_free_heap_size();
    out->stack_net = stack_free_bytes();
    out->stack_main = s_rec.stack_main;

    /* NVS headroom, measured rather than assumed — main.c's
       nvs_flash_erase() on ESP_ERR_NVS_NO_FREE_PAGES silently wipes every
       stored setting, the owner's WiFi and MQTT credentials included, and
       nothing has ever reported how close this device is to that. Only
       setup mode (entered because the device wakes with no SSID) gets
       credentials back in; everything else restores from the normal reseed.
       free_entries is the whole answer: total is a constant
       of a partition table that is frozen for OTA'd devices, and used is
       total - free, so publishing either would be publishing the same
       fact twice. A failed call leaves 0, which reads as "no headroom"
       — the alarming direction, which is the right way for a
       diagnostic to fail. */
    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK) {
        out->nvs_free = (ns.free_entries > 65535u) ? 65535u : (uint16_t)ns.free_entries;
    }
}

#endif /* !NATIVE */
