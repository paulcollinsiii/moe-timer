#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pyyaml>=6",
#     "paho-mqtt>=2",
# ]
# ///
"""Generate the MagTag Home Assistant dashboard, and everything around it.

    uv run tools/gen_ha_dashboard.py [--devices tools/ha_devices.yaml]
                                     [--part {setup,automation,dashboard,all}]
    uv run tools/gen_ha_dashboard.py --mqtt [--sdkconfig sdkconfig] [--wait 10]
                                     [--devices FILE] [--part ...]

Prints three parts, in order:

  1. the setup steps (OTA, re-registration, one To-do list per device);
  2. the config-publishing automation, verbatim from
     tools/ha/magtag_publish_config.yaml, with its install notes;
  3. the dashboard YAML: one view (tab) per device, to paste into a new
     dashboard's raw configuration editor.

The generator never talks to Home Assistant: no API, no token. The devices
come from a small local file (tools/ha_devices.yaml, gitignored; see
tools/ha_devices.example.yaml), or with --mqtt from the MQTT broker.

THE ENTITY SET IS AN INPUT. build_dashboard() takes, per device, the set of
entities that device has. File mode derives it from the firmware's own
tables (firmware_entities(): main/stats_json.c ENTITIES, main/ha_config.c
FIELDS and the two hand-written discovery payloads in main/mqtt_ha.c), so
every device gets every entity. The layout (LAYOUT below) names keys; a key
the device does not have is simply skipped. So file mode names every timer
slot (1-4) and chore row (1-3): a device with a disabled slot or fewer
chores shows "entity not available" rows for them, and part 1 and the
dashboard header say so.

--mqtt reads the broker settings (CONFIG_MAGTAG_MQTT_URI/USER/PASS) from the
gitignored sdkconfig, collects the retained discovery documents
(homeassistant/<component>/magtag-<node>_<key>/config) and builds the device
list and each device's exact entity set from them: an empty (retired)
payload is skipped, so a disabled slot or an unused chore row gets no card.
The tab label is the discovery dev.name. A key this generator's layout does
not know (newer firmware) goes to an "Other" part on the tab, with a warning.
A device on older firmware than this checkout (it lacks an entity the
checkout's firmware always publishes, or publishes no state_class where the
checkout's firmware has one) still gets a tab, built from what it publishes,
plus a warning to OTA it and re-run. In --mqtt mode a statistics graph keeps
only the entities whose published state_class suits its stat_types
(graph_compatible()), and is dropped when none is left. A device whose
payloads carry no def_ent_id (firmware before discovery schema v20) gets no
tab: its HA entity ids are name-derived and cannot be known here; part 1
says to OTA it first.

The password is never printed. It is read into Broker.password (kept out of
its repr) and handed to paho, nowhere else. Everything --mqtt prints once it
is read -- errors, warnings and the three parts, stderr and stdout alike --
leaves through one function, _emit(), which replaces the password with
"<password>" wherever it occurs: in a library's error text, and in broker
data too (a device named after the password would show "<password>" on its
tab). The only messages printed before the password is read (a missing or
malformed sdkconfig) are built from key names and the path.

The firmware tables are read as plain C rows. A preprocessor conditional
(#if / #ifdef) inside ENTITIES[] or FIELDS[], or a discovery payload in
mqtt_ha.c whose def_ent_id the parser cannot read, stops the generator with
a ValueError naming the table: loud on purpose, since a row it skipped would
silently go missing from the dashboard. Move the conditional out of the
table (or teach the parser here), and give every hand-written payload a
literal def_ent_id "<component>.%s_<key>".

paho-mqtt is declared above for that mode and must only ever be imported
inside it: file mode and its tests run without it.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
import textwrap
import time
from collections import Counter
from dataclasses import dataclass, field
from urllib.parse import urlsplit

import yaml

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEFAULT_DEVICES = os.path.join("tools", "ha_devices.yaml")
EXAMPLE_DEVICES = os.path.join("tools", "ha_devices.example.yaml")
AUTOMATION = os.path.join(REPO, "tools", "ha", "magtag_publish_config.yaml")

NODE_RE = re.compile(r"^[0-9a-f]{6}$")


# --------------------------------------------------------------------------
# Entity ids
# --------------------------------------------------------------------------
#
# Every discovery payload carries def_ent_id = "<component>.<uniq_id>", and
# uniq_id is "<device_id>_<key>" with device_id "magtag-<node>" (device_id.c:
# "magtag-%02x%02x%02x" over the last three MAC bytes, lowercase hex). So the
# published value is e.g. "sensor.magtag-1a0a5c_battery".
#
# HA keeps what follows the first dot and runs it through
# homeassistant.util.slugify (async_generate_entity_id -> slugify), which
# lowercases and replaces every run of characters outside [a-z0-9] with the
# separator "_". The only such character here is the '-' of the device id,
# so the registered entity id is "sensor.magtag_1a0a5c_battery". Keys are
# already [a-z0-9_] (the parser below asserts it), so nothing else moves.
# docs/home_assistant.md ("Entity IDs are stable") documents the same rule.


def entity_id(component: str, node: str, key: str) -> str:
    return f"{component}.magtag_{node}_{key}"


def todo_entity_id(node: str) -> str:
    """The Local To-do list the owner creates as 'MagTag <node> chores'.

    HA slugs the list's name into its entity id; the committed automation
    finds every list matching todo.magtag_<6 hex>_chores."""
    return f"todo.magtag_{node}_chores"


def todo_list_name(node: str) -> str:
    return f"MagTag {node} chores"


# --------------------------------------------------------------------------
# The firmware's entity set, read from source
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Entity:
    component: str  # sensor / binary_sensor / number / text / switch / select
    key: str  # the uniq_id suffix, e.g. "battery", "weekday_min"
    name: str  # the firmware's default display name
    state_class: str | None = None  # ENTITIES only; None = no statistics


_KEY_RE = re.compile(r"^[a-z0-9_]+$")


def _strip_comments(src: str) -> str:
    """Drop /* */ and // comments, leaving string and char literals alone
    (a '"' char literal must not open a string)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i : j + 1])
            i = j + 1
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _split_top(text: str, sep: str = ",") -> list[str]:
    """Split on `sep` outside strings, (), {} and []."""
    parts, depth, cur, i = [], 0, [], 0
    while i < len(text):
        c = text[i]
        if c == '"':
            j = i + 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            cur.append(text[i : j + 1])
            i = j + 1
            continue
        if c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    tail = "".join(cur).strip()
    if tail:
        parts.append(tail)
    return parts


def _array_body(src: str, decl_re: str) -> str:
    """The text between the braces of `<decl> = { ... };`."""
    m = re.search(decl_re + r"\s*=\s*\{", src)
    if not m:
        raise ValueError(f"table not found: {decl_re}")
    i, depth = m.end(), 1
    start = i
    while depth:
        c = src[i]
        if c == '"':
            j = i + 1
            while src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return src[start : i - 1]


def _reject_preprocessor(body: str, table: str, parser: str) -> None:
    """A conditional row cannot be evaluated here; fail loudly, not by guessing."""
    m = re.search(r"^\s*#\s*\w+.*$", body, re.M)
    if m:
        raise ValueError(
            f"{table}: preprocessor line {m.group(0).strip()!r} inside the table. "
            f"tools/gen_ha_dashboard.py reads the table as plain rows and cannot evaluate it: "
            f"move the conditional out of {table}, or extend {parser}() to handle it."
        )


def _string_macros(src: str) -> dict[str, str]:
    """#define NAME "literal" (object-like string macros)."""
    return {m.group(1): m.group(2) for m in re.finditer(r'^#define\s+(\w+)\s+"((?:[^"\\]|\\.)*)"\s*$', src, re.M)}


def _c_string(expr: str, env: dict[str, str | None]) -> str | None:
    """Evaluate a C string expression: adjacent literals, #param, names from env, NULL."""
    expr = expr.strip()
    if expr == "NULL":
        return None
    out = []
    for tok in re.finditer(r'"((?:[^"\\]|\\.)*)"|#\s*(\w+)|(\w+)', expr):
        lit, stringified, name = tok.groups()
        if lit is not None:
            out.append(lit)
        elif stringified is not None:
            out.append(str(env[stringified]).strip())
        else:
            val = env.get(name)
            if val is None:
                return None
            out.append(val)
    return "".join(out)


def parse_stats_entities(src: str) -> list[Entity]:
    """main/stats_json.c ENTITIES[]: positional rows
    {component, key, name, unit, dev_class, tpl, topic_suffix, expire_after,
     binary, ent_cat, state_class, last_reset_tpl}."""
    macros = _string_macros(src)
    env: dict[str, str | None] = dict(macros)
    body = _array_body(_strip_comments(src), r"ha_entity_t\s+ENTITIES\[\]")
    _reject_preprocessor(body, "main/stats_json.c ENTITIES[]", "parse_stats_entities")
    rows = []
    for row in _split_top(body):
        if not (row.startswith("{") and row.endswith("}")):
            raise ValueError(f"unexpected ENTITIES row: {row[:60]}")
        f = _split_top(row[1:-1])
        if len(f) != 12:
            raise ValueError(f"ENTITIES row has {len(f)} fields, want 12: {row[:60]}")
        comp, key, name = (_c_string(x, env) for x in f[:3])
        rows.append(Entity(comp, key, name, _c_string(f[10], env)))
    return rows


def _fn_macros(src: str) -> dict[str, tuple[list[str], str]]:
    """Function-like #defines, with line continuations joined."""
    joined = re.sub(r"\\\n", " ", src)
    out = {}
    for m in re.finditer(r"^#define\s+(\w+)\(([^)]*)\)(.*)$", joined, re.M):
        params = [p.strip() for p in m.group(2).split(",") if p.strip()]
        out[m.group(1)] = (params, m.group(3))
    return out


def _designated(body: str, field: str) -> str | None:
    m = re.search(r"\." + field + r"\s*=\s*", body)
    if not m:
        return None
    rest = body[m.end() :]
    return _split_top(rest.strip().lstrip("{").rstrip("}"))[0]


def parse_config_fields(src: str) -> list[Entity]:
    """main/ha_config.c FIELDS[]: every row is a macro invocation
    (NUM_U16("weekday_min", ...), TIMER_NAME(1), ...). Each macro's own
    definition says where .key / .component / .name come from, so a new
    macro or a changed key expression is followed, not hand-copied."""
    macros = _fn_macros(src)
    body = _array_body(_strip_comments(src), r"cfg_field_t\s+FIELDS\[\]")
    _reject_preprocessor(body, "main/ha_config.c FIELDS[]", "parse_config_fields")
    rows = []
    for row in _split_top(body):
        m = re.match(r"^(\w+)\((.*)\)$", row, re.S)
        if not m or m.group(1) not in macros:
            raise ValueError(f"unexpected FIELDS row: {row[:60]}")
        params, mbody = macros[m.group(1)]
        args = _split_top(m.group(2))
        if len(args) != len(params):
            raise ValueError(f"{m.group(1)}: {len(args)} args for {len(params)} params")
        env: dict[str, str | None] = {}
        for p, a in zip(params, args):
            env[p] = _c_string(a, {}) if a.startswith('"') else a
        vals = {}
        for fld in ("key", "component", "name"):
            expr = _designated(mbody, fld)
            if expr is None:
                raise ValueError(f"{m.group(1)} sets no .{fld}")
            vals[fld] = _c_string(expr, env)
        rows.append(Entity(vals["component"], vals["key"], vals["name"]))
    return rows


def parse_action_entities(src: str) -> list[Entity]:
    """main/mqtt_ha.c's hand-written discovery payloads (screen_bonus,
    locate), found by their def_ent_id "<component>.%s_<key>" literal and
    the "name" literal of the same payload. The quotes are escaped inside a
    C string (\\"), and whitespace around the colon is tolerated.

    Cross-check: every payload's uniq_id "%s_<key>" literal must have a
    def_ent_id this parser read. A payload without one (or with a form it
    cannot read) raises instead of dropping the entity."""
    q = r'\\"'  # an escaped quote inside a C string literal
    rows = []
    for m in re.finditer(q + r"def_ent_id" + q + r"\s*:\s*" + q + r"(\w+)\.%s_(\w+)" + q, src):
        names = re.findall(q + r"name" + q + r"\s*:\s*" + q + r'([^"\\]*)' + q, src[: m.start()])
        rows.append(Entity(m.group(1), m.group(2), names[-1] if names else m.group(2)))
    uniq = re.findall(q + r"uniq_id" + q + r"\s*:\s*" + q + r"%s_(\w+)" + q, src)
    unread = sorted(set(uniq) - {e.key for e in rows})
    if unread or len(uniq) != len(rows):
        raise ValueError(
            f"main/mqtt_ha.c: {len(uniq)} discovery payload(s) carry a uniq_id but the generator read "
            f"{len(rows)} def_ent_id(s); unread: {unread or '(count mismatch)'}. Give every hand-written "
            f'payload a literal \\"def_ent_id\\":\\"<component>.%s_<key>\\" (see the comment above '
            f"screen_bonus in mqtt_ha.c), or extend parse_action_entities() in tools/gen_ha_dashboard.py."
        )
    return rows


def firmware_entities(repo: str = REPO) -> dict[str, Entity]:
    """Every entity the firmware can publish, keyed by key."""

    def read(rel):
        with open(os.path.join(repo, rel), encoding="utf-8") as fh:
            return fh.read()

    rows = (
        parse_stats_entities(read("main/stats_json.c"))
        + parse_config_fields(read("main/ha_config.c"))
        + parse_action_entities(read("main/mqtt_ha.c"))
    )
    out: dict[str, Entity] = {}
    for e in rows:
        if not _KEY_RE.match(e.key or ""):
            raise ValueError(f"key {e.key!r} is not [a-z0-9_]: the entity id rule would not hold")
        if e.key in out:
            raise ValueError(f"duplicate key {e.key!r}")
        out[e.key] = e
    return out


# --------------------------------------------------------------------------
# Layout
# --------------------------------------------------------------------------
#
# The tab, top to bottom: Settings, Activity, Graphs, Diagnostics (owner,
# 2026-09-24). Each tuple below is one HA "sections" section. Rows name
# firmware keys; the builder turns them into cards and skips a key the
# device does not have.
#
# NAMES. Rows default to `name: {type: entity}` (HA 2025.11+): the card
# shows the entity's own name from discovery, without the device name in
# front, so a timer or chore renamed on the device shows through (the
# per-slot rows are named "<timer> remaining" etc. at runtime). An explicit
# string is given only where the discovery name is too long for the space
# it gets: the paired allocation tiles, half a row wide each.

PER_SLOT = ("remaining", "limit", "completions", "day_runs")
SLOTS = (1, 2, 3, 4)
CHORES = (1, 2, 3)

# (day type, allocation key, chore_free key) -- the pairs ha_config.c's
# NUM_CHORE_FREE rows name; the test re-derives them from source.
ALLOC_PAIRS = (
    ("Weekday", "weekday_min", "chore_free_wd"),
    ("Weekend", "weekend_min", "chore_free_we"),
    ("Holiday", "holiday_min", "chore_free_hol"),
    ("Summer", "summer_min", "chore_free_sum"),
)

# Firmware keys deliberately not on the tab, each with its reason. Empty
# today; the coverage test fails on any firmware key that is neither placed
# nor listed here.
LEFT_OUT: dict[str, str] = {}

TIMER_FIELDS = ("name", "min", "reload", "break")

SETTINGS = [
    ("Chores", ["todo"] + [f"chore_{i}" for i in CHORES]),
    ("Allocations & chore-free (min)", ["alloc_pairs", "screen_bonus"]),
    ("Timers", [("timer", n) for n in SLOTS]),
    ("Breaks", ["break_interval_min", "break_duration_min"]),
    ("Quiet hours & bed time", ["quiet_start", "quiet_end", "bedtime"]),
    ("Tones & volume", ["tone_expiry", "tone_break", "tone_bed", "alert_volume"]),
    ("System & OTA", ["name", "tz", "ota_url", "ota_on_sync", "locate"]),
]

NOW = [
    "state",
    "active_timer",
    "screen_remaining",
    "screen_limit",
    "screen_break",
    "break_remaining",
    "screen_exposure",
    "charge_lock",
    "chores_left",
    "chores_done",
    "battery",
    "day_type",
]
NOW_SLOTS = [f"{p}_{n}" for n in SLOTS for p in ("remaining", "limit", "completions")]

# Graphs: (title, card spec). stat_types / period per the owner's list.
HISTORY_KEYS = ["screen_remaining"] + [f"remaining_{n}" for n in SLOTS]
GRAPHS = [
    ("Battery", dict(keys=["battery"], stat_types=["mean"], period="day", days=56, chart_type="line")),
    (
        "Extra timer runs per day",
        dict(
            keys=[f"day_runs_{n}" for n in SLOTS],
            stat_types=["change"],
            period="day",
            days=28,
            chart_type="bar",
            note=True,
        ),
    ),
    (
        "Extra timer runs per week",
        dict(
            keys=[f"day_runs_{n}" for n in SLOTS],
            stat_types=["change"],
            period="week",
            days=84,
            chart_type="bar",
            note=True,
        ),
    ),
    (
        "Screen minutes per day",
        dict(keys=["screen_used_day"], stat_types=["change"], period="day", days=28, chart_type="bar", note=True),
    ),
    ("Chores done per day", dict(keys=["chores_done"], stat_types=["max"], period="day", days=56, chart_type="bar")),
]

# The state_classes each statistics-graph stat_type can read (HA
# statistics): a `change` needs a sum, which only total / total_increasing
# keep; mean/min/max need a measurement. build_view() keeps a graph entity
# only if its state_class suits every stat_type of the card: in file mode
# that is the firmware table's state_class (all suit), in --mqtt mode the
# stat_cla the device published (older firmware publishes none). The
# history-graph needs no state_class (recorder history) and is not filtered.
STAT_TYPE_STATE_CLASSES = {
    "change": frozenset({"total", "total_increasing"}),
    "sum": frozenset({"total", "total_increasing"}),
    "mean": frozenset({"measurement"}),
    "min": frozenset({"measurement"}),
    "max": frozenset({"measurement"}),
}


def graph_compatible(state_class: str | None, stat_types) -> bool:
    return all(state_class in STAT_TYPE_STATE_CLASSES[st] for st in stat_types)


DAY_SHIFT_NOTE = (
    "Screen minutes and extra timer runs come from the daily summary, which the device "
    "sends at its first check-in after midnight. HA files each day's figures under the "
    "**following** day, so a run finished on a Sunday counts in the next week."
)

DIAGNOSTICS = [
    ("Health", ["config_warning", "battery_mv", "light", "last_reset", "nvs_free"]),
    ("Memory", ["heap_free", "heap_min", "stack_main", "stack_net"]),
    ("Panic", ["panic_count", "panic_phase", "panic_uptime", "panic_heap", "panic_stack_main", "panic_stack_net"]),
    ("Updates", ["ota_result", "ota_target", "ota_fails", "ota_dl_ms"]),
    ("Daily summary (last received)", ["screen_used_day"] + [f"day_runs_{n}" for n in SLOTS]),
]


def placed_keys() -> set[str]:
    """Every firmware key the layout places (whether or not a device has it)."""
    keys: set[str] = set()
    for _, rows in SETTINGS:
        for r in rows:
            if r == "todo":
                continue
            if r == "alloc_pairs":
                for _, a, f in ALLOC_PAIRS:
                    keys |= {a, f}
            elif isinstance(r, tuple):
                keys |= {f"timer{r[1]}_{fld}" for fld in TIMER_FIELDS}
            else:
                keys.add(r)
    keys |= set(NOW) | set(NOW_SLOTS) | set(HISTORY_KEYS)
    for _, g in GRAPHS:
        keys |= set(g["keys"])
    for _, rows in DIAGNOSTICS:
        keys |= set(rows)
    return keys


def coverage_gaps(fw_keys) -> set[str]:
    """Firmware keys neither placed on the tab nor explicitly LEFT_OUT."""
    return set(fw_keys) - placed_keys() - set(LEFT_OUT)


# A key the layout does not know -- a device on newer firmware than this
# generator, seen through --mqtt -- is not dropped: build_view() lists it in
# an "Other" part above Diagnostics. File mode never has one (the coverage
# test above fails first).
OTHER_PART = "Other"
OTHER_TITLE = "Not in this generator's layout (newer firmware?)"


# --------------------------------------------------------------------------
# Dashboard
# --------------------------------------------------------------------------

ENTITY_NAME = {"type": "entity"}


def _heading(text: str, style: str = "title") -> dict:
    return {"type": "heading", "heading": text, "heading_style": style}


def _row(eid: str, name=None) -> dict:
    return {"entity": eid, "name": name if name is not None else dict(ENTITY_NAME)}


def build_view(node: str, label: str, entities: dict[str, Entity]) -> dict:
    """One tab for one device. `entities` is that device's entity set."""

    def eid(key):
        e = entities[key]
        return entity_id(e.component, node, key)

    def rows(keys):
        return [_row(eid(k)) for k in keys if k in entities]

    sections = []

    def section(title, cards, part=None, span=None):
        cards = [c for c in cards if c]
        if not cards:
            return
        head = [_heading(part, "title")] if part else []
        s = {"type": "grid", "cards": head + [_heading(title, "subtitle")] + cards}
        if span:
            s["column_span"] = span
        sections.append(s)

    def entities_card(keys):
        r = rows(keys)
        return {"type": "entities", "entities": r} if r else None

    # ---- Settings
    for i, (title, spec) in enumerate(SETTINGS):
        cards = []
        plain = []
        for r in spec:
            if r == "todo":
                cards.append({"type": "todo-list", "entity": todo_entity_id(node)})
            elif r == "alloc_pairs":
                for day, a, f in ALLOC_PAIRS:
                    # Tiles without a numeric-input feature: its buttons
                    # step by 1 over a 1..1440 range and its slider cannot
                    # land on an exact minute. A tap opens the more-info
                    # dialog, where the number's box mode takes a typed value.
                    pair = [
                        {"type": "tile", "entity": eid(k), "name": nm}
                        for k, nm in ((a, day), (f, "Chore-free"))
                        if k in entities
                    ]
                    if pair:
                        cards.append({"type": "horizontal-stack", "cards": pair})
            elif isinstance(r, tuple):
                n = r[1]
                keys = [f"timer{n}_{fld}" for fld in TIMER_FIELDS if f"timer{n}_{fld}" in entities]
                if keys:
                    # The firmware names these "Timer N name", "Timer N
                    # minutes"... and {type: entity} shows exactly that, so
                    # a "Timer N" label above them would say it twice. A
                    # divider groups them instead; hard-coding short names
                    # ("Minutes") would copy firmware strings into this file.
                    if plain:
                        plain.append({"type": "divider"})
                    plain.extend(rows(keys))
            else:
                plain.extend(rows([r]))
        if plain:
            cards.append({"type": "entities", "entities": plain})
        # The paired tiles are half a row each: a double-width section keeps
        # their labels whole on a wide screen (a phone shows one column).
        section(title, cards, part="Settings" if i == 0 else None, span=2 if "alloc_pairs" in spec else None)

    # ---- Activity
    section("Now", [entities_card(NOW), entities_card(NOW_SLOTS)], part="Activity")
    all_ids = sorted(eid(k) for k in entities)
    section("Activity log", [{"type": "logbook", "hours_to_show": 48, "target": {"entity_id": all_ids}}], span=2)

    # ---- Graphs
    hist = rows(HISTORY_KEYS)
    section(
        "Remaining time today",
        [{"type": "history-graph", "hours_to_show": 24, "entities": hist} if hist else None],
        part="Graphs",
        span=2,
    )
    for title, g in GRAPHS:
        ents = rows(
            [k for k in g["keys"] if k in entities and graph_compatible(entities[k].state_class, g["stat_types"])]
        )
        if not ents:
            continue
        section(
            title,
            [
                {"type": "markdown", "content": DAY_SHIFT_NOTE} if g.get("note") else None,
                {
                    "type": "statistics-graph",
                    "entities": ents,
                    "stat_types": list(g["stat_types"]),
                    "period": g["period"],
                    "days_to_show": g["days"],
                    "chart_type": g["chart_type"],
                }
            ],
            span=2,
        )

    # ---- Other: entities the layout does not place (see OTHER_PART)
    section(OTHER_TITLE, [entities_card(sorted(coverage_gaps(entities)))], part=OTHER_PART)

    # ---- Diagnostics (bottom)
    for i, (title, keys) in enumerate(DIAGNOSTICS):
        section(title, [entities_card(keys)], part="Diagnostics" if i == 0 else None)

    return {"title": label, "path": f"magtag-{node}", "type": "sections", "max_columns": 4, "sections": sections}


def build_dashboard(devices: list[dict], entity_sets: dict[str, dict[str, Entity]]) -> dict:
    """devices: [{node, label}]; entity_sets: node -> that device's entities.
    A device with no entity set gets no tab: --mqtt leaves out a device
    whose entity ids it cannot know (see no_tab_devices())."""
    views = [build_view(d["node"], d["label"], entity_sets[d["node"]]) for d in devices if d["node"] in entity_sets]
    return {"title": "MagTag", "views": views}


def no_tab_devices(devices: list[dict], entity_sets: dict[str, dict[str, Entity]]) -> list[dict]:
    return [d for d in devices if d["node"] not in entity_sets]


# --------------------------------------------------------------------------
# Devices file
# --------------------------------------------------------------------------


class UsageError(Exception):
    pass


def load_devices(path: str) -> list[dict]:
    if not os.path.exists(path):
        raise UsageError(
            f"devices file not found: {path}\n"
            f"Copy {EXAMPLE_DEVICES} to {DEFAULT_DEVICES} and list your devices "
            f"(node = the 6 hex digits after 'magtag-' in the device id), or pass --devices."
        )
    with open(path, encoding="utf-8") as fh:
        doc = yaml.safe_load(fh)
    return parse_devices(doc, path)


def parse_devices(doc, where: str = "devices file") -> list[dict]:
    if not isinstance(doc, dict) or not isinstance(doc.get("devices"), list) or not doc["devices"]:
        raise UsageError(f"{where}: expected a non-empty 'devices:' list")
    out, seen = [], set()
    for i, d in enumerate(doc["devices"]):
        if not isinstance(d, dict):
            raise UsageError(f"{where}: devices[{i}] is not a mapping")
        node, label = d.get("node"), d.get("label")
        if not isinstance(node, str) or not NODE_RE.match(node):
            raise UsageError(
                f"{where}: devices[{i}].node = {node!r}: want 6 lowercase hex digits in quotes, "
                f'e.g. node: "1a0a5c" (unquoted, 123456 reads as a number)'
            )
        if not isinstance(label, str) or not label.strip():
            raise UsageError(f"{where}: devices[{i}].label must be a non-empty string")
        if node in seen:
            raise UsageError(f"{where}: node {node} listed twice")
        seen.add(node)
        out.append({"node": node, "label": label.strip()})
    return out


# --------------------------------------------------------------------------
# --mqtt: devices and entity sets from the retained discovery documents
# --------------------------------------------------------------------------
#
# Split in two so the logic is testable without a broker:
#   fetch_retained()    the thin paho adapter: connect, subscribe, collect
#                       {topic: payload} until the broker goes quiet;
#   collect_discovery() pure: those messages -> devices, entity sets, warnings.
#
# THE PASSWORD. It is read from sdkconfig into Broker.password (repr=False)
# and handed to paho, and goes nowhere else: no message built here names it,
# and main_mqtt() prints everything -- errors, warnings, the parts -- through
# _emit(), which scrubs it, in case a library or the broker data ever
# carries it.

DEFAULT_SDKCONFIG = "sdkconfig"
SDK_URI, SDK_USER, SDK_PASS = "CONFIG_MAGTAG_MQTT_URI", "CONFIG_MAGTAG_MQTT_USER", "CONFIG_MAGTAG_MQTT_PASS"

# MQTT wildcards match whole levels only, so "magtag-*" cannot be filtered
# on the broker; collect_discovery() drops every other integration's topic.
DISCOVERY_FILTER = "homeassistant/+/+/config"
DISC_TOPIC_RE = re.compile(r"^homeassistant/([a-z_]+)/magtag-([0-9a-f]{6})_([a-z0-9_]+)/config$")

IDLE_S = 2.0  # stop this long after the last message (retained ones arrive in a burst after SUBACK)
DEFAULT_WAIT_S = 10.0  # hard cap on the whole scan, connect included
DEFAULT_PORT = {"mqtt": 1883, "mqtts": 8883}


class MqttError(Exception):
    """A --mqtt failure. Its text names the host, never the password."""


@dataclass(frozen=True)
class Broker:
    host: str
    port: int
    tls: bool
    user: str
    password: str = field(repr=False)

    def where(self) -> str:
        return f"{'mqtts' if self.tls else 'mqtt'}://{self.host}:{self.port}"


def _scrub(text: str, secret: str | None) -> str:
    return text.replace(secret, "<password>") if secret else text


def _emit(stream, text: str, secret: str | None = None) -> None:
    """The one exit for everything this tool prints, argparse's usage
    errors aside (see the module docstring): --mqtt passes the password as
    `secret` once it has read it."""
    stream.write(_scrub(text, secret))


_SDK_LINE_RE = re.compile(r"^(CONFIG_\w+)=(.*)$")
_SDK_STRING_RE = re.compile(r'^"((?:[^"\\]|\\.)*)"$')


def parse_sdkconfig(text: str, where: str = "sdkconfig") -> dict[str, str]:
    """The three broker keys from an sdkconfig. Kconfig writes a string as
    "..." with '\\' and '"' backslash-escaped. Messages name the key, never
    its value."""
    out: dict[str, str] = {}
    for line in text.splitlines():
        m = _SDK_LINE_RE.match(line.strip())
        if not m or m.group(1) not in (SDK_URI, SDK_USER, SDK_PASS):
            continue
        s = _SDK_STRING_RE.match(m.group(2).strip())
        if not s:
            raise UsageError(f"{where}: {m.group(1)} is not a quoted string")
        out[m.group(1)] = re.sub(r"\\(.)", r"\1", s.group(1))
    missing = [k for k in (SDK_URI, SDK_USER, SDK_PASS) if k not in out]
    if missing:
        raise UsageError(
            f"{where}: {', '.join(missing)} not found. Is this the firmware's sdkconfig? "
            f"Run `idf.py reconfigure` once, or pass --sdkconfig PATH."
        )
    if not out[SDK_URI].strip():
        raise UsageError(f"{where}: {SDK_URI} is empty; set the broker URI (idf.py menuconfig) or pass --sdkconfig.")
    return out


def parse_broker_uri(uri: str) -> tuple[bool, str, int]:
    """mqtt://host[:port] or mqtts://host[:port] -> (tls, host, port).
    The URI is never echoed: a user:password@ part in it would be a secret."""
    scheme = uri.split("://", 1)[0].lower() if "://" in uri else ""
    if scheme not in DEFAULT_PORT:
        raise UsageError(
            f"{SDK_URI}: want mqtt://host[:port] or mqtts://host[:port]"
            + (f", not {scheme}://" if scheme.isalnum() else "")
        )
    parts = urlsplit(uri.strip())
    if "@" in parts.netloc:
        raise UsageError(f"{SDK_URI}: credentials inside the URI are not supported; use {SDK_USER} / {SDK_PASS}")
    host = parts.hostname
    if not host:
        raise UsageError(f"{SDK_URI}: no host")
    try:
        port = parts.port
    except ValueError:
        raise UsageError(f"{SDK_URI}: bad port for host {host}") from None
    return scheme == "mqtts", host, port or DEFAULT_PORT[scheme]


def load_broker(path: str) -> Broker:
    if not os.path.exists(path):
        raise UsageError(f"sdkconfig not found: {path} (pass --sdkconfig PATH)")
    with open(path, encoding="utf-8") as fh:
        cfg = parse_sdkconfig(fh.read(), path)
    tls, host, port = parse_broker_uri(cfg[SDK_URI])
    return Broker(host, port, tls, cfg[SDK_USER], cfg[SDK_PASS])


def _failed(rc) -> bool:
    """paho 2's ReasonCode, or a plain int (0 = success)."""
    is_failure = getattr(rc, "is_failure", None)
    return bool(is_failure) if is_failure is not None else rc != 0


def _paho_client(mqtt=None):
    """A paho client with an empty client id and a clean session: the broker
    assigns an id (paho retries with a random one if the broker rejects an
    empty id), so two runs never share one and kick each other off, and the
    broker keeps no session behind. mqtt: tests pass a stand-in module."""
    if mqtt is None:
        # --mqtt only: file mode must never import paho (its test checks).
        import paho.mqtt.client as mqtt

    return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="", clean_session=True)


def fetch_retained(
    broker: Broker,
    cap_s: float = DEFAULT_WAIT_S,
    idle_s: float = IDLE_S,
    client_factory=None,
    clock=None,
    warnings: list[str] | None = None,
) -> dict[str, bytes]:
    """Subscribe to DISCOVERY_FILTER and return {topic: payload}, the last
    payload per topic. Stops idle_s after the last message (or after the
    SUBACK, if nothing is retained), and at cap_s whatever happens. Stopped
    by the cap while messages were still arriving, it appends a warning to
    `warnings`: the set may be incomplete.
    client_factory / clock: tests pass a stub client and a fake clock."""
    where = broker.where()
    clock = clock or time.monotonic
    client = (client_factory or _paho_client)()
    st = {"conn": None, "why": "", "sub": None, "last": 0.0}
    msgs: dict[str, bytes] = {}

    def on_connect(c, userdata, flags, rc, props=None):
        if _failed(rc):
            st["conn"], st["why"] = False, str(rc)
        else:
            st["conn"] = True
            c.subscribe(DISCOVERY_FILTER, qos=0)

    def on_subscribe(c, userdata, mid, rcs, props=None):
        st["sub"] = not any(_failed(r) for r in rcs)
        st["last"] = clock()

    def on_message(c, userdata, msg):
        msgs[msg.topic] = bytes(msg.payload)
        st["last"] = clock()

    client.on_connect, client.on_subscribe, client.on_message = on_connect, on_subscribe, on_message
    start = clock()
    try:
        if broker.user:
            client.username_pw_set(broker.user, broker.password or None)
        if broker.tls:
            client.tls_set()  # the system CA store
        client.connect(broker.host, broker.port, keepalive=30)
    except Exception as e:  # DNS, refused, TLS, timeout: the library's words, scrubbed by main()
        raise MqttError(f"cannot connect to {where}: {type(e).__name__}: {e}") from None
    try:
        while True:
            rc = client.loop(timeout=0.1)
            if st["conn"] is False:
                auth = any(s in st["why"].lower() for s in ("author", "password", "user name"))
                hint = f" (check {SDK_USER} / {SDK_PASS} in sdkconfig)" if auth else ""
                raise MqttError(f"{where} refused the connection: {st['why']}{hint}")
            if st["sub"] is False:
                raise MqttError(f"{where} refused the subscription to {DISCOVERY_FILTER}")
            if _failed(rc):
                raise MqttError(f"lost the connection to {where}: {rc}")
            now = clock()
            if st["sub"] and now - st["last"] >= idle_s:
                return msgs
            if now - start >= cap_s:
                if not st["conn"]:
                    raise MqttError(f"no answer from {where} within {cap_s:g} s")
                if not st["sub"]:
                    raise MqttError(f"{where} did not confirm the subscription within {cap_s:g} s")
                # Subscribed and not idle (the idle check above returns
                # first), so messages were still arriving: keep what came,
                # but say the set may be short.
                if warnings is not None:
                    warnings.append(
                        f"the scan stopped at the --wait limit ({cap_s:g} s) while {where} was still sending "
                        f"retained discovery ({len(msgs)} topic(s) so far, the last {now - st['last']:.1f} s ago): "
                        f"devices or entities may be missing. Re-run with a larger --wait."
                    )
                return msgs
    finally:
        try:
            client.disconnect()
        except Exception:
            pass


@dataclass
class Scan:
    devices: list[dict]  # [{node, label}] -- every device seen, tab or not
    entity_sets: dict[str, dict[str, Entity]]  # node -> entities; devices that get a tab
    warnings: list[str]
    topics: int = 0  # discovery topics seen, MagTag or not (for the "nothing found" message)


def conditional_keys() -> set[str]:
    """Keys current firmware publishes only sometimes: the per-slot sensors
    (retired with a disabled slot) and the chore rows (retired past the
    chore count). mqtt_ha.c publish_discovery() publishes every other
    entity on every pass, so a device that lacks one runs older firmware."""
    return {f"{p}_{n}" for p in PER_SLOT for n in SLOTS} | {f"chore_{n}" for n in CHORES}


def collect_discovery(messages, labels: dict[str, str] | None = None, fw: dict[str, Entity] | None = None) -> Scan:
    """Pure. messages: (topic, payload) pairs, later pairs winning for the
    same topic. labels: node -> tab label overrides (--devices). fw: this
    checkout's firmware entities (firmware_entities()), to spot a device on
    older firmware; None skips that check.

    - A topic outside homeassistant/<component>/magtag-<node>_<key>/config
      is someone else's and ignored.
    - An empty payload is a retired entity (disabled slot, chore row past
      the list, the RETIRED[] keys): skipped.
    - The component is the topic's; name and stat_cla the payload's.
    - The label is dev.name, the most frequent if payloads disagree.
    - No def_ent_id: firmware before schema v20, whose HA ids are
      name-derived. All of a device's payloads -> no tab; some -> those
      entities are left off. Either way a warning.
    - Older firmware (v20 or later, but older than `fw`): it lacks a key
      `fw` always publishes (anything outside conditional_keys()), or a key
      `fw` gives a state_class carries no stat_cla. It keeps its tab, built
      from what it publishes (build_view() drops the statistics graphs its
      stat_cla cannot feed), and gets a warning to OTA it and re-run."""
    latest: dict[str, bytes] = {}
    for topic, payload in messages:
        latest[topic] = payload
    labels = labels or {}
    per: dict[str, dict] = {}
    warnings: list[str] = []
    for topic in sorted(latest):
        m = DISC_TOPIC_RE.match(topic)
        payload = latest[topic]
        if not m or not payload:
            continue
        comp, node, key = m.groups()
        d = per.setdefault(
            node, {"ents": {}, "keys": set(), "names": Counter(), "sw": Counter(), "no_def": [], "odd": []}
        )
        d["keys"].add(key)
        try:
            doc = json.loads(payload)
        except (ValueError, UnicodeDecodeError):
            doc = None
        if not isinstance(doc, dict):
            d["odd"].append(f"{key} (payload is not a JSON object; skipped)")
            continue
        uid = f"magtag-{node}_{key}"
        if doc.get("uniq_id") != uid:
            d["odd"].append(f"{key} (uniq_id {doc.get('uniq_id')!r}, want {uid!r})")
        dev = doc.get("dev")
        name = dev.get("name") if isinstance(dev, dict) else None
        if isinstance(name, str) and name.strip():
            d["names"][name.strip()] += 1
        sw = dev.get("sw") if isinstance(dev, dict) else None
        if isinstance(sw, str) and sw.strip():
            d["sw"][sw.strip()] += 1
        de = doc.get("def_ent_id")
        if de is None:
            d["no_def"].append(key)
            continue
        if de != f"{comp}.{uid}":
            want = f"{comp}.{uid}"
            d["odd"].append(f"{key} (def_ent_id {de!r}, want {want!r}; its id on the tab may be wrong)")
        sc = doc.get("stat_cla")
        d["ents"][key] = Entity(comp, key, str(doc.get("name") or key), sc if isinstance(sc, str) else None)

    devices, sets = [], {}
    for node in sorted(per):
        d = per[node]
        names = sorted(d["names"].items(), key=lambda kv: (-kv[1], kv[0]))
        if node in labels:
            label = labels[node]
        elif names:
            label = names[0][0]
            if len(names) > 1:
                seen = ", ".join(f"{n!r} x{c}" for n, c in names)
                warnings.append(f"magtag-{node}: its payloads disagree on the device name ({seen}); using {label!r}.")
        else:
            label = f"MagTag {node}"
            warnings.append(f"magtag-{node}: no device name in its discovery; the tab is labelled {label!r}.")
        who = f"{label} (magtag-{node})"
        devices.append({"node": node, "label": label})
        if d["odd"]:
            warnings.append(f"{who}: unexpected discovery for {', '.join(d['odd'])}.")
        if d["no_def"] and not d["ents"]:
            warnings.append(
                f"{who} runs firmware older than discovery schema v20: its discovery carries "
                f"no default entity id, so its Home Assistant entity ids are name-derived and cannot be "
                f"known here. It gets NO TAB. OTA it to current firmware first (step 1), re-register it if "
                f"needed (steps 3-4), then re-run this generator."
            )
            continue
        if d["no_def"]:
            warnings.append(
                f"{who}: {len(d['no_def'])} retained discovery document(s) carry no default entity id "
                f"(left over from firmware before schema v20?), so their ids are unknown and they are "
                f"left off the tab: {', '.join(sorted(d['no_def']))}."
            )
        if not d["ents"]:
            warnings.append(f"{who}: no readable discovery; no tab.")
            continue
        other = sorted(coverage_gaps(d["ents"]))
        if other:
            warnings.append(
                f"{who}: {len(other)} entit{'y' if len(other) == 1 else 'ies'} this generator's layout does "
                f"not know (newer firmware?), put under '{OTHER_PART}' on its tab: {', '.join(other)}. "
                f"Update tools/gen_ha_dashboard.py's layout to place them."
            )
        if fw is not None:
            missing = sorted(set(fw) - conditional_keys() - d["keys"])
            no_cla = sorted(k for k, e in d["ents"].items() if k in fw and fw[k].state_class and not e.state_class)
            if missing or no_cla:
                sw = d["sw"].most_common(1)
                why = [f"It reports firmware {sw[0][0]}."] if sw else []
                if missing:
                    why.append(f"Missing entities current firmware always publishes: {', '.join(missing)}.")
                if no_cla:
                    why.append(
                        f"No state class on {', '.join(no_cla)}, so the statistics graphs that need one leave "
                        f"{'it' if len(no_cla) == 1 else 'them'} out."
                    )
                warnings.append(
                    f"{who} runs older firmware: OTA it (step 1), then re-run this tool so its tab includes "
                    f"the newer entities. " + " ".join(why)
                )
        sets[node] = d["ents"]
    devices.sort(key=lambda x: (x["label"].casefold(), x["node"]))
    return Scan(devices, sets, warnings, topics=len(latest))


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

PARTS = ("setup", "automation", "dashboard")
BANNER = {
    "setup": "PART 1 of 3: SETUP STEPS",
    "automation": "PART 2 of 3: CONFIG-PUBLISHING AUTOMATION (tools/ha/magtag_publish_config.yaml)",
    "dashboard": "PART 3 of 3: DASHBOARD YAML",
}


FILE_MODE_NOTE = """\
File mode (this output) puts every timer slot (1-4) and every chore row (1-3)
on every tab. A device with a disabled slot or fewer than 3 chores shows
"entity not available" rows for them. Run with --mqtt to build each tab from
the device's real entity set instead."""


def mqtt_mode_note(where: str) -> str:
    return textwrap.fill(
        f"Built from the retained discovery on {where}: each tab shows exactly the entities "
        "that device publishes, so a disabled timer slot or an unused chore row gets no row. "
        "Re-run after enabling a slot or adding a chore, and after an OTA or a re-register: "
        "newer firmware publishes entities an older one lacks.",
        width=78,
    )


def _wrap_warning(text: str) -> list[str]:
    return textwrap.wrap(text, width=78, initial_indent="  - ", subsequent_indent="    ")


def render_setup(devices: list[dict], note: str = FILE_MODE_NOTE, warnings=()) -> str:
    lines = [
        "Requirements: Home Assistant 2025.11 or newer. The entity ids need 2025.10",
        "(default_entity_id in MQTT discovery); the dashboard's entity names",
        "(name: {type: entity}) need 2025.11.",
        "",
    ]
    if warnings:
        lines.append("WARNINGS from the broker scan:")
        for w in warnings:
            lines += _wrap_warning(w)
        lines.append("")
    lines += [
        note,
        "",
        "Once, for all devices: install the automation (part 2).",
        "",
        "Then once per device, in this order:",
        "",
        "1. OTA the device to current firmware first. If its current firmware",
        "   predates discovery schema v20, do NOT delete it in HA (step 3) before",
        "   this OTA: that firmware sends no default entity id, so the device would",
        "   come back under the old name-derived ids.",
        "",
        "2. Create its chore list: Settings -> Devices & services -> Add integration ->",
        "   Local To-do, named exactly as below. HA makes it the entity id the",
        "   automation looks for; rename the list's display name afterwards if you",
        "   like -- the entity id stays.",
        "",
    ]
    for d in devices:
        lines.append(f'     {d["label"]}: "{todo_list_name(d["node"])}"  ->  {todo_entity_id(d["node"])}')
    lines += [
        "",
        "3. Only if its entity ids are the old name-derived ones (e.g.",
        "   sensor.testing_timer_...) rather than <component>.magtag_<node>_<key>:",
        "   delete the device in HA (Settings -> Devices & services -> MQTT -> the",
        "   device -> Delete). This loses its entity history and statistics (they",
        "   stay behind under the old ids), its area, and any custom names or icons",
        "   on its entities. A device already on the magtag_<node> ids skips steps",
        "   3 and 4.",
        "",
        "4. After a delete, make the device republish its discovery: it does so only",
        "   when its discovery changes, and it may not come back on its own. Rename",
        "   one of its chores in its \"MagTag <node> chores\" To-do list -- the chore",
        "   list is part of the firmware's discovery fingerprint. The device",
        "   applies the renamed list after that window's discovery pass, so it",
        "   republishes within two network windows and HA adds it back with the",
        "   new ids. To force the two windows, press Button D on the timer screen",
        "   twice, a minute apart. If its chore checklist is showing, D is the",
        "   chore 3 tick there, not a sync: press Button A first to get back to",
        "   the timer screen. Once the device has re-appeared, rename the chore",
        "   back.",
        "",
        "Finally paste the dashboard (part 3): Settings -> Dashboards -> Add dashboard",
        "-> New dashboard from scratch, open it, Edit -> three-dot menu -> Raw",
        "configuration editor, replace everything, Save.",
    ]
    return "\n".join(lines) + "\n"


AUTOMATION_NOTES = """\
# ---------------------------------------------------------------------------
# Install: add this to your Home Assistant configuration -- automations.yaml,
# or wherever your config repo keeps automations (e.g. FluxCD) -- and reload
# automations. It is NOT part of the dashboard; do not paste it there.
#
# One automation covers every device: it finds every To-do list whose entity
# id is todo.magtag_<node>_chores and publishes that device's retained config
# document. A new device needs only its list (setup step 2).
#
# Calendar: it reads calendar.school_schedule. Events named "No School..."
# become holidays; "No School: Summer" events give the summer season dates.
# Without that calendar it still publishes the chores, sends no calendar
# fields, and raises a persistent notification saying so.
# ---------------------------------------------------------------------------
"""


def read_automation() -> str:
    with open(AUTOMATION, encoding="utf-8", newline="") as fh:
        return fh.read()


def render_automation() -> str:
    return AUTOMATION_NOTES + read_automation()


def dashboard_header(note: str = FILE_MODE_NOTE, no_tab=()) -> str:
    head = """\
# MagTag dashboard: one tab per device. Paste into a new dashboard's
# Settings -> Dashboards -> (dashboard) -> Edit -> Raw configuration editor.
# Needs Home Assistant 2025.11 or newer.
# Generated by tools/gen_ha_dashboard.py; regenerate rather than hand-edit.
#
""" + "".join(f"# {ln}\n" for ln in note.splitlines())
    for d in no_tab:
        head += f"# NO TAB for {d['label']} (magtag-{d['node']}): see the warnings in the setup steps.\n"
    return head


DASHBOARD_HEADER = dashboard_header()


def render_dashboard(devices: list[dict], entity_sets: dict[str, dict[str, Entity]], note: str = FILE_MODE_NOTE) -> str:
    doc = build_dashboard(devices, entity_sets)
    head = dashboard_header(note, no_tab_devices(devices, entity_sets))
    return head + yaml.safe_dump(doc, sort_keys=False, allow_unicode=True, width=100)


def render(
    part: str,
    devices: list[dict],
    entity_sets: dict[str, dict[str, Entity]],
    note: str = FILE_MODE_NOTE,
    warnings=(),
) -> str:
    """note: FILE_MODE_NOTE, or mqtt_mode_note() in --mqtt mode; warnings go
    at the top of the setup steps."""
    renderers = {
        "setup": lambda: render_setup(devices, note, warnings),
        "automation": render_automation,
        "dashboard": lambda: render_dashboard(devices, entity_sets, note),
    }
    if part != "all":
        return renderers[part]()
    out = []
    for p in PARTS:
        bar = "=" * 78
        out.append(f"# {bar}\n# {BANNER[p]}\n# {bar}\n")
        out.append(renderers[p]())
        out.append("\n")
    return "".join(out)


def _err(msg: str, secret: str | None = None) -> None:
    _emit(sys.stderr, f"gen_ha_dashboard: {msg}\n", secret)


def main_mqtt(args, client_factory=None, clock=None) -> int:
    """--mqtt. Returns the exit status; 2 = bad input, 1 = broker/scan failure.
    Every line it prints once the password is read goes through _emit()
    with the password as the secret."""
    sdk = args.sdkconfig or os.path.join(REPO, DEFAULT_SDKCONFIG)
    try:
        broker = load_broker(sdk)
    except UsageError as e:
        _err(str(e))  # the password is not read yet: built from key names and the path only
        return 2
    secret = broker.password
    try:
        labels = {}
        if args.devices:
            labels = {d["node"]: d["label"] for d in load_devices(args.devices)}
        fetch_warnings: list[str] = []
        msgs = fetch_retained(broker, args.wait, IDLE_S, client_factory, clock, fetch_warnings)
        scan = collect_discovery(msgs.items(), labels, firmware_entities())
        scan.warnings[:0] = fetch_warnings
        for node in sorted(set(labels) - {d["node"] for d in scan.devices}):
            scan.warnings.append(f"{args.devices}: magtag-{node} ({labels[node]}) has no discovery on the broker.")
    except UsageError as e:
        _err(str(e), secret)
        return 2
    except Exception as e:  # MqttError, or anything paho raises: never let a traceback carry the password
        _err(str(e) if isinstance(e, MqttError) else f"{type(e).__name__}: {e}", secret)
        return 1
    for w in scan.warnings:
        _err(f"warning: {w}", secret)
    if not scan.devices:
        _err(
            f"no MagTag discovery on {broker.where()} ({scan.topics} discovery topic(s) under "
            f"homeassistant/, none magtag-<node>_<key>). Is this the broker the devices use, and has "
            f"a device completed a network window?",
            secret,
        )
        return 1
    if not scan.entity_sets:
        _err("no device can have a tab (see the warnings above): OTA them to current firmware first.", secret)
        return 1
    note = mqtt_mode_note(broker.where())
    _emit(sys.stdout, render(args.part, scan.devices, scan.entity_sets, note, scan.warnings), secret)
    return 0


def main(argv=None, client_factory=None, clock=None) -> int:
    """client_factory / clock: tests hand --mqtt a stub instead of a paho
    client, and a fake clock."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument(
        "--devices",
        help=f"devices file (default {DEFAULT_DEVICES}); with --mqtt, optional tab-label overrides",
    )
    ap.add_argument("--part", choices=PARTS + ("all",), default="all", help="print one part only (default all)")
    ap.add_argument(
        "--mqtt", action="store_true", help="find the devices and their exact entity sets on the MQTT broker"
    )
    ap.add_argument(
        "--sdkconfig", help=f"with --mqtt: where the broker settings are (default {DEFAULT_SDKCONFIG} at the repo root)"
    )
    ap.add_argument(
        "--wait",
        type=float,
        default=DEFAULT_WAIT_S,
        metavar="SECONDS",
        help=f"with --mqtt: longest the scan may take (default {DEFAULT_WAIT_S:g}; it stops {IDLE_S:g} s "
        f"after the last retained message)",
    )
    args = ap.parse_args(argv)
    if not args.mqtt and (args.sdkconfig or args.wait != DEFAULT_WAIT_S):
        ap.error("--sdkconfig and --wait need --mqtt")
    if not math.isfinite(args.wait) or args.wait <= 0:  # nan passes `<= 0` and would disable the cap
        ap.error("--wait must be a positive number of seconds")
    if args.mqtt:
        return main_mqtt(args, client_factory, clock)
    try:
        devices = load_devices(args.devices or os.path.join(REPO, DEFAULT_DEVICES))  # from any cwd
    except UsageError as e:
        _err(str(e))
        return 2
    fw = firmware_entities()
    entity_sets = {d["node"]: fw for d in devices}
    _emit(sys.stdout, render(args.part, devices, entity_sets))
    return 0


if __name__ == "__main__":
    sys.exit(main())
