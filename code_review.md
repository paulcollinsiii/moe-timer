# Code review — `feature/ha-editable-config` (vs `main`)

Reviewed against plan `~/.claude/plans/harmonic-sniffing-abelson.md` (editable HA config entities, Phases A–C).
Scope: commits `9699371..4c1694a`. Host tests: **16/16 pass** as of this review.
Findings are ordered by severity. Nothing has been fixed — this is documentation for the implementing agent.

---

## HIGH severity

### H1. `set/tz` is misrouted as a bulk config document (topic matched by length only)
`main/mqtt_ha.c:108` — the MQTT event handler routes DATA events by comparing **topic length only**:
```c
if (ev->topic_len == s_config_topic_len && ...)          /* config */
else if (ev->topic_len == s_cmd_topic_len && ...)        /* cmd */
else if (ev->topic_len > s_set_prefix_len && ...)        /* set/+ */
```
The device id is always 13 chars (`magtag-xxxxxx`, `main/device_id.c:14`), so:
- config topic `magtag/<id>/config` = **27** chars
- set prefix `magtag/<id>/set/` = 25 chars → `magtag/<id>/set/tz` = **27** chars

A retained `set/tz` message therefore matches the **config** branch first. Consequences:
1. Editing the Timezone text entity in HA **never applies** — the raw TZ string (e.g. `Pacific/Auckland`) lands in `s_config_buf`, fails cJSON parse in `config_apply`, and a `CONFIG_ERROR` ack is retained on `config_ack`.
2. Worse: retained messages are delivered in subscription order (config, cmd, then the set/+ flood), so the retained `set/tz` **overwrites the real retained config document in `s_config_buf` every window** (last write wins, single `s_config_received` flag). One tz edit permanently breaks the bulk-config/holidays path until someone manually clears the retained `set/tz` topic.

Fix direction: compare topic content (`strncmp` against the full topic), not just length — for all three branches. Any future 2-char key would hit the same hole; a 24-length set key can't exist today but the cmd branch has the same latent flaw.

### H2. `cfg` state JSON can exceed its 512-byte buffer and is published truncated
- `main/mqtt_ha.c:228` — `static char cfg[512]`.
- `main/mqtt_ha.c:265` — the return value of `ha_config_state_json(cfg, sizeof(cfg))` is **ignored** and the buffer published unconditionally (contrast the stat publish at mqtt_ha.c:345, which checks `< sizeof(payload)`).

Worst-case registry output is ~**550 bytes**: 8 numeric fields (~160), `name` ≤31 chars (~41), `tz` ≤47 chars (~55), 4 × timer triplets (~73 each ≈ 292), braces/commas. A realistic config (long TZ string, four named timers) is within ~30 bytes of the cliff; a maximal one is over it. `jcat` keeps the buffer NUL-terminated, so the failure mode is silently publishing **truncated, invalid JSON** to the retained `cfg` topic → every editable entity's `value_json.<key>` template fails and all controls in HA go unknown/unavailable.

Fix direction: size the buffer to a computed worst case (≥640, or `#define` derived from the registry) *and* skip/log the publish when the returned length ≥ buffer size. The unit test uses a 768-byte buffer (`test/test_ha_config/test_ha_config.c:311`), so tests can never catch the firmware's 512 — add a test asserting the worst-case length fits the actual firmware buffer size (export the size or a `HA_CONFIG_STATE_MAX`).

### H3. `publish_action_discovery` does not JSON-escape `dev_name` (user-controlled via the new "name" entity)
`main/mqtt_ha.c:202-222` — the Screen-bonus and Find-my-timer discovery payloads interpolate `dev_name` raw into JSON (`"name\":\"%s\"`). The device name is now **directly editable from HA** as free text (`ha_config_set("name", ...)` accepts any bytes, including `"` and `\`). A name containing a quote produces invalid discovery JSON for both action entities — and is a JSON-injection vector into the discovery document (arbitrary keys can be injected into the `dev` block). `ha_config_discovery()` and `stats_json_discovery_named()` both escape via `jesc`; this function is the outlier. Neither snprintf result is checked for truncation either.

Fix direction: route these two payloads through the same escaping (or reuse `ha_config_discovery`-style builders), and check length before publishing. Consider also rejecting `"`/`\`/control chars in `CFG_STR` values at `ha_config_set` (defense in depth) — nothing in the current validation stops them (`main/ha_config.c:157-161`).

---

## MEDIUM severity

### M1. Retained `set/<key>` commands are never cleared → they permanently override the bulk config document
Discovery sets `"retain":true`, so every HA edit stays retained on `magtag/<id>/set/<key>` forever. Each window, `config_apply` (bulk document) runs first (mqtt_ha.c:385) and `apply_sets` runs after (mqtt_ha.c:414) — so a months-old retained `set/weekday_min` **silently re-overrides** any later value from the bulk config document, every single window. The plan explicitly keeps both paths alive; their interaction is unresolved. The `cmd` path clears its retained topic after applying (mqtt_ha.c:407); the set path does not.

Fix direction (pick one, document it): (a) clear each `set/<key>` with a retained empty publish after applying (note: handler ignores empty payloads via the `total_data_len <= 0` guard, so a cleared topic is safe on redelivery); or (b) declare per-field sets authoritative and strip the overlapping fields from the config-document schema/docs.

### M2. Publish-drain accounting skew: action-discovery PUBACKs are counted, the publishes are not
`main/mqtt_ha.c:341` — `publish_action_discovery()` returns void; its 2 QoS-1 publishes are not added to `published`, but their PUBACKs **do** increment `s_pub_acks` (MQTT_EVENT_PUBLISHED, mqtt_ha.c:97). The drain loop (`s_pub_acks < published`) can therefore exit while up to 2 *counted* discovery/stat messages are still unacked, after which `disc_ver` is persisted (mqtt_ha.c:366) — leaving entities missing from HA **permanently** (until the next schema bump), since discovery only republishes on version change.

Fix direction: make `publish_action_discovery` return its publish count and add it to `published`, like `publish_discovery`/`publish_config_discovery`.

### M3. No wait for `set/+` retained delivery, and a data race on the set buffer
- The only gate before `apply_sets` is the config/cmd wait loop (mqtt_ha.c:376), which exits as soon as **config and cmd** have both arrived. The set/+ subscription is issued last, so its retained flood arrives after those — `apply_sets` can run before all (or any) retained sets are in. Retained ones self-heal next window, but the `cfg` republished this window shows pre-edit values, so the HA control the user just changed visibly snaps back for a full window cycle.
- `s_sets[]`/`s_set_count` are written on the MQTT client task and read by `apply_sets` on the main task with no synchronization beyond `volatile int` on the count. Entries arriving mid-iteration are silently skipped for the window, and there is no barrier ordering the struct writes before the count increment (compiler may reorder the non-volatile `memcpy`s past the volatile store).

Fix direction: include "no DATA event for N ms" or a set-specific quiescence check in the wait loop; snapshot `s_set_count` once with an explicit barrier (or stop the subscription before applying).

### M4. NVS setter failures are swallowed — ack says `ok:true`, cfg shows the truth
`main/ha_config.c:145,154,160,169,181,188` — every `set_u16`/`set_str`/`nvs_config_set_timer_defs` return value is discarded. On NVS write failure the function still acks `{"ok":true}` and returns `HA_CFG_OK`. The user's only signal is the control snapping back after the cfg republish, indistinguishable from M3's race. Fix: propagate `esp_err_t` and reject with `"err":"nvs"`.

### M5. Editing "Device name" never updates the HA device block
Discovery (which carries `dev.name`) republishes only when `DISC_SCHEMA_VER` changes (mqtt_ha.c:335-342). Renaming via the new text entity persists to NVS and shows in `cfg`, but the HA device keeps its old name indefinitely. Fix direction: track a name change (e.g. compare against the name used at last discovery, or fold the name into the stored `disc_ver` stamp) and force `fresh_discovery` when it differs.

---

## LOW severity

### L1. `SET_MAX` is exactly at capacity
`main/mqtt_ha.c:52` — `SET_MAX 24` vs 22 registry fields + `screen_bonus` + `locate` = 24 retained commands. Any duplicate (retained + fresh in-window edit) or any future field silently drops messages (no log on the drop path either). Add headroom + a drop log, and a `_Static_assert` tying SET_MAX to the registry size.

### L2. Registry hardcodes slots 1–4 with no compile-time tie to `TIMER_EXTRA_SLOTS`
`main/ha_config.c:81-92` — `TIMER_NAME(1)`..`TIMER_RELOAD(4)` are literal. If `TIMER_EXTRA_SLOTS` (timer.h:39) is ever lowered, `b.defs[f->slot - 1]` writes out of bounds of `nvs_timer_defs_blob_t.defs[TIMER_EXTRA_SLOTS]`. Add `_Static_assert(TIMER_EXTRA_SLOTS == 4, ...)` next to the registry, or bounds-check `slot` in the CFG_T* cases.

### L3. Rollover bonus-clear robustness (plan-accepted, but two sharp edges)
- `s_bonus_clear_pending` (mqtt_ha.c:59) is plain RAM. If the rollover window fails (WiFi down), the flag is lost at deep sleep; the next successful window **re-grants the retained target first** (bonus_applied was reset to 0 by `timer_reset`) and never publishes the clear — the bonus then repeats until the *next* rollover. The plan called a single repeat benign; the lost flag makes it repeat daily while connectivity is bad at rollover time. Persisting the pending flag in `rtc_state_t` (and re-queuing on restore) would close it.
- During the rollover window, `apply_sets` publishes the `act` state from the **pre-reset** `bonus_applied` (mqtt_ha.c:261-263, `timer_reset` runs later at main.c:522), so HA's bonus number shows yesterday's value (e.g. 15) alongside the just-cleared command until the next window.

### L4. Silent skips on truncated discovery payloads
`publish_config_discovery` (mqtt_ha.c:190) drops a too-long payload with no log — an entity just never appears in HA and `disc_ver` is stamped anyway. Log at WARN.

### L5. Per-field ack is built but never published
`ha_config_set` writes an ack JSON per the plan/API contract, but `apply_sets` only `ESP_LOGI`s it (mqtt_ha.c:245). The bulk path publishes `config_ack`; the per-field path gives HA no rejection feedback — an out-of-range HHMM (the number UI happily allows 1275) just snaps back silently. Consider a retained `magtag/<id>/set_ack` (or documenting that cfg-republish is the only feedback).

### L6. `CFG_TRELOAD` accepts any value as OFF
`main/ha_config.c:184-187` — anything other than exactly `"ON"` (including `"on"`, `"true"`, garbage) silently writes `reload=0` and acks ok. Should reject values that are neither `"ON"` nor `"OFF"`.

### L7. Kconfig timer defaults are now write-once
`main/timer_defs.c:57-70` — materializing the Kconfig table into the NVS blob on first boot means later `MAGTAG_TIMER<n>_*` menuconfig changes never take effect once a blob exists (and unlike the scalar defaults, timer defaults are not folded into `nvs_config_defaults_fingerprint`). Intended per the comment ("blob becomes the single source of truth"), but it's a behavior change worth a line in CLAUDE.md/docs; also `nvs_config_set_timer_defs`' failure on this path is ignored.

### L8. Minor robustness/cosmetics
- `jesc` truncation: `esc[80]` for a ≤47-char tz / `dname[64]` for a ≤63-char device name can truncate heavily-escaped strings silently (main/ha_config.c:207,210,231). Benign (output stays valid JSON) but surprising.
- `apply_sets` puts ~290 bytes (`ack[96]`, `topic[96]`, `act[96]`) on the main-task stack at the deepest point of the window; the project deliberately made sibling buffers `static` because this stack is tight under WiFi+MQTT (see the memory note / publish_discovery comment). Worth converting for consistency.
- Set-branch guard reads `ev->topic + s_set_prefix_len - 5` (mqtt_ha.c:121-122); if a DATA event ever arrived with `s_set_prefix_len == 0` this is an OOB read at `topic[-5]`. Unreachable today (prefix is set before any subscribe) but fragile — guard `s_set_prefix_len >= 5`.
- Docs (`docs/home_assistant.md`) advertise `tz` as a working native control — false until H1 is fixed.
- Numbers don't set `"mode"`; HA's auto mode renders 0–2359 HHMM fields as a box (fine), but the plan mentioned an explicit box mode. Cosmetic.

---

## Plan-conformance check (what *was* delivered)

| Plan item | Status |
|---|---|
| Field registry + `ha_config.c` (set/state/discovery) | ✅ |
| `config_is_iso_date` extraction, both callers, tests preserved | ✅ |
| `set/+` subscribe, buffered apply off the MQTT task, cfg republish each window | ✅ (see M3 timing) |
| Entity categories + "Today's limit" rename | ✅ matches plan's grouping exactly |
| `DISC_SCHEMA_VER` bump | ✅ (1→3) |
| Phase B timer fields, empty-name-disables, RMW blob, Kconfig materialization | ✅ |
| Phase C idempotent bonus (`bonus_applied`, snapshot v5, rollover reset + clear) | ✅ (see L3) |
| Self-clearing locate switch (retained OFF publish — `publish()` retain arg confirmed) | ✅ |
| Retained-command clearing for per-field sets | ❌ never specified/implemented → M1 |
| Per-field ack surfaced | ⚠️ built but only logged → L5 |
| `CFG_DATE` kind from the plan sketch | Dropped — fine, no date field was in the Phase A field list |
| Host ctest green + firmware build | Host: ✅ 16/16. `idf.py build` not verified in this review (no toolchain run) |

## Unit-test gaps (for the fixing agent to add)

Positive/negative gaps, in priority order:
1. **State-JSON worst-case size**: build a max-value config (1440s, 31-char name, 47-char tz, four 15-char timer names) and assert `ha_config_state_json`'s return < the *firmware's* buffer size (currently fails → H2). Also assert the return value equals `strlen(buf)` when it fits.
2. **Escaping**: state JSON with `name`/timer-name containing `"` and `\` stays parseable; discovery with a quoted `dev_name` (would have caught H3 had action discovery been in a testable unit — extract it).
3. **Setter-failure propagation**: mock NVS forced to fail → expect `HA_CFG_REJECTED` (currently asserts nothing; would fail until M4 fixed).
4. **`CFG_TRELOAD` negative**: `"on"`, `"true"`, `""` → rejected (fails until L6).
5. **HHMM boundaries**: `"2359"` accepted, `"2400"`/`"0060"` rejected, `"-1"` rejected (only 2130/2160 covered today).
6. **`tz` round trip** through `ha_config_set`/state JSON (registry entry currently untested; also documents the field H1 breaks at the MQTT layer).
7. **`parse_int` trailing junk**: `"45x"`, `" 45 "`, `""` on a CFG_U16 (only `"lots"` covered).
8. **`timer_bonus_reconcile` guards**: negative slot, slot ≥ TIMER_SLOT_COUNT, negative target → no state change.
9. **`timer_defs_install` materialization** (timer_defs.c is currently not host-tested at all): missing blob → Kconfig values land in NVS and in `timer_get_def`; existing blob → Kconfig ignored.
10. **Registry/SET_MAX invariant**: `_Static_assert` or a test that `field_count + 2 <= SET_MAX`.
11. **Discovery truncation**: `ha_config_discovery` into a deliberately small buffer returns ≥ len and the caller contract (skip publish) is exercised.

Untestable-on-host but worth firmware verification: H1 (`set/tz` routing), M2 (drain accounting), M3 (retained-delivery timing) — all live in mqtt_ha.c glue.
