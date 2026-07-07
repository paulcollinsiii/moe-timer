/* Extra-timer definition table: menuconfig -> timer_set_defs(). To add a
   slot: bump TIMER_EXTRA_SLOTS in timer.h, add the MAGTAG_TIMER<n>_* block
   in Kconfig.projbuild, and one table line here. */
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

static const timer_def_t s_timer_defs[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false}, /* slot 0: allocation comes from schedule.c */
    EXTRA_TIMER_DEF(1),   EXTRA_TIMER_DEF(2), EXTRA_TIMER_DEF(3), EXTRA_TIMER_DEF(4),
};

void timer_defs_install(void) {
    timer_set_defs(s_timer_defs, TIMER_SLOT_COUNT);
}
