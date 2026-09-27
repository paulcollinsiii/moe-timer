# MOE Timer
The Massively Over Engineered Timer for kids.

A MagTag(link needed) timer to help track screen time for kids. Some of the major features:
* Configurable screen times that can differ based on Weekday, Weekend, Holiday, Summer Break
* Configurable screen breaks
* Support for Bonus or Loss of screen time that resets daily
* Up to 4 additional timers that can be used (or not) during Screen Breaks. Examples:
  * Piano practice can be done during a screen break, reload this timer to get high scores per week
  * Folding Laundry cannot be done during a screen break since for us the TV can be on in the background and isn't counted against normal screen time
* Up to 3 chores that have to be done to unlock the remaining (or all) screen time for the day. Configurable per screen time type (weekday / weekend / holiday / summer break)
* Configuration, Tracking & Reporting through Home Assistant
* Approx 1 week of battery life with a 420mAh LiPo battery pack.

## Screenshots
(todo, get some from HA and from the timer itself)

## Requirements and Setup

### Home Assistant
* HA 2025.11+
* MQTT server connected to HA

[!WARNING]
There are several options for running MQTT with HA but locally on a trusted network is strongly recommended. While communications from HA <--> MQTT are easily secured with TLS, the MOE Timer communicates *in cleartext* for performance and battery reasons.

### ESP-IDF for local flashing (included in DevContainer)

* The dev container included here for VSCode pre-installs the tooling needed. It assumes you're on an Ubuntu machine as it mounts /dev/bus/usb and /dev into the container. If you're running from a different host you'll need to adjust .devcontainer/devcontainer.json
* In a vs code terminal window inside the devcontainer
  * `source ~/esp/esp-idf/export.sh`
  * `idf.py menuconfig`
  * In the MagTag Timer settings, set your MQTT server credentials. Feel free to pre-configure other settings here but they're defaults in the absence of HA
  * In `include/credentials.local.h` (copy the .example file) set your WiFi credentials
  * Put your MagTag in firmware download mode (hold the boot button, press reset, then let go of the boot button) and then you can `idf.py flash`

## Home Assitant Configuration
Once your device is auto-discovered in HA

* Create a Calendar called `School Schedule` and use AI to generate and ics for `No School: REASON` days based on your school
* Run `uv run ./tools/gen_ha_dashboard.py --mqtt > ha_setup.txt` to generate the Dashboard and Automation config for your MOE Timer
* Create the ToDo lists with the `MagTag XXXXXX chores` name initially. You can change the Label afterwards.

Once you have the dashboard setup some recommendations
* Configure the volume around 150% so it's loud enough to hear but not so loud the entire alert is clipping while playing.
* Set your timezone, e.g. `EST5EDT,M3.2.0,M11.1.0` for Eastern US time. Example values [from esp-idf documentation](https://docs.rainmaker.espressif.com/docs/dev/firmware/fw_usage_guides/time-service-usage/)


## Deep Dive docs
* [Architecture Overview](./docs/architecture.md)
* [Developer Setup](./docs/developer_setup.md)
* [OTA Setup](./docs/ota_manifest.md) - pushing custom firmwares to multiple MOE Timers is much easier this way.
* [Product Overview](./docs/ProductOverview.md)
* [Vibe Coding AI planning docs](./docs/planning) - Folder with various plans

## Other Notes
Early versions of this were written in Circuit Python, but migrated purely with Claude to C for performance on the device. The code is 100% AI generated, and many of the [docs](./docs/) are as well. Expect many "load bearing" invariants and unnecessary verbosity.

References to `kaffi.internal` are my home kubernetes server that hosts HA, MQTT and a small HTTP server for OTA firmware updates.

The real device is actually used by a real kiddo daily, so beyond all the automated tests it has survived extensive field testing. I'll continue cleaning this project up as my claude pro account rate limit resets 🍻
