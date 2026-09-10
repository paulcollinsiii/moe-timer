# Hardware Smoke Test — MagTag Screen Timer

Run in order with the MagTag on USB and `idf.py -p /dev/ttyACM0 flash monitor`
(activate the toolchain first: `source ~/esp/esp-idf/export.sh`).

Prerequisite: real WiFi credentials in NVS. Set `NVS_DEFAULT_WIFI_SSID/PASS` in
`include/nvs_defaults.h` temporarily, or pre-write NVS. **Do NOT commit
credentials.**

1. [ ] **Flash + cold boot**: monitor shows boot, `Wakeup causes: 0x0`, no panics.
2. [ ] **First screen**: IDLE layout renders — date/time header, full bar,
       allocation + `IDLE` footer. If garbled/blank, note the symptom:
       - all-black or inverted → flip the `!bit` test in `main/display.c` flush_cb
       - rotated/mirrored → toggle `ROT_FLIP_X` / `ROT_FLIP_Y` in `main/display.c`
       - image shifted 8 px along the short axis → adjust `SSD1680_XRAM_OFFSET`
         in `components/ssd1680/include/ssd1680.h` (0 or 1)
       - weak/faded full refresh → switch the driver to Adafruit's custom-LUT
         init (see the comment at the top of `components/ssd1680/ssd1680.c`)
3. [ ] **55 s tick**: device deep-sleeps, wakes ~55 s later, partial refresh
       (no black/white flash).
4. [ ] **Anti-ghosting**: every 5th wake does a full refresh (visible flash).
5. [ ] **Button A (start)**: timer starts immediately (state pixel WHITE ->
       GREEN after ~250 ms), then WiFi joins and SNTP syncs (WiFi pixel blue);
       header shows sync time, bar full, state `RUNNING`. With bad WiFi creds:
       timer still starts (fail-open), WiFi pixel blinks red 3x, remaining
       time counts down on the uncorrected clock.
6. [ ] **Countdown**: remaining decreases ~55 s per wake, shown as
       `HH:MM:SS`.
7. [ ] **Button A (pause/resume)**: pause shows `PAUSED`, remaining freezes
       across wakes; resume is immediate (AMBER -> GREEN after ~250 ms, sync
       after) and continues from the frozen value.
8. [ ] **Button B (reset)**: with the timer PAUSED (or expired), returns to
       IDLE with today's full allocation. While RUNNING, B is dropped from
       the wake mask — pressing it does nothing (no wake, no refresh).
9. [ ] **Button D (force sync)**: WiFi cycle + full refresh; sync time updates.
10. [ ] **Button C**: with no extra timers configured (the default), does
        nothing at all — not a wake source (kept out of the EXT1 mask so
        mashing it cannot burn battery or refreshes). With an extra timer
        configured (`MAGTAG_TIMER1_NAME` etc.), swaps the selected timer.
        While a timer is RUNNING or in a Screen Break, C is dropped from
        the wake mask entirely — pressing it does nothing (no wake, no
        refresh) until the timer is paused.
11. [ ] **Expiry**: temporarily lower `NVS_DEFAULT_WEEKDAY_MIN` to 1-2 min (and
        erase NVS: `idf.py erase-flash`), let it expire: TIME'S UP screen,
        3 beeps x 5 cycles, red NeoPixel pulse; any button stops the alert
        immediately; device returns to deep sleep after.
12. [ ] **Day rollover**: only a genuine date change resets the allocation.
        Power cycling does NOT fake it — the boot re-syncs, restores the
        same-day NVS snapshot, and the countdown continues (see case 19).
        To test a real rollover, erase the NVS region so no same-day
        snapshot exists (`python -m esptool --chip esp32s2 erase-region
        0x9000 0x6000`) or wait past midnight: wake re-syncs and resets to
        IDLE with the new day's allocation.
13. [ ] **Wake buttons**: A and D always wake the device. B and C wake only
        when their press would succeed (the EXT1 mask is rebuilt at every
        sleep entry): B needs a reloadable selected timer and never wakes
        mid-run (case 8); C needs extra timers configured and no
        RUNNING/BREAK (case 10).
14. [ ] **Panel protection**: mash buttons rapidly — refreshes serialize, log
        shows `refresh rejected` if under 1 s apart, no crash.
15. [ ] **Held-button dismissal**: dismiss the expiry alert while *holding*
        each button (also try holding two at once) — the hold must not
        re-fire the button's action. enter_deep_sleep waits up to 3 s for
        release, then sleeps with the held mask recorded; instant re-wakes
        by a recorded button log `still held from previous wake - ignoring`
        and loop harmlessly until release.
16. [ ] **Buttons after first sleep cycle**: after at least one full
        sleep/wake cycle, verify buttons still read correctly during an alert
        (guards the RTC pad-hold release in buttons_init).
17. [ ] **Speaker pin**: confirm beeps come from the speaker (GPIO 17 carries
        a `verify against schematic` note in audio.c).
18. [ ] **Idle current** (optional, needs a meter): deep-sleep current < 1 mA.
        GPIO 21 (NeoPixel gate) and GPIO 16 (amp enable) are held through
        sleep — if current is high, probe those nets first.
19. [ ] **Crash recovery**: with the timer RUNNING, press the reset button
        or power-cycle. On the next boot (after the boot sync corrects the
        clock, for power-on) the log shows `Timer state restored from NVS
        snapshot` and the countdown continues — the allocation is NOT
        refunded. Refunding the Screen allocation requires a genuine day
        rollover — Button B reloads only a reloadable extra (case 20). With
        WiFi unavailable on a power-on the restore cannot validate (no
        clock) and the device fails open to IDLE. Note: EN reset mid-run
        (before expiry) intentionally restores the in-flight countdown —
        that is crash recovery, not a refund; the run resumes with the
        remaining time it had.
20. [ ] **Screen reset gate**: with Screen selected, a plain press of
        Button B does nothing at all — no wake, no refresh, and no log
        line. That is correct, not a fault: Screen carries no def, so it is
        never reloadable, B is dropped from the EXT1 mask (case 13) and the
        driver leaves the pad unconfigured, so a press on a sleeping device
        is never seen. To observe the refusal itself, hold B down through a
        periodic 55 s tick wake so the press rides in on that wake's latch —
        the log then shows `Button B reset unavailable` and nothing resets;
        the Screen allocation resets only on a day rollover. Swap to a
        reloadable extra (not RUNNING) and the same press does reset that
        timer to full.
21. [ ] **Final-minute countdown**: the pre-expiry wake lands ~70 s out
        (planner); the display then steps through 00:01:00 / 00:00:45 /
        00:00:30 / 00:00:15 as partial refreshes, the last 15 s count down
        on the four pixels in binary (light green, dim; dark during quiet
        hours), and TIME'S UP + red pulse + beeps fire within ~1 s of the
        expiry wall time. Pressing A anywhere in the final minute pauses
        instead (PAUSED full refresh, no alarm) — presses are ISR-latched,
        so even a quick tap DURING one of the quarter-mark partial
        refreshes registers and pauses as soon as the flush completes;
        expiry only fires if the countdown truly reaches zero. Quick taps
        also dismiss the TIME'S UP / break alarms reliably.
22. [ ] **Battery gauge**: log shows `battery: N mV (P%)` each wake; on USB
        ~4300+ mV -> 100%, on LiPo 3300-4200 mV with a plausible %. Icon +
        percent render left of the right-justified HH:MM:SS.
23. [ ] **Low battery**: at <= 15% the bar carries a "Charge Me!!!" badge;
        at <= 10% the panel shows only "Charge Me!", buttons go dead, and
        the log shows 600 s charge-lock sleeps with no further refreshes.
        Charging past 15% restores the normal layout on the next wake
        (a RUNNING timer will have been paused at lock entry).
24. [ ] **Break fires**: (shorten via menuconfig: interval 2 min, duration
        1 min) with the timer RUNNING, after ~interval of accumulated run
        time the device flips to the inverted SCREEN BREAK screen and the
        break alarm fires: 2-beep pattern + pulsing cyan NeoPixels (alert-
        class — fires during quiet hours too); any button silences it.
        Break start may lag the interval by up to one 55 s tick.
25. [ ] **Break is enforced**: during the break, Button A logs
        `button A ignored during screen break` and nothing resumes. B does
        nothing on Screen; D still syncs.
26. [ ] **Break end**: at the end of the break (within ~1 s), double-beep
        chime, display returns to the normal layout showing PAUSED with the
        frozen remaining time; Button A resumes and accrual starts fresh
        (next break ~interval later).
27. [ ] **Break persistence**: power-cycle mid-break -> after the boot sync
        the break resumes with the SAME end time (not restarted). Power
        cycle after break end -> comes back PAUSED.
28. [ ] **Pause accrual**: run ~half the interval, pause, wait, resume —
        the break still fires after a total of ~interval of running time
        (pauses don't reset the accrual).
29. [ ] **Minute alignment (all states)**: the header time flips within a
        few seconds of real clocks in every state (log shows
        `Entering deep sleep (N s)` with varying N). IDLE/PAUSED/EXPIRED
        re-sync NTP on MAGTAG_IDLE_SYNC_INTERVAL_MIN (default 60 min);
        RUNNING on MAGTAG_RUNNING_SYNC_INTERVAL_MIN (default 10 min).
30. [ ] **RUNNING renders on the countdown grid**: a start/resume shows one
        precise value (e.g. 1:12:23); every subsequent tick render shows a
        round minute (1:12:00, 1:11:00...) because wakes land when the
        remaining value crosses its own minute grid. When the sync is due,
        the log shows an early wake (~20 s before the grid point) and the
        render still lands on the grid after the sync.
31. [ ] **Quiet hours**: between 22:30 and 08:00 local (menuconfig:
        MAGTAG_QUIET_START/END_HHMM) button/status NeoPixels stay dark;
        alert pulses (expiry red, break cyan) still fire. Set start == end
        to disable for testing.
32. [ ] **Status LED brightness**: MAGTAG_STATUS_LED_BRIGHTNESS (menuconfig)
        visibly scales the state/sync/countdown pixels; alert pulses
        (expiry red, break cyan) are unaffected. Alarm lengths follow
        MAGTAG_EXPIRY/BREAK_ALARM_CYCLES.
33. [ ] **Summer category**: on a summer-break weekday the footer reads
        `Summer - 120 min` (MAGTAG_SUMMER_MIN); weekends still read
        `Weekend`. After 2026-08-20 (first day of school) weekdays revert
        to `Weekday`, and Dublin no-school days (e.g. 2026-10-16) read
        `Holiday`.

## Home Assistant integration (v1.4)

Requires a broker URI in NVS (`NVS_DEFAULT_MQTT_URI` in
`include/credentials.local.h`, or menuconfig). Full HA-side verification
(entity discovery, config_ack, dashboards) is in `docs/home_assistant.md`;
these items cover the on-hardware behaviour.

34. [ ] **Ambient light sensor**: log shows a plausible `light_mv` in the
        stat payload (or add a temporary `ESP_LOGI`); covering GPIO 3 with a
        finger vs. a bright light visibly changes the value. Confirms the
        ALS-PT19 read and that light.c sharing battery.c's ADC1 unit works
        (battery % must still read correctly in the same wake).
35. [ ] **Network window (MQTT enabled)**: a sync wake connects WiFi, syncs
        NTP, then connects the broker and publishes — log shows
        `published N messages`; the device appears under MQTT in HA within
        one window. The window is ~1-3 s longer than a plain NTP sync.
36. [ ] **MQTT disabled**: with an empty broker URI, the sync wake does NTP
        only (no broker connect attempt, no added delay, no error spam) —
        confirms the empty-URI skip path.
37. [ ] **Grant on hardware**: publish a grant to an EXPIRED Screen timer
        (see home_assistant.md); on the next window the panel shows PAUSED
        holding the granted time and Button A starts it — the TIME'S UP
        alarm does NOT re-fire. A grant while RUNNING extends the countdown
        in place.
38. [ ] **Locate alarm**: publish a locate command; on the next window the
        device beeps with a red NeoPixel pulse until any button is pressed
        (or ~10 min). Confirm it does not trip the awake failsafe early and
        that the normal layout returns after dismissal.
39. [ ] **Daily summary**: after a day rollover, the log/HA shows a
        `summary` publish (screen seconds used + per-timer completions) for
        the finished day; the charge-lock entry (case 23) publishes one
        final stat with `charge_lock` true before the long sleeps.

Record failures with the monitor log snippet and the step number.
