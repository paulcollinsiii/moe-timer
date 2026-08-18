/* Extra-timer definition table. HA config (NVS blob, phase 2) is the
   source of truth when present; the menuconfig table is the first-boot
   default and fallback. To add a slot: bump TIMER_EXTRA_SLOTS in timer.h,
   add the MAGTAG_TIMER<n>_* block in Kconfig.projbuild, and one line to
   the Kconfig table below. */
#include <stdio.h>
#include <string.h>

#include "nvs_config.h"
#include "timer.h"

/* menuconfig on firmware builds; host tests have no sdkconfig and fall
   back to the #ifndef defaults below (pattern: timer.h, nvs_defaults.h).
   The file is host-compiled since BUG-8 so timer_defs_install() itself is
   under test, not a transcription of it. */
#ifndef NATIVE
#include "esp_log.h"
#include "nvs.h" /* ESP_ERR_NVS_NOT_FOUND — esp_compat.h only defines it on NATIVE */
#include "sdkconfig.h"
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

/* File-scoped, and deliberately not plain `TAG`: ha_config.c has its own,
   and since BUG-8 the two files are close enough that a single test TU
   compiles both (test_timer_defs). Two `static const char *TAG` at file
   scope in one translation unit is a redefinition. */
static const char *TAG_TIMER_DEFS = "timer_defs";

/* Kconfig emits bool symbols only when =y; default the rest to 0 so the
   table can reference every slot unconditionally. */
#ifndef CONFIG_MAGTAG_TIMER1_RELOADABLE
#define CONFIG_MAGTAG_TIMER1_RELOADABLE 0
#endif
#ifndef CONFIG_MAGTAG_TIMER2_RELOADABLE
#define CONFIG_MAGTAG_TIMER2_RELOADABLE 0
#endif
#ifndef CONFIG_MAGTAG_TIMER3_RELOADABLE
#define CONFIG_MAGTAG_TIMER3_RELOADABLE 0
#endif
#ifndef CONFIG_MAGTAG_TIMER4_RELOADABLE
#define CONFIG_MAGTAG_TIMER4_RELOADABLE 0
#endif
#ifndef CONFIG_MAGTAG_TIMER1_BREAK_ELIGIBLE
#define CONFIG_MAGTAG_TIMER1_BREAK_ELIGIBLE 0
#endif
#ifndef CONFIG_MAGTAG_TIMER2_BREAK_ELIGIBLE
#define CONFIG_MAGTAG_TIMER2_BREAK_ELIGIBLE 0
#endif
#ifndef CONFIG_MAGTAG_TIMER3_BREAK_ELIGIBLE
#define CONFIG_MAGTAG_TIMER3_BREAK_ELIGIBLE 0
#endif
#ifndef CONFIG_MAGTAG_TIMER4_BREAK_ELIGIBLE
#define CONFIG_MAGTAG_TIMER4_BREAK_ELIGIBLE 0
#endif
/* Names are strings, and the host build has no sdkconfig at all: an empty
   name leaves the slot disabled, which is the right host default. */
#ifndef CONFIG_MAGTAG_TIMER1_NAME
#define CONFIG_MAGTAG_TIMER1_NAME ""
#endif
#ifndef CONFIG_MAGTAG_TIMER2_NAME
#define CONFIG_MAGTAG_TIMER2_NAME ""
#endif
#ifndef CONFIG_MAGTAG_TIMER3_NAME
#define CONFIG_MAGTAG_TIMER3_NAME ""
#endif
#ifndef CONFIG_MAGTAG_TIMER4_NAME
#define CONFIG_MAGTAG_TIMER4_NAME ""
#endif
/* Same for _MIN: "depends on" hides them while the name is empty. */
#ifndef CONFIG_MAGTAG_TIMER1_MIN
#define CONFIG_MAGTAG_TIMER1_MIN 0
#endif
#ifndef CONFIG_MAGTAG_TIMER2_MIN
#define CONFIG_MAGTAG_TIMER2_MIN 0
#endif
#ifndef CONFIG_MAGTAG_TIMER3_MIN
#define CONFIG_MAGTAG_TIMER3_MIN 0
#endif
#ifndef CONFIG_MAGTAG_TIMER4_MIN
#define CONFIG_MAGTAG_TIMER4_MIN 0
#endif

#define EXTRA_TIMER_DEF(n)                                                                                     \
    {                                                                                                          \
        CONFIG_MAGTAG_TIMER##n##_NAME, CONFIG_MAGTAG_TIMER##n##_MIN * 60, CONFIG_MAGTAG_TIMER##n##_RELOADABLE, \
            CONFIG_MAGTAG_TIMER##n##_BREAK_ELIGIBLE                                                            \
    }

/* Slot 0 is permanently break_eligible = false: screen time IS the
   exposure the break exists to interrupt. Not configurable. */
#define SCREEN_DEF \
    { "Screen", 0, false, false }

static const timer_def_t s_kconfig_defs[TIMER_SLOT_COUNT] = {
    SCREEN_DEF, /* slot 0: allocation comes from schedule.c */
    EXTRA_TIMER_DEF(1), EXTRA_TIMER_DEF(2), EXTRA_TIMER_DEF(3), EXTRA_TIMER_DEF(4),
};

/* NVS-derived table: names live in this static store because timer.c keeps
   the def pointers (they must outlive install). Rebuilt each boot from the
   HA blob; unchanged across the run. */
static char s_names[TIMER_SLOT_COUNT][16];
static timer_def_t s_nvs_defs[TIMER_SLOT_COUNT];

/* The menuconfig table, for callers that need the DEFAULT rather than the
   installed or the stored one. config_apply.c's apply_timers() resolves an
   optional key that a retained document omits for a slot it is defining for
   the FIRST time from here. Exposed rather than duplicated because
   sdkconfig.h is only reachable from this file. */
const timer_def_t *timer_defs_compiled(int slot) {
    if (slot < 0 || slot >= TIMER_SLOT_COUNT)
        return NULL;
    return &s_kconfig_defs[slot];
}

/* See the ESP_LOGW below: one warning per boot, not one per install. */
static bool s_no_blob_warned;

void timer_defs_install(void) {
    nvs_timer_defs_blob_t blob;
    esp_err_t err = nvs_config_get_timer_defs(&blob);
    if (err == ESP_OK) {
        s_no_blob_warned = false; /* a later loss warns about it again */
    } else {
        /* No readable HA-managed blob: run THIS BOOT on the Kconfig table,
           but do not persist it — BUG-8. Writing it here made an invented
           value indistinguishable from an operator's: apply_timers() then
           saw have_prev/existed true and BUG-6's "absent means unchanged"
           rule preserved the compile-time break_eligible as though someone
           had chosen it. Leaving NVS empty keeps "the blob exists" meaning
           "something authoritative wrote it", so the first retained config
           document is correctly recognised as a first-time definition.

           What the boot write DID get right, by the wrong mechanism, is
           that a menuconfig value outranks an empty default: laundering the
           Kconfig table into NVS made apply_timers() see it as tier-1
           stored state. That precedence is restored honestly in
           apply_timers() itself, via timer_defs_compiled() below -- as a
           default consulted in place, with no write to flash to earn its
           standing. Do not reinstate the write to get it back.

           ha_config.c's load_defs() covers the readers that used to depend
           on this write (the discovery hash and the cfg state JSON): they
           now fall back to this same installed table via
           timer_slot_def_raw(), the unfiltered form of the accessor
           mqtt_ha.c already uses for the per-timer entities. */
        /* Once per boot, not once per call: main.c installs at wake and
           net_apply's reconcile_defs() re-installs after EVERY network
           window, so an unconditional warn is a per-window log line on a
           device that legitimately has no blob. The latch clears on a
           successful read, so losing the blob later still says so. Statics
           do not survive deep sleep, so "per boot" is "per wake".

           Two messages, not one, and this is BUG-5's unconditional
           constraint: "never configured" and "configured, but the stored
           bytes no longer parse" are different events that happen to take
           the same branch. The first is the expected state of a fresh or
           erased device and costs nothing. The second means a table
           somebody chose is sitting in flash unreachable — the timers
           silently revert to compile-time values and every HA per-timer
           control NAKs until a config document rebuilds it. Sharing one
           line made the second indistinguishable from the first. */
        if (!s_no_blob_warned) {
            if (err == ESP_ERR_NVS_NOT_FOUND)
                ESP_LOGW(TAG_TIMER_DEFS,
                         "no timer-defs blob stored; running on the compile-time table (not persisted)");
            else
                ESP_LOGW(TAG_TIMER_DEFS,
                         "timer-defs blob present but UNREADABLE (%s): the stored table is NOT in use; running on the "
                         "compile-time table (not persisted, not overwritten)",
                         esp_err_to_name(err));
            s_no_blob_warned = true;
        }
        memset(&blob, 0, sizeof(blob));
        blob.version = TIMER_DEFS_BLOB_VERSION;
        for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
            const timer_def_t *d = &s_kconfig_defs[i + 1];
            if (d->name != NULL)
                snprintf(blob.defs[i].name, sizeof(blob.defs[i].name), "%s", d->name);
            blob.defs[i].min = d->duration_sec / 60;
            blob.defs[i].reload = d->reloadable ? 1 : 0;
            blob.defs[i].break_eligible = d->break_eligible ? 1 : 0;
        }
    }
    s_nvs_defs[0] = (timer_def_t)SCREEN_DEF;
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        snprintf(s_names[i + 1], sizeof(s_names[i + 1]), "%s", blob.defs[i].name);
        s_nvs_defs[i + 1].name = s_names[i + 1];
        s_nvs_defs[i + 1].duration_sec = blob.defs[i].min * 60;
        s_nvs_defs[i + 1].reloadable = blob.defs[i].reload != 0;
        s_nvs_defs[i + 1].break_eligible = blob.defs[i].break_eligible != 0;
    }
    timer_set_defs(s_nvs_defs, TIMER_SLOT_COUNT);
}
