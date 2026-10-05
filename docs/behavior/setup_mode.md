# Setup mode

Setup mode is how the device gets its WiFi and its MQTT broker. Neither is
built into the firmware. The device opens a short-lived WiFi network of its own,
you give it your WiFi with a phone app and your broker with a web page, and it
saves both. This is when setup starts, what to do in it, and how it ends.

## When setup starts

| Situation | What starts setup |
|-----------|-------------------|
| A new device, or one that has lost its stored WiFi | Power it on or reset it, or press any button. No hold is needed |
| A device that has WiFi and works | Hold BOOT until the panel says `Release to enter setup`, then let go |
| A device with no WiFi that restarted after a crash | A button press. The panel first says `Setup failed` and `Press any button to retry`, and the device sleeps until you press one |

A device that has no WiFi and is woken by its timer does nothing new: it keeps
its timers running offline and waits for you.

**The BOOT hold.** Hold BOOT for `MAGTAG_BOOT_HOLD_MS` (5 seconds by default).
There are two ways in:
- hold BOOT while the device is asleep, and it wakes and starts timing;
- hold BOOT first, then press any other button: the press wakes the device with
  BOOT already down. This is the only way on a build with `MAGTAG_BOOT_WAKES`
  off, which cannot be woken by BOOT.

The count starts when the device wakes, so it runs a moment longer than the
figure. Let go before the prompt and nothing happens: a tap does nothing. If you
keep holding for about 10 seconds after the prompt, the device gives up and
repaints what it was showing.

**Locks.** A setup request that a lock outranks does nothing:
- **Charge Me!** and **Bed Time** arm no buttons, so BOOT cannot wake the
  device. Bed Time also outranks a new device's setup while it is in force, and
  the device waits for its next wake. A new device with an unset clock has no
  Bed Time, so it reaches setup.
- **No Clock** leaves BOOT armed, so a device whose WiFi stopped working can
  hold BOOT to reopen setup.
- **Config Error** arms D alone. Hold BOOT first, then press D. The lock is
  cleared from Home Assistant, which a device with broken WiFi cannot reach.

The locks themselves are in [Locks](locks.md).

## What the screen shows

```
[QR code]   Scan with ESP SoftAP Prov
            AP: MagTag-a1b2c3
            User: magtag
            Password (PoP):
            K7mNp3Rt4W
            MQTT: http://192.168.4.1/mqtt
```

The device is now a WiFi network named `MagTag-` and its id. The password is new
every session, shown only here, and doubles as the proof of possession for the
app.

**WiFi.**
1. Install Espressif's **ESP SoftAP Prov** app on a phone.
2. Choose to provision a device and scan the QR code. The app joins the
   device's network itself.
3. Pick your home WiFi and enter its password. The device tries the join. A
   wrong password is reported in the app and saved nowhere, and you can try
   again in the same session.

If the scan fails, join `MagTag-xxxxxx` by hand with the password on the panel,
then use the app's manual option with username `magtag` and the password as the
PoP.

**MQTT.** While your phone is joined to the device's network, open
`http://192.168.4.1/mqtt`. The page only exists on that network. It asks for:

| Field | Rule |
|-------|------|
| Broker URI | `mqtt://host`, with an optional `:port` and a trailing `/`. Up to 127 characters, no user name or path in it. Left empty, MQTT is turned off and the saved user and password are cleared |
| Username | Up to 63 characters |
| Password | Up to 63 characters. Left blank, the saved password is kept. It is never shown back |

The URI and username are filled in from what is saved. `mqtts://` is accepted
too, but the device sets up no broker certificate, so use `mqtt://`. A field
that fails is named on the page and nothing is saved.

Enter the broker **before** you finish the WiFi step: setup ends about 15
seconds after WiFi is saved. To add it later, see
[below](#wifi-first-mqtt-later).

## How setup ends

The session lasts `MAGTAG_SETUP_MAX_SEC` (10 minutes by default). It is the
most expensive thing the device does, and while it runs the device does nothing
else: no sync, no timer repaint, no alarms.

| Ends because | Panel | The device then |
|--------------|-------|-----------------|
| WiFi saved | `Setup complete`, `Connecting to WiFi...` | Sleeps 1 second and runs its first network window: the clock, then Home Assistant. A broker saved in the same session is used there |
| Broker saved, and the device already had WiFi | `Setup complete`, `MQTT broker saved` | The same first network window |
| Ran out of time | `Setup timed out` | See below |
| Could not start, stopped unexpectedly, or could not save the credentials | `Setup failed` | See below |

After a timeout or a failure, the panel says what retries setup. With no WiFi
it is `Press any button to retry`, and the device sleeps with no timer wake at
all, so a device in a drawer does not keep switching its radio on. With WiFi, it
names the BOOT hold, and the device returns to its normal schedule.

A broker saved on a device with no WiFi does not end setup: the session waits
for the WiFi.

## The No WiFi hint

After 3 network windows in a row find no working network, the header's
`Last sync` spot reads `No WiFi: hold BOOT`. On a build where BOOT cannot wake
the device it reads `BOOT+button: setup`. A window that works clears it. A
running Screen Break chip takes the spot first.

The hint is advice, never a trigger: a router outage must not turn every wake
into a setup session that drains the battery. A device with no WiFi never shows
it, because any press already starts setup.

## WiFi first, MQTT later

A device does not need an MQTT broker. Without one, it keeps its own timers and
syncs its clock, and Home Assistant never sees it. To add or change the broker
later, hold BOOT, join the device's network, and open the `/mqtt` page. Setup
ends as soon as the broker is saved, and WiFi is untouched. To change WiFi, hold
BOOT and go through the app again.

## What survives

- **A firmware update** keeps the credentials.
- **A reflash** with `idf.py flash` keeps them too. Erasing flash, or an NVS
  wipe, removes them with every other saved setting. The device then wakes with
  no WiFi and opens setup, and you enter both again. Home Assistant reapplies its
  own settings.

## Security

- The network is WPA2 with a random 10-character password per session, shown
  only on the panel. It exists only while setup runs.
- WiFi provisioning is encrypted with SRP6a, using that password as the proof of
  possession.
- The `/mqtt` page is plain HTTP. It is reachable only on that network, and
  the broker password crosses it unencrypted.
- No saved password is ever shown back, on the panel or on the page.

The firmware side is in [Wake cycle](../architecture/wake_cycle.md#setup-mode)
and [Network and Home Assistant](../architecture/network_and_ha.md#provisioning).
