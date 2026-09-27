#pragma once
#include <stdbool.h>

/* The download's own task. One function, and the reason it exists is a
   hardware constraint rather than a preference.

   ota_flow_apply() runs esp_https_ota_perform() — mbedTLS record buffers
   plus the flash write path — on whatever stack calls it.
   CONFIG_ESP_MAIN_TASK_STACK_SIZE is 7168 B (sdkconfig.defaults), which
   does not fit; net_window.c already spawns a dedicated 10240 B task for
   a strictly smaller job. Overflowing here presents on the bench as an
   unexplained reboot, because USB CDC eats the panic output — so the
   task logs uxTaskGetStackHighWaterMark() the way net_window.c does, and
   the size below is meant to be tuned from that number.

   This file is the ONLY reason ota_flow.c does not have to know FreeRTOS
   exists: the flow stays host-testable with no ESP-IDF present, and the
   task mechanics sit next to it in the same shape net_window.c uses for
   the network window. Like net_window.c it has no host suite — there is
   nothing in it that is not FreeRTOS.

   ---- what the caller owes this function ----

   Sample batt_pct and charge_locked ON THE MAIN TASK and pass them in.
   The ADC and the lock gate are main-task concerns, and re-sampling them
   is the whole reason ota_flow_apply takes them as arguments (see
   ota_flow.h). Check ota_flow_pending() first: this call is not free —
   it allocates a 16 KB stack — and the flow's own "nothing pending"
   early-out would pay for that allocation on every wake to answer one
   comparison.

   ---- what this function owes the caller ----

   It blocks until the download has finished, failed, or been declined by
   the second gate, and it does NOT return at all when the update
   commits: ota_flow_apply reboots into the new image from inside the
   spawned task. The wait is deliberately unbounded. A wedged download is
   bounded by the awake failsafe, which is the layer that owns "this wake
   has gone on too long"; a timeout here would only let the main task run
   ahead into deep sleep while the spawned task was still writing flash,
   which is strictly worse than being force-slept by the failsafe.

   Returns false when the task could not be created (and nothing was
   attempted), true otherwise. */

#ifdef __cplusplus
extern "C" {
#endif

bool ota_task_run_apply(int batt_pct, bool charge_locked);

#ifdef __cplusplus
}
#endif
