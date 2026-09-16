/* Snapshot save/restore policy, moved out of main.c. Policy only: the
   snapshot's shape and its validation belong to timer.c, the blob's
   storage to nvs_config.c. What lives here is the pair of decisions —
   when a write is worth the flash wear, and when a stored day may
   overrule the live RTC state. */
#include "timer_persist.h"

#include <string.h>

#include "chore_store.h"
#include "chores.h"
#include "date_fmt.h"
#include "nvs_config.h"
#include "timer.h"

#ifndef NATIVE
#include "esp_log.h"
#else
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif

static const char *TAG = "timer_persist";

void timer_persist_save(void) {
    timer_snapshot_t snap, stored;
    timer_make_snapshot(&snap);
    /* Short-circuit order is load-bearing: a load that fails (missing,
       truncated, stale layout) leaves `stored` partly or wholly
       uninitialised, so the memcmp must not run — comparing against
       unspecified stack bytes would decide "unchanged" at random. */
    if (nvs_config_load_timer_snapshot(&stored) == ESP_OK && memcmp(&snap, &stored, sizeof(snap)) == 0) {
        return;
    }
    esp_err_t ret = nvs_config_save_timer_snapshot(&snap);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "snapshot save failed: %s", esp_err_to_name(ret));
    }
}

bool timer_persist_try_restore(time_t now) {
    if (timer_current_date()[0] != '\0')
        return false; /* RTC state intact — normal deep-sleep wake */
    timer_snapshot_t snap;
    if (nvs_config_load_timer_snapshot(&snap) != ESP_OK)
        return false;
    if (!timer_restore_snapshot(&snap, now))
        return false;
    /* Hoisted out of the log argument (HAZ-1). The (void) is not
       decoration: the host stub above discards its varargs, so `st` would
       have no reader at all here and -Wall would report it as an unused
       variable. On device the compiled-out form still references it, so
       the cast is for the host build alone. */
    const timer_state_t st = timer_get_state();
    (void)st;
    ESP_LOGW(TAG, "Timer state restored from NVS snapshot, state=%d", (int)st);

    /* C14, and it belongs HERE rather than in a boot block of its own:
       this branch IS "RTC memory was lost and the day came back from
       flash", which is the one arrangement where a re-armed gate does
       damage. The day's allocation has just been restored already
       carrying whatever the release granted, so leaving chore_released
       false lets the same withheld remainder be granted a SECOND time —
       measured at 100 minutes on a 60-minute day, repeatable per reset,
       with adjust_today_sec still 0 (the state timer.h calls impossible).

       Deliberately NOT on the failure path above. There the caller resets
       the day to a fresh full allocation and the gate is SUPPOSED to be
       armed: the withheld part is withheld again and one release hands it
       back, so the day still totals one allocation and nothing is farmed.

       Ordering against timer_rtc_state_guard() — the one hard requirement
       in timer_persist.h, because the guard memsets g_rtc_state when it
       rejects an image — is satisfied by construction: this function is
       only reached with last_date empty, and on the esp_restart path that
       is the guard's own doing, two calls earlier in app_main.

       The return is discarded because there is nothing to do with it. A
       false means "no usable record" and leaves g_rtc_state untouched,
       which is the right answer for a device that has never had a chore
       configured; it must NOT be turned into a restore failure, because
       the timer day genuinely did come back. */
    char names[CHORE_MAX][CHORE_NAME_BUF];
    uint8_t n = 0;
    /* Return discarded on the same terms button_actions.c discards it:
       every failure path in chore_store_load_names() sets n = 0 first, so
       an unreadable names blob hashes as the empty list. That mismatches
       the stored hash and takes chores_reconcile()'s C10 arm, which
       clears the acks but PRESERVES `released` — so the one thing a
       transient NVS fault on this key cannot do is re-arm the gate. */
    (void)chore_store_load_names(names, &n);
    (void)timer_persist_restore_chore_acks(now, chores_list_hash(names, n));
    return true;
}

/* Lives here rather than in timer.c, where the two RTC fields it writes
   live, because it CALLS chore_store_load_ack(). main/timer.c is compiled
   into nine host suites and eight of them link no chore_store.c —
   test_timer, test_app_state, test_button_actions, test_net_apply,
   test_cmd_apply, test_config_apply, test_timer_defs, test_ha_config — so
   putting the call in timer.c ends in eight "undefined reference to
   chore_store_load_ack" link failures unless every one of those suites
   grows a stub. (test_wake_flow is NOT one of them: it does not compile
   timer.c at all.) timer_persist.c already includes both sides, which is
   what this file is for. */
bool timer_persist_restore_chore_acks(time_t now, uint16_t current_hash) {
    /* The date is DERIVED from the caller's `now` rather than taken as a
       string, and that IS the contract — see timer_persist.h, IT TAKES A
       time_t AND NOT A DATE STRING. Same idiom as timer_record_date() and
       timer_restore_snapshot(), so the day the record is matched against
       is the day every other date comparison in the firmware computes
       from the same `now`.

       No date guard here, and none can be needed: date_fmt_iso()
       NUL-terminates into this 11-byte buffer and its format cannot
       render fewer than ten characters, so `today` is always a
       ten-character date and every one of chore_store_load_ack()'s date
       refusals — NULL, short, overlong — is unreachable from this caller.
       The NULL guard this function used to carry went with the string
       parameter that made it reachable. */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[11];
    date_fmt_iso(today, sizeof(today), &tm_now);

    chore_ack_t ack;
    /* Anything but ESP_OK means the record could not be believed at all —
       never written, or a foreign layout — and the live RTC copy is then
       a better answer than zeros, so it is left alone. ESP_OK covers the
       rollover case too (a cleared record for another day), and writing
       those zeros IS correct there: a new day starts locked (C13). */
    if (chore_store_load_ack(today, current_hash, &ack) != ESP_OK) {
        return false;
    }
    timer_chore_set_acked(ack.acked);
    timer_chore_set_released(ack.released);
    return true;
}
