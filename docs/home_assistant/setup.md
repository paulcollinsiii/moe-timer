# Setting up Home Assistant

This is the runbook from an MQTT broker to a working dashboard, in order. Each
step says what to do and links to the page that explains it.

**One idea underlies every Home Assistant page:** the MagTag sleeps almost
all the time and talks to HA only during a brief network window: every 10
minutes while a timer runs, hourly while idle, and whenever you press D on
the timer screen
([when it syncs](../behavior/power_and_sync.md#syncing) has the full
list). So everything between the two is a **retained** MQTT message. HA
publishes whenever it likes, the broker holds the message, and the device
picks it up at its next window. An edit you make in HA takes effect then, not
at once.

## What you need

- **Home Assistant 2025.11 or newer**, with the **MQTT integration** and a
  broker. The Mosquitto add-on works. Create a broker user for the devices.
  Keep the broker on a trusted network: the device talks to it in cleartext.
- **[uv](https://docs.astral.sh/uv/)** on the machine where you run the
  dashboard generator.

## 1. Point the device at the broker

The broker is entered on the device, not in the build. Flash the device
([README](../../README.md#esp-idf-for-local-flashing-included-in-the-dev-container)),
and a device with no stored WiFi opens [setup mode](../behavior/setup_mode.md)
on its own. After you give it your WiFi, join the device's setup network and
open `http://192.168.4.1/mqtt`. Enter the broker URI, for example
`mqtt://homeassistant.local:1883`, and the broker user and password from
above. An empty URI turns MQTT off.

A device that already has WiFi gets back into setup mode by holding BOOT, so
the broker can be added or changed later without touching WiFi. The
[setup mode page](../behavior/setup_mode.md) has the steps.

## 2. Find it in HA

When setup ends, the device runs its first network window at once, and it
appears under **Settings → Devices & services → MQTT** as `magtag-xxxxxx`:
the last three bytes of its WiFi MAC address. The name is unique on your
network and survives every reflash. If it does not show up, press **D** on the
timer screen to run another window, or see
[troubleshooting](troubleshooting.md#the-device-never-appears-in-ha).

## 3. Create the school calendar

The config-publishing automation (step 4) reads holidays and the summer break
from a calendar whose entity ID is **`calendar.school_schedule`**. The simplest
is a **Local Calendar** (Settings → Devices & services → Add integration →
Local Calendar) named `School schedule`. A remote or ICS calendar works too,
as long as you set its entity ID to `calendar.school_schedule`.

Add one **all-day** event per break, spanning the days off. The title decides
what an event is, and the match is case-sensitive and anchored at the start:

- a title that starts with **`No School: Summer`** is a summer break;
- any other title that starts with **`No School`** (`No School`,
  `No School: Winter break`, …) is a holiday;
- anything else (`no school`, `School closed`, `Holiday: No School`) is
  ignored.

Include **next summer as well as this one**. The automation needs both to
date the school year. [Configuring](configuring.md#the-config-publishing-automation)
says what it takes from these events.

## 4. Install the config-publishing automation

The automation, `tools/ha/magtag_publish_config.yaml`, sends each device its
chore list and school calendar. Install it **once**, however many devices you
have. It is a YAML list with one entry. Add it to your configuration in one of
these ways, then reload automations (Developer tools → YAML → Automations):

- **In `automations.yaml`:** append it. A fresh HA's `automations.yaml`
  holds only `[]`: **replace** the `[]` with the file's contents. A list entry
  after `[]` is invalid YAML, and HA then loads no automations at all.
- **In a directory of its own**, included with
  `automation manual: !include_dir_merge_list automations/`: drop the file in
  as it is.
- **In a package:** wrap it under an `automation:` key.

It has no `id:`, so HA treats it as YAML-managed: it runs normally, but the
automation editor cannot edit it. Edit the YAML.

## 5. Set up each device

For each device:

1. **If it runs firmware older than this checkout, OTA it first** and let it
   run a network window ([the OTA manifest](../ota_manifest.md)). A device
   you just flashed in step 2 is already current.
2. **Create its chore list:** Settings → Devices & services → Add integration
   → **Local To-do**, named exactly **`MagTag <node> chores`**, where `<node>`
   is the six hex digits after `magtag-`. For `magtag-1a0a5c` that is
   `MagTag 1a0a5c chores`, which HA turns into `todo.magtag_1a0a5c_chores`,
   the entity ID the automation looks for. You can change the list's display
   name afterwards; the entity ID stays. Adding a device is creating its
   list: nothing in the automation changes.
3. **Check its entity IDs.** They should read
   `<component>.magtag_<node>_<key>`, for example
   `sensor.magtag_1a0a5c_battery`. A device first paired on older firmware
   may still have IDs derived from its name, such as
   `sensor.testing_timer_battery`. Re-register it
   ([below](#re-registering-a-device-on-old-entity-ids)) before you
   generate the dashboard.

To check that the automation reached a device, add its chores to the list
and give it two network windows (press **D** on the timer screen twice, a
minute apart). Its chores then appear on its device page as "`<chore>`
done". If they do not, see
[A document did not take](troubleshooting.md#a-document-did-not-take).

## 6. Generate the dashboard

Run the dashboard generator from the repository root. It finds the devices
on the broker:

```sh
uv run tools/gen_ha_dashboard.py --mqtt --sdkconfig ~/magtag-mqtt.cfg > ha_setup.txt
```

The firmware build does not hold the broker, so the generator takes it from a
small file you keep outside the repository, here `~/magtag-mqtt.cfg`
([the format](dashboard.md#from-the-broker---mqtt)).

`ha_setup.txt` has three parts: **1**, the steps of step 5 with each
device's names filled in, handy for the next device you add; **2**, the
automation you installed in step 4 (it does not go into the dashboard); and
**3**, the dashboard YAML. [Dashboard](dashboard.md#from-the-broker---mqtt)
has the generator's other options.

## 7. Paste the dashboard

Settings → Dashboards → Add dashboard → New dashboard from scratch. Open it,
choose Edit → three-dot menu → **Raw configuration editor**, replace
everything with part 3 of `ha_setup.txt`, and Save.

When you add a device, or after a firmware update that adds entities,
regenerate and paste again rather than editing the dashboard by hand.

Then set **Timezone** (the default is US Eastern) and the allocations on the
dashboard's cards. [Configuring](configuring.md#the-controls) covers each
control.

## 8. Optional: a low-battery notification

At 10 % or below, the device locks itself with a "charge me" screen and
stops opening network windows. Its last report before that sets **Charge
lock** on, which makes a good trigger. Add it the way you added the
automation in step 4:

```yaml
- alias: "MagTag needs charging"
  triggers:
    - trigger: state
      entity_id: binary_sensor.magtag_xxxxxx_charge_lock
      to: "on"
  actions:
    - action: notify.mobile_app_phone
      data:
        message: "The screen timer battery is at 10% — charge it."
```

## Renaming or re-registering a device

**Renaming.** Change the **Device name** control (or the device's name in
HA). Only the display name changes. Entity IDs come from the MAC-based
device ID, never from the name, so dashboards and automations keep working.
The To-do list's entity ID does not change either.

### Re-registering a device on old entity IDs

A device paired before discovery schema v20 keeps the name-derived entity IDs
HA gave it at the time, because HA sets an entity ID only when the entity is
first registered. The generated dashboard cannot find those entities. To move
the device to stable IDs, do these steps in this order:

1. **OTA it to current firmware first**, and let it run a window. Firmware
   older than v20 does not send the stable IDs, so a device deleted while
   still on it comes back under name-derived IDs again.
2. **Delete it in HA:** Settings → Devices & services → MQTT → the device →
   **Delete**.
3. **Make it republish its discovery.** The device republishes only when its
   discovery changes, so it may not come back on its own. Rename one of its
   chores in its `MagTag <node> chores` list (on a device with no chores, add
   the first one). It republishes within **two network windows**: one applies
   the new list, the next reports it. To force them, press **D** on the timer
   screen twice, a minute apart. If the chore checklist is showing, D ticks
   chore 3 there instead, so press **A** first to get back to the timer
   screen.
4. **Once the device has re-appeared**, rename the chore back. Not sooner:
   renamed back before the device's next window, the list is the one it
   already has, so the device skips the document and nothing republishes.

What the delete costs, as read from HA's source and **not confirmed on a real
install:** HA 2025.7 and later restore a re-created entity's area, custom
name and icon, labels and flags. History and statistics most likely move to
the new IDs. What does break is anything that names the old IDs: dashboards,
automations, scripts and template sensors. Update those by hand.

HA 2025.10 also added a **Recreate entity IDs** action, which might do this
without a delete. It is untested here. If you try it, check that the new IDs
read `<component>.magtag_<node>_<key>`.

Skipping re-registration is a valid choice. The device keeps working, but the
generated dashboard will not find its entities.
