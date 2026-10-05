# MOE Timer
The Massively Over Engineered Timer for kids.

A [MagTag](https://www.adafruit.com/product/4800) timer to help track screen
time for kids. Some of the major features:
* Configurable screen time that can differ by day type: weekday, weekend,
  holiday or summer break
* Configurable screen breaks
* Bonus or lost screen time that resets daily
* Up to 4 additional timers, each of which can be used (or not) during screen
  breaks. Examples:
  * Piano practice can be done during a screen break. Reload this timer to get
    high scores per week.
  * Folding laundry cannot be done during a screen break, since for us the TV
    can be on in the background, and that isn't counted against normal screen
    time.
* Up to 3 chores that have to be done to unlock the remaining (or all) screen
  time for the day. The screen time allowed before the chores are done is
  configurable per day type.
* Configuration, tracking and reporting through Home Assistant
* About 1 week of battery life with a 420 mAh LiPo battery pack

## Screenshots
(todo, get some from HA and from the timer itself)

## Requirements and Setup

### Home Assistant
* HA 2025.11+
* An MQTT broker connected to HA

> [!WARNING]
> There are several options for running MQTT with HA, but running it locally
> on a trusted network is strongly recommended. While communications between
> HA and MQTT are easily secured with TLS, the MOE Timer communicates *in
> cleartext* for performance and battery reasons.

### ESP-IDF for local flashing (included in the dev container)

* The dev container included here for VS Code pre-installs the tooling needed.
  It assumes a Linux host such as Ubuntu, since it mounts /dev/bus/usb and /dev
  into the container. If you're running from a different host, you'll need to
  adjust `.devcontainer/devcontainer.json`.
* In a VS Code terminal window inside the dev container:
  * Optionally, copy `include/credentials.local.h.example` to
    `include/credentials.local.h` and set the OTA URL (read [OTA
    Setup](./docs/ota_manifest.md) first). That is the only value it holds:
    WiFi and MQTT credentials are not part of the build, and you enter them on
    the device after flashing. Create the file before you build.
  * `source ~/esp/esp-idf/export.sh`
  * Optionally, run `idf.py menuconfig` and pre-configure other settings in the
    MagTag Timer menu. They're only defaults, used in the absence of HA.
  * Put your MagTag in firmware download mode (hold the Boot button, press
    Reset, then let go of the Boot button), and then run `idf.py flash`.
    If the device doesn't start once flashing finishes, press Reset. It can
    take a few presses.
* A device with no stored WiFi opens **setup mode** as soon as it boots. The
  panel shows a QR code: scan it with Espressif's "ESP SoftAP Prov" phone app
  to give the device your WiFi, then join the device's own network and open
  `http://192.168.4.1/mqtt` to give it your MQTT broker. The full walkthrough
  is [Setup mode](./docs/behavior/setup_mode.md).
* When setup finishes, the device runs its first network window right away,
  and that's when it appears in HA as `magtag-xxxxxx`. If it doesn't show up,
  press D (force sync) to run another window.

## Home Assistant Configuration
The full runbook, step by step, is
[Home Assistant setup](./docs/home_assistant/setup.md). In short, once your
device is auto-discovered in HA:

* Create a calendar called `School Schedule` and use AI to generate an .ics
  file of `No School: REASON` days based on your school district. Make each
  summer break one all-day `No School: Summer` event spanning the break, and
  include next summer as well as this one: the automation needs both to date
  the school year.
* Install the config-publishing automation,
  `tools/ha/magtag_publish_config.yaml` (step 4 of the
  [runbook](./docs/home_assistant/setup.md)).
* Create the To-do lists with the `MagTag XXXXXX chores` name initially. You
  can change the name afterwards. Check the entity IDs before the next step
  (step 5 of the runbook).
* Run
  `uv run ./tools/gen_ha_dashboard.py --mqtt --sdkconfig ~/magtag-mqtt.cfg > ha_setup.txt`
  to generate the dashboard and automation config for your MOE Timer(s).
  * The firmware build does not hold the broker, so the generator takes it
    from a small file kept outside the repo
    ([the format](./docs/home_assistant/dashboard.md#from-the-broker---mqtt)).
* Paste the dashboard from `ha_setup.txt` (step 7 of the runbook).

Once you have the dashboard set up, here are some recommendations:
* Set the alert volume to around 150%, so it's loud enough to hear but not so
  loud that the entire alert clips while playing.
* Set your timezone, e.g. `EST5EDT,M3.2.0,M11.1.0` for US Eastern time. Example
  values are in the [ESP RainMaker
  documentation](https://docs.rainmaker.espressif.com/docs/dev/firmware/fw_usage_guides/time-service-usage/).


## Deep Dive docs
* [Product Overview](./docs/product_overview.md) - what the timer does, with
  one [behavior doc](./docs/behavior) per feature.
* [Architecture Overview](./docs/architecture.md) - how the firmware is built,
  with one [page per subsystem](./docs/architecture).
* [Developer Setup](./docs/developer_setup.md)
* [Home Assistant](./docs/home_assistant/setup.md) - setup, then the
  [dashboard](./docs/home_assistant/dashboard.md),
  [configuring](./docs/home_assistant/configuring.md) the timer from HA, the
  [reference](./docs/home_assistant/reference.md) and
  [troubleshooting](./docs/home_assistant/troubleshooting.md).
* [OTA Setup](./docs/ota_manifest.md) - pushing custom firmware to multiple
  MOE Timers is much easier this way.
* [Hardware Checklist](./docs/hardware_checklist.md) - the checks only a real
  MagTag can answer.
* [Agent Notes](./docs/agent_notes/README.md) - for AI agents editing the code.
* [Vibe Coding AI planning docs](./docs/planning) - folder with various plans

## Other Notes
Early versions of this were written in CircuitPython, but were migrated purely
with Claude to C for performance on the device. The code is 100% AI generated,
and many of the [docs](./docs/) are as well. Expect many "load bearing"
invariants and unnecessary verbosity.

References to `kaffi.internal` are to my home Kubernetes server, which hosts
HA, MQTT and a small HTTPS server for OTA firmware updates.

The real device is actually used by a real kiddo daily, so beyond all the
automated tests, it has survived extensive field testing. I'll continue
cleaning this project up as my Claude Pro account rate limit resets 🍻
