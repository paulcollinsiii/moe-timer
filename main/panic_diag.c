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

/* ALLOWLIST CANDIDATE, and deliberately not one: unlike
   ota_policy_reason_str() this is never called from inside an ESP_LOGx
   argument list (standing rule R3 — a call there stops executing once
   the level is compiled out). Every log site below hoists it to a local
   first. Keep it that way rather than adding a line to
   scripts/check-log-args.py. */
const char *panic_diag_phase_str(panic_phase_t phase) {
    switch (phase) {
        case PANIC_PHASE_NONE:
            return "NONE";
        case PANIC_PHASE_BOOT:
            return "BOOT";
        case PANIC_PHASE_AWAKE:
            return "AWAKE";
        case PANIC_PHASE_RENDER:
            return "RENDER";
        case PANIC_PHASE_SLEEP:
            return "SLEEP";
        case PANIC_PHASE_NET:
            return "NET";
        case PANIC_PHASE_OTA_CHECK:
            return "OTA_CHECK";
        case PANIC_PHASE_MQTT:
            return "MQTT";
        case PANIC_PHASE_OTA_DL:
            return "OTA_DL";
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
        case PANIC_PHASE_NET:
        case PANIC_PHASE_OTA_CHECK:
        case PANIC_PHASE_MQTT:
        case PANIC_PHASE_OTA_DL:
            return true;
        default:
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

/* The longest label the table above can produce, pinned against the
   published field's width rather than against a comment. "RENDER" is the
   longest main phase and "OTA_CHECK" the longest net phase, so the
   worst case is 6 + 1 + 9 + NUL. If a longer phase name is ever added,
   this fails the build instead of silently truncating the evidence. */
_Static_assert(DIAG_PHASE_MAX >= sizeof("RENDER") + sizeof("OTA_CHECK"),
               "DIAG_PHASE_MAX cannot hold the worst-case "
               "phase label");

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
        if (sample != NULL)
            rec->stack_net = sample->stack_free;
    } else {
        rec->main_phase = (uint8_t)value;
        if (sample != NULL)
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

static uint16_t stack_free_bytes(void) {
    /* ESP-IDF's watermark is in BYTES (net_window.c's stack-floor log
       says the same). Saturating rather than wrapping: a 16-bit field is
       plenty for a stack this side of 64 KB, and a wrapped small number
       would read as an imminent overflow that is not happening. */
    const UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    return (hwm > 65535u) ? 65535u : (uint16_t)hwm;
}

static void sample_now(panic_sample_t *out) {
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

void panic_diag_init(void) {
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
    portENTER_CRITICAL(&s_rec_mux);
    panic_diag_rec_mark(&s_rec, phase, prev, &s);
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
       stored setting, and nothing has ever reported how close this device
       is to that. free_entries is the whole answer: total is a constant
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
