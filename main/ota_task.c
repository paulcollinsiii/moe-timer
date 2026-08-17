/* The download's own task — task lifecycle only, no sequencing. What the
   download DOES is ota_flow.c's; this file exists so that ota_flow.c can
   stay a host-tested pure sequence with no FreeRTOS in it, exactly as
   net_window.c holds the network window's task mechanics apart from
   net_apply.c's orchestration. Why it must not run on the main task is
   on the declaration in ota_task.h. */
#include "ota_task.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ota_flow.h"

static const char *TAG = "ota_task";

/* 16 KB, the top of the plan's 12-16 KB range, because the two things
   that land on this stack are both hard to bound from the source:
   mbedTLS's record handling during the handshake, and display_ota() ->
   LVGL, which ota_flow_apply paints from here before the radio comes up.
   The watermark log below is what turns this into a measurement; do not
   cut it on a guess. */
#define OTA_TASK_STACK 16384

/* Same priority as net_win (net_window.c:127), and for the same reason:
   above the main task (CONFIG_ESP_MAIN_TASK_PRIORITY = 1) so the socket
   work is not scheduled behind it. The main task is blocked on the
   semaphore below for the whole download anyway, so the number only
   decides who runs against the IDF service tasks. */
#define OTA_TASK_PRIO 3

/* By value through the task argument rather than through statics: the
   facts were sampled on the main task (ota_task.h says why) and this is
   the handover. The caller blocks until `done` is given, so this struct
   lives on a stack frame that cannot go away underneath the task. */
typedef struct {
    int batt_pct;
    bool charge_locked;
    SemaphoreHandle_t done;
} ota_task_arg_t;

static void ota_apply_task(void *arg) {
    const ota_task_arg_t *a = (const ota_task_arg_t *)arg;
    /* Copied out before the flow runs. Nothing below may touch `a`: the
       moment the semaphore is given the caller is free to return, and
       with it the frame `a` points into. */
    int batt_pct = a->batt_pct;
    bool charge_locked = a->charge_locked;
    SemaphoreHandle_t done = a->done;

    ota_flow_apply(batt_pct, charge_locked);

    /* Only a declined, failed or aborted attempt reaches this line — a
       committed image restarts from inside ota_flow_apply and never
       comes back. So the watermark below is the FAILURE path's floor,
       which is the pessimistic one anyway: it has been through the
       manifest-sized TLS session, the repaint, and however many chunks
       landed before the failure. */
    ESP_LOGI(TAG, "ota task stack floor: %u B free", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

bool ota_task_run_apply(int batt_pct, bool charge_locked) {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == NULL) {
        ESP_LOGE(TAG, "ota task semaphore alloc failed - skipping the download");
        return false;
    }
    ota_task_arg_t arg = {.batt_pct = batt_pct, .charge_locked = charge_locked, .done = done};
    if (xTaskCreate(ota_apply_task, "ota_dl", OTA_TASK_STACK, &arg, OTA_TASK_PRIO, NULL) != pdPASS) {
        /* A 16 KB stack is the largest single allocation this firmware
           ever asks for, so this is a real outcome on a fragmented heap
           rather than a formality. Nothing was attempted and nothing was
           painted; the update stays undone and the next check finds it
           again. Deliberately NOT recorded to NVS as a failure: no
           attempt was made, and charging the retry budget for a heap
           condition would eventually give up on a perfectly good image.  */
        ESP_LOGE(TAG, "ota task create failed (%d B stack) - skipping the download", OTA_TASK_STACK);
        vSemaphoreDelete(done);
        return false;
    }
    /* Unbounded on purpose — ota_task.h, "what this function owes the
       caller". The awake failsafe is the bound. */
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return true;
}
