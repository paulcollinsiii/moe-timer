# Agent notes

These are agent-facing: notes for an AI agent about to change the firmware.
Humans may skip them; everything a person needs to use, set up or understand
the device is in the other docs.

Each note covers detail an agent needs before editing a particular area, and
that would only clutter the human docs.

| Note | Read it before you |
|------|--------------------|
| [layering.md](layering.md) | add a call that crosses layers, or rely on any list of the known cross-layer edges |
| [rtc_and_reboot.md](rtc_and_reboot.md) | add or change state in RTC memory, or anything that must survive a wake, a panic or an OTA reboot |
| [home_assistant.md](home_assistant.md) | add or rename an HA field or entity, or edit `mqtt_ha`, `ha_config`, `config_apply`, `stats_json` or the dashboard generator |

## What lives in code comments instead

Some agent-level detail stays next to the code it describes, so there is
no note for it here. Read these where they are:

- **OTA internals** (the two time budgets, when results are reported, the
  certify wake, rollback): `include/ota_flow.h`, `main/ota_flow.c`,
  `include/ota_timing.h` and the rollback comments in `sdkconfig.defaults`.
  The one-page summary is [architecture/ota.md](../architecture/ota.md).
- **Wake flow** (the break-end latch, the press-latch pick order, the
  two-event sleep): `include/wake_flow.h`, `button_latch_pick()` in
  `main/button_latch.c`, `main/buttons.c`, `main/buttons_policy.c` and
  `include/sleep_plan.h`.
- **What `main.c` may contain**: the header comment of `main/main.c` states
  the rule and its four numbered reasons, and each symbol below it that
  executes anything names the reason that admits it.
