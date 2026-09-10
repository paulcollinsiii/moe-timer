#pragma once
#include <stdbool.h>

#ifndef NATIVE
#include "esp_err.h"
#endif

/* Orchestrator side of a network window (mechanics live in net_window.c):
   pre-window timer-def capture, the post-join apply of every buffered
   network→timer effect (clock-step shift, bonus, grant, def reconcile,
   locate), all single-threaded on the calling task. Host-tested — device
   effects (chirp, expiry alert, stats collection, locate alarm, config
   cache invalidation) are injected once via net_apply_init. */

typedef enum {
    NET_FINISH_IDLE = 0, /* nothing display-relevant happened */
    NET_FINISH_CHANGED,  /* timer state/remaining changed: re-render */
    NET_FINISH_ALERTED,  /* expiry alert ran: display fully handled */
} net_finish_t;

typedef struct {
    void (*join_poll)(void);               /* nullable; keeps Button B live during the join */
    void (*on_config_applied)(void);       /* drop wake-scoped config caches */
    void (*on_active_reset_chirp)(void);   /* active slot redefined mid-run */
    void (*on_active_expired_alert)(void); /* owns the display: TIME'S UP + repaint */
    void (*post_stats)(void);              /* collect + post the HA stats snapshot */
    void (*on_locate)(void);               /* locate alarm; radio already down */
} net_apply_ops_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Install the device effects. Must run once before any open/finish. */
void net_apply_init(const net_apply_ops_t *ops);

/* Capture the pre-window defs, then spawn the window task. false =
   fail-open, no window this wake. */
bool net_apply_open(void);

/* The wake started/resumed a timer but painted before the sync settled:
   if the sync lands during the MQTT tail, the finish below must still
   apply the measured clock step to the expiry (timer_shift_expiry). */
void net_apply_note_start_unsynced(void);

/* Close out a window: join, apply the buffered network→timer effects,
   reconcile redefined timers, run a pending locate alarm. Safe to call
   when no window is open. */
net_finish_t net_apply_finish(void);

/* One blocking radio window, for the unattended paths (timer tick, day
   rollover, final-minute sync): open → sync → stats → finish. The return
   reflects the SNTP result only — MQTT is best-effort and can never fail
   the sync that opened the window. */
esp_err_t net_apply_try_window(void);

#ifdef __cplusplus
}
#endif
