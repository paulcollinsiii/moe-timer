/* Reproduction: break_eligible reset survives the BUG-6 fix when the
   timer-defs blob is LOST (NVS erase / fresh blob), because
   timer_defs_install() materializes a Kconfig blob BEFORE the retained
   HA config document is applied. */
#include <stdio.h>
#include <string.h>

#include "bedtime.c"
#include "cJSON.h"
#include "config_apply.c"
#include "config_validate.c"
#include "mock_hal_nvs.c"
#include "nvs_config.c"
#include "quiet_hours.c"
#include "tones.c"

/* Faithful transcription of main/timer_defs.c timer_defs_install()'s
   materialization branch, with a Kconfig table of the shape the device
   ships: slot1 "Piano" 15min, slot2 "Meditation" 10min, both
   BREAK_ELIGIBLE=n (Kconfig default is `n` for all four slots). */
static void timer_defs_install_materialize(void) {
    nvs_timer_defs_blob_t blob;
    if (nvs_config_get_timer_defs(&blob) != ESP_OK) {
        memset(&blob, 0, sizeof(blob));
        blob.version = TIMER_DEFS_BLOB_VERSION;
        snprintf(blob.defs[0].name, sizeof(blob.defs[0].name), "%s", "Piano");
        blob.defs[0].min = 15;
        blob.defs[0].reload = 0;
        blob.defs[0].break_eligible = 0; /* CONFIG_MAGTAG_TIMER1_BREAK_ELIGIBLE default n */
        snprintf(blob.defs[1].name, sizeof(blob.defs[1].name), "%s", "Meditation");
        blob.defs[1].min = 10;
        blob.defs[1].reload = 0;
        blob.defs[1].break_eligible = 0;
        /* slots 3/4: Kconfig name empty -> stay disabled */
        nvs_config_set_timer_defs(&blob);
    }
}

static void show(const char *when) {
    nvs_timer_defs_blob_t b;
    if (nvs_config_get_timer_defs(&b) != ESP_OK) {
        printf("  %-34s <no blob>\n", when);
        return;
    }
    printf("  %-34s", when);
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++)
        if (b.defs[i].name[0])
            printf(" [%s min=%d reload=%d break=%d]", b.defs[i].name, (int)b.defs[i].min, b.defs[i].reload,
                   b.defs[i].break_eligible);
    printf("\n");
}

/* The retained HA config document as it exists on a broker that was set up
   before the docs gained `break` -- documentation-shaped, no break key. */
static const char *RETAINED_DOC =
    "{\"ver\":\"20260708\",\"timers\":["
    "{\"name\":\"Piano\",\"min\":15,\"reload\":true},"
    "{\"name\":\"Meditation\",\"min\":10,\"reload\":true}]}";

int main(void) {
    char ack[256];
    nvs_timer_defs_blob_t b;

    printf("\n=== A: steady state, blob intact (BUG-6 fix works) ===\n");
    mock_nvs_reset();
    timer_defs_install_materialize();
    /* operator flips both break switches ON in HA */
    nvs_config_get_timer_defs(&b);
    b.defs[0].break_eligible = 1;
    b.defs[1].break_eligible = 1;
    nvs_config_set_timer_defs(&b);
    show("after HA switch edits:");
    config_apply(RETAINED_DOC, ack, sizeof(ack));
    show("after retained doc applies:");

    printf("\n=== B: blob LOST (NVS erase / panic), same doc ===\n");
    mock_nvs_reset(); /* NVS wiped: nvs_flash_erase() path */
    show("boot, before timer_defs_install:");
    timer_defs_install_materialize(); /* main.c:315 -- WRITES Kconfig blob */
    show("after timer_defs_install:");
    config_apply(RETAINED_DOC, ack, sizeof(ack));
    show("after retained doc applies:");

    nvs_config_get_timer_defs(&b);
    printf("\nRESULT: Piano break=%d  Meditation break=%d  (operator had set both to 1)\n", b.defs[0].break_eligible,
           b.defs[1].break_eligible);
    printf("%s\n", (b.defs[0].break_eligible == 0 && b.defs[1].break_eligible == 0)
                       ? "*** REPRODUCED: both flags silently reset to Kconfig default ***"
                       : "not reproduced");

    printf("\n=== C: same loss, but doc carries \"break\":true ===\n");
    mock_nvs_reset();
    timer_defs_install_materialize();
    config_apply(
        "{\"ver\":\"20260708\",\"timers\":["
        "{\"name\":\"Piano\",\"min\":15,\"reload\":true,\"break\":true},"
        "{\"name\":\"Meditation\",\"min\":10,\"reload\":true,\"break\":true}]}",
        ack, sizeof(ack));
    show("after retained doc applies:");
    return 0;
}
