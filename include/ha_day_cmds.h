#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "stats_json.h" /* act_state_t */

/* The day-scoped decisions of one HA window, lifted out of mqtt_ha.c so
   they carry host tests (test_ha_day_cmds): mqtt_ha.c needs esp-mqtt and
   is in no host suite. Pure: mqtt_ha.c owns every flag and buffer, asks
   these what to do, and does it.

   "Day-scoped" is BUG-14's word (owner decision Q1, 2026-09-25): a
   Screen-adjust target and the clear of it only mean something against a
   settled day, so while the snapshot says no_clock (stats_json.h) the
   window consumes neither. A cmd grant is day-scoped too; its hold lives
   in cmd_apply (cmd_apply_for_snapshot). */

#ifdef __cplusplus
extern "C" {
#endif

/* What a set/screen_bonus target that arrived this window becomes. */
typedef enum {
    HA_BONUS_BUFFER, /* handed to the orchestrator, applied at the join */
    HA_BONUS_HOLD,   /* no settled day: left retained, reported in act */
    HA_BONUS_DROP,   /* the day it belongs to is being cleared */
} ha_bonus_fate_t;

/* no_clock OUTRANKS clear_pending: without a settled day nobody knows
   whether the target is stale, so it is held, not dropped. With a day,
   a pending clear means the retained target is the cleared day's, never
   the one the finish applies to (the no-clock release queues its clear
   after settling a fresh day and before the window's MQTT phase), so it
   is dropped: a new day starts with no bonus (owner decision Q-A). */
ha_bonus_fate_t ha_day_bonus_fate(bool no_clock, bool clear_pending);

/* Whether the window publishes the day's bonus clear (retained "0" on
   set/screen_bonus). Never under no_clock: the power-on rollover queues a
   clear before a window whose NTP then fails, and published it would
   revoke the bonus of a day the release later restores. The caller
   consumes the pending flag either way: a clear refused here is DROPPED,
   not deferred. Each path that then starts a fresh day queues a new one:
     - the no-clock lock's release, when (and only when) the day it
       settles starts fresh (lock_gate.c, settle_day);
     - the power-on rollover whose NTP landed after the stats post, on its
       reset branch (wake_flow.c, wake_flow_handle_day_rollover). */
bool ha_day_publish_clear(bool no_clock, bool clear_pending);

/* The act payload's inputs (stats_json_act): the snapshot's applied
   figure, and the target this window saw, whether buffered or held. A
   held target is reported so HA's box keeps the parent's value instead of
   snapping to the locked stand-in's 0. day_cleared is what
   ha_day_publish_clear answered. */
act_state_t ha_day_act_state(int32_t applied_s, bool buffered, int32_t buffered_s, bool held, int32_t held_s,
                             bool day_cleared);

#ifdef __cplusplus
}
#endif
