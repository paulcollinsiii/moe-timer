# Setup mode

Setup mode is how the device gets its WiFi and its MQTT broker. Neither is
built into the firmware. The device opens a short-lived WiFi network of its own,
you join it by scanning the panel's QR code with your phone's camera, give it
your WiFi and your broker on one web page, and it saves both. No app is needed.
This is when setup starts, what to do in it, and how it ends.

## When setup starts

| Situation | What starts setup |
|-----------|-------------------|
| A new device, or one that has lost its stored WiFi | Power it on or reset it, or press any button. No hold is needed |
| A device that has stored WiFi | Hold BOOT until the panel says `Release to enter setup`, then let go |
| A device with no WiFi that restarted after a crash | A button press. The panel first says `Setup failed` and `Press any button to retry`, and the device sleeps until you press one |

A device that has no WiFi and is woken by its timer does nothing new: it keeps
its timers running offline and waits for you.

**The BOOT hold.** Hold BOOT for `MAGTAG_BOOT_HOLD_MS` (5 seconds by default).
There are two ways in:
- hold BOOT while the device is asleep, and it wakes and starts timing;
- hold BOOT first, then press B or D (or any other button that is armed): the
  press wakes the device with BOOT already down. This is the only way on a
  build with `MAGTAG_BOOT_WAKES` off, which cannot be woken by BOOT.

The count starts when the device wakes, so it runs a moment longer than the
figure. Let go before the prompt and nothing happens: a tap does nothing. If you
keep holding for about 10 seconds after the prompt, the device gives up and
repaints what it was showing.

**Locks.** A setup request that a lock outranks does nothing:
- **Charge Me!** and **Bed Time** arm no buttons, so BOOT cannot wake the
  device. Both also hold back a new device's setup while they are in force: a
  new device powered on with a flat cell shows Charge Me!, and under Bed Time
  it waits for its next wake. A new device with an unset clock has no Bed Time,
  so it reaches setup.
- **No Clock** leaves BOOT armed, so a device whose WiFi stopped working can
  hold BOOT to reopen setup.
- **Config Error** arms D alone. Hold BOOT first, then press D. The lock is
  cleared from Home Assistant, which a device with broken WiFi cannot reach.

The locks themselves are in [Locks](locks.md).

## What the screen shows

```
[QR code]   Scan to join, then open
            192.168.4.1
            AP: MagTag-a1b2c3
            Password:
            K7mNp3Rt4W
            It may open by itself
```

The device is now a WiFi network named `MagTag-` and its id. The password is new
every session and shown only here.

1. Scan the QR code with your phone's camera. Android and iOS offer to join the
   network; accept.
2. The setup page usually opens by itself, as a "sign in to network" prompt. If
   it does not, open `http://192.168.4.1` in a browser. The page only exists on
   that network, and a phone may warn that the network has no internet: stay
   connected.
3. Fill in the form and press Save.

If the camera will not join, join `MagTag-xxxxxx` by hand with the password on
the panel, then open the address.

| Field | Rule |
|-------|------|
| Network name | Up to 32 bytes. Left blank on a device that already has WiFi, the saved network is kept; the current name is shown greyed as a hint. Required on a device with none |
| WiFi password | Left blank for an open network. Otherwise 8 to 63 characters, or exactly 64 hex digits (a raw key; any other 64 characters are refused). Accents and other non-ASCII characters work: the page is UTF-8, and the 32-byte limit on the name counts bytes. Ignored when the network name is blank |
| Broker URI | `mqtt://host`, with an optional `:port` and a trailing `/`. Up to 127 characters, no user name or path in it. Left blank, the saved broker is kept. The page cannot turn MQTT off |
| Username | Up to 63 characters. Ignored when the URI is blank |
| Password | Up to 63 characters. Left blank, the saved password is kept. Ignored when the URI is blank |

The broker URI and username are filled in from what is saved. Neither password is
ever shown back. `mqtts://` is accepted too, but the device sets up no broker
certificate, so use `mqtt://`. A field that fails is named on the page and
nothing is saved.

**The WiFi join is checked before it is saved.** After Save, the page says
`Connecting to WiFi...` and updates itself. The device tries the network, and
only a join that works is saved. A join that fails, after three tries, is
reported on the page with the likely cause (a wrong password, a network not
found, or a security mismatch, which is what a blank password against a protected
network usually produces). Nothing is saved, and you can correct it and press
Save again in the same session. Without JavaScript, reload the page to see the
result. Joining moves the device's own network to your router's channel, so the
phone may drop off it, and a phone's sign-in sheet may close with it. The page
checks for about two minutes; if it stops hearing from the device it says so and
points you at the device screen. Reconnect to `MagTag-xxxxxx` and reopen
`http://192.168.4.1` to see the result. After a successful join the network
disappears, so an open page ends the same way.

A page submitted while a join is running, or after WiFi is already saved, does
not change the WiFi. A broker in that same submit is still saved.

The broker goes in the same submit as the WiFi, so fill in both at once. A device
that already has WiFi ends setup the moment a broker-only submit is saved. To add
the broker later, see [below](#wifi-first-mqtt-later).

**Fallback: the app.** Espressif's **ESP SoftAP Prov** app still works for the WiFi
step, but the QR code no longer carries what it needs. Join the network by hand,
choose the app's manual option, and enter username `magtag` and the panel
password as the PoP.

## How setup ends

The session lasts `MAGTAG_SETUP_MAX_SEC` (10 minutes by default). It is the
most expensive thing the device does, and while it runs the device does nothing
else: no sync, no timer repaint, no alarms.

| Ends because | Panel | The device then |
|--------------|-------|-----------------|
| WiFi saved | `Setup complete`, `Connecting to WiFi...` | Sleeps 1 second and runs its first network window: the clock, then Home Assistant. A broker saved in the same session is used there |
| Broker saved, and the device already had WiFi | `Setup complete`, `MQTT broker saved` | The same first network window |
| Ran out of time | `Setup timed out` | See below |
| Could not start, stopped unexpectedly, or could not save the credentials | `Setup failed` (except a start refused before anything was painted) | See below |

After a timeout or a failure, the panel says what retries setup. With no WiFi
it is `Press any button to retry`, and the device sleeps with no timer wake at
all, so a device in a drawer does not keep switching its radio on. With WiFi, it
names the BOOT hold, and the device returns to its normal schedule.

A broker saved on a device with no WiFi does not end setup: the session waits
for the WiFi.

## The No WiFi hint

After `SETUP_TRIGGER_WIFI_FAIL_HINT_THRESHOLD` (3) network windows in a row find
no working network, the header's `Last sync` spot reads `No WiFi: hold BOOT`. On
a build where BOOT cannot wake the device it reads `BOOT+button: setup`. A window
that works clears it, and so does midnight. A running Screen Break chip takes
the spot first.

The hint is advice, never a trigger: a router outage must not turn every wake
into a setup session that drains the battery. A device with no WiFi never shows
it, because any press already starts setup.

## WiFi first, MQTT later

A device does not need an MQTT broker. Without one, it keeps its own timers and
syncs its clock, and Home Assistant never sees it. To add or change the broker
later, hold BOOT, join the device's network, and fill in only the broker fields.
Setup ends as soon as the broker is saved, and WiFi is untouched. To change WiFi,
hold BOOT and enter the new network on the same page.

## What survives

- **A firmware update** keeps the credentials.
- **A reflash** with `idf.py flash` keeps them too. Erasing flash, or an NVS
  wipe, removes them with every other saved setting. The device then wakes with
  no WiFi and opens setup, and you enter both again. Home Assistant reapplies its
  own settings.

## Security

- The network is WPA2 with a random 10-character password per session, shown
  only on the panel. It exists only while setup runs. If the device cannot
  narrow the network to WPA2-only it stays WPA/WPA2 mixed and logs a warning.
- The QR code carries the network password, so anyone who can see the panel can
  join. That is the same exposure as the printed password.
- The setup page is plain HTTP. It is reachable only on that network, and the
  WiFi and broker passwords cross it unencrypted. The app route authenticates
  with SRP6a and encrypts the session; the page does not.
- The device answers every DNS name on that network with its own address, so a
  phone's connectivity probe opens the page. It forwards nothing.
- No saved password is ever shown back, on the panel or on the page.

The firmware side is in [Wake cycle](../architecture/wake_cycle.md#setup-mode)
and [Network and Home Assistant](../architecture/network_and_ha.md#provisioning).
