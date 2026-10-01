# Network and Home Assistant

How a network window works, how its results get back to the main task, how
clock corrections reach a running timer, and when Home Assistant is told
about new entities. WiFi is powered only inside a window (S29), and a window
is short and bounded, so everything the device exchanges with HA rides one.

## One window, in order

A window runs on its own FreeRTOS task, `net_win` (`main/net_window.c`,
10 KB stack), while the main task paints:

```
net_win task                               main task
────────────                               ─────────
WiFi up
SNTP sync, measure the clock step
signal "NTP settled" ────────────────────▶ interactive wake: paint now
                                           collect the stats snapshot
wait for the snapshot ◀─────────────────── post the snapshot
OTA manifest check, if armed
MQTT session with HA
WiFi down
signal "window done" ────────────────────▶ join, then apply the results
```

The snapshot handoff is also power serialization: the main task posts it only
after its e-ink refresh, so the panel's refresh current never coincides with a
WiFi transmit burst or a config flash write. Together they once browned out
the rail. If WiFi does not come up, the task skips straight to "window done";
the timer keeps running on the clock it has.

There are two ways to run a window. Unattended paths (a tick sync, the day
rollover, the final-minute sync, the locks) use the blocking
`net_apply_try_window()` and paint after it. An interactive wake (a start, a
resume or a Button D sync) paints as soon as NTP settles and lets the MQTT
phase drain behind the panel; Button B stays live during that drain.

## Who does what

The window task owns the radio and nothing else. It never paints, never
drives the LEDs and never touches the timer slots (S28). Results it receives
(grants, a Screen-adjust target, a locate request, new timer definitions)
are buffered and applied on the main task by `net_apply_finish()` after the
join. That includes reconciling an extra timer whose definition changed
mid-window.

The one accepted exception is a chore-list edit in a config document, which
rewrites the RTC chore ticks on the window task. It is safe only because the
main task is then inside the join, which never paints. The task also writes
NVS directly: config documents and the OTA check result.

## Clock steps

NTP corrects the system clock, and a running timer follows by subtraction
([state.md](state.md)). The exception is a start or resume that painted
before the sync settled: its expiry was computed on the uncorrected clock. The
task measures the step against the monotonic clock, and the step is applied
to that expiry through `timer_shift_expiry()` exactly once. Whichever side
reaches it first takes it: the paint that waited for NTP, or
`net_apply_finish()` when the sync landed during the MQTT tail
(`net_window_take_clock_step()` is consume-once). No NTP code writes an expiry
(S13).

## The HA session

`mqtt_ha_window()` (`main/mqtt_ha.c`) does three things in order:
1. publishes discovery if it is stale, then the stat payload;
2. waits briefly for the retained config document and command;
3. applies them: the bulk config document (`config_apply`), the command
   (`cmd_apply`), and the per-entity `set/<key>` edits (`ha_config`).

HA mechanics from the parent's side, including why everything is retained, are
in [Home Assistant setup](../home_assistant/setup.md). The firmware-side details an
agent needs before editing this code (buffer ceilings, the field registry,
schema bumps) are in [agent_notes/home_assistant.md](../agent_notes/home_assistant.md).

**Two config channels, one durable.** The retained bulk document is kept by
the broker and reapplied whenever its `ver` differs from the stored one
(S30). A per-entity `set/` command is cleared once applied, so the document
is the only copy that survives. A field added as an HA entity must therefore
also be accepted by the document; the rule lives above `FIELDS` in
`main/ha_config.c`.

**Day-scoped commands wait for a real day.** While the clock or the day in
RAM is unset, grants and the Screen-adjust target are held on the broker
rather than applied to a placeholder day (S33).

## Discovery republish

HA learns of a new or renamed entity only when discovery is republished. That
happens when `STATS_JSON_DISC_SCHEMA_VER` (`include/stats_json.h`) moves, or
when the discovery fingerprint moves: device name and firmware version, each
extra timer's name and enabled bit, and the chore list (S32). A chore list
that cannot be read is skipped rather than guessed at, and the fingerprint is
not stamped, so the next window tries again.

Discovery and the stat payload go out before the incoming config is applied.
A list change applied in one window therefore reaches HA in the next.
