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
   window under test. lock_gate_check_charge() is boot sub-phase 6 of 6.
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

/* The OTHER soak, and why it is a second switch rather than a mode of
   the first one.
   ========================================================================

   WHAT IT DOES. When MAGTAG_PANIC_SOAK_FAST_LOCKS is 1, EXACTLY ONE
   constant changes: BEDTIME_SLEEP_SEC in include/sleep_plan.h goes from
   7200 s to 90 s. No restart, no truncated wake, no skipped work, and no
   other interval - the device engages the bedtime lock exactly as it
   always does and then re-wakes on the short cadence, running the whole
   locked path each time. A bedtime-locked re-wake therefore happens
   every minute and a half instead of every two hours.

   CHARGE_LOCK_SLEEP_SEC IS NOT TOUCHED, and an earlier draft of this
   header shortened it alongside the bedtime one. It has nothing to
   contribute: lock_gate_check_charge() calls net_apply_try_window() only
   inside its `if (!s_charge_locked)` engage branch, and s_charge_locked
   is RTC_DATA_ATTR, so a charge-locked RE-wake opens no network window
   at all - there is no window here to soak. Shortening it only bought a
   device at or below 10 % battery a full boot every 90 s instead of
   every 600 s, ~6.7x the wake rate of the mechanism that exists to stop
   draining a nearly-dead battery, and it silently shortened the
   fail-closed fallback in sleep_plan_outcome() by the same factor.

   WHY. The reset loop above did not reproduce the nightly panic - 18
   iterations, zero panics - and the reason is readable in the code
   rather than a mystery. Two things are wrong with it as a model of the
   failure. esp_restart() reports ESP_RST_SW with an undefined wake cause
   and ZEROES every RTC_DATA_ATTR, so each iteration boots against a
   blank g_rtc_state, while every real panic is a genuine deep-sleep wake
   with that state intact. And the `#if MAGTAG_PANIC_SOAK` block sits at
   the END of app_main, before wake_flow_handle_wake() has run, so the
   loop never reaches the one thing the nightly cluster has in common.

   That thing is lock_gate_check_bedtime(). On a bedtime-locked re-wake
   it calls net_apply_try_window() UNCONDITIONALLY (main/lock_gate.c),
   whereas the daytime path opens a window only when wake_policy says a
   sync is due. Overnight, then, essentially every wake runs a full
   network window and by day only a percent or two do - which is the
   shape of a panic cluster that appears after bedtime and nowhere else.
   The bedtime-locked re-wake is the event to reproduce, and the only
   thing making it slow to reproduce is the two-hour sleep between them.

   ---- ORTHOGONAL TO MAGTAG_PANIC_SOAK, AND EXCLUSIVE WITH IT ----------

   The two flags are independent #defines on purpose: they exercise
   different populations and neither is a mode of the other. They must
   not both be 1. The reset loop fires at the end of the BOOT window,
   which is BEFORE wake_flow.c reaches the bedtime check, so with both
   set the restart pre-empts the scenario this flag exists to create and
   the shortened sleep is never reached at all - a soak that looks like
   it is running and is measuring the wrong thing. Enable one, or the
   other, never both. The #error below enforces it, rather than leaving
   the rule as prose an operator flipping a second #define never reads:
   this file's whole failure mode is a hand-edited constant, and the rest
   of the feature pins its invariants at compile time too (the label
   budget and phase-table guards in main/panic_diag.c).

   ---- HOW TO USE ------------------------------------------------------

     0. USB power, charged battery, serial monitor - the same
        preconditions as the reset loop, and partly for a new reason:
        below BATT_LOCK_PCT the charge lock wins the precedence in
        wake_sleep_mode_select(), and you would be sitting in that lock
        (still on its unshortened 600 s) instead of the bedtime one.
     1. Change the 0 below to 1, by hand. Rebuild and flash.
     2. From Home Assistant, set Bed Time to a time that has ALREADY
        PASSED today. bedtime_active() is `now_min >= bed_min` and never
        wraps midnight, so the lock is active from HHMM until midnight
        and any earlier HHMM engages it on the next wake. (The validator
        accepts 0 or 1800-2359, so "already passed" means after 18:00
        local; before then, wait or move the device clock.)
     3. That wake engages the lock: timer paused, one full Bed Time
        repaint, one network window, deep sleep. From there the device
        re-wakes, opens a FULL network window and re-checks the gate -
        which is the span under investigation.

        The CYCLE is not the 90 s: it is 90 s of sleep plus a whole awake
        window on top. On a healthy network that window is a WiFi assoc,
        an SNTP settle and a short MQTT session - call it 10-20 s, for a
        ~100-110 s period, about 65x the unsoaked bedtime cadence. When
        the window cannot join it runs to NET_JOIN_TIMEOUT_MS (90 s) and
        the wake is bounded by CONFIG_MAGTAG_MAX_AWAKE_SEC (180 s), so
        the period stretches to ~270 s - about 27x. Budget the run
        against the slow end: 27-65x, not a single figure.

        (The 90 s sleep and the 90 s NET_JOIN_TIMEOUT_MS are the same
        number by coincidence. The sleep was picked as "short enough to
        watch, long enough to be a real deep-sleep cycle"; nothing
        derives one from the other, and changing either leaves the other
        alone.)
     4. TO GET OUT, SET BED TIME IN HA TO 0 - or, if you want the lock to
        re-engage later tonight, to an HHMM STRICTLY LATER than the
        current local time. Re-setting it to just any value does NOT
        release: bedtime_active() is `bed_min >= 0 && now_min >= bed_min`,
        so every HHMM at or before now leaves the lock engaged, and the
        validator only accepts 0 or 1800-2359 - which means the "later
        than now" escape exists only between 18:00 and 23:59, and shrinks
        as the evening goes on (at 23:00 you have 2301-2359 and nothing
        else). 0 is the reliable exit at any hour; use it unless you
        deliberately want another engage tonight.

        Whichever value you pick, it has to arrive over MQTT: buttons are
        dark for the whole lock (sleep_plan_outcome sets enable_buttons
        false on both locked modes), so a config edit picked up by one of
        those network windows is the only exit short of a USB reflash -
        and shortening the sleep is exactly what makes that edit land in
        ~2 min instead of up to 2 h. Day rollover clears the lock too, if
        waiting for midnight is easier.
     5. Change it back to 0 before committing anything else. It ships at
        0, always - and the host suite enforces that: test_sleep_plan
        pins both knobs to 0 and asserts the production 7200, so a flag
        left flipped fails the suite rather than reaching a device. */
#define MAGTAG_PANIC_SOAK_FAST_LOCKS 0

/* The "never both" rule above, made unskippable. Prose cannot stop the
   one thing that goes wrong here - somebody hand-flips a second #define
   - so the build refuses instead. With both set, the `#if
   MAGTAG_PANIC_SOAK` block near the end of app_main restarts the device
   BEFORE wake_flow_handle_wake() runs, and the bedtime gate is reached
   from inside that handler: the reset loop pre-empts the bedtime
   scenario entirely, the shortened BEDTIME_SLEEP_SEC is never reached,
   and the operator watches a soak that looks healthy while measuring the
   other flag's population. */
#if MAGTAG_PANIC_SOAK && MAGTAG_PANIC_SOAK_FAST_LOCKS
#error \
    "MAGTAG_PANIC_SOAK and MAGTAG_PANIC_SOAK_FAST_LOCKS are mutually exclusive: the reset loop restarts before wake_flow_handle_wake() reaches the bedtime gate, so the fast-lock soak would never run its scenario. Enable one, or the other, never both."
#endif
