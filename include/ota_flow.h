#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota_policy.h"

/* OTA sequencing — layer 2. Owns the ORDER of an update: when a check may
   run, where the manifest fetch sits inside the existing network window,
   when the panel is painted relative to the radio, how long the download
   is allowed, and what is persisted when it fails. Every device effect
   arrives through ota_flow_ops_t, so the whole sequence is host-tested in
   test_ota_flow with no ESP-IDF present.

   The decisions themselves are not here: ota_policy.c answers "may I" and
   "should I", ota.c moves the bytes. What this module contributes is the
   sequence those two are placed in, which is where the hazards live.

   ---- the two windows, and why the split is load-bearing ----

   A check is one small HTTPS GET that RIDES the window the wake was
   already opening (ota_flow_check, on the network task, after the
   snapshot rendezvous and before the MQTT phase). A download is a second
   window, opened only on the days an update actually exists
   (ota_flow_apply, on the main task, after the first window has closed).

   Between them the panel is painted, and that gap is the entire reason
   for the split. display_ota() carries no net_window_active() guard and
   its flush blocks for a full refresh; painting inside an open window
   walks into the brownout this project already paid for once
   (net_window.c:65-79). So: window 1 closes -> paint -> window 2 opens.
   ota_flow_apply enforces that ordering rather than leaving it to the
   call site, and test_ota_flow asserts it.

   ---- threading ----

   ota_flow_check runs on the network task; everything else runs on the
   main task. The handover is the same one every other network->device
   effect in this tree uses: the check BUFFERS its result, and the main
   task reads it only after net_window_join(), which is the barrier. */

#ifdef __cplusplus
extern "C" {
#endif

/* What opened this window, as far as the OTA check is concerned. The
   trigger is the caller's fact; whether it earns a check is this
   module's (ota_flow_arm), because the sync case depends on a runtime
   flag that only NVS knows. */
typedef enum {
    OTA_TRIGGER_NONE = 0, /* tick wake, break end, final-minute sync: never checks */
    OTA_TRIGGER_ROLLOVER, /* day rollover: always checks */
    OTA_TRIGGER_SYNC,     /* Button D full sync: checks iff ota_on_sync */
} ota_trigger_t;

/* One turn of the download loop. */
typedef enum {
    OTA_STEP_MORE = 0, /* a chunk landed, more to come */
    OTA_STEP_DONE,     /* the whole image has been received */
    OTA_STEP_FAIL,     /* transport gave up; facts are filled */
} ota_step_t;

/* Everything this module cannot do itself. Injected once at init, exactly
   like net_apply_ops_t, so the sequence above is exercised on the host
   with counters in place of a radio and a panel.

   The download is FOUR calls rather than one because the deadline check
   lives in the loop between them, and because "committed the image" and
   "discarded it" have to be separately observable: the guarantee that a
   timed-out download never switches the boot partition is only a
   guarantee if a test can see it being kept. dl_finish is the commit
   (esp_https_ota_finish, which is what sets the boot partition);
   dl_abort discards. Exactly one of the two runs per begun download. */
typedef struct {
    /* Manifest GET. Returns bytes written to buf, or < 0 on failure with
       facts filled. Does not NUL-terminate on the caller's behalf — the
       length is authoritative, as ota_policy_decide requires. */
    int (*manifest_get)(const char *url, char *buf, size_t len, ota_error_facts_t *facts);

    /* Open the image transfer and validate its header. One transfer is
       in flight at a time, so the handle stays inside the driver.

       false means NOTHING is in flight: a driver that got part-way has
       already cleaned up after itself, so dl_abort must not follow. Same
       for dl_finish, which frees its handle whichever way it answers.
       dl_abort therefore pairs only with a dl_step that failed or a
       deadline that expired. */
    bool (*dl_begin)(const char *url, ota_error_facts_t *facts);
    ota_step_t (*dl_step)(ota_error_facts_t *facts);
    bool (*dl_finish)(ota_error_facts_t *facts); /* COMMIT: sets the boot partition */
    void (*dl_abort)(void);                      /* discard a partial image */

    bool (*session_begin)(void); /* wifi_session_begin() == ESP_OK */
    void (*session_end)(void);

    void (*paint_update)(const char *from, const char *to); /* display_ota */
    void (*repaint)(void);                                  /* back to the normal screen */

    /* Push the awake failsafe out (main.c's extend_awake_failsafe). */
    void (*extend_awake)(int seconds);
    void (*restart)(void); /* esp_restart */
    uint32_t (*free_heap)(void);
    int64_t (*mono_ms)(void); /* monotonic; only differences are read */
} ota_flow_ops_t;

/* The numbers the sequence is measured against. Passed in rather than
   read from sdkconfig.h here, because test_ota_flow asserts on the
   deadline and the retry budget: a host build that supplied its own
   CONFIG_ defines would be asserting against values the firmware does
   not necessarily use. app_main passes the real symbols in one line. */
typedef struct {
    int max_sec;                 /* CONFIG_MAGTAG_OTA_MAX_SEC: the download's own budget */
    int awake_sec;               /* CONFIG_MAGTAG_MAX_AWAKE_SEC: restored after a failure */
    int min_batt_pct;            /* CONFIG_MAGTAG_OTA_MIN_BATT_PCT */
    uint16_t max_fails;          /* CONFIG_MAGTAG_OTA_MAX_FAILS */
    uint32_t min_free_heap;      /* headroom the TLS session needs */
    const char *running_version; /* esp_app_get_description()->version */
} ota_flow_cfg_t;

/* Install the effects and the budgets. Once, before any other call. */
void ota_flow_init(const ota_flow_ops_t *ops, const ota_flow_cfg_t *cfg);

/* Main task, before the window opens: clear last wake's buffered result
   and decide whether this wake's trigger earns a check. batt_pct < 0
   means "unreadable", which does not gate (see ota_gate_in_t). The
   battery and charge-lock facts are sampled HERE, on the main task,
   because the network task must not touch the ADC or the lock gate. */
void ota_flow_arm(ota_trigger_t trigger, int batt_pct, bool charge_locked);

/* Network task, inside the window: gate, GET the manifest, decide, and
   buffer an update for the main task. Placed after the snapshot
   rendezvous (the panel is idle by then, which is the brownout
   condition) and before mqtt_ha_window (so a failure reaches Home
   Assistant in the same window that produced it). No-op unless armed. */
void ota_flow_check(bool time_valid);

/* Main task, after net_window_join: did the check buffer an update? */
bool ota_flow_pending(void);

/* Main task, at the late pre-sleep point, with the first window CLOSED:
   paint, extend the failsafe, open the second window, download, commit,
   reboot. Returns without opening anything when nothing is pending, so
   the common path costs one comparison.

   The battery facts are sampled AGAIN by the caller rather than carried
   over from ota_flow_arm: the two windows are minutes and a full-panel
   repaint apart, and a cell that has crossed into the charge lock in
   between is exactly the one that must not be asked for a sustained
   radio burst followed by a flash write. The clock is not re-sampled —
   a pending update implies the check gate passed, and a clock, once
   set, stays set for the boot.

   Does not return on success — the device reboots into the new image. */
void ota_flow_apply(int batt_pct, bool charge_locked);

/* Last download's wall time, 0 if none this boot. The stat payload
   publishes it so a link trending toward the deadline is visible before
   it becomes chronic. */
uint32_t ota_flow_last_dl_ms(void);

#ifdef __cplusplus
}
#endif
