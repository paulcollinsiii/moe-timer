/* The day-scoped decisions of one HA window (BUG-14). Pure; the rules and
   their reasons are in ha_day_cmds.h. */
#include "ha_day_cmds.h"

ha_bonus_fate_t ha_day_bonus_fate(bool no_clock, bool clear_pending) {
    if (no_clock) {
        return HA_BONUS_HOLD;
    }
    return clear_pending ? HA_BONUS_DROP : HA_BONUS_BUFFER;
}

bool ha_day_publish_clear(bool no_clock, bool clear_pending) {
    return clear_pending && !no_clock;
}

act_state_t ha_day_act_state(int32_t applied_s, bool buffered, int32_t buffered_s, bool held, int32_t held_s,
                             bool day_cleared) {
    return (act_state_t){
        .applied_s = applied_s,
        .target_s = held ? held_s : buffered_s,
        .target_pending = buffered || held,
        .day_cleared = day_cleared,
    };
}
