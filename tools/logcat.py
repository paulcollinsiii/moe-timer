#!/usr/bin/env python3
"""Raw USB-CDC log reader that never resets the target.

The ESP32-S2 native-USB console reboots the chip on an RTS *falling edge*
(IDF esp_usb_cdc_rom_console/usb_console.c: `if (!rts && s_prev_rts_state)`;
DTR high at that moment selects bootloader, low selects app). The Linux
cdc-acm driver asserts DTR+RTS on every open, so any reader that then
lowers RTS (pyserial default, esp_idf_monitor even with --no-reset)
reboots the device on every reconnect. The fix: keep RTS asserted for the
whole session — no falling edge, no reset.

Additionally, the S2 ROM CDC driver can PANIC (LoadProhibited in ISR
context) if the host's port-open control transfers race the device's boot
print burst — seen in hardware bring-up as "button resets the timer"
(panic -> reboot -> RTC state wiped -> day rollover -> IDLE). Opening the
port IS the dangerous act, so device presence is detected via sysfs
(/sys/class/tty/<port>, no control transfers) and the open is delayed ~1 s
until the early-boot burst is over.

Caveat: quitting while the device is awake drops DTR+RTS and may cause one
final reset; harmless, and usually the device is asleep (port gone) anyway.

Usage: tools/logcat.py [port]        (default /dev/ttyACM0)
Quit:  Ctrl+C
"""
import os
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
SETTLE_SEC = 1.0  # boot print burst is over ~1 s after enumeration


def read_session(port: str) -> None:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 1
    # Must be set BEFORE open(). The kernel raises DTR+RTS at open; keeping
    # both True here means pyserial never *lowers* RTS afterwards — the
    # falling edge is what triggers the firmware's reset FSM.
    s.dtr = True
    s.rts = True
    s.open()
    sys.stderr.write(f"--- logcat: connected to {port} (Ctrl+C quits)\n")
    try:
        while True:
            data = s.read(4096)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    finally:
        s.close()


def wait_for_device() -> None:
    """Block until the USB device is enumerated, without opening the port."""
    sys_node = "/sys/class/tty/" + os.path.basename(PORT)
    if not os.path.isdir("/sys/class/tty"):
        return  # no sysfs (unusual): fall back to open-and-retry
    while not os.path.exists(sys_node):
        time.sleep(0.25)
    time.sleep(SETTLE_SEC)


def main() -> None:
    announced = False
    while True:
        try:
            if not announced:
                sys.stderr.write(f"--- logcat: waiting for {PORT}\n")
                announced = True
            wait_for_device()
            read_session(PORT)
            announced = False
        except (serial.SerialException, OSError):
            time.sleep(0.3)
            continue


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
