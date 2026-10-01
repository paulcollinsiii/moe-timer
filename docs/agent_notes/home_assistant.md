# Home Assistant internals

For an agent editing `main/mqtt_ha.c`, `main/ha_config.c`,
`main/config_apply.c`, `main/stats_json.c` or `tools/gen_ha_dashboard.py`.
The user-facing contract is in `docs/home_assistant/`. This note covers what
a change must keep true on the firmware side. The detail lives in the code
comments it names; read them before you edit.

## Adding or renaming a field or entity

Each item below fails **silently** if you skip it. A green build proves none
of them.

1. **Bump `STATS_JSON_DISC_SCHEMA_VER`** (`include/stats_json.h`, with a
   one-line comment naming the change). This applies to any added, renamed or
   removed entity, and to any changed discovery payload. Discovery is
   republished only when this version or the discovery fingerprint
   (`ha_config_discovery_gate()`, `include/ha_config.h`: device name,
   firmware version, timer slot names and enablement, chore list) moves.
   Without the bump the entity works on the wire but never appears in HA.
   `mqtt_ha.c` only aliases the constant as `DISC_SCHEMA_VER`.
2. **A settable field goes in `FIELDS`** (`main/ha_config.c`) **and in
   `config_apply()`** (`main/config_apply.c`). Read the durability-rule
   comment above `FIELDS` first.
3. **`def_ent_id` must be a full entity ID**, `"<component>.<uniq_id>"`. HA
   keeps only what follows the first dot, so a dotless value registers an
   empty object ID. The registry and `stats_json.c` tables build it for you.
   The two hand-written action payloads in `publish_action_discovery()`
   (`mqtt_ha.c`: `screen_bonus` and `locate`, the only entities outside both
   tables) do not. HA removed `obj_id` in 2026.4.0. Its MQTT schemas strip
   unknown keys without a warning, so a wrong key name costs nothing visible.
4. **Place the key in the dashboard generator**, in a layout group or in
   `LEFT_OUT` with a reason (`tools/gen_ha_dashboard.py`).
   `test_gen_ha_dashboard`'s coverage check fails until you do.
5. **Measure the buffers** (below), and document the field in the controls
   table of `docs/home_assistant/configuring.md`, the one list of keys.

## Order inside one window

`mqtt_ha_window()`:

1. subscribe to `config`, `cmd` and `set/+`, so retained delivery overlaps
   the publishes;
2. `publish_states()`: the discovery gate and passes, `stat`, and the pending
   `summary`;
3. drain the QoS-1 acks; only after a full drain does it stamp `disc_ver` and
   the fingerprint, and clear the summary;
4. `apply_incoming()`: wait up to `RETAINED_RX_TIMEOUT_MS` (1500 ms) for the
   retained `config` and `cmd`; publish any `too_long` refusals; apply the
   document (`config_ack` only when not `CONFIG_SKIPPED`); apply the command;
   then `apply_sets()`: every `set/<key>`, then republish `act` and `cfg`;
5. a final drain.

Consequences: anything the document changes reaches discovery and `stat` in
the **next** window (the setup text's "two windows" depends on this, and
`test_two_window_claim_matches_the_firmware_order` pins it). Within one
window a pending `set/` edit lands after the document and wins.

## How `set/` and `cmd` are consumed

- A `set/<key>` that `ha_config_set()` answers `HA_CFG_OK` (including a
  clamp) is cleared by publishing an empty retained payload. Every refusal
  stays retained and is re-refused every window. The per-field ack goes to
  the serial log only.
- `set/locate` is cleared by publishing `OFF`. `set/screen_bonus` is never
  cleared on apply. It is idempotent (`timer_bonus_reconcile`), and the day
  rollover publishes `0` to it.
- Day-scoped commands (the Screen-adjust target, a `cmd` grant) follow
  `include/ha_day_cmds.h` under `no_clock`: hold (stay retained, no ack), or
  drop when the day is being cleared.
- `cmd` is deduplicated on the last applied `id` (NVS). Only
  `CMD_GRANT`/`CMD_LOCATE` publish an `event` ack and clear `cmd`. A refused
  or duplicate command publishes nothing and stays retained.
- A `ver` match skips the document with no ack. The one exception rebuilds an
  unreadable timer table from the retained `timers` array (BUG-5; see the
  comment in `config_apply()`).

## Buffer ceilings

| Buffer | Size | Over it | Guarded by |
|--------|------|---------|------------|
| inbound `config` (`CONFIG_BUF_MAX`, `include/config_apply.h`) | 2048 (2047 usable) | refused whole, retained `config_ack` `too_long` | `test_the_worst_case_document_fits_the_receive_buffer` (it prints the current worst case) |
| MQTT receive (`buffer.size` in `mqtt_ha_window()`) | `CONFIG_BUF_MAX` + packet overhead | an accepted document would fragment and drop **silently** | the comment above the client config there: keep it at least the document ceiling |
| inbound `cmd` (`CMD_BUF_MAX`) | 256 | `too_long` on `event`, not retained | — |
| outbound `cfg` (`HA_CONFIG_STATE_MAX`, `include/ha_config.h`) | 1536 | `cfg` not published: **every control goes unavailable** in HA | `test_state_json_worst_case_fits_firmware_buffer` (`test_ha_config`) |
| stat/discovery payload (`stats_json.h`) | see there | that payload skipped, logged | `test_stat_payload_worst_case_fits_the_publish_buffer` |

A worst-case test must render the longest form of every axis, or it
understates the maximum.

## Known silent edges

- **`screen_bonus` and `locate` have no `FIELDS` row.** They are published
  from `mqtt_ha.c`, with their state on `act`, not `cfg`.
- **The bulk document leaves an invalid chore-free pair standing**, and names
  it in the ack. The `set/` path refuses or clamps. This asymmetry is
  deliberate: the only `set/`-free paths to the config-error lock are the bulk
  document and a reseed. See `config_validate.h`.
- **The document skips the character check on `name`, `tz` and
  `timers[].name`** (`apply_str()` with `valid=NULL`, and a length-only check
  in the timers loop), which the `set/` path refuses as `char`. Open with the
  owner; `configuring.md` tells parents to keep those characters out.
- **A same-window collision** between a retained document and a pending
  `set/` can leave `config_ack` reporting an error that the `set/` clamp has
  already fixed. Recorded in `ha_config_set()`, not fixed.
- **Text blanks** arrive as the two characters `""` (`HA_CONFIG_TEXT_BLANK`,
  schema v24). A zero-length retained publish is an MQTT delete and never
  arrives.
