# Network and Home Assistant

How a network window works, how its results get back to the main task, how
clock corrections reach a running timer, when Home Assistant is told about new
entities, and how WiFi and the broker get onto the device. WiFi is powered only
inside a window or a setup session (S29), and a window is short and bounded, so
everything the device exchanges with HA rides one.

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

## Provisioning

A window needs WiFi and, for HA, a broker. Both arrive through setup mode
([wake_cycle.md](wake_cycle.md#setup-mode)), which runs instead of a window and
never beside one: the SoftAP, the HTTP server and the SRP6a handshake share
internal RAM with only a few KB to spare, and a window's MQTT and OTA work needs
the same room. With no broker URI stored, `mqtt_ha_window()` logs
`MQTT disabled: no broker configured; enter it in setup mode` and skips the HA
phase; a WiFi-only device is a supported state.

**Components.** `setup_session_idf.c` runs Espressif's `network_provisioning`
manager (SoftAP scheme, security 2, which is SRP6a) on an `esp_http_server`
instance the app creates itself (6144 B stack, 10 handler slots) and hands to
the scheme. The app registers two more entry points on that server: `GET` and
`POST /mqtt`, a page with no script and no external resources, and a protocomm
endpoint `mqtt-config` that takes the same fields as JSON, so
`esp_prov.py --custom_data` can script it. The stock phone apps collect WiFi
only, which is why MQTT needs its own path.

**The AP and the QR.** `setup_session_make_ap_password()` draws a 10-character
password per session from a 49-character alphabet that drops confusable glyphs,
by rejection sampling over `esp_random()`. It is the WPA2 key, the SRP6a proof
of possession and part of the QR payload, which carries the fields both stock
apps read: `ver`, `name`, `username` (`magtag`), `pop`, `password`, `transport`
(`softap`) and `security` (2). The password lives in RAM and is zeroed once the
AP is up.

**The form.** `mqtt_form.c` parses the urlencoded and JSON bodies through one
validator. It caps the body at 1024 B, accepts only a flat JSON object (the
nesting check runs before cJSON, which recurses on the small httpd stack) and
rejects duplicate fields, embedded NULs and DEL. The URI grammar,
`config_mqtt_uri_check()`, is `mqtt[s]://host[:port][/]` and nothing else, with no
userinfo, so a password can never land in the logged URI. An empty URI is valid
and clears the user and password; a blank password with a URI means keep the
stored one. Both entry points store synchronously through
`setup_session_apply_mqtt()` before they reply, so `Saved.` never appears for a
write that failed. A rejected form leaves a zeroed result, so a decoded password
does not outlive it.

**One credential store.** The app keeps WiFi in its own NVS keys
(`include/nvs_keys.h`), and `wifi_session.c` hands them to `esp_wifi_set_config`
itself, with the driver in `WIFI_STORAGE_RAM`. Those keys are therefore the only
persisted copy. The manager writes its own copy to flash when credentials
arrive, so on a verified join the session copies the SSID and password into the
app's keys and then clears the driver's store (`esp_wifi_restore`, deferred to
teardown), leaving no second copy to disagree. If the copy fails, the session
ends in error and leaves the driver's copy alone. A wrong password is reported
to the phone app, resets the manager so the app can retry, and is stored
nowhere. After a verified join the session stays up for 15 s
(`SETUP_SESSION_SUCCESS_LINGER_MS`): the stock apps keep polling for status and
report failure if the endpoints disappear. The STA netif is created in one
place, `wifi_session_sta_netif()`, and shared with the window code.
