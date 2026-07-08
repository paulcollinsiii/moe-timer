/* Extra-timer definition table. HA config (NVS blob, phase 2) is the
   source of truth when present; the menuconfig table is the first-boot
   default and fallback. To add a slot: bump TIMER_EXTRA_SLOTS in timer.h,
   add the MAGTAG_TIMER<n>_* block in Kconfig.projbuild, and one line to
   the Kconfig table below. */
#include <stdio.h>

#include "nvs_config.h"
#include "sdkconfig.h"
#include "timer.h"

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

#define EXTRA_TIMER_DEF(n) \
    { CONFIG_MAGTAG_TIMER##n##_NAME, CONFIG_MAGTAG_TIMER##n##_MIN * 60, CONFIG_MAGTAG_TIMER##n##_RELOADABLE }

static const timer_def_t s_kconfig_defs[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false}, /* slot 0: allocation comes from schedule.c */
    EXTRA_TIMER_DEF(1),   EXTRA_TIMER_DEF(2), EXTRA_TIMER_DEF(3), EXTRA_TIMER_DEF(4),
};

/* NVS-derived table: names live in this static store because timer.c keeps
   the def pointers (they must outlive install). Rebuilt each boot from the
   HA blob; unchanged across the run. */
static char s_names[TIMER_SLOT_COUNT][16];
static timer_def_t s_nvs_defs[TIMER_SLOT_COUNT];

void timer_defs_install(void) {
    nvs_timer_defs_blob_t blob;
    if (nvs_config_get_timer_defs(&blob) != ESP_OK) {
        timer_set_defs(s_kconfig_defs, TIMER_SLOT_COUNT); /* no HA config yet */
        return;
    }
    s_nvs_defs[0] = (timer_def_t){"Screen", 0, false};
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        snprintf(s_names[i + 1], sizeof(s_names[i + 1]), "%s", blob.defs[i].name);
        s_nvs_defs[i + 1].name = s_names[i + 1];
        s_nvs_defs[i + 1].duration_sec = blob.defs[i].min * 60;
        s_nvs_defs[i + 1].reloadable = blob.defs[i].reload != 0;
    }
    timer_set_defs(s_nvs_defs, TIMER_SLOT_COUNT);
}
