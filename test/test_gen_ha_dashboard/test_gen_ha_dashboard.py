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
    card's stat_types (history-graph is exempt: recorder history is enough);
  - the devices file: the example's nodes and labels, and invalid input;
  - the setup text: HA 2025.11+, the re-registration order (OTA, delete,
    rename a chore to force a republish), and file mode's every-slot note;
  - the day-shift note on every per-day and per-week summary graph;
  - file mode does not import paho, checked in the script's own uv
    environment, where paho is installed.
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
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

# stat_types each state_class supports (HA statistics): a `change` needs a
# sum, which only total / total_increasing keep; mean/min/max need a
# measurement.
STAT_OK = {
    "change": {"total", "total_increasing"},
    "sum": {"total", "total_increasing"},
    "mean": {"measurement"},
    "min": {"measurement"},
    "max": {"measurement"},
}


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
                    for st in card["stat_types"]:
                        with self.subTest(entity=r, stat=st):
                            self.assertIn(FW[key].state_class, STAT_OK[st])

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
                     ((f"sensor.magtag_{n}_chores_done",), ("max",), "day")]:
            self.assertIn(want, stats)
        notes = [c["content"] for c in walk(view) if c.get("type") == "markdown"]
        self.assertTrue(notes and all("following" in t for t in notes))

    def test_every_summary_graph_carries_the_day_shift_note(self):
        # Per-day AND per-week graphs on the daily summary sensors: a day's
        # figures land under the next day, so a Sunday run counts next week.
        summary = re.compile(r"_(?:screen_used_day|day_runs_\d)$")
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
            self.assertEqual(n, 3)  # runs per day, runs per week, screen minutes per day

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
        self.assertTrue(all(ln.startswith("#") for ln in notes.splitlines()))
        self.assertIsInstance(yaml.safe_load(part), list)

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
                  "loses its entity history", "its area", "custom names or icons", "discovery fingerprint",
                  "Button D on the timer screen", '"MagTag <node> chores" To-do list'):
            self.assertIn(s, flat)
        self.assertNotIn("restart HA", flat)

    def test_file_mode_names_every_slot_and_chore_and_says_so(self):
        for text in (g.render("setup", DEVICES, SETS), g.render("dashboard", DEVICES, SETS)):
            flat = " ".join(text.replace("#", " ").split())
            for s in ("every timer slot (1-4)", "every chore row (1-3)", '"entity not available"', "--mqtt",
                      "real entity set"):
                self.assertIn(s, flat)
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
