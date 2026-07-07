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
13. [ ] **Wake buttons**: A and D wake the device; B wakes only in
        parent-testing builds (`CONFIG_MAGTAG_PARENT_TESTING=y`); C never
        wakes.
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
        refunded. Refunding requires Button B (parent mode, case 20) or a
        genuine day rollover. With WiFi unavailable on a power-on the
        restore cannot validate (no clock) and the device fails open to
        IDLE. Note: EN reset mid-run (before expiry) intentionally restores
        the in-flight countdown — that is crash recovery, not a refund; the
        run resumes with the remaining time it had.
21. [ ] **Final-minute countdown**: the pre-expiry wake lands ~70 s out
        (planner); the display then steps through 00:01:00 / 00:00:45 /
        00:00:30 / 00:00:15 as partial refreshes, the last 15 s count down
        on the four pixels in binary (light green, dim; dark during quiet
        hours), and TIME'S UP + red pulse + beeps fire within ~1 s of the
        expiry wall time.
22. [ ] **Battery gauge**: log shows `battery: N mV (P%)` each wake; on USB
        ~4300+ mV -> 100%, on LiPo 3300-4200 mV with a plausible %. Icon +
        percent render left of the right-justified HH:MM:SS.
23. [ ] **Break fires**: (shorten via menuconfig: interval 2 min, duration
        1 min) with the timer RUNNING, after ~interval of accumulated run
        time the device flips to the inverted SCREEN BREAK screen and the
        break alarm fires: 2-beep pattern + pulsing cyan NeoPixels (alert-
        class — fires during quiet hours too); any button silences it.
        Break start may lag the interval by up to one 55 s tick.
24. [ ] **Break is enforced**: during the break, Button A logs
        `button A ignored during screen break` and nothing resumes. B
        (parent mode) still resets; D still syncs.
25. [ ] **Break end**: at the end of the break (within ~1 s), double-beep
        chime, display returns to the normal layout showing PAUSED with the
        frozen remaining time; Button A resumes and accrual starts fresh
        (next break ~interval later).
26. [ ] **Break persistence**: power-cycle mid-break -> after the boot sync
        the break resumes with the SAME end time (not restarted). Power
        cycle after break end -> comes back PAUSED.
27. [ ] **Pause accrual**: run ~half the interval, pause, wait, resume —
        the break still fires after a total of ~interval of running time
        (pauses don't reset the accrual).
28. [ ] **Minute alignment (all states)**: the header time flips within a
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
31. [ ] **Status LED brightness**: MAGTAG_STATUS_LED_BRIGHTNESS (menuconfig)
        visibly scales the state/sync/countdown pixels; alert pulses
        (expiry red, break cyan) are unaffected. Alarm lengths follow
        MAGTAG_EXPIRY/BREAK_ALARM_CYCLES.
32. [ ] **Summer category**: on a summer-break weekday the footer reads
        `Summer - 120 min` (MAGTAG_SUMMER_MIN); weekends still read
        `Weekend`. After 2026-08-20 (first day of school) weekdays revert
        to `Weekday`, and Dublin no-school days (e.g. 2026-10-16) read
        `Holiday`.
29. [ ] **Quiet hours**: between 22:30 and 08:00 local (menuconfig:
        MAGTAG_QUIET_START/END_HHMM) button/status NeoPixels stay dark;
        alert pulses (expiry red, break cyan) still fire. Set start == end
        to disable for testing.
20. [ ] **Production reset gate**: with `CONFIG_MAGTAG_PARENT_TESTING=n`,
        Button B logs `Button B reset disabled` and does not reset; the
        allocation resets only on day rollover. (Default build: =y, B resets.)

Record failures with the monitor log snippet and the step number.
