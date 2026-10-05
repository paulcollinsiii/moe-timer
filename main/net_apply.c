/* Orchestrator side of a network window, moved from main.c: pre-window
   def capture, the stats hand-off, and the post-join reconcile/apply.
   Runs entirely on the calling (main) task — the network task never
   mutates timer state. */
#include "net_apply.h"

#include <stdio.h>

#include "hal_time.h"
#include "mqtt_ha.h"
#include "net_window.h"
#include "nvs_config.h"
#include "timer.h"

#ifndef NATIVE
#include "esp_log.h"
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#endif

static const char *TAG = "net_apply";

static net_apply_ops_t s_ops;

/* C3 guard: a start/resume painted before the sync settled. If the sync
   lands during the MQTT tail, the finish applies the clock step then. */
static bool s_shift_pending;

/* Pre-window copy of every extra slot's definition, for the post-join
   reconcile: a config edit during the window may redefine any timer,
   including a PAUSED non-active one whose frozen remaining would otherwise
   go stale (field case: paused 10-min Violin shrunk to 2 min in HA kept
   its 10 min). Deep copy — the def names point into timer_defs' static
   table, which the post-join re-install overwrites. [0] unused (Screen). */
static struct {
    bool valid;
    char name[sizeof(((nvs_timer_def_t *)0)->name)];
    timer_def_t def;
} s_prewindow_defs[TIMER_SLOT_COUNT];

void net_apply_init(const net_apply_ops_t *ops) {
    s_ops = *ops;
}

bool net_apply_open(void) {
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        const timer_def_t *def = timer_slot_def(i); /* NULL = disabled */
        s_prewindow_defs[i].valid = (def != NULL);
        if (def != NULL) {
            snprintf(s_prewindow_defs[i].name, sizeof(s_prewindow_defs[i].name), "%s", def->name);
            /* Designated, not positional: a positional literal silently
               drops any field added to timer_def_t later, and this copy
               is the ONLY record of the pre-window definition — the
               reconcile compares against it, so a dropped field reads as
               an edit that never happened (break_eligible did exactly
               that, making every window look like an eligibility flip). */
            s_prewindow_defs[i].def = (timer_def_t){
                .name = s_prewindow_defs[i].name,
                .duration_sec = def->duration_sec,
                .reloadable = def->reloadable,
                .break_eligible = def->break_eligible,
            };
        }
    }
    s_shift_pending = false;
    return net_window_spawn();
}

void net_apply_note_start_unsynced(void) {
    s_shift_pending = true;
}

/* Post-join reconcile: config edits during the window rewrote the NVS defs
   blob only — re-install the in-memory table, then reconcile EVERY extra
   slot whose definition changed mid-run. Only the active slot drives sound
   and display (chirp / expiry alert / re-render); non-active slots — which
   can only be PAUSED, IDLE, or EXPIRED — are fixed silently and show their
   corrected state when the user swaps to them. */
static net_finish_t reconcile_defs(void) {
    timer_defs_install(); /* re-read the (possibly edited) blob from NVS */
    net_finish_t nf = NET_FINISH_IDLE;
    int active_slot = timer_active_slot();
    for (int slot = 1; slot < TIMER_SLOT_COUNT; slot++) {
        if (!s_prewindow_defs[slot].valid)
            continue;                         /* was disabled pre-window: nothing running to fix */
        s_prewindow_defs[slot].valid = false; /* one reconcile per window */
        bool was_running = false;
        timer_reconcile_t rc =
            timer_reconcile_def(slot, &s_prewindow_defs[slot].def, timer_slot_def(slot), hal_time_now(), &was_running);
        if (rc == TIMER_RECONCILE_NONE)
            continue;
        ESP_LOGW(TAG, "slot %d redefined during window: reconcile=%d", slot, (int)rc);
        if (timer_slot_def(slot) == NULL && timer_active_slot() == slot) {
            /* Slot disabled by the edit — same-wake analogue of the snapshot
               restore guard: never strand the selection on a dead slot. The
               reconcile above already folded and reset the run, so the
               guard finds the slot IDLE and folds nothing a second time. */
            timer_ensure_active_slot_enabled(hal_time_now());
        }
        if (slot != active_slot)
            continue; /* background slot: state fixed, seen at swap */
        switch (rc) {
            case TIMER_RECONCILE_RESET:
                if (was_running) {
                    s_ops.on_active_reset_chirp(); /* single chirp: your timer changed */
                }
                nf = NET_FINISH_CHANGED;
                break;
            case TIMER_RECONCILE_EXPIRED:
                s_ops.on_active_expired_alert(); /* owns the display: TIME'S UP + alert + repaint */
                nf = NET_FINISH_ALERTED;
                break;
            default:
                nf = NET_FINISH_CHANGED; /* UPDATED: remaining moved */
                break;
        }
    }
    return nf;
}

net_finish_t net_apply_finish(void) {
    if (!net_window_active())
        return NET_FINISH_IDLE; /* no window this wake: nothing arrived */
    if (!net_window_join(NET_JOIN_TIMEOUT_MS, s_ops.join_poll)) {
        timer_note_wifi_join_result(false);
        return NET_FINISH_IDLE; /* wedged: no results to apply */
    }
    /* The one place every window ends, so the one place to count how they
       end. The sync result stands in for "the WiFi join worked": a failed
       join leaves it non-OK, and a join that worked but never reached an
       SNTP server is no network path either, which is all the "WiFi
       failing" hint claims. A call with no window open (above) says
       nothing about the network and counts as neither. */
    timer_note_wifi_join_result(net_window_ntp_result() == ESP_OK);
    /* C3: the sync settled after the paint's bounded wait gave up, but a
       timer was started this wake against the uncorrected clock — apply
       the measured step now, before anything below reads the expiry. The
       step is consume-once, so this can never double-apply with the
       wait-succeeded path in the wake handler. */
    if (s_shift_pending) {
        s_shift_pending = false;
        if (net_window_ntp_result() == ESP_OK) {
            int64_t step = net_window_take_clock_step();
            if (step != 0) {
                ESP_LOGI(TAG, "late NTP sync: shifting expiry by %lld s", (long long)step);
                timer_shift_expiry(step);
            }
        }
    }
    /* The window may have applied HA config edits (allocations, holidays,
       school dates, quiet hours, bedtime): drop the wake-scoped caches so
       every read below and after sees the edited values. */
    s_ops.on_config_applied();
    /* Both are tracked, not just applied: the paint on an interactive wake
       happens BEFORE this join, so an adjustment nobody reports leaves the
       pre-adjustment figure on the panel until some later wake — press
       sync, see no change, and the minutes only appear when the timer
       starts. A state diff cannot stand in for this: an adjustment applied
       while the slot is IDLE is banked, so the state stays IDLE while the
       allocation the panel renders has moved.

       Tracked PER SLOT, because the panel draws one: every field the main
       layout reads comes from timer_active_slot(), so an adjustment
       anywhere else changes nothing a repaint could show. The bonus
       always targets slot 0 and a cmd grant targets any slot, so
       "something moved" is not the question — "the drawn slot moved" is.
       The two slot-0 states that look like exceptions are not: a BREAK
       running behind another timer draws the header chip off
       break_expiry_wall, which timer_adjust never touches, and a grant
       that flips an EXPIRED slot 0 to PAUSED moves a state only
       break_banner reads, and only for BREAK. */
    uint32_t moved_slots = 0;
    int32_t bonus_target;
    if (mqtt_ha_take_bonus_target(&bonus_target)) {
        if (timer_bonus_reconcile(0, bonus_target))
            moved_slots |= 1u;
    }
    int grant_slot;
    int32_t grant_sec;
    if (mqtt_ha_take_grant(&grant_slot, &grant_sec)) {
        if (timer_adjust(grant_slot, grant_sec) && grant_slot >= 0 && grant_slot < TIMER_SLOT_COUNT)
            moved_slots |= 1u << grant_slot;
    }
    net_finish_t nf = reconcile_defs();
    /* Read the selection AFTER the reconcile: that is the slot the paint
       will render. (A reconcile that moves the selection has already
       answered CHANGED or ALERTED, so this only ever reads a settled
       one.) Promote only from IDLE — ALERTED means the expiry path owns
       the display and must not be downgraded to an ordinary repaint, and
       CHANGED already says everything this would. */
    if (nf == NET_FINISH_IDLE && (moved_slots & (1u << timer_active_slot())) != 0) {
        nf = NET_FINISH_CHANGED;
    }
    /* Locate last, after the radio is down (audio/LEDs, and it extends
       the awake failsafe). */
    if (mqtt_ha_locate_pending()) {
        s_ops.on_locate();
    }
    return nf;
}

esp_err_t net_apply_try_window_then(void (*after_ntp)(void)) {
    if (!net_apply_open())
        return ESP_FAIL;
    net_window_wait_ntp();
    /* BETWEEN THE SYNC AND THE STATS POST, and nowhere else will do: the
       window task holds its MQTT phase until the snapshot arrives
       (net_window.c), so whatever the hook changes is what that phase
       reports and what the finish below applies the buffered HA effects
       to. The no-clock lock settles the real day here (BUG-14). */
    if (after_ntp != NULL) {
        after_ntp();
    }
    s_ops.post_stats();
    net_apply_finish();
    return net_window_ntp_result();
}

esp_err_t net_apply_try_window(void) {
    return net_apply_try_window_then(NULL);
}
