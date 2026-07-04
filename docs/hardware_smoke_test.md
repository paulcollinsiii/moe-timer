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
5. [ ] **Button A (start)**: WiFi joins, SNTP syncs, header shows sync time,
       bar full, state `RUNNING`. With bad WiFi creds: "No sync - check WiFi"
       screen, still IDLE.
6. [ ] **Countdown**: remaining decreases ~55 s per wake; below 5 min it shows
       `M min S sec`.
7. [ ] **Button A (pause/resume)**: pause shows `PAUSED`, remaining freezes
       across wakes; resume continues from the frozen value.
8. [ ] **Button B (reset)**: returns to IDLE with today's full allocation.
9. [ ] **Button D (force sync)**: WiFi cycle + full refresh; sync time updates.
10. [ ] **Button C**: wakes the device and redraws, but changes nothing
        (unbound in v1).
11. [ ] **Expiry**: temporarily lower `NVS_DEFAULT_WEEKDAY_MIN` to 1-2 min (and
        erase NVS: `idf.py erase-flash`), let it expire: TIME'S UP screen,
        3 beeps x 5 cycles, red NeoPixel pulse; any button stops the alert
        immediately; device returns to deep sleep after.
12. [ ] **Day rollover**: fake it by clearing `last_date` (power-cycle resets
        RTC memory) or wait past midnight: wake re-syncs and resets to IDLE
        with the new day's allocation.
13. [ ] **Every button wakes from deep sleep** (A, B, C, D each wake the
        device; C just redraws).
14. [ ] **Panel protection**: mash buttons rapidly — refreshes serialize, log
        shows `refresh rejected` if under 1 s apart, no crash.
15. [ ] **Held-button dismissal**: dismiss the expiry alert while *holding*
        each button — the hold must not re-fire the button's action after the
        device sleeps and re-wakes (enter_deep_sleep waits for release).
16. [ ] **Buttons after first sleep cycle**: after at least one full
        sleep/wake cycle, verify buttons still read correctly during an alert
        (guards the RTC pad-hold release in buttons_init).
17. [ ] **Speaker pin**: confirm beeps come from the speaker (GPIO 17 carries
        a `verify against schematic` note in audio.c).
18. [ ] **Idle current** (optional, needs a meter): deep-sleep current < 1 mA.
        GPIO 21 (NeoPixel gate) and GPIO 16 (amp enable) are held through
        sleep — if current is high, probe those nets first.

Record failures with the monitor log snippet and the step number.
