#pragma once

/* Reset-loop soak harness: reproduce the boot panic in minutes instead of
   waiting for nightfall.
   ========================================================================

   WHY. The device panics on its own roughly three times a night and the
   breadcrumb says BOOT. Waiting for the next occurrence costs hours per
   observation, which is the wrong loop length for a bisect. Roughly
   10-15 % of boots fail, so a tight reset loop should hit one inside a
   minute: the same code, at maximum rate, with a console attached.

   WHAT IT DOES. When MAGTAG_PANIC_SOAK is 1, app_main runs the entire
   BOOT window exactly as it always does and then, at the last statement
   before panic_diag_enter(PANIC_PHASE_AWAKE), logs a marked line, waits
   MAGTAG_PANIC_SOAK_SETTLE_MS and calls esp_restart(). Nothing of the
   wake runs. Every iteration re-exercises NVS init, the OTA rollback
   detector, the ADC, the panel bring-up with lv_init() and the charge
   gate, which is the span the breadcrumb points at.

   ---- why a plain #define and not a Kconfig symbol --------------------

   A Kconfig symbol is the obvious answer and it is the wrong one HERE.
   This project's `sdkconfig` is gitignored and hand-maintained: it
   carries MQTT credentials, the timer names and CONFIG_MAGTAG_PARENT_-
   TESTING=n, none of which exist anywhere else in the tree and none of
   which survive a regeneration. Adding a symbol means a reconfigure, and
   a reconfigure means rewriting that file. The whole feature is worth
   less than that risk.

   So: one integer, in one header, flipped by hand. It is the least
   clever thing that works, and the flip is visible in `git diff` rather
   than buried in a build directory. `#if` rather than `if (...)`, so an
   ordinary build cannot reach the code at all - it is not merely
   unreachable, it is not compiled.

   ---- what the evidence looks like ------------------------------------

   The loop never reaches AWAKE, so wake_flow_handle_wake() never runs:
   no network window, no MQTT, and therefore NOTHING IS PUBLISHED. The
   HA entities this firmware normally reports the breadcrumb through are
   silent for the whole soak. Two things stand in for them:

     - THE CONSOLE. The panic backtrace itself, which this build prints
       (CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y, a 3 s reboot delay, and
       CONFIG_ESP_CONSOLE_USB_CDC_SUPPORT_ETS_PRINTF=y so it survives USB
       CDC), plus the "PANIC #n: phase ..." line that panic_diag_commit()
       logs on the boot AFTER a panic. That second line is the whole
       point of the exercise: it is the subdivided phase, printed, one
       boot later, without needing a network at all.

     - NVS. panic_diag_commit() still bumps `panic_cnt` and still files
       the breadcrumb blob, because esp_restart() reports ESP_RST_SW and
       only a genuine ESP_RST_PANIC moves either. The counter therefore
       measures real panics per soak, not iterations, and the last one is
       readable over MQTT on the first normal wake after the flag goes
       back to 0.

   Note the timing interaction, which is not a bug: wake_flow_boot_quiet_-
   after_panic() holds the console quiet for PANIC_QUIET_MS (2000 ms) at
   the top of every post-panic boot. The commit log lands after that
   delay, so it is visible; the delay is simply added to that iteration.

   ---- the settle delay ------------------------------------------------

   Without it the loop free-runs at whatever a boot costs and the console
   becomes unreadable. It also throttles the panel: display_init() runs
   on EVERY iteration (it is inside the window being tested), so the
   SSD1680 is brought up each time. ON A HEALTHY BATTERY that is all that
   happens - display.c's render() path is not reached before AWAKE, so
   the panel is initialised rather than refreshed, and init does not age
   the e-ink film the way a refresh does. The delay is there so an
   operator can read the log and interrupt the loop, not to protect the
   display.

   ---- RUN IT ON USB POWER, WITH A CHARGED BATTERY ---------------------

   The paragraph above holds only ABOVE the charge-lock threshold, and it
   is worth stating plainly because an earlier draft of this header
   claimed no refresh happens at all. One does, and it is inside the
   window under test. lock_gate_check_charge() is boot sub-phase 5 of 5.
   At or below BATT_LOCK_PCT (10 %, main/battery_policy.c) it takes the
   locked path in lock_gate.c: pause the timer, paint Charge Me! with a
   FULL e-ink refresh, open an entire network window, then deep-sleep for
   CHARGE_LOCK_SLEEP_SEC (600 s). It DOES NOT RETURN.

   The operational consequence is not the refresh, it is the silence: the
   `#if MAGTAG_PANIC_SOAK` block sits at the END of app_main, past that
   call, so on a low battery IT IS NEVER REACHED. The soak does not stop
   and does not say why. It quietly becomes a 10-minute deep-sleep cycle,
   which from the console is indistinguishable from a soak that hung.

   The refresh itself is a one-off, and the reason is the same sentence:
   because the locked path never returns, this harness's esp_restart()
   never runs while the lock is engaged, so the `if (!s_charge_locked)`
   guard is only ever reached with the flag already false. s_charge_locked
   is RTC_DATA_ATTR and IS zeroed by esp_restart() - but every path that
   reaches the restart left it false anyway, and the deep-sleep wakes that
   follow a lock preserve RTC_DATA_ATTR. So: one paint, then stall. Charge
   the battery and leave it on USB, and neither happens.

   ---- the cost to be aware of -----------------------------------------

   Each REAL panic in the loop costs one NVS counter write plus one blob
   write (the write-amplification item deferred under BUG-10 in
   docs/planning/refactor.bugdiscoveries.md). A soak that runs for an
   hour at ~10 % panic rate is on the order of a few hundred writes, not
   the ~68k/day the deferred note worried about. A soak left running
   overnight is a different matter - don't.

   EVERY ITERATION ALSO THROWS AWAY THE TIMER STATE, unflushed.
   g_rtc_state is RTC_DATA_ATTR: it survives deep sleep and is zeroed by
   esp_restart(), and it carries the running slot, the banked bonus and
   last_date. The firmware knows this - the OTA path is wired with
   .persist_state = timer_persist_save precisely so the snapshot is
   flushed before its own esp_restart() (see main.c's OTA_FLOW_OPS and
   ota_flow.h for what a stale snapshot costs). THIS HARNESS DOES NOT DO
   THAT, DELIBERATELY: adding a flush would put an NVS write on every
   iteration of a loop whose write cost is the item above, and would
   change the state the boot window is being measured against. What it
   means for the operator is that a soak discards whatever the device was
   timing, back to the last persisted snapshot, and that
   timer_persist_try_restore() takes the NVS-fallback branch on every
   iteration rather than the RTC one. Don't soak a device that is
   mid-session, and don't read the timer afterwards as evidence.

   ---- HOW TO USE ------------------------------------------------------

     0. Put the device on USB power with a CHARGED battery - above 15 %.
        Below 10 % the charge lock swallows the loop; see above.
     1. Change the 0 below to 1, by hand.
     2. Rebuild and flash over USB, with a serial monitor attached.
     3. Watch for a backtrace, then for the "PANIC #n: phase BOOT_*" line
        on the following boot.
     4. Change it back to 0 before committing anything else. It ships at
        0, always. */
#define MAGTAG_PANIC_SOAK 0

/* Milliseconds between the end of one boot window and the reset that
   starts the next. 1500 ms keeps the console readable at roughly one
   iteration every 2-3 s (boot itself is the rest) while still fitting
   ~20-30 boots into a minute, which at a 10-15 % failure rate is two to
   four panics. Raise it if the log scrolls faster than you can read;
   lowering it below ~1000 ms buys little, because the boot window and
   the 3 s panic reboot delay dominate. */
#define MAGTAG_PANIC_SOAK_SETTLE_MS 1500
