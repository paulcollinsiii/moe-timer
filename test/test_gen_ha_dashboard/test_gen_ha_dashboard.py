#!/usr/bin/env python3
"""Host tests for tools/gen_ha_dashboard.py -- the HA dashboard generator.

Runs under uv, not the bare interpreter: the tool declares its libraries
in PEP 723 inline metadata and never hand-rolls them, so its suite needs
pyyaml too. test/CMakeLists.txt registers it as

    uv run --no-project --with pyyaml python3 test_gen_ha_dashboard.py

and fails the test loudly (rather than skipping it) when uv is missing.

What is pinned, and why each matters:
  - the three printed parts, and that each can be printed alone;
  - the committed automation is the owner's byte for byte (md5), and part 2
    carries it verbatim;
  - every entity id on the dashboard is <component>.magtag_<node>_<key> for
    a key and component the firmware really publishes;
  - COVERAGE: every key the firmware defines -- read from main/stats_json.c,
    main/ha_config.c and main/mqtt_ha.c by the tool's own parser, which this
    suite checks against independent crude row counts -- is on the tab or
    on LEFT_OUT with a reason. A firmware entity added later fails here
    instead of silently missing from the dashboard;
  - each allocation shares a row with its chore_free (pairs re-derived from
    ha_config.c, not from the tool's table);
  - each statistics-graph entity carries a state_class compatible with the
    card's stat_types, judged by the tool's own rule (graph_compatible();
    history-graph is exempt: recorder history is enough);
  - the devices file: the example's nodes and labels, and invalid input;
  - the setup text: HA 2025.11+, the re-registration order (OTA, delete,
    rename a chore to force a republish, rename it back only after), what
    the delete costs (from HA source, marked unconfirmed), and file mode's
    every-slot note;
  - the day-shift note on every per-day and per-week summary graph;
  - file mode does not import paho, checked in the script's own uv
    environment, where paho is installed.

--mqtt (no broker in ctest: recorded retained messages go to the pure
collect_discovery(), and a stub client stands in for paho in
fetch_retained() and main()):
  - devices come from the discovery topics; the label from dev.name;
  - an empty (retired) payload is skipped, so a disabled slot and chore
    rows past the configured count get no card, and a later payload on a
    topic replaces an earlier one;
  - a pre-v20 device (no def_ent_id; a synthetic fixture) gets a named
    warning and no tab;
  - a v20 device (Julia's Timer on 1.5.4: def_ent_id, none of the v21-v23
    keys, no stat_cla) gets a tab from what it publishes, statistics graphs
    only where its stat_cla allows (an emptied card is dropped), and a
    warning to OTA it and re-run; a current device raises none;
  - a key the layout does not know lands in an "Other" part, with a warning;
  - a def_ent_id that disagrees with the topic warns;
  - the scan: stops when idle; stopped by --wait mid-stream it warns that
    the set may be incomplete; --wait must be finite and positive; the
    paho client has an empty (broker/random) client id and a clean session;
    username-only auth hands paho no password;
  - sdkconfig parsing (quoting, escapes, missing keys) and broker URIs;
  - THE PASSWORD never appears in any output, error or repr: a sentinel
    goes through every error path reachable without a network, including
    a library error that echoes it, and broker data that carries it on
    stdout and stderr; the real paho adapter is exercised against a closed
    local port under uv.
"""

import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

import yaml

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(_HERE, "..", ".."))
TOOL = os.path.join(REPO, "tools", "gen_ha_dashboard.py")
EXAMPLE = os.path.join(REPO, "tools", "ha_devices.example.yaml")
AUTOMATION = os.path.join(REPO, "tools", "ha", "magtag_publish_config.yaml")
OWNER_MD5 = "f27165e96317e0b3c72d1bcbb7953d8e"

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(TOOL))
import gen_ha_dashboard as g  # noqa: E402

FW = g.firmware_entities()
DEVICES = g.load_devices(EXAMPLE)
SETS = {d["node"]: FW for d in DEVICES}

ID_RE = re.compile(r"^([a-z_]+)\.magtag_([0-9a-f]{6})_([a-z0-9_]+)$")


def read(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as fh:
        return fh.read()


def walk(node):
    """Yield every dict in a parsed YAML tree."""
    if isinstance(node, dict):
        yield node
        for v in node.values():
            yield from walk(v)
    elif isinstance(node, list):
        for v in node:
            yield from walk(v)


def entity_refs(tree):
    """Every entity id a card names: `entity:`, `entities:` items (string or
    {entity:}) and logbook `target: {entity_id: [...]}`."""
    out = []
    for d in walk(tree):
        if isinstance(d.get("entity"), str):
            out.append(d["entity"])
        for item in d.get("entities", []) if isinstance(d.get("entities"), list) else []:
            if isinstance(item, str):
                out.append(item)
        tgt = d.get("target")
        if isinstance(tgt, dict):
            ids = tgt.get("entity_id", [])
            out.extend([ids] if isinstance(ids, str) else ids)
    return out


def dashboard():
    return yaml.safe_load(g.render("dashboard", DEVICES, SETS))


def find_uv():
    return os.environ.get("UV") or shutil.which("uv") or os.path.expanduser("~/.local/bin/uv")


class TestParsers(unittest.TestCase):
    """The key set is read from the firmware source. These pin that the
    parser reads every row, and that a new row is picked up."""

    STATS_SNIPPET = """
#define DIAG "diagnostic"
#define SC "total"
static const ha_entity_t ENTITIES[] = {
    /* a comment with a { brace and "quote" */
    {"sensor", "battery", "Battery", "%", "battery", "{{ value_json.batt_pct }}", "stat", 7500, false, NULL,
     "measurement", NULL},
    {"binary_sensor", "fake_new", "Fake, with comma", NULL, NULL, "{{ 'ON' if x else 'OFF' }}", "stat", 0, true,
     DIAG, SC, "{{ value_json.date ~ 'T00:00:00+00:00' }}"},
};
"""

    def test_stats_rows_new_row_is_picked_up(self):
        rows = g.parse_stats_entities(self.STATS_SNIPPET)
        self.assertEqual([(e.component, e.key, e.name, e.state_class) for e in rows],
                         [("sensor", "battery", "Battery", "measurement"),
                          ("binary_sensor", "fake_new", "Fake, with comma", "total")])

    FIELDS_SNIPPET = r"""
#define NUM_U16(k, nm, un, lo_, hi_, st, set, get)                                                                  \
    {                                                                                                               \
        .key = k, .component = "number", .name = nm, .unit = un, .kind = CFG_U16, .lo = lo_, .hi = hi_, .step = st, \
        .set_u16 = set, .get_u16 = get                                                                              \
    }
#define TIMER_NAME(n)                                                                                   \
    {                                                                                                   \
        .key = "timer" #n "_name", .component = "text", .name = "Timer " #n " name", .kind = CFG_TNAME, \
        .hi = CFG_TIMER_NAME_CAP, .slot = n                                                             \
    }
static const cfg_field_t FIELDS[] = {
    NUM_U16("weekday_min", "Weekday allocation", "min", 1, 1440, 1, a, b),
    TIMER_NAME(7), /* trailing comment */
    NUM_U16("fake_setting", "Fake setting", NULL, 0, 9, 1, c, d),
};
"""

    def test_config_rows_follow_macro_definitions(self):
        rows = g.parse_config_fields(self.FIELDS_SNIPPET)
        self.assertEqual([(e.component, e.key, e.name) for e in rows],
                         [("number", "weekday_min", "Weekday allocation"),
                          ("text", "timer7_name", "Timer 7 name"),
                          ("number", "fake_setting", "Fake setting")])

    def test_config_row_with_unknown_macro_is_an_error(self):
        with self.assertRaises(ValueError):
            g.parse_config_fields(self.FIELDS_SNIPPET.replace("TIMER_NAME(7)", "NEW_KIND(7)"))

    def test_action_payloads(self):
        src = r'''
    snprintf(p, n, "{\"name\":\"Screen adjust (min) today\",\"uniq_id\":\"%s_screen_bonus\","
                 "\"def_ent_id\":\"number.%s_screen_bonus\",", id, id);
    snprintf(p, n, "{\"name\":\"New thing\",\"uniq_id\":\"%s_fake_act\","
                 "\"def_ent_id\":\"button.%s_fake_act\",", id, id);
'''
        rows = g.parse_action_entities(src)
        self.assertEqual([(e.component, e.key, e.name) for e in rows],
                         [("number", "screen_bonus", "Screen adjust (min) today"),
                          ("button", "fake_act", "New thing")])

    def test_action_payload_with_spaces_around_the_colons(self):
        src = r'''
    snprintf(p, n, "{\"name\" : \"Spaced\",\"uniq_id\": \"%s_spaced\","
                 "\"def_ent_id\" :\"button.%s_spaced\",", id, id);
'''
        rows = g.parse_action_entities(src)
        self.assertEqual([(e.component, e.key, e.name) for e in rows], [("button", "spaced", "Spaced")])

    def test_action_payload_without_a_readable_def_ent_id_is_an_error(self):
        for bad in (r'"{\"name\":\"No id\",\"uniq_id\":\"%s_no_id\",\"stat_t\":\"x\"}"',
                    r'"{\"name\":\"Odd\",\"uniq_id\":\"%s_odd\",\"def_ent_id\":\"%s\"}"'):
            with self.subTest(src=bad):
                with self.assertRaises(ValueError) as cm:
                    g.parse_action_entities(bad)
                self.assertIn("def_ent_id", str(cm.exception))

    def test_preprocessor_line_in_a_table_is_a_loud_error(self):
        cases = [
            (g.parse_stats_entities, self.STATS_SNIPPET.replace("    {\"binary_sensor\"", "#if 0\n    {\"binary_sensor\"")
             .replace('"},\n};', '"},\n#endif\n};')),
            (g.parse_config_fields, self.FIELDS_SNIPPET.replace("    TIMER_NAME(7)", "#ifdef X\n    TIMER_NAME(7)")
             .replace("c, d),\n};", "c, d),\n#endif\n};")),
        ]
        for fn, src in cases:
            self.assertIn("#", src[src.index("[] = {"):])
            with self.subTest(fn=fn.__name__):
                with self.assertRaises(ValueError) as cm:
                    fn(src)
                self.assertIn("move the conditional out of", str(cm.exception))

    def test_real_source_row_counts_match_a_crude_count(self):
        # Independent of the tool's tokenizer: one row per line that opens
        # with {"component" in ENTITIES, one per macro call in FIELDS.
        stats = read("main/stats_json.c")
        ents = stats[stats.index("ENTITIES[] = {"):]
        ents = ents[:ents.index("\n};")]
        n_stats = len(re.findall(r'^\s*\{"(?:sensor|binary_sensor)"', ents, re.M))
        cfg = read("main/ha_config.c")
        flds = cfg[cfg.index("cfg_field_t FIELDS[] = {"):]
        flds = flds[:flds.index("\n};")]
        n_cfg = len(re.findall(r"^\s*[A-Z][A-Z0-9_]*\(", flds, re.M))
        # mqtt_ha.c: counted by what every discovery payload has whether or
        # not it carries a def_ent_id -- its uniq_id, and the literal key it
        # hands mqtt_disc_topic() -- so a payload the parser misses shows.
        mq = read("main/mqtt_ha.c")
        n_act = len(re.findall(r'\\"uniq_id\\"\s*:', mq))
        n_topic = len(re.findall(r'mqtt_disc_topic\([^;]*,\s*"\w+"\s*\)\s*;', mq))
        self.assertEqual(n_act, n_topic)
        src = g.parse_stats_entities(stats), g.parse_config_fields(cfg), g.parse_action_entities(mq)
        self.assertEqual([len(x) for x in src], [n_stats, n_cfg, n_act])
        self.assertEqual(n_act, 2)
        self.assertEqual(len(FW), n_stats + n_cfg + n_act)
        self.assertGreater(n_stats, 40)
        self.assertGreater(n_cfg, 30)

    def test_real_source_anchors(self):
        want = {
            "battery": ("sensor", "measurement"),
            "screen_used_day": ("sensor", "total"),
            "day_runs_4": ("sensor", "total"),
            "day_chores": ("sensor", "total"),
            "chores_done": ("sensor", "measurement"),
            "completions_1": ("sensor", None),
            "chore_3": ("binary_sensor", None),
            "weekday_min": ("number", None),
            "chore_free_sum": ("number", None),
            "timer4_break": ("switch", None),
            "tone_bed": ("select", None),
            "ota_url": ("text", None),
            "screen_bonus": ("number", None),
            "locate": ("switch", None),
        }
        for k, (comp, sc) in want.items():
            with self.subTest(key=k):
                self.assertEqual((FW[k].component, FW[k].state_class), (comp, sc))


class TestCoverage(unittest.TestCase):
    def test_every_firmware_key_is_placed_or_left_out(self):
        self.assertEqual(g.coverage_gaps(FW), set())

    def test_a_new_firmware_key_fails_coverage(self):
        fake = dict(FW)
        for e in g.parse_stats_entities(TestParsers.STATS_SNIPPET):
            fake.setdefault(e.key, e)
        self.assertEqual(g.coverage_gaps(fake), {"fake_new"})

    def test_left_out_entries_are_real_keys_with_reasons(self):
        for k, why in g.LEFT_OUT.items():
            with self.subTest(key=k):
                self.assertIn(k, FW)
                self.assertNotIn(k, g.placed_keys())
                self.assertTrue(isinstance(why, str) and len(why.strip()) > 10)

    def test_layout_names_no_key_the_firmware_lacks(self):
        self.assertEqual(g.placed_keys() - set(FW), set())

    def test_every_placed_key_is_rendered_on_every_tab(self):
        # placed_keys() is the layout's claim; this checks the builder keeps it.
        for view, dev in zip(dashboard()["views"], DEVICES):
            seen = {ID_RE.match(r).group(3) for r in entity_refs(view) if not r.startswith("todo.")}
            self.assertEqual(seen, set(FW) - set(g.LEFT_OUT), view["title"])

    def test_missing_entities_are_skipped_not_rendered(self):
        # The entity set is an input (M4-T3 supplies it from discovery).
        sub = {k: v for k, v in FW.items() if not k.endswith("_3") and k != "chore_free_hol"}
        view = g.build_view("abcdef", "Sub", sub)
        keys = {ID_RE.match(r).group(3) for r in entity_refs(view) if not r.startswith("todo.")}
        self.assertEqual(keys, set(sub))


class TestDashboard(unittest.TestCase):
    def setUp(self):
        self.doc = dashboard()

    def test_one_sections_view_per_device(self):
        views = self.doc["views"]
        self.assertEqual([v["title"] for v in views], ["Testing Timer", "Julia's Timer"])
        self.assertEqual([v["path"] for v in views], ["magtag-1a0a5c", "magtag-1aaa76"])
        for v in views:
            self.assertEqual(v["type"], "sections")
            self.assertTrue(all(s["type"] == "grid" for s in v["sections"]))

    def test_every_entity_id_is_a_real_firmware_entity_of_that_device(self):
        for view, dev in zip(self.doc["views"], DEVICES):
            refs = entity_refs(view)
            self.assertGreater(len(refs), 90)
            for r in refs:
                with self.subTest(view=view["title"], ref=r):
                    if r.startswith("todo."):
                        self.assertEqual(r, g.todo_entity_id(dev["node"]))
                        continue
                    m = ID_RE.match(r)
                    self.assertIsNotNone(m)
                    comp, node, key = m.groups()
                    self.assertEqual(node, dev["node"])
                    self.assertIn(key, FW)
                    self.assertEqual(comp, FW[key].component)

    def test_entity_id_rule(self):
        # def_ent_id "sensor.magtag-1a0a5c_battery", slugified by HA: '-' -> '_'.
        self.assertEqual(g.entity_id("sensor", "1a0a5c", "battery"), "sensor.magtag_1a0a5c_battery")
        self.assertEqual(g.todo_entity_id("1a0a5c"), "todo.magtag_1a0a5c_chores")
        self.assertEqual(g.todo_list_name("1a0a5c"), "MagTag 1a0a5c chores")

    def test_settings_start_with_the_todo_card_and_chore_flags(self):
        for view, dev in zip(self.doc["views"], DEVICES):
            first = view["sections"][0]["cards"]
            self.assertEqual(first[0], {"type": "heading", "heading": "Settings", "heading_style": "title"})
            todo = [c for c in first if c.get("type") == "todo-list"]
            self.assertEqual(todo, [{"type": "todo-list", "entity": g.todo_entity_id(dev["node"])}])
            flags = entity_refs(first)
            for i in (1, 2, 3):
                self.assertIn(f"binary_sensor.magtag_{dev['node']}_chore_{i}", flags)

    def test_each_allocation_shares_a_row_with_its_chore_free(self):
        # Pairs from the source (NUM_CHORE_FREE's third argument), not the tool.
        pairs = re.findall(r'NUM_CHORE_FREE\("(\w+)",\s*"[^"]*",\s*"(\w+)"', read("main/ha_config.c"))
        self.assertEqual(len(pairs), 4)
        self.assertEqual({(a, f) for _, a, f in g.ALLOC_PAIRS}, {(a, f) for f, a in pairs})
        for view, dev in zip(self.doc["views"], DEVICES):
            rows = [sorted(entity_refs(c)) for c in walk(view) if c.get("type") == "horizontal-stack"]
            for free, alloc in pairs:
                with self.subTest(view=view["title"], alloc=alloc):
                    want = sorted([f"number.magtag_{dev['node']}_{alloc}", f"number.magtag_{dev['node']}_{free}"])
                    self.assertIn(want, rows)

    def test_statistics_graphs_carry_a_compatible_state_class(self):
        for view in self.doc["views"]:
            graphs = [c for c in walk(view) if c.get("type") == "statistics-graph"]
            self.assertGreaterEqual(len(graphs), 5)
            for card in graphs:
                self.assertTrue(card["stat_types"])
                for r in entity_refs(card):
                    key = ID_RE.match(r).group(3)
                    with self.subTest(entity=r, stats=card["stat_types"]):
                        self.assertIsNotNone(FW[key].state_class)
                        self.assertTrue(g.graph_compatible(FW[key].state_class, card["stat_types"]))

    def test_the_stat_type_rule_itself(self):
        # The one rule both modes use (the tool's; this suite keeps no copy).
        # Spot facts from HA statistics: change/sum need a sum, which a
        # measurement does not keep; mean/min/max need a measurement.
        ok = g.graph_compatible
        self.assertTrue(ok("total", ["change"]) and ok("total_increasing", ["change"]))
        self.assertTrue(ok("measurement", ["mean"]) and ok("measurement", ["max"]))
        self.assertFalse(ok("measurement", ["change"]) or ok("total", ["mean"]) or ok(None, ["mean"]))
        self.assertFalse(ok("measurement", ["mean", "change"]))  # every stat_type of the card must suit
        # file mode is unaffected: every graph key has a state_class that suits its card
        for _, spec in g.GRAPHS:
            for k in spec["keys"]:
                self.assertTrue(ok(FW[k].state_class, spec["stat_types"]), k)

    def test_the_owners_graphs_are_there(self):
        view = self.doc["views"][0]
        n = DEVICES[0]["node"]
        hist = [c for c in walk(view) if c.get("type") == "history-graph"]
        self.assertEqual(len(hist), 1)
        self.assertEqual(hist[0]["hours_to_show"], 24)
        for i in (1, 2, 3, 4):
            self.assertIn(f"sensor.magtag_{n}_remaining_{i}", entity_refs(hist[0]))
        stats = {(tuple(sorted(entity_refs(c))), tuple(c["stat_types"]), c["period"])
                 for c in walk(view) if c.get("type") == "statistics-graph"}
        runs = tuple(sorted(f"sensor.magtag_{n}_day_runs_{i}" for i in (1, 2, 3, 4)))
        for want in [((f"sensor.magtag_{n}_battery",), ("mean",), "day"),
                     (runs, ("change",), "day"),
                     (runs, ("change",), "week"),
                     ((f"sensor.magtag_{n}_screen_used_day",), ("change",), "day"),
                     ((f"sensor.magtag_{n}_day_chores",), ("change",), "day")]:
            self.assertIn(want, stats)
        self.assertEqual(len(stats), 5)
        notes = [c["content"] for c in walk(view) if c.get("type") == "markdown"]
        self.assertTrue(notes and all("following" in t for t in notes))

    def test_chores_per_day_reads_the_summary_not_the_live_count(self):
        # A daily `max` of the live chores_done counts the value carried
        # across midnight, so a day with nothing done could show the day
        # before's full count. The graph reads day_chores with `change`;
        # chores_done stays on the tab as the live value, in no graph.
        view = self.doc["views"][0]
        n = DEVICES[0]["node"]
        graphs = [c for c in walk(view) if c.get("type") == "statistics-graph"]
        chores = [c for c in graphs if f"sensor.magtag_{n}_day_chores" in entity_refs(c)]
        self.assertEqual(len(chores), 1)
        self.assertEqual(entity_refs(chores[0]), [f"sensor.magtag_{n}_day_chores"])
        self.assertEqual((chores[0]["stat_types"], chores[0]["period"]), (["change"], "day"))
        live = f"sensor.magtag_{n}_chores_done"
        self.assertFalse([c for c in graphs if live in entity_refs(c)])
        now = next(s for s in view["sections"] if any(c.get("heading") == "Now" for c in s["cards"]))
        self.assertIn(live, [r for c in now["cards"] for r in entity_refs(c)])

    def test_every_summary_graph_carries_the_day_shift_note(self):
        # Per-day AND per-week graphs on the daily summary sensors: a day's
        # figures land under the next day, so a Sunday run counts next week.
        summary = re.compile(r"_(?:screen_used_day|day_runs_\d|day_chores)$")
        for view in self.doc["views"]:
            n = 0
            for s in view["sections"]:
                graphs = [c for c in s["cards"] if c.get("type") == "statistics-graph"]
                if not any(summary.search(r) for c in graphs for r in entity_refs(c)):
                    continue
                n += 1
                notes = [c["content"] for c in s["cards"] if c.get("type") == "markdown"]
                self.assertEqual(len(notes), 1, s["cards"][0])
                self.assertIn("**following** day", notes[0])
                self.assertIn("next week", notes[0])
            self.assertEqual(n, 4)  # runs per day, runs per week, screen minutes per day, chores per day

    def test_timer_rows_say_timer_n_once(self):
        # The firmware's own names ("Timer 1 minutes") carry the slot, so
        # no "Timer N" section label repeats it; dividers group the slots.
        for view in self.doc["views"]:
            sec = [s for s in view["sections"] if any(c.get("heading") == "Timers" for c in s["cards"])]
            self.assertEqual(len(sec), 1)
            rows = [r for c in sec[0]["cards"] if c.get("type") == "entities" for r in c["entities"]]
            self.assertEqual([r for r in rows if "entity" not in r], [{"type": "divider"}] * 3)
            self.assertFalse([r for r in rows if r.get("type") == "section"])
            timer = [r for r in rows if "entity" in r]
            self.assertEqual(len(timer), 16)
            for r in timer:
                self.assertEqual(r["name"], {"type": "entity"})
                self.assertRegex(FW[ID_RE.match(r["entity"]).group(3)].name, r"^Timer [1-4] ")

    def test_logbook_lists_every_entity_of_the_device(self):
        for view, dev in zip(self.doc["views"], DEVICES):
            logs = [c for c in walk(view) if c.get("type") == "logbook"]
            self.assertEqual(len(logs), 1)
            want = {g.entity_id(e.component, dev["node"], k) for k, e in FW.items()}
            self.assertEqual(set(logs[0]["target"]["entity_id"]), want)

    def test_parts_run_settings_activity_graphs_diagnostics(self):
        for view in self.doc["views"]:
            titles = [c["heading"] for s in view["sections"] for c in s["cards"]
                      if c.get("type") == "heading" and c.get("heading_style") == "title"]
            self.assertEqual(titles, ["Settings", "Activity", "Graphs", "Diagnostics"])
            last = entity_refs(view["sections"][-1])
            self.assertTrue(last)
            diag = {ID_RE.match(r).group(3) for s in view["sections"][-5:] for r in entity_refs(s)}
            for k in ("battery_mv", "heap_free", "stack_net", "panic_count", "ota_result", "nvs_free",
                      "config_warning"):
                self.assertIn(k, diag)

    def test_runtime_named_entities_take_their_name_from_discovery(self):
        # Per-slot rows are named "<timer> remaining" etc. by the device at
        # runtime, chore_N "<chore> done": a hard-coded name would hide a rename.
        runtime = re.compile(r"_(?:(?:remaining|limit|completions|day_runs)_\d|chore_\d)$")
        n = 0
        for d in walk(self.doc):
            if isinstance(d.get("entity"), str) and runtime.search(d["entity"]):
                n += 1
                self.assertEqual(d.get("name"), {"type": "entity"}, d)
        self.assertGreater(n, 20)


class TestParts(unittest.TestCase):
    def test_committed_automation_is_the_owners(self):
        with open(AUTOMATION, "rb") as fh:
            self.assertEqual(hashlib.md5(fh.read()).hexdigest(), OWNER_MD5)

    def test_automation_part_carries_the_file_verbatim_with_notes(self):
        with open(AUTOMATION, encoding="utf-8", newline="") as fh:
            body = fh.read()
        part = g.render("automation", DEVICES, SETS)
        self.assertTrue(part.endswith(body))
        notes = part[: -len(body)]
        for s in ("automations.yaml", "FluxCD", "todo.magtag_<node>_chores", "calendar.school_schedule",
                  '"No School..."', '"No School: Summer"', "persistent notification", "every device"):
            self.assertIn(s, notes)
        # A fresh automations.yaml is `[]`; appending a block item after it
        # is invalid YAML and HA loads no automations at all.
        self.assertIn("REPLACE the [] with this, do not append below it", " ".join(notes.replace("#", "").split()))
        self.assertTrue(all(ln.startswith("#") for ln in notes.splitlines()))
        self.assertIsInstance(yaml.safe_load(part), list)
        # "needs only its list (setup step N)": N is the step that creates it
        n = re.search(r"needs only its list \(setup step (\d)\)", notes).group(1)
        self.assertIn(f"\n{n}. Create its chore list", g.render("setup", DEVICES, SETS))

    def test_setup_part(self):
        part = g.render("setup", DEVICES, SETS)
        for s in ("OTA", "v20", "Delete", "history", "Local To-do", "2025.10", "2025.11",
                  '"MagTag 1a0a5c chores"', "todo.magtag_1a0a5c_chores",
                  '"MagTag 1aaa76 chores"', "todo.magtag_1aaa76_chores", "Julia's Timer"):
            self.assertIn(s, part)

    def test_minimum_ha_version_is_2025_11(self):
        # Cards use name: {type: entity} (frontend 20251105.0); 2025.10 is
        # enough for the entity ids alone and must never read as the floor.
        setup = g.render("setup", DEVICES, SETS)
        self.assertIn("Home Assistant 2025.11 or newer", setup)
        self.assertIn("2025.11 or newer", g.DASHBOARD_HEADER)
        for s in (g.render("all", DEVICES, SETS), read("tools/gen_ha_dashboard.py")):
            self.assertIsNone(re.search(r"2025\.10(?:\+| or newer| or later)", s))

    def test_reregistration_steps_run_ota_delete_rename(self):
        # Works whether or not HA clears a deleted device's retained
        # discovery: OTA first, then delete, then force a republish.
        flat = " ".join(g.render("setup", DEVICES, SETS).split())
        pos = [flat.index(s) for s in ("OTA the device to current firmware first", "predates discovery schema v20",
                                       "-> Delete", "Rename one of its chores", "rename the chore back")]
        self.assertEqual(pos, sorted(pos))
        for s in ("do NOT delete it in HA (step 3) before this OTA", "name-derived", "sensor.testing_timer_",
                  "discovery fingerprint",
                  "Button D on the timer screen", '"MagTag <node> chores" To-do list',
                  "after that window's discovery pass", "within two network windows", "twice, a minute apart"):
            self.assertIn(s, flat)
        # What the delete costs, from HA core source (entity registry restore
        # since 2025.7, MQTT's rename to default_entity_id, the recorder's
        # move on an entity_id change): customisations and history most
        # likely survive; references to the old ids break. Unconfirmed on a
        # real install, and it says so. It must never again claim a loss.
        for s in ("NOT yet confirmed on a real install", "HA 2025.7+ restores", "area, custom names and icons",
                  "recorder moves its history and statistics", "most likely keeps",
                  "anything naming the old ids -- dashboards, automations, scripts",
                  '"Recreate entity IDs"', "untested"):
            self.assertIn(s, flat)
        for s in ("loses its", "stay behind under the old ids", "orphaned"):
            self.assertNotIn(s, flat)
        # Renaming the chore back too early restores the same document and
        # ver: the device skips it and never republishes.
        self.assertIn("rename the chore back -- not sooner: renamed back before the device's next window", flat)
        self.assertIn("the device skips it, and nothing republishes", flat)
        # On the chore checklist D is the chore 3 tick (docs/ProductOverview.md
        # "Buttons -- the mode"): the step says to leave it with A first.
        pos = [flat.index(s) for s in ("If its chore checklist is showing, D is the chore 3 tick there, not a sync",
                                       "press Button A first to get back to the timer screen",
                                       "rename the chore back")]
        self.assertEqual(pos, sorted(pos))
        overview = " ".join(read("docs/ProductOverview.md").split())
        self.assertIn("B, C and D tick chores 1, 2 and 3", overview)
        self.assertIn("press A to get back to the timer screen", overview)
        self.assertNotIn("restart HA", flat)
        self.assertNotIn("publishes at its next network window", flat)

    def test_two_window_claim_matches_the_firmware_order(self):
        # Step 4's "two windows" rests on mqtt_ha_window() running the
        # discovery gate (publish_states) BEFORE it applies a received
        # config document (apply_incoming). If that order flips, one window
        # is enough and the setup text must change with it.
        src = read("main/mqtt_ha.c")
        body = src[src.index("void mqtt_ha_window(const stats_snapshot_t *snap) {"):]
        gate = body.index("= publish_states(client, snap,")
        apply = body.index("apply_incoming(client, snap);")
        self.assertLess(gate, apply)
        states = src[src.index("static int publish_states("):src.index("static int apply_incoming(")]
        self.assertIn("ha_config_discovery_gate(", states)
        self.assertIn("publish_discovery(client,", states)
        inc = src[src.index("static int apply_incoming("):src.index("void mqtt_ha_window(")]
        self.assertIn("config_apply(", inc)
        self.assertNotIn("publish_discovery(", inc)

    def test_file_mode_names_every_slot_and_chore_and_says_so(self):
        for text in (g.render("setup", DEVICES, SETS), g.render("dashboard", DEVICES, SETS)):
            flat = " ".join(text.replace("#", " ").split())
            for s in ("every timer slot (1-4)", "every chore row (1-3)", '"entity not available"',
                      "Run with --mqtt to build each tab from the device's real entity set"):
                self.assertIn(s, flat)
            self.assertNotIn("M4-T3", flat)
        # ...and it does: the full slot/chore set is on every tab.
        for view in dashboard()["views"]:
            keys = {ID_RE.match(r).group(3) for r in entity_refs(view) if not r.startswith("todo.")}
            for n in g.SLOTS:
                self.assertTrue({f"remaining_{n}", f"timer{n}_name", f"day_runs_{n}"} <= keys)
            for n in g.CHORES:
                self.assertIn(f"chore_{n}", keys)

    def test_dashboard_part_is_yaml_alone(self):
        part = g.render("dashboard", DEVICES, SETS)
        self.assertNotIn("PART", part)
        self.assertEqual(len(yaml.safe_load(part)["views"]), 2)

    def test_all_is_the_three_parts_in_order(self):
        out = g.render("all", DEVICES, SETS)
        pos = [out.index(g.BANNER[p]) for p in g.PARTS]
        self.assertEqual(pos, sorted(pos))
        for p in g.PARTS:
            self.assertIn(g.render(p, DEVICES, SETS), out)
        self.assertLess(out.index(g.BANNER["automation"]), out.index(g.render("automation", DEVICES, SETS)))
        self.assertLess(out.index(g.BANNER["dashboard"]), out.index(g.render("dashboard", DEVICES, SETS)))


class TestDevices(unittest.TestCase):
    def test_example_file(self):
        self.assertEqual(DEVICES, [{"node": "1a0a5c", "label": "Testing Timer"},
                                   {"node": "1aaa76", "label": "Julia's Timer"}])

    def test_invalid_nodes_rejected(self):
        for bad in ("1A0A5C", "1a0a5", "1a0a5c0", "zzzzzz", "magtag-1a0a5c", 123456, None):
            with self.subTest(node=bad), self.assertRaises(g.UsageError):
                g.parse_devices({"devices": [{"node": bad, "label": "x"}]})

    def test_other_bad_files_rejected(self):
        for doc in (None, {}, {"devices": []}, {"devices": ["1a0a5c"]},
                    {"devices": [{"node": "1a0a5c", "label": ""}]},
                    {"devices": [{"node": "1a0a5c", "label": "a"}, {"node": "1a0a5c", "label": "b"}]}):
            with self.subTest(doc=doc), self.assertRaises(g.UsageError):
                g.parse_devices(doc)

    def test_missing_file_points_at_the_example(self):
        with self.assertRaises(g.UsageError) as cm:
            g.load_devices(os.path.join(_HERE, "no_such_devices.yaml"))
        self.assertIn("ha_devices.example.yaml", str(cm.exception))


class TestCli(unittest.TestCase):
    """The shipped entry point, through uv and the script's own PEP 723
    metadata -- the way the owner runs it."""

    def run_tool(self, *args):
        uv = find_uv()
        self.assertTrue(os.path.exists(uv), f"uv not found (looked for {uv}); install uv")
        return subprocess.run([uv, "run", "--quiet", TOOL, *args], capture_output=True, text=True, cwd=REPO,
                              timeout=120)

    def test_dashboard_via_uv(self):
        r = self.run_tool("--devices", EXAMPLE, "--part", "dashboard")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, g.render("dashboard", DEVICES, SETS))

    def test_missing_devices_file_exits_2_with_help(self):
        r = self.run_tool("--devices", os.path.join(_HERE, "no_such_devices.yaml"))
        self.assertEqual(r.returncode, 2)
        self.assertIn("ha_devices.example.yaml", r.stderr)
        self.assertEqual(r.stdout, "")

    def test_paho_is_not_imported_by_file_mode(self):
        # In the script's OWN uv environment, where paho-mqtt IS installed:
        # render every part in file mode, then look at sys.modules.
        uv = find_uv()
        self.assertTrue(os.path.exists(uv), f"uv not found (looked for {uv}); install uv")
        r = subprocess.run([uv, "sync", "--quiet", "--script", TOOL], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = subprocess.run([uv, "python", "find", "--script", TOOL], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        py = r.stdout.strip()
        harness = (
            "import contextlib, importlib.util, io, sys\n"
            "sys.dont_write_bytecode = True\n"
            f"sys.path.insert(0, {os.path.dirname(TOOL)!r})\n"
            "assert importlib.util.find_spec('paho') is not None, 'paho not installed here'\n"
            "import gen_ha_dashboard as g\n"
            "with contextlib.redirect_stdout(io.StringIO()) as out:\n"
            f"    rc = g.main(['--devices', {EXAMPLE!r}, '--part', 'all'])\n"
            "assert rc == 0 and 'PART 3 of 3' in out.getvalue()\n"
            "print(sorted(m for m in sys.modules if m == 'paho' or m.startswith('paho.')))\n"
        )
        r = subprocess.run([py, "-c", harness], capture_output=True, text=True, cwd=REPO, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "[]")

    def test_mqtt_against_a_closed_port_fails_cleanly_through_real_paho(self):
        # The real adapter (paho, in the script's uv env) against a local
        # port nothing listens on: exit 1, the host named, the password not.
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        for scheme in ("mqtt", "mqtts"):
            with self.subTest(scheme=scheme), tempfile.TemporaryDirectory() as d:
                sdk = write_sdkconfig(d, uri=f"{scheme}://127.0.0.1:{port}")
                r = self.run_tool("--mqtt", "--sdkconfig", sdk, "--wait", "5")
                self.assertEqual(r.returncode, 1, r.stderr)
                self.assertIn(f"127.0.0.1:{port}", r.stderr)
                self.assertIn("cannot connect", r.stderr)
                self.assertEqual(r.stdout, "")
                self.assertNotIn(SENTINEL, r.stderr + r.stdout)


# --------------------------------------------------------------------------
# --mqtt
# --------------------------------------------------------------------------

SENTINEL = "Sentinel-Pa55-q7Zx"  # the "password" every --mqtt test uses; must never surface
RETIRED = ("remaining", "allocation", "screen_used")  # mqtt_ha.c RETIRED[]: always published empty
SLOT_RE = re.compile(r"^(?:remaining|limit|completions|day_runs)_(\d)$")
CHORE_RE = re.compile(r"^chore_(\d)$")


def disc(comp, node, key, name, dev_name, def_ent=True, stat_cla=None, sw="2.0.0"):
    """One retained discovery message, shaped like the firmware's
    (stats_json.c / ha_config.c / mqtt_ha.c builders)."""
    uid = f"magtag-{node}_{key}"
    doc = {"name": name, "uniq_id": uid}
    if def_ent:
        doc["def_ent_id"] = f"{comp}.{uid}"
    if stat_cla:
        doc["stat_cla"] = stat_cla
    doc["stat_t"] = f"magtag/magtag-{node}/stat"
    doc["dev"] = {"ids": [f"magtag-{node}"], "name": dev_name, "mf": "Adafruit", "mdl": "MagTag 2.9", "sw": sw}
    return f"homeassistant/{comp}/{uid}/config", json.dumps(doc, separators=(",", ":")).encode()


def device_msgs(node, dev_name, disabled_slots=(), chores=3, def_ent=True, lacks=(), stat_cla=True, sw="2.0.0"):
    """A device's full retained set: every firmware key, empty for a
    disabled slot's per-slot sensors and chore rows past `chores`, plus the
    RETIRED[] keys (always empty). Older firmware: `lacks` keys it never
    published (no topic at all), stat_cla=False for none on any entity."""
    out = []
    for k, e in FW.items():
        if k in lacks:
            continue
        slot, chore = SLOT_RE.match(k), CHORE_RE.match(k)
        retired = bool(slot and int(slot.group(1)) in disabled_slots) or bool(chore and int(chore.group(1)) > chores)
        topic, payload = disc(e.component, node, k, e.name, dev_name, def_ent, e.state_class if stat_cla else None, sw)
        out.append((topic, b"" if retired else payload))
    out += [(f"homeassistant/sensor/magtag-{node}_{k}/config", b"") for k in RETIRED]
    return out


FOREIGN = [
    ("homeassistant/sensor/zigbee_0x00158d_temperature/config", b'{"name":"Temp","uniq_id":"0x00158d_t"}'),
    ("homeassistant/light/kitchen/config", b'{"name":"Kitchen"}'),
    ("homeassistant/sensor/magtag-1a0a5c_battery/state", b"57"),  # not a /config topic
    ("homeassistant/sensor/magtag-XYZ_battery/config", b'{"name":"bad node"}'),
]

# The keys discovery schemas v21-v23 added (per git history of stats_json.c,
# ha_config.c and mqtt_ha.c; none was removed after v20). Before v23 no
# payload carried a stat_cla.
V21_PLUS = ({f"chore_{n}" for n in (1, 2, 3)} | {f"day_runs_{n}" for n in (1, 2, 3, 4)}
            | {"chore_free_wd", "chore_free_we", "chore_free_hol", "chore_free_sum", "chores_left", "chores_done",
               "config_warning", "screen_used_day", "day_chores"})

# The recorded broker:
#  - Testing Timer on current firmware, timer slots 3 and 4 disabled, 2 chores;
#  - Julia's Timer on 1.5.4, which is discovery schema v20: it sends
#    def_ent_id, but none of the v21-v23 keys and no stat_cla;
#  - Old Timer, SYNTHETIC: firmware before v20, no def_ent_id at all.
TESTING_MSGS = device_msgs("1a0a5c", "Testing Timer", disabled_slots=(3, 4), chores=2)
JULIA_MSGS = device_msgs("1aaa76", "Julia's Timer", lacks=V21_PLUS, stat_cla=False, sw="1.5.4")
OLD_MSGS = device_msgs("0b1c2d", "Old Timer", lacks=V21_PLUS, stat_cla=False, def_ent=False, sw="1.4.0")
RECORDED = FOREIGN + TESTING_MSGS + JULIA_MSGS + OLD_MSGS
TESTING_RETIRED = {f"{p}_{n}" for p in ("remaining", "limit", "completions", "day_runs") for n in (3, 4)} | {
    "chore_3"}


def write_sdkconfig(d, uri="mqtt://broker.test", user="magtag", password=SENTINEL, extra=""):
    def q(s):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

    path = os.path.join(d, "sdkconfig")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# Espressif IoT Development Framework (ESP-IDF) Project Configuration\n"
                 "CONFIG_IDF_TARGET=\"esp32s2\"\n"
                 f"CONFIG_MAGTAG_MQTT_URI={q(uri)}\n"
                 f"CONFIG_MAGTAG_MQTT_USER={q(user)}\n"
                 f"CONFIG_MAGTAG_MQTT_PASS={q(password)}\n"
                 "CONFIG_MAGTAG_WEEKDAY_MIN=120\n" + extra)
    return path


class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class Rc:
    """paho 2's ReasonCode, as far as the adapter looks at it."""

    def __init__(self, failure, text):
        self.is_failure, self.text = failure, text

    def __str__(self):
        return self.text


class StubClient:
    """Stands in for paho.mqtt.client.Client: each loop() call advances the
    clock 0.1 s and delivers the next pending event."""

    CHUNK = 50

    def __init__(self, clock, messages=(), connack=Rc(False, "Success"), suback=Rc(False, "Granted QoS 0"),
                 connect_exc=None, answer=True, loop_exc=None, loop_rc=0, stream=False):
        self.clock, self.messages, self.connack, self.suback = clock, list(messages), connack, suback
        self.connect_exc, self.answer, self.loop_exc, self.loop_rc, self.stream = (
            connect_exc, answer, loop_exc, loop_rc, stream)
        self.calls, self.events = [], []

    def username_pw_set(self, user, password=None):
        self.calls.append(("auth", user, password))

    def tls_set(self, *a, **kw):
        self.calls.append(("tls",))

    def connect(self, host, port, keepalive=60):
        self.calls.append(("connect", host, port))
        if self.connect_exc:
            raise self.connect_exc
        if self.answer:
            self.events.append("connack")

    def subscribe(self, topic, qos=0):
        self.calls.append(("subscribe", topic))
        self.events.append("suback")

    def loop(self, timeout=1.0):
        self.clock.t += 0.1
        self.loops = getattr(self, "loops", 0) + 1
        if self.loops > 10000:  # 1000 fake seconds: a scan with no working cap fails instead of hanging
            raise RuntimeError("stub: the scan never ended")
        if self.loop_exc:
            raise self.loop_exc
        if self.events:
            ev = self.events.pop(0)
            if ev == "connack":
                self.on_connect(self, None, {}, self.connack, None)
            elif ev == "suback":
                self.on_subscribe(self, None, 1, [self.suback], None)
                # the retained burst, CHUNK messages per loop(). Real paho
                # 2.1.0 reads ONE packet per loop() call for QoS 0; chunking
                # only keeps the stub fast. The adapter's idle clock runs
                # from the last message either way.
                self.events += [("burst", self.messages[i:i + self.CHUNK])
                                for i in range(0, len(self.messages), self.CHUNK)]
            else:
                for topic, payload in ev[1]:
                    self.on_message(self, None, type("Msg", (), {"topic": topic, "payload": payload,
                                                                 "retain": True})())
        elif self.stream and any(c[0] == "subscribe" for c in self.calls):
            self.on_message(self, None, type("Msg", (), {"topic": "homeassistant/x/y/config", "payload": b"",
                                                         "retain": False})())
        return self.loop_rc

    def disconnect(self):
        self.calls.append(("disconnect",))


class FakePahoModule:
    """Stands in for paho.mqtt.client in _paho_client(): records how the
    Client is constructed."""

    class CallbackAPIVersion:
        VERSION2 = "VERSION2"

    def __init__(self):
        self.made = []

    def Client(self, *args, **kwargs):  # noqa: N802 -- paho's name
        self.made.append((args, kwargs))
        return object()


def broker(tls=False, user="magtag", password=SENTINEL):
    return g.Broker("broker.test", 8883 if tls else 1883, tls, user, password)


class TestMqttCollect(unittest.TestCase):
    def setUp(self):
        self.scan = g.collect_discovery(RECORDED, fw=FW)

    def test_devices_come_from_the_discovery_topics_with_dev_name_labels(self):
        self.assertEqual(self.scan.devices, [{"node": "1aaa76", "label": "Julia's Timer"},
                                             {"node": "0b1c2d", "label": "Old Timer"},
                                             {"node": "1a0a5c", "label": "Testing Timer"}])
        self.assertEqual(self.scan.topics, len({t for t, _ in RECORDED}))
        # one warning each for Julia (older firmware) and Old Timer (pre-v20):
        # retired payloads, foreign topics and a current device raise none
        self.assertEqual(len(self.scan.warnings), 2, self.scan.warnings)
        self.assertEqual(sorted(re.search(r"magtag-(\w+)", w).group(1) for w in self.scan.warnings),
                         ["0b1c2d", "1aaa76"])
        self.assertTrue(V21_PLUS <= set(FW))  # the fixture's history names real keys

    def test_v20_device_gets_a_tab_and_an_older_firmware_warning(self):
        # Julia's Timer on 1.5.4 (schema v20): def_ent_id present, so its
        # ids are known and it gets a tab, from what it publishes.
        ents = self.scan.entity_sets["1aaa76"]
        self.assertEqual(set(ents), set(FW) - V21_PLUS)
        w = [x for x in self.scan.warnings if "magtag-1aaa76" in x]
        self.assertEqual(len(w), 1)
        self.assertIn("Julia's Timer (magtag-1aaa76) runs older firmware: OTA it (step 1), then re-run this tool "
                      "so its tab includes the newer entities.", w[0])
        self.assertIn("It reports firmware 1.5.4.", w[0])
        # the always-published v21-v23 keys are named; the conditional ones
        # (chore rows, day_runs_N) could be a short list, not old firmware
        for k in ("chore_free_wd", "chores_left", "chores_done", "config_warning", "screen_used_day", "day_chores"):
            self.assertIn(k, w[0])
        for k in ("chore_1", "day_runs_1"):
            self.assertNotIn(k, w[0])
        self.assertIn("No state class on battery", w[0])
        self.assertNotIn("schema v20", w[0])
        # the tab: no statistics graph (battery has no stat_cla, the rest
        # are absent), and no empty graph card; the history graph stays
        view = g.build_view("1aaa76", "Julia's Timer", ents)
        cards = list(walk(view))
        self.assertFalse([c for c in cards if c.get("type") == "statistics-graph"])
        self.assertEqual(len([c for c in cards if c.get("type") == "history-graph"]), 1)
        self.assertIn("sensor.magtag_1aaa76_battery", entity_refs(view))  # still on the tab, as a row
        headings = [c.get("heading") for c in cards if c.get("type") == "heading"]
        for t, _ in g.GRAPHS:
            self.assertNotIn(t, headings)
        # ...while the current device keeps every graph its slots allow
        tv = g.build_view("1a0a5c", "Testing Timer", self.scan.entity_sets["1a0a5c"])
        self.assertEqual(len([c for c in walk(tv) if c.get("type") == "statistics-graph"]), len(g.GRAPHS))

    def test_graph_entities_follow_the_published_stat_cla(self):
        # One card, two entities: only the one whose stat_cla suits `change`
        # stays. A card left with none is dropped, never emitted empty.
        msgs = [disc("sensor", "abcdef", "day_runs_1", "R1", "X", stat_cla="total"),
                disc("sensor", "abcdef", "day_runs_2", "R2", "X", stat_cla="measurement"),
                disc("sensor", "abcdef", "battery", "B", "X", stat_cla="total"),
                disc("sensor", "abcdef", "remaining_1", "Rem", "X")]
        view = g.build_view("abcdef", "X", g.collect_discovery(msgs).entity_sets["abcdef"])
        graphs = [c for c in walk(view) if c.get("type") == "statistics-graph"]
        self.assertEqual([entity_refs(c) for c in graphs], [["sensor.magtag_abcdef_day_runs_1"]] * 2)
        for c in graphs:
            self.assertEqual(c["stat_types"], ["change"])
        hist = [c for c in walk(view) if c.get("type") == "history-graph"]
        self.assertEqual([entity_refs(c) for c in hist], [["sensor.magtag_abcdef_remaining_1"]])  # exempt

    def test_older_firmware_check_needs_the_firmware_table_and_spares_current_devices(self):
        self.assertFalse(g.collect_discovery(JULIA_MSGS).warnings)  # fw=None: no check
        current = g.collect_discovery(TESTING_MSGS, fw=FW)  # disabled slots, 2 chores: still current
        self.assertEqual(current.warnings, [])
        # one missing always-published key is enough, and so is one lost stat_cla
        one = [m for m in TESTING_MSGS if "_config_warning/" not in m[0]]
        self.assertIn("Missing entities current firmware always publishes: config_warning.",
                      " ".join(g.collect_discovery(one, fw=FW).warnings))
        t, p = disc("sensor", "1a0a5c", "screen_used_day", "S", "Testing Timer")  # no stat_cla
        cla = g.collect_discovery(TESTING_MSGS + [(t, p)], fw=FW).warnings
        self.assertEqual(len(cla), 1)
        self.assertIn("runs older firmware", cla[0])
        self.assertIn("No state class on screen_used_day,", cla[0])
        self.assertNotIn("Missing entities", cla[0])

    def test_a_device_without_day_chores_is_older_firmware_and_loses_only_that_graph(self):
        # The M3 build (and M4-T1..T3) publishes no day_chores. It is not a
        # conditional key (never retired), so its absence means older
        # firmware: a warning naming it, and the tab keeps every other
        # graph while the chores graph is dropped, never emitted empty or
        # fed from chores_done.
        self.assertNotIn("day_chores", g.conditional_keys())
        msgs = [m for m in TESTING_MSGS if "_day_chores/" not in m[0]]
        self.assertEqual(len(msgs), len(TESTING_MSGS) - 1)
        scan = g.collect_discovery(msgs, fw=FW)
        self.assertEqual(len(scan.warnings), 1, scan.warnings)
        self.assertIn("runs older firmware", scan.warnings[0])
        self.assertIn("Missing entities current firmware always publishes: day_chores.", scan.warnings[0])
        view = g.build_view("1a0a5c", "Testing Timer", scan.entity_sets["1a0a5c"])
        headings = [c.get("heading") for c in walk(view) if c.get("type") == "heading"]
        self.assertNotIn("Chores done per day", headings)
        graphs = [c for c in walk(view) if c.get("type") == "statistics-graph"]
        self.assertEqual(len(graphs), len(g.GRAPHS) - 1)
        self.assertFalse([c for c in graphs if "sensor.magtag_1a0a5c_chores_done" in entity_refs(c)])

    def test_def_ent_id_that_disagrees_with_the_topic_warns(self):
        t, p = disc("sensor", "abcdef", "battery", "B", "X")
        doc = json.loads(p)
        doc["def_ent_id"] = "sensor.magtag-abcdef_battery_2"
        scan = g.collect_discovery([(t, json.dumps(doc).encode())])
        self.assertEqual(set(scan.entity_sets["abcdef"]), {"battery"})  # kept, by the topic's key
        self.assertEqual(len(scan.warnings), 1)
        self.assertIn("battery (def_ent_id 'sensor.magtag-abcdef_battery_2', want 'sensor.magtag-abcdef_battery'",
                      scan.warnings[0])
        self.assertIn("its id on the tab may be wrong", scan.warnings[0])
        self.assertFalse(g.collect_discovery([disc("sensor", "abcdef", "battery", "B", "X")]).warnings)

    def test_retired_payloads_are_skipped(self):
        # Disabled slots 3-4 and chore_3 are published empty: not in the set,
        # not on the tab. Their config rows (timer3_name...) stay: the
        # firmware always publishes those.
        ents = self.scan.entity_sets["1a0a5c"]
        self.assertEqual(set(ents), set(FW) - TESTING_RETIRED)
        view = g.build_view("1a0a5c", "Testing Timer", ents)
        keys = {ID_RE.match(r).group(3) for r in entity_refs(view) if not r.startswith("todo.")}
        self.assertEqual(keys, set(FW) - TESTING_RETIRED)
        for k in ("remaining_3", "day_runs_4", "chore_3", "completions_4", *RETIRED):
            self.assertNotIn(k, keys)
        for k in ("chore_1", "chore_2", "remaining_2", "timer3_name", "timer4_min"):
            self.assertIn(k, keys)

    def test_the_last_payload_on_a_topic_wins(self):
        on, off = disc("sensor", "abcdef", "battery", "Battery", "X")[1], b""
        t = "homeassistant/sensor/magtag-abcdef_battery/config"
        name = disc("sensor", "abcdef", "chores_left", "Chores left", "X")
        self.assertIn("battery", g.collect_discovery([(t, off), (t, on), name]).entity_sets["abcdef"])
        self.assertNotIn("battery", g.collect_discovery([(t, on), (t, off), name]).entity_sets["abcdef"])

    def test_the_component_is_the_topics(self):
        msgs = [disc("button", "abcdef", "locate", "Find", "X"), disc("sensor", "abcdef", "battery", "B", "X")]
        ents = g.collect_discovery(msgs).entity_sets["abcdef"]
        self.assertEqual(ents["locate"].component, "button")
        view = g.build_view("abcdef", "X", ents)
        self.assertIn("button.magtag_abcdef_locate", entity_refs(view))
        self.assertNotIn("switch.magtag_abcdef_locate", entity_refs(view))

    def test_foreign_topics_only_means_no_devices(self):
        scan = g.collect_discovery(FOREIGN)
        self.assertEqual((scan.devices, scan.entity_sets), ([], {}))
        self.assertEqual(scan.topics, len(FOREIGN))

    def test_pre_v20_device_is_named_and_gets_no_tab(self):
        self.assertNotIn("0b1c2d", self.scan.entity_sets)
        w = [x for x in self.scan.warnings if "magtag-0b1c2d" in x]
        self.assertEqual(len(w), 1)
        for s in ("Old Timer", "schema v20", "NO TAB", "OTA it to current firmware first", "re-run"):
            self.assertIn(s, w[0])
        self.assertNotIn("1.5.4", w[0])  # 1.5.4 is v20: it sends def_ent_id
        out = g.render("all", self.scan.devices, self.scan.entity_sets, g.mqtt_mode_note("mqtt://b:1883"),
                       self.scan.warnings)
        flat = " ".join(out.split())
        self.assertIn("WARNINGS from the broker scan:", out)
        self.assertIn("Old Timer (magtag-0b1c2d) runs firmware older than discovery schema v20", flat)
        self.assertIn('Old Timer: "MagTag 0b1c2d chores" -> todo.magtag_0b1c2d_chores', flat)
        self.assertIn("# NO TAB for Old Timer (magtag-0b1c2d)", out)
        self.assertNotIn("# NO TAB for Julia", out)
        views = yaml.safe_load(g.render("dashboard", self.scan.devices, self.scan.entity_sets))["views"]
        self.assertEqual([v["title"] for v in views], ["Julia's Timer", "Testing Timer"])
        self.assertNotIn("0b1c2d", json.dumps(views))

    def test_payloads_partly_without_def_ent_id_are_left_off_with_a_warning(self):
        msgs = [disc("sensor", "abcdef", "battery", "B", "X"), disc("sensor", "abcdef", "heap_free", "H", "X",
                                                                    def_ent=False)]
        scan = g.collect_discovery(msgs)
        self.assertEqual(set(scan.entity_sets["abcdef"]), {"battery"})
        self.assertTrue(any("heap_free" in w and "left off the tab" in w for w in scan.warnings))

    def test_unknown_key_goes_to_other_with_a_warning(self):
        extra = [disc("sensor", "1a0a5c", "fake_future", "Future thing", "Testing Timer"),
                 disc("button", "1a0a5c", "reboot_now", "Reboot", "Testing Timer")]
        scan = g.collect_discovery(TESTING_MSGS + extra)
        w = [x for x in scan.warnings if "layout does not know" in x]
        self.assertEqual(len(w), 1)
        self.assertIn("fake_future, reboot_now", w[0])
        self.assertIn("Testing Timer (magtag-1a0a5c)", w[0])
        view = g.build_view("1a0a5c", "Testing Timer", scan.entity_sets["1a0a5c"])
        titles = [c["heading"] for s in view["sections"] for c in s["cards"]
                  if c.get("type") == "heading" and c.get("heading_style") == "title"]
        self.assertEqual(titles, ["Settings", "Activity", "Graphs", "Other", "Diagnostics"])
        other = [s for s in view["sections"] if s["cards"][0].get("heading") == "Other"]
        self.assertEqual(len(other), 1)
        self.assertEqual(sorted(entity_refs(other[0])),
                         ["button.magtag_1a0a5c_reboot_now", "sensor.magtag_1a0a5c_fake_future"])
        # ...and nothing else is there, and the file-mode tab has no Other part.
        self.assertFalse([x for x in self.scan.warnings if "layout does not know" in x])
        for v in dashboard()["views"]:
            self.assertNotIn("Other", [c.get("heading") for s in v["sections"] for c in s["cards"]])

    def test_labels_disagreeing_pick_the_most_common_and_warn(self):
        msgs = [disc("sensor", "abcdef", k, k, n) for k, n in
                (("battery", "Old"), ("heap_free", "New"), ("heap_min", "New"))]
        scan = g.collect_discovery(msgs)
        self.assertEqual(scan.devices, [{"node": "abcdef", "label": "New"}])
        self.assertTrue(any("disagree" in w and "'Old'" in w for w in scan.warnings))
        tie = g.collect_discovery(msgs[:2])
        self.assertEqual(tie.devices[0]["label"], "New")  # tie: alphabetical, not arrival order
        self.assertEqual(g.collect_discovery(msgs[1::-1]).devices[0]["label"], "New")

    def test_label_overrides(self):
        scan = g.collect_discovery(RECORDED, {"1a0a5c": "Kitchen"})
        self.assertIn({"node": "1a0a5c", "label": "Kitchen"}, scan.devices)

    def test_no_device_name_falls_back_with_a_warning(self):
        t, p = disc("sensor", "abcdef", "battery", "B", "X")
        doc = json.loads(p)
        del doc["dev"]
        scan = g.collect_discovery([(t, json.dumps(doc).encode())])
        self.assertEqual(scan.devices, [{"node": "abcdef", "label": "MagTag abcdef"}])
        self.assertTrue(any("no device name" in w for w in scan.warnings))

    def test_odd_payloads_warn(self):
        t, p = disc("sensor", "abcdef", "battery", "B", "X")
        bad_uid = json.loads(p)
        bad_uid["uniq_id"] = "magtag-abcdef_other"
        scan = g.collect_discovery([(t, json.dumps(bad_uid).encode()),
                                    ("homeassistant/sensor/magtag-abcdef_heap_free/config", b"{not json")])
        self.assertEqual(set(scan.entity_sets["abcdef"]), {"battery"})
        text = " ".join(scan.warnings)
        self.assertIn("uniq_id 'magtag-abcdef_other'", text)
        self.assertIn("heap_free (payload is not a JSON object; skipped)", text)


class TestSdkconfig(unittest.TestCase):
    def test_quoted_values_and_escapes(self):
        text = ('CONFIG_MAGTAG_MQTT_URI="mqtt://h"\nCONFIG_MAGTAG_MQTT_USER="a\\"b"\n'
                'CONFIG_MAGTAG_MQTT_PASS="x\\\\y\\"z"\nCONFIG_MAGTAG_MQTT_URI_OTHER="no"\n')
        cfg = g.parse_sdkconfig(text)
        self.assertEqual(cfg, {"CONFIG_MAGTAG_MQTT_URI": "mqtt://h", "CONFIG_MAGTAG_MQTT_USER": 'a"b',
                               "CONFIG_MAGTAG_MQTT_PASS": 'x\\y"z'})

    def test_round_trip_through_a_file(self):
        with tempfile.TemporaryDirectory() as d:
            b = g.load_broker(write_sdkconfig(d, uri="mqtts://mq.example:8884", user='u"1', password=SENTINEL))
        self.assertEqual((b.host, b.port, b.tls, b.user, b.password), ("mq.example", 8884, True, 'u"1', SENTINEL))
        self.assertNotIn(SENTINEL, repr(b))
        self.assertNotIn(SENTINEL, str(b))
        self.assertEqual(b.where(), "mqtts://mq.example:8884")

    def test_empty_user_and_password_are_allowed(self):
        cfg = g.parse_sdkconfig('CONFIG_MAGTAG_MQTT_URI="mqtt://h"\nCONFIG_MAGTAG_MQTT_USER=""\n'
                                'CONFIG_MAGTAG_MQTT_PASS=""\n')
        self.assertEqual(cfg["CONFIG_MAGTAG_MQTT_PASS"], "")

    def test_errors_name_the_key_never_the_value(self):
        good = ('CONFIG_MAGTAG_MQTT_URI="mqtt://h"\nCONFIG_MAGTAG_MQTT_USER="u"\n'
                f'CONFIG_MAGTAG_MQTT_PASS="{SENTINEL}"\n')
        cases = {
            "missing": (good.replace("CONFIG_MAGTAG_MQTT_USER", "# CONFIG_X"), "CONFIG_MAGTAG_MQTT_USER not found"),
            "all missing": ("CONFIG_IDF_TARGET=\"esp32s2\"\n", "CONFIG_MAGTAG_MQTT_URI, CONFIG_MAGTAG_MQTT_USER"),
            "unquoted": (good.replace(f'"{SENTINEL}"', SENTINEL), "CONFIG_MAGTAG_MQTT_PASS is not a quoted string"),
            "unterminated": (good.replace(f'"{SENTINEL}"', f'"{SENTINEL}'), "is not a quoted string"),
            "empty uri": (good.replace('"mqtt://h"', '""'), "CONFIG_MAGTAG_MQTT_URI is empty"),
        }
        for name, (text, want) in cases.items():
            with self.subTest(case=name):
                with self.assertRaises(g.UsageError) as cm:
                    g.parse_sdkconfig(text, "/x/sdkconfig")
                self.assertIn(want, str(cm.exception))
                self.assertIn("/x/sdkconfig", str(cm.exception))
                self.assertNotIn(SENTINEL, str(cm.exception))

    def test_empty_uri_points_a_credentials_local_h_owner_at_sdkconfig_file(self):
        # The preferred broker setting (include/credentials.local.h) overrides
        # Kconfig in the firmware and never reaches sdkconfig, whose URI is
        # then empty: the error says how to hand the tool the three lines.
        text = f'CONFIG_MAGTAG_MQTT_URI=""\nCONFIG_MAGTAG_MQTT_USER="u"\nCONFIG_MAGTAG_MQTT_PASS="{SENTINEL}"\n'
        with self.assertRaises(g.UsageError) as cm:
            g.parse_sdkconfig(text, "/x/sdkconfig")
        msg = str(cm.exception)
        self.assertIn("include/credentials.local.h is not in sdkconfig", msg)
        self.assertIn("pass --sdkconfig FILE, a file holding the three CONFIG_MAGTAG_MQTT_ lines", msg)
        self.assertNotIn(SENTINEL, msg)

    def test_missing_file(self):
        with self.assertRaises(g.UsageError) as cm:
            g.load_broker(os.path.join(_HERE, "no_such_sdkconfig"))
        self.assertIn("--sdkconfig", str(cm.exception))


class TestBrokerUri(unittest.TestCase):
    def test_schemes_hosts_and_ports(self):
        for uri, want in (("mqtt://mq.lan", (False, "mq.lan", 1883)),
                          ("mqtts://mq.lan", (True, "mq.lan", 8883)),
                          ("mqtt://mq.lan:1884", (False, "mq.lan", 1884)),
                          ("mqtts://mq.lan:8884/", (True, "mq.lan", 8884)),
                          ("MQTT://MQ.Lan", (False, "mq.lan", 1883)),
                          ("mqtt://192.168.1.5:1883", (False, "192.168.1.5", 1883)),
                          ("mqtts://[fd00::5]:8884", (True, "fd00::5", 8884))):
            with self.subTest(uri=uri):
                self.assertEqual(g.parse_broker_uri(uri), want)

    def test_bad_uris_never_echo_their_text(self):
        for uri, want in (("ws://mq.lan", "not ws://"),
                          ("mq.lan:1883", "want mqtt://host[:port]"),
                          (f"mqtt://magtag:{SENTINEL}@mq.lan", "credentials inside the URI"),
                          (f"mqtt://{SENTINEL}@mq.lan", "credentials inside the URI"),
                          ("mqtt://:1883", "no host"),
                          ("mqtt://mq.lan:99999", "bad port for host mq.lan"),
                          ("mqtt://mq.lan:abc", "bad port"),
                          (f"{SENTINEL}", "want mqtt://")):
            with self.subTest(uri=uri):
                with self.assertRaises(g.UsageError) as cm:
                    g.parse_broker_uri(uri)
                self.assertIn(want, str(cm.exception))
                self.assertNotIn(SENTINEL, str(cm.exception))


class TestMqttFetch(unittest.TestCase):
    def fetch(self, stub_kw=None, b=None, cap=10.0, warnings=None):
        clock = FakeClock()
        stub = StubClient(clock, **(stub_kw or {}))
        start = clock.t
        try:
            return g.fetch_retained(b or broker(), cap, 2.0, lambda: stub, clock, warnings), stub, clock.t - start
        except g.MqttError as e:
            e.stub, e.elapsed = stub, clock.t - start
            raise

    def test_the_paho_client_gets_a_broker_or_random_id_and_a_clean_session(self):
        # A fixed id (or one from a small, repeatable container pid) lets two
        # runs kick each other off; a persistent session leaves state behind.
        fake = FakePahoModule()
        g._paho_client(fake)
        self.assertEqual(fake.made, [(("VERSION2",), {"client_id": "", "clean_session": True})])

    def test_username_only_auth_sends_no_password(self):
        _, stub, _ = self.fetch({}, broker(password=""))
        self.assertEqual(stub.calls[0], ("auth", "magtag", None))

    def test_collects_every_retained_message_and_stops_when_idle(self):
        warnings = []
        msgs, stub, elapsed = self.fetch({"messages": RECORDED}, warnings=warnings)
        self.assertEqual(warnings, [])  # stopped by the idle gap: complete, nothing to say
        self.assertEqual(msgs, dict(RECORDED))
        self.assertIn(("subscribe", "homeassistant/+/+/config"), stub.calls)
        self.assertEqual(stub.calls[0], ("auth", "magtag", SENTINEL))
        self.assertIn(("connect", "broker.test", 1883), stub.calls)
        self.assertNotIn(("tls",), stub.calls)
        self.assertEqual(stub.calls[-1], ("disconnect",))
        # connack, suback, the retained burst over several loops, then 2 s
        # of quiet counted from the LAST message, not from the SUBACK
        bursts = -(-len(RECORDED) // StubClient.CHUNK)
        self.assertGreaterEqual(bursts, 4)
        self.assertAlmostEqual(elapsed, 0.1 * (2 + bursts) + 2.0, delta=0.05)

    def test_tls_and_anonymous(self):
        _, stub, _ = self.fetch({}, broker(tls=True, user=""))
        self.assertIn(("tls",), stub.calls)
        self.assertIn(("connect", "broker.test", 8883), stub.calls)
        self.assertFalse([c for c in stub.calls if c[0] == "auth"])

    def test_nothing_retained_still_ends_after_the_idle_gap(self):
        msgs, _, elapsed = self.fetch({})
        self.assertEqual(msgs, {})
        self.assertLess(elapsed, 3.0)

    def test_stream_past_the_cap_returns_what_came_and_says_it_may_be_short(self):
        warnings = []
        msgs, _, elapsed = self.fetch({"stream": True, "messages": TESTING_MSGS[:3]}, cap=4.0, warnings=warnings)
        self.assertTrue(set(dict(TESTING_MSGS[:3])) <= set(msgs))
        self.assertAlmostEqual(elapsed, 4.0, delta=0.15)
        self.assertEqual(len(warnings), 1, warnings)
        for s in ("stopped at the --wait limit (4 s)", "mqtt://broker.test:1883 was still sending",
                  f"({len(msgs)} topic(s) so far", "may be missing", "larger --wait"):
            self.assertIn(s, warnings[0])
        self.assertNotIn(SENTINEL, warnings[0])
        # no list passed: the same partial set, no error
        self.assertEqual(self.fetch({"stream": True}, cap=4.0)[0], {"homeassistant/x/y/config": b""})

    def test_failures(self):
        cases = {
            "refused": ({"connect_exc": ConnectionRefusedError(111, "Connection refused")},
                        "cannot connect to mqtt://broker.test:1883: ConnectionRefusedError"),
            "auth": ({"connack": Rc(True, "Not authorized")},
                     "refused the connection: Not authorized (check CONFIG_MAGTAG_MQTT_USER / CONFIG_MAGTAG_MQTT_PASS"),
            "bad password": ({"connack": Rc(True, "Bad user name or password")}, "CONFIG_MAGTAG_MQTT_PASS in sdkconfig"),
            "no connack": ({"answer": False}, "no answer from mqtt://broker.test:1883 within 10 s"),
            "suback refused": ({"suback": Rc(True, "Not authorized")}, "refused the subscription"),
            "lost": ({"loop_rc": 7}, "lost the connection to mqtt://broker.test:1883"),
        }
        for name, (kw, want) in cases.items():
            with self.subTest(case=name):
                with self.assertRaises(g.MqttError) as cm:
                    self.fetch(kw)
                self.assertIn(want, str(cm.exception))
                self.assertNotIn(SENTINEL, str(cm.exception))
                self.assertLessEqual(cm.exception.elapsed, 10.15)
                if kw.get("connect_exc") is None:
                    self.assertEqual(cm.exception.stub.calls[-1], ("disconnect",))


class TestMqttMain(unittest.TestCase):
    """main(['--mqtt', ...]) end to end with the stub client."""

    def run_main(self, stub_kw=None, args=(), sdk_kw=None):
        clock = FakeClock()
        stub = StubClient(clock, **(stub_kw or {}))
        with tempfile.TemporaryDirectory() as d:
            sdk = write_sdkconfig(d, **(sdk_kw or {}))
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                try:
                    rc = g.main(["--mqtt", "--sdkconfig", sdk, *args], client_factory=lambda: stub, clock=clock)
                except SystemExit as e:
                    rc = e.code
        return rc, out.getvalue(), err.getvalue()

    def test_happy_path(self):
        rc, out, err = self.run_main({"messages": RECORDED})
        self.assertEqual(rc, 0, err)
        self.assertNotIn(SENTINEL, out + err)
        for p in g.PARTS:
            self.assertIn(g.BANNER[p], out)
        dash = out[out.index(g.BANNER["dashboard"]):]
        doc = yaml.safe_load(dash[dash.index("\n# MagTag dashboard"):])
        self.assertEqual([v["title"] for v in doc["views"]], ["Julia's Timer", "Testing Timer"])
        keys = {ID_RE.match(r).group(3) for r in entity_refs(doc["views"][1]) if not r.startswith("todo.")}
        self.assertEqual(keys, set(FW) - TESTING_RETIRED)
        keys = {ID_RE.match(r).group(3) for r in entity_refs(doc["views"][0]) if not r.startswith("todo.")}
        self.assertEqual(keys, set(FW) - V21_PLUS)
        # --mqtt text: its own note, not file mode's
        flat = " ".join(out.split())
        self.assertIn("Built from the retained discovery on mqtt://broker.test:1883", flat)
        self.assertIn("Re-run after enabling a slot or adding a chore, and after an OTA or a re-register", flat)
        self.assertLessEqual(max(len(ln) for ln in out[:out.index(g.BANNER["automation"])].splitlines()), 80)
        self.assertNotIn("entity not available", out)
        self.assertNotIn("Run with --mqtt", out)
        setup = " ".join(out[:out.index(g.BANNER["automation"])].split())
        # main() hands collect_discovery() the firmware table: Julia's older-firmware warning, in both places
        older = "Julia's Timer (magtag-1aaa76) runs older firmware: OTA it (step 1), then re-run this tool"
        self.assertIn(f"gen_ha_dashboard: warning: {older}", err)
        self.assertIn(f"- {older}", setup)
        self.assertIn("gen_ha_dashboard: warning: Old Timer (magtag-0b1c2d) runs firmware older", err)
        self.assertIn("WARNINGS from the broker scan: - ", setup)
        self.assertIn("- Old Timer (magtag-0b1c2d) runs firmware older", setup)
        self.assertIn("# NO TAB for Old Timer (magtag-0b1c2d)", dash)
        self.assertNotIn("--wait limit", err)

    def test_a_scan_cut_short_by_wait_warns_in_the_output(self):
        rc, out, err = self.run_main({"messages": TESTING_MSGS, "stream": True}, ("--wait", "3"))
        self.assertEqual(rc, 0, err)
        self.assertIn("gen_ha_dashboard: warning: the scan stopped at the --wait limit (3 s)", err)
        setup = " ".join(out[:out.index(g.BANNER["automation"])].split())
        self.assertIn("WARNINGS from the broker scan: - the scan stopped at the --wait limit (3 s)", setup)

    def test_wait_must_be_finite_and_positive(self):
        # nan passes `<= 0` and would disable the cap: a scan that never ends.
        for bad in ("nan", "inf", "-inf", "0", "-1"):
            with self.subTest(wait=bad):
                rc, out, err = self.run_main({"answer": False}, (f"--wait={bad}",))
                self.assertEqual(rc, 2)
                self.assertIn("--wait must be a positive number of seconds", err)
                self.assertEqual(out, "")

    def test_broker_data_carrying_the_password_is_scrubbed_on_stdout_and_stderr(self):
        # Everything --mqtt prints leaves through the one scrubbing exit,
        # not only the error texts: here a device NAME carries it.
        name = f"Kid {SENTINEL}"
        msgs = device_msgs("abcdef", name) + [disc("sensor", "abcdef", "fake_future", "F", name)]
        rc, out, err = self.run_main({"messages": msgs})
        self.assertEqual(rc, 0, err)
        self.assertNotIn(SENTINEL, out)
        self.assertNotIn(SENTINEL, err)
        self.assertIn("title: Kid <password>", out)
        self.assertIn("Kid <password> (magtag-abcdef)", err)

    def test_devices_file_overrides_labels(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "devices.yaml")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write('devices:\n  - {node: "1a0a5c", label: "Kitchen"}\n  - {node: "abcdef", label: "Gone"}\n')
            rc, out, err = self.run_main({"messages": RECORDED}, ("--devices", path, "--part", "dashboard"))
        self.assertEqual(rc, 0, err)
        self.assertEqual([v["title"] for v in yaml.safe_load(out)["views"]], ["Julia's Timer", "Kitchen"])
        self.assertIn("magtag-abcdef (Gone) has no discovery on the broker", err)

    def test_no_magtag_on_the_broker(self):
        rc, out, err = self.run_main({"messages": FOREIGN})
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIn(f"no MagTag discovery on mqtt://broker.test:1883 ({len(FOREIGN)} discovery topic(s)", err)

    def test_only_pre_v20_devices(self):
        rc, out, err = self.run_main({"messages": OLD_MSGS})
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIn("no device can have a tab", err)

    def test_the_password_never_surfaces_on_any_error_path(self):
        leaky = f"handshake failed for magtag:{SENTINEL}@broker.test"  # a library echoing it
        cases = {
            "connect raises": ({"connect_exc": OSError(leaky)}, {}, 1),
            "connect raises ssl": ({"connect_exc": ValueError(leaky)}, {}, 1),
            "auth refused": ({"connack": Rc(True, "Not authorized")}, {}, 1),
            "auth refused, echoing": ({"connack": Rc(True, leaky)}, {}, 1),
            "no answer": ({"answer": False}, {}, 1),
            "loop raises": ({"loop_exc": RuntimeError(leaky)}, {}, 1),
            "lost": ({"loop_rc": 7}, {}, 1),
            "bad uri": ({}, {"uri": f"mqtt://u:{SENTINEL}@h"}, 2),
            "bad scheme": ({}, {"uri": "ws://h"}, 2),
            "nothing found": ({"messages": FOREIGN}, {}, 1),
            "ok": ({"messages": RECORDED}, {}, 0),
        }
        for name, (stub_kw, sdk_kw, want_rc) in cases.items():
            with self.subTest(case=name):
                rc, out, err = self.run_main(stub_kw, (), sdk_kw)
                self.assertEqual(rc, want_rc, err)
                self.assertNotIn(SENTINEL, out)
                self.assertNotIn(SENTINEL, err)
                if want_rc:
                    self.assertTrue(err.startswith("gen_ha_dashboard: "), err)
                    if want_rc == 1 and name != "nothing found":
                        self.assertIn("broker.test", err)
        rc, out, err = self.run_main({"connect_exc": OSError(leaky)})
        self.assertIn("<password>", err)  # scrubbed, not dropped: the rest of the message survives

    def test_unquoted_password_line_is_rejected_without_echo(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "sdkconfig")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(f'CONFIG_MAGTAG_MQTT_URI="mqtt://h"\nCONFIG_MAGTAG_MQTT_USER="u"\n'
                         f'CONFIG_MAGTAG_MQTT_PASS={SENTINEL}\n')
            err = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                rc = g.main(["--mqtt", "--sdkconfig", path], client_factory=lambda: self.fail("must not connect"))
        self.assertEqual(rc, 2)
        self.assertIn("CONFIG_MAGTAG_MQTT_PASS is not a quoted string", err.getvalue())
        self.assertNotIn(SENTINEL, err.getvalue())

    def test_mqtt_options_need_mqtt(self):
        for args in (["--sdkconfig", "x"], ["--wait", "3"]):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    g.main(["--devices", EXAMPLE, *args])
                self.assertEqual(cm.exception.code, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
