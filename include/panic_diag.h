#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stats_json.h" /* diag_stat_t, DIAG_PHASE_MAX — the published view */

/* Panic forensics: how many, and doing what.
   ========================================================================

   WHY THIS EXISTS. This device panics unattended a few times a day and
   the console is not there to see it. Two things were missing and both
   are cheap:

     1. HOW OFTEN. `last_reset` is a STATE sensor, so Home Assistant
        collapses PANIC -> PANIC into a single row: only a
        PANIC -> DEEPSLEEP -> PANIC sequence is countable, and the real
        rate is therefore unknowable from HA. A monotonic counter makes
        it exact — the delta between any two points is the number of
        panics between them.

     2. DOING WHAT. The single most diagnostic bit available without a
        coredump partition (which is not an option here: the partition
        table is frozen for OTA'd devices). The firmware writes a
        breadcrumb to memory that survives a panic reset, and the next
        boot publishes it.

   ---- storage, and why RTC_NOINIT_ATTR is right HERE ------------------

   The live breadcrumb lives in RTC_NOINIT_ATTR. That deserves an
   argument, because this tree carries a note saying RTC_NOINIT_ATTR was
   "the wrong fix" — that note is about preserving TIMER STATE across an
   OTA reboot, which is a different problem with a different failure
   mode, and it must not be cargo-culted in either direction.

     - RTC_DATA_ATTR is zeroed by esp_restart() AND by a panic reset. It
       survives deep sleep only. A breadcrumb kept there reads 0 on
       exactly the boot that needs it, so it is not a candidate at all.

     - RTC_NOINIT_ATTR is not zeroed by the startup code, so it carries
       across a panic reset. Its cost is that it is UNINITIALISED on a
       cold power-on: whatever the RTC RAM held at power-up is what you
       read. Publishing that as a reading would be publishing garbage as
       evidence, which is worse than publishing nothing.

       That cost is paid off with a magic word plus a checksum over the
       whole record (see panic_diag_rec_t). A cold boot has to match a
       32-bit constant AND a 32-bit hash of the bytes underneath it
       before anything is believed. That is the difference between this
       use and the one the project note rejected: timer state has no
       inert value that means "I am not real", so a guard there still
       leaves a valid-looking-but-wrong restore; a breadcrumb does — an
       unguarded record is simply not published.

   The breadcrumb is ALSO copied into NVS on the boot that finds it, and
   that is not belt-and-braces. RTC memory survives to the NEXT boot and
   no further, and this firmware does not open a network window on every
   wake (wake_policy_sync_due), so the wake following a panic frequently
   has no way to publish anything. Without the NVS copy the evidence
   would be discarded by the wake after that one. In NVS it is sticky:
   it reports the last panic until the next one replaces it, exactly the
   way ota_result already behaves.

   ---- two phase slots, and why one byte would lie ---------------------

   Phases are marked from more than one task. The main task boots,
   renders and sleeps; the `net_win` task runs the window (wifi, SNTP,
   the OTA check, MQTT); the `ota_dl` task runs the download. A single
   shared phase byte would be written by whichever task moved last, so a
   panic during MQTT could easily be reported as RENDER — which is the
   one thing this feature must not do, since "was it in the OTA/TLS
   path?" is the question being asked.

   So the record carries TWO phases, and which slot a phase belongs to is
   a property of the phase itself (panic_diag_phase_is_net), not of the
   caller. Each slot has exactly one writer at a time:

     - main slot  BOOT / AWAKE / RENDER / SLEEP
     - net slot   NET / OTA_CHECK / MQTT / OTA_DL

   The published label is both, joined: "RENDER+OTA_CHECK" means the main
   task was painting while the network task was in the update check.

   Two writers do touch the MAIN slot — display.c's render() is also
   reached from the OTA task (wake_flow_repaint_current_state and
   display_ota) — but never concurrently: ota_task_run_apply blocks the
   main task on a semaphore for the whole attempt. The pairing is
   therefore serialised, and "RENDER+OTA_DL" is an honest reading of the
   OTA task painting its progress screen.

   ---- what is NOT counted --------------------------------------------

   The counter moves on ESP_RST_PANIC only. The watchdog reasons
   (INT_WDT / TASK_WDT / WDT) and BROWNOUT are deliberately excluded:
   `last_reset` already tells those apart, CONFIG_ESP_TASK_WDT_PANIC is
   off on this build, and a counter named "panics" that also counted
   watchdogs would answer a different question than the one an operator
   reading it thinks they are asking. */

#ifdef __cplusplus
extern "C" {
#endif

/* The phases, in the order a healthy wake passes through them. Values are
   STORED (RTC memory and an NVS blob), so append rather than renumber:
   an old record read by a new image would otherwise decode to the wrong
   phase. PANIC_PHASE__COUNT is the range guard, not a phase. */
typedef enum {
    PANIC_PHASE_NONE = 0,  /* nothing running on this side */
    PANIC_PHASE_BOOT,      /* app_main init, before the wake handler */
    PANIC_PHASE_AWAKE,     /* the wake itself: tick, buttons, grid wait, event watch */
    PANIC_PHASE_RENDER,    /* inside display.c's render(): LVGL + the SPI flush */
    PANIC_PHASE_SLEEP,     /* the deep-sleep funnel */
    PANIC_PHASE_NET,       /* network window open: wifi assoc + SNTP */
    PANIC_PHASE_OTA_CHECK, /* the manifest GET (HTTPS/TLS) */
    PANIC_PHASE_MQTT,      /* the MQTT session */
    PANIC_PHASE_OTA_DL,    /* the image download + flash write */
    PANIC_PHASE__COUNT
} panic_phase_t;

/* Longest label is "RENDER+OTA_CHECK" (16) — see the static assert in
   panic_diag.c, which pins DIAG_PHASE_MAX against the real table rather
   than against this comment. */

/* ---- the record ------------------------------------------------------
   Layout is hole-free by construction and asserted to be 24 bytes: the
   checksum runs over the raw bytes, so a padding hole would make the
   guard depend on whatever the compiler left in it. */
typedef struct {
    uint32_t magic;      /* PANIC_DIAG_MAGIC when this record is real */
    uint8_t main_phase;  /* panic_phase_t, main-task side */
    uint8_t net_phase;   /* panic_phase_t, network/OTA side */
    uint16_t stack_main; /* main-slot task stack high-water, BYTES free */
    uint16_t stack_net;  /* net-slot task stack high-water, BYTES free */
    uint16_t rsvd;       /* explicit, so there is no implicit padding to hash */
    uint32_t uptime_ms;  /* esp_timer_get_time()/1000 at the last mark */
    uint32_t heap_free;  /* esp_get_free_heap_size() at the last mark */
    uint32_t sum;        /* hash of every byte above; MUST stay last */
} panic_diag_rec_t;

#define PANIC_DIAG_MAGIC 0x50414E31u /* "PAN1" */

/* What one mark samples. Gathered by the device layer and handed to the
   pure record update below, so the whole state machine is host-testable
   without a clock, a heap or a scheduler. */
typedef struct {
    uint32_t uptime_ms;
    uint32_t heap_free;
    uint16_t stack_free; /* the MARKING task's high-water mark, bytes */
    /* Set when the marking task does NOT own the slot the mark selects,
       which leaves that slot's stack field alone. The main slot belongs
       to the main task, but two other tasks mark it: render() runs from
       ota_dl while a download paints, and the awake failsafe runs
       enter_deep_sleep() from the esp_timer task. Without this, a panic
       during an OTA publishes ota_dl's 16 KB-stack high-water mark under
       the label "stack free (main)" — a figure the 7 KB main task cannot
       physically produce, sending the reader after a stack bug that is
       not there. Defaults false so a sample that does not think about
       ownership behaves as before. */
    bool stack_foreign;
} panic_sample_t;

/* ---- pure layer (host-tested: test_panic_diag) ----------------------- */

/* Enum -> literal. Out-of-range answers "?" rather than a phase name: a
   record that survived its checksum but carries a value this image does
   not know about must not be reported as something it is not. */
const char *panic_diag_phase_str(panic_phase_t phase);

/* Which slot a phase owns. NONE and any out-of-range value answer false
   (the main slot), which only matters for panic_diag_exit's argument. */
bool panic_diag_phase_is_net(panic_phase_t phase);

/* Join the two slots for publication: "RENDER+OTA_CHECK", "MQTT",
   "AWAKE", or "NONE" when neither side is in a phase. snprintf
   semantics. Takes raw uint8_t rather than panic_phase_t because the
   input may have come off flash. */
int panic_diag_phase_label(char *buf, size_t len, uint8_t main_phase, uint8_t net_phase);

/* Re-arm a record: magic, zeroed fields, valid checksum. */
void panic_diag_rec_reset(panic_diag_rec_t *rec);

/* Is this record one we wrote, intact? Magic AND checksum — see the
   cold-boot argument at the top of this file. NULL answers false. */
bool panic_diag_rec_valid(const panic_diag_rec_t *rec);

/* Apply one phase transition and re-seal.

   `slot_of` selects the slot (by panic_diag_phase_is_net) and `value` is
   what lands in it — enter passes the same phase for both, exit passes
   the phase being left plus whatever should be restored. Splitting them
   is what lets RENDER nest inside AWAKE without AWAKE being forgotten.

   The sample's stack figure lands in the SELECTED slot's stack field, so
   the two numbers stay attached to the tasks they were taken on. */
void panic_diag_rec_mark(panic_diag_rec_t *rec, panic_phase_t slot_of, panic_phase_t value,
                         const panic_sample_t *sample);

/* Leave a phase: restore `prev` into the slot `phase` owns, but ONLY if
   that slot still holds `phase`.

   The conditional is the whole point. enter/exit is a read-modify-write
   spanning the phase body, and the spinlock only makes each end atomic —
   it cannot stop a THIRD task moving the slot in between. The awake
   failsafe does exactly that: it fires on the esp_timer task and marks
   SLEEP while a render() started on ota_dl is still inside its 2-4 s
   e-ink refresh. An unconditional restore then puts AWAKE back over
   SLEEP, and a panic in the sleep funnel publishes the wrong phase — on
   the wedged-device path this feature exists to explain.

   Answers true when the restore happened. When it did not, the record is
   left completely untouched (uptime and heap included): the task that
   owns the slot now is the one whose view should be published, and a
   late exit has nothing to add to it. */
bool panic_diag_rec_exit(panic_diag_rec_t *rec, panic_phase_t phase, panic_phase_t prev, const panic_sample_t *sample);

/* Compose the panic half of the published view. `last` is the stored
   record, or NULL when there is none (or it failed its guard) — which
   publishes an EMPTY phase string, deliberately distinct from "NONE":
   empty means "no breadcrumb on file", NONE means "a panic was recorded
   but neither side was in a marked phase". Does not touch the live
   fields; the device layer fills those. */
void panic_diag_fill_stat(diag_stat_t *out, uint32_t panics, const panic_diag_rec_t *last);

/* ---- device layer ---------------------------------------------------- */

/* Latch the previous boot's breadcrumb and start this boot's.

   Call FIRST in app_main, before anything that could itself panic — it
   touches no peripheral (RTC memory, the reset-reason register and the
   monotonic timer only), so it does not displace neopixel_init()'s claim
   on the first PERIPHERAL call. Everything before this point is
   unattributable: a panic there would be reported against the phase the
   PREVIOUS wake ended in.

   Does not write NVS — NVS is not up yet at that point in app_main.
   panic_diag_commit() is the other half. */
void panic_diag_init(void);

/* Persist what init() latched: the counter, and the breadcrumb blob.
   Call once, after nvs_flash_init()/nvs_config_init_defaults(). A no-op
   unless this boot followed a panic, so the ordinary wake pays one
   comparison. */
void panic_diag_commit(void);

/* Enter `phase`; returns the slot's PREVIOUS phase so a nested phase can
   put it back (display.c's render() is the case that needs it). */
panic_phase_t panic_diag_enter(panic_phase_t phase);

/* Leave `phase`, restoring `prev` into the slot `phase` belongs to. Pass
   PANIC_PHASE_NONE for "this side is idle again". */
void panic_diag_exit(panic_phase_t phase, panic_phase_t prev);

/* Fill the whole diagnostic leg of the stat payload: the stored panic
   record and counter, plus the live heap / stack / NVS readings.

   Called at PUBLISH time, on the network task, for the same reason
   ota_flow_stat() is (see stats_json.h): the live numbers must be this
   window's, and stack_net is only meaningful when read from the task
   that owns that stack. NULL-safe. */
void panic_diag_stat(diag_stat_t *out);

#ifdef __cplusplus
}
#endif
