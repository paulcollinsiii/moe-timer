#!/usr/bin/env python3
"""Generate a differential harness: OLD (7aab085 main.c) vs NEW (wake_flow.c)
for the two wake handlers and the post-action tail they share, driven over a
cartesian product, comparing FULL ORDERED EFFECT TRACES.

This is the payload cycle. What moved:
  post_stats_snapshot, render_action_result, finish_action_and_render,
  finish_or_break, maybe_wait_for_event, handle_timer_tick,
  handle_button_wake, and the held-through-sleep RTC pair.

Functions are extracted MECHANICALLY (brace matching by name) from the two
sources so no body is hand-transcribed. Only these substitutions are made,
each of them a change this refactor explicitly claims is device-identical:
  * time(NULL)     -> hal_time_now()   (hal_time.c: `return time(NULL);`)
  * make_state(...) -> make_display_state(...)
    The state-assembly seam is extracted VERBATIM from the current
    wake_flow.c and compiled in, and BOTH sides are routed through it, so
    the ADC read and the assembly are the shipping ones rather than a
    model. It lived in main.c (as a thunk over a make_state that did the
    work) until the residency audit's review moved it: "the battery ADC
    read has no host answer" is not one of the four residency reasons, and
    lock_gate.c refutes it by reading the same ADC under a host test.
    Identical treatment to cycle 10.
  * post_stats_snapshot -> wake_flow_post_stats_snapshot (a rename; the
    function is ADDRESS-TAKEN by main.c's net_apply ops table, so it had to
    stay exposed). Handled by the old_/new_ namespacing, not by a textual
    rewrite: both sides keep their own name and the harness compares the
    calls those names make.
  * symbol renaming to old_/new_ namespaces
The rewrites make the two sides agree by construction on the IDENTITY of the
clock read and of the state assembly. What is still pinned - and what
actually matters here - is their COUNT, their ARGUMENTS and their POSITION
in the trace.

Both sides read the SAME s_held_mask_at_sleep / s_sleep_entry_time storage
(defined once in the harness), which is what makes row 7 a real axis: the
guard's operands arrive from RTC memory, not from a caller.

wake_policy.c is compiled in for real rather than stubbed: the render
choice, the snap, the sync cadence and the grid-wait length are all
decisions both sides delegate to it, and a stub would be a second
implementation to keep in sync.

enter_deep_sleep DOES NOT RETURN on device, and does not here: it longjmps
to the driver. Every early exit is therefore a trace that simply stops,
which is exactly how a wake ends.
"""
import re
import subprocess
import sys
import os

REPO = "/workspaces/magtag-espidf/.claude/worktrees/refactor-main-impl"
OUT = os.path.dirname(os.path.abspath(__file__))
BASE = "7aab085"


def read_git(rev, path):
    return subprocess.check_output(["git", "-C", REPO, "show", f"{rev}:{path}"], text=True)


def read_file(path):
    with open(os.path.join(REPO, path)) as f:
        return f.read()


def extract(src, name):
    """Pull a whole function definition out by name via brace matching."""
    m = re.search(r"^[A-Za-z_][A-Za-z0-9_ \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", src, re.M | re.S)
    if not m:
        sys.exit(f"could not find definition of {name}")
    start = m.start()
    i = src.index("{", m.start())
    depth = 0
    while True:
        c = src[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return src[start:i + 1]


def namespace(body, prefix, names):
    for n in names:
        body = re.sub(r"\b" + re.escape(n) + r"\b", prefix + n, body)
    return body


old_main = read_git(BASE, "main/main.c")
new_flow = read_file("main/wake_flow.c")

OLD_NAMES = ["post_stats_snapshot", "render_action_result", "finish_action_and_render",
             "finish_or_break", "maybe_wait_for_event", "handle_timer_tick", "handle_button_wake"]
NEW_NAMES = ["wake_flow_post_stats_snapshot", "render_action_result", "finish_action_and_render",
             "finish_or_break", "maybe_wait_for_event", "wake_flow_handle_timer_tick",
             "wake_flow_handle_button_wake"]

old_bodies = "\n\n".join(extract(old_main, n) for n in OLD_NAMES)
new_bodies = "\n\n".join(extract(new_flow, n) for n in NEW_NAMES)
seams = extract(new_flow, "make_display_state")

# --- the only permitted rewrites -------------------------------------------
old_bodies = old_bodies.replace("time(NULL)", "hal_time_now()")
old_bodies = re.sub(r"\bmake_state\(", "make_display_state(", old_bodies)
if "vTaskDelay" in old_bodies or "pdMS_TO_TICKS" in old_bodies:
    sys.exit("a vTaskDelay survived the delay-seam rewrite - the sweep would not build")
if "time(NULL)" in old_bodies:
    sys.exit("a raw time(NULL) survived the clock-seam rewrite")

old_bodies = namespace(old_bodies, "old_", OLD_NAMES)
# The NEW side is namespaced AFTER the mutant edits are applied, so every
# anchor below is written against the source exactly as it appears in
# wake_flow.c - including the calls these functions make to each other.
seams = re.sub(r"\bstatic\b\s+", "", seams)  # make_state is ours; the seam stays non-static

# Both sides are file-scope here, so drop the `static` the definitions carried
# inside their own translation units. Only at the start of a line, which in
# these bodies is only ever a definition.
old_bodies = re.sub(r"^static\s+", "", old_bodies, flags=re.M)
new_bodies = re.sub(r"^static\s+", "", new_bodies, flags=re.M)

# The NEW side's tick handler reads its sync cadence from a macro that only
# sdkconfig.h defines on device; wake_flow.c carries the Kconfig default as a
# host fallback. The OLD side had the same macro in main.c. Define it once here
# so both sides compile against the same number - a difference in THAT would be
# a build-configuration change, not a refactor, and is not what is under test.
if "IDLE_SYNC_INTERVAL_SEC" not in old_bodies or "IDLE_SYNC_INTERVAL_SEC" not in new_bodies:
    sys.exit("the sync cadence macro vanished from one side - check the extraction")

MUTANTS = {
    "control": [],
    # ---- ROW 7: the held-through-sleep guard (120569f) ----
    "row7_delete_the_held_guard": [
        ("    if (btn != BTN_NONE && (s_held_mask_at_sleep & (1u << (int)btn)) &&\n"
         "        (int64_t)hal_time_now() - s_sleep_entry_time <= 2) {",
         "    if (false) {")],
    "row7_window_plus1": [
        ("(int64_t)hal_time_now() - s_sleep_entry_time <= 2) {",
         "(int64_t)hal_time_now() - s_sleep_entry_time <= 3) {")],
    "row7_window_minus1": [
        ("(int64_t)hal_time_now() - s_sleep_entry_time <= 2) {",
         "(int64_t)hal_time_now() - s_sleep_entry_time <= 1) {")],
    "row7_window_strict": [
        ("(int64_t)hal_time_now() - s_sleep_entry_time <= 2) {",
         "(int64_t)hal_time_now() - s_sleep_entry_time < 2) {")],
    "row7_mask_ignored": [
        ("    if (btn != BTN_NONE && (s_held_mask_at_sleep & (1u << (int)btn)) &&\n"
         "        (int64_t)hal_time_now() - s_sleep_entry_time <= 2) {",
         "    if (btn != BTN_NONE && (s_held_mask_at_sleep != 0) &&\n"
         "        (int64_t)hal_time_now() - s_sleep_entry_time <= 2) {")],
    "row7_mask_bit_off_by_one": [
        ("(s_held_mask_at_sleep & (1u << (int)btn))",
         "(s_held_mask_at_sleep & (1u << ((int)btn + 1)))")],
    "row7_none_guard_dropped": [
        ("    if (btn != BTN_NONE && (s_held_mask_at_sleep & (1u << (int)btn)) &&",
         "    if ((s_held_mask_at_sleep & (1u << (int)btn)) &&")],
    # ---- ROW 8: the grid wait belongs to deep-sleep wakes (6639bde) ----
    "row8_grid_wait_always": [
        ("    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {\n        wake_flow_wait_for_render_grid(25);\n    }",
         "    wake_flow_wait_for_render_grid(25);")],
    "row8_grid_wait_never": [
        ("    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {\n        wake_flow_wait_for_render_grid(25);\n    }",
         "    ;")],
    "row8_grid_wait_inverted": [
        ("    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {\n        wake_flow_wait_for_render_grid(25);",
         "    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {\n        wake_flow_wait_for_render_grid(25);")],
    "row8_grid_wait_cap_plus1": [
        ("wake_flow_wait_for_render_grid(25);", "wake_flow_wait_for_render_grid(26);")],
    "row8_grid_wait_cap_minus1": [
        ("wake_flow_wait_for_render_grid(25);", "wake_flow_wait_for_render_grid(24);")],
    "row8_early_pixel_inverted": [
        ("    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {\n        status_led_show_timer_state();\n    }",
         "    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {\n        status_led_show_timer_state();\n    }")],
    "row8_early_pixel_deleted": [
        ("    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {\n        status_led_show_timer_state();\n    }",
         "    ;")],
    "row8_before_captured_after_the_wait": [
        ("    timer_state_t before = timer_get_state();\n"
         "    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {\n"
         "        wake_flow_wait_for_render_grid(25);\n"
         "    }",
         "    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {\n"
         "        wake_flow_wait_for_render_grid(25);\n"
         "    }\n"
         "    timer_state_t before = timer_get_state();")],
    # ---- ROW 10: expiry-then-break in ONE wake (aa8be4c) ----
    "row10_delete_the_post_render_break_check": [
        ("    now = hal_time_now();\n"
         "    if (wake_flow_maybe_start_break(now)) {\n"
         "        enter_deep_sleep(lock_gate_sleep_mode()); /* break just started; sleep through it */\n"
         "    }\n\n"
         "    /* A press that landed while this wake was awake",
         "    now = hal_time_now();\n\n"
         "    /* A press that landed while this wake was awake")],
    "row10_delete_the_fast_path_break_check": [
        ("    if (wake_flow_maybe_start_break(now)) {\n"
         "        enter_deep_sleep(lock_gate_sleep_mode()); /* break just started; sleep through it */\n"
         "    }\n\n"
         "    /* Land the render on the state's grid",
         "    ;\n\n"
         "    /* Land the render on the state's grid")],
    "row10_post_render_check_before_the_render": [
        ("    wake_render_t wr = wake_policy_render(before, timer_get_state(), false,"
         " wake_flow_break_ended_this_wake(), false);",
         "    if (wake_flow_maybe_start_break(hal_time_now())) {\n"
         "        enter_deep_sleep(lock_gate_sleep_mode());\n"
         "    }\n"
         "    wake_render_t wr = wake_policy_render(before, timer_get_state(), false,"
         " wake_flow_break_ended_this_wake(), false);")],
    "row10_post_render_check_reuses_the_stale_clock": [
        ("    now = hal_time_now();\n"
         "    if (wake_flow_maybe_start_break(now)) {\n"
         "        enter_deep_sleep(lock_gate_sleep_mode()); /* break just started; sleep through it */\n"
         "    }\n\n"
         "    /* A press that landed while this wake was awake",
         "    if (wake_flow_maybe_start_break(now)) {\n"
         "        enter_deep_sleep(lock_gate_sleep_mode()); /* break just started; sleep through it */\n"
         "    }\n\n"
         "    /* A press that landed while this wake was awake")],
    "row10_break_check_after_the_latch_drain": [
        ("    now = hal_time_now();\n"
         "    if (wake_flow_maybe_start_break(now)) {\n"
         "        enter_deep_sleep(lock_gate_sleep_mode()); /* break just started; sleep through it */\n"
         "    }\n\n"
         "    /* A press that landed while this wake was awake",
         "    now = hal_time_now();\n\n"
         "    /* A press that landed while this wake was awake"),
        ("    maybe_wait_for_event();\n    enter_deep_sleep(lock_gate_sleep_mode());\n}",
         "    if (wake_flow_maybe_start_break(now)) {\n"
         "        enter_deep_sleep(lock_gate_sleep_mode());\n"
         "    }\n"
         "    maybe_wait_for_event();\n    enter_deep_sleep(lock_gate_sleep_mode());\n}")],
    # ---- ROW 11: the MQTT phase is released only after the paint (f96962d) ----
    "row11_stats_posted_before_the_render": [
        ("    render_action_result(btn, before, now, selection_changed);\n"
         "    bool force_full = (btn == BTN_D);",
         "    wake_flow_post_stats_snapshot();\n"
         "    render_action_result(btn, before, now, selection_changed);\n"
         "    bool force_full = (btn == BTN_D);"),
        ("    wake_flow_post_stats_snapshot();\n    timer_state_t painted = timer_get_state();",
         "    timer_state_t painted = timer_get_state();")],
    "row11_stats_posted_after_the_join": [
        ("    wake_flow_post_stats_snapshot();\n"
         "    timer_state_t painted = timer_get_state();\n"
         "    net_finish_t nf = net_apply_finish();",
         "    timer_state_t painted = timer_get_state();\n"
         "    net_finish_t nf = net_apply_finish();\n"
         "    wake_flow_post_stats_snapshot();")],
    "row11_stats_never_posted": [
        ("    wake_flow_post_stats_snapshot();\n    timer_state_t painted = timer_get_state();",
         "    timer_state_t painted = timer_get_state();")],
    "row11_break_screen_stats_after_the_join": [
        ("        wake_flow_post_stats_snapshot(); /* break screen painted: release MQTT */\n"
         "        net_apply_finish();              /* drain + apply deferred before sleeping */",
         "        net_apply_finish();              /* drain + apply deferred before sleeping */\n"
         "        wake_flow_post_stats_snapshot(); /* break screen painted: release MQTT */")],
    "row11_window_guard_dropped": [
        ("    if (!net_window_active())\n        return;", "    ;")],
    "row11_collect_after_post": [
        ("    stats_snapshot_t snap;\n    stats_collect(&snap);\n    net_window_post_snapshot(&snap);",
         "    stats_snapshot_t snap;\n    net_window_post_snapshot(&snap);\n    stats_collect(&snap);")],
    # ---- the post-action render ----
    "render_break_drain_after_the_tick": [
        ("    wake_flow_break_end();\n    int32_t remaining = timer_tick(now);",
         "    int32_t remaining = timer_tick(now);\n    wake_flow_break_end();")],
    "render_force_full_dropped": [("    bool force_full = (btn == BTN_D);\n"
                                   "    wake_render_t bwr =", "    bool force_full = false;\n"
                                   "    wake_render_t bwr =")],
    "render_force_full_on_c_instead": [
        ("    bool force_full = (btn == BTN_D);\n    wake_render_t bwr =",
         "    bool force_full = (btn == BTN_C);\n    wake_render_t bwr =")],
    "render_button_wake_flag_flipped": [
        ("        wake_policy_render(before, timer_get_state(), true, wake_flow_break_ended_this_wake(),"
         " selection_changed);",
         "        wake_policy_render(before, timer_get_state(), false, wake_flow_break_ended_this_wake(),"
         " selection_changed);")],
    "render_before_replaced_by_the_live_state": [
        ("        wake_policy_render(before, timer_get_state(), true, wake_flow_break_ended_this_wake(),"
         " selection_changed);",
         "        wake_policy_render(timer_get_state(), timer_get_state(), true,"
         " wake_flow_break_ended_this_wake(), selection_changed);")],
    "render_selection_change_dropped": [
        ("        wake_policy_render(before, timer_get_state(), true, wake_flow_break_ended_this_wake(),"
         " selection_changed);",
         "        wake_policy_render(before, timer_get_state(), true, wake_flow_break_ended_this_wake(),"
         " false);")],
    "render_led_after_the_flush": [
        ("        status_led_show_timer_state(); /* resulting state, lit until sleep */\n"
         "        if (force_full || bwr == WAKE_RENDER_FULL) {\n"
         "            display_full_refresh(&st);\n"
         "        } else {\n"
         "            display_update(&st); /* partial cadence: every Nth is promoted */\n"
         "        }",
         "        if (force_full || bwr == WAKE_RENDER_FULL) {\n"
         "            display_full_refresh(&st);\n"
         "        } else {\n"
         "            display_update(&st); /* partial cadence: every Nth is promoted */\n"
         "        }\n"
         "        status_led_show_timer_state(); /* resulting state, lit until sleep */")],
    # ---- the post-join re-render ----
    "rerender_alerted_guard_dropped": [
        ("    if (nf != NET_FINISH_ALERTED && (nf == NET_FINISH_CHANGED || timer_get_state() != painted)) {",
         "    if (nf == NET_FINISH_CHANGED || timer_get_state() != painted) {")],
    "rerender_state_change_half_dropped": [
        ("    if (nf != NET_FINISH_ALERTED && (nf == NET_FINISH_CHANGED || timer_get_state() != painted)) {",
         "    if (nf != NET_FINISH_ALERTED && nf == NET_FINISH_CHANGED) {")],
    "rerender_changed_half_dropped": [
        ("    if (nf != NET_FINISH_ALERTED && (nf == NET_FINISH_CHANGED || timer_get_state() != painted)) {",
         "    if (nf != NET_FINISH_ALERTED && timer_get_state() != painted) {")],
    "rerender_painted_sampled_after_the_join": [
        ("    timer_state_t painted = timer_get_state();\n    net_finish_t nf = net_apply_finish();",
         "    net_finish_t nf = net_apply_finish();\n    timer_state_t painted = timer_get_state();")],
    "rerender_reuses_the_pre_join_clock": [
        ("        time_t rnow = hal_time_now();", "        time_t rnow = now;")],
    "rerender_break_drain_after_its_tick": [
        ("        wake_flow_break_end();\n        int32_t rrem = timer_tick(rnow);",
         "        int32_t rrem = timer_tick(rnow);\n        wake_flow_break_end();")],
    "rerender_reports_a_selection_change": [
        ("            wake_policy_render(painted, timer_get_state(), true, wake_flow_break_ended_this_wake(),"
         " false);",
         "            wake_policy_render(painted, timer_get_state(), true, wake_flow_break_ended_this_wake(),"
         " true);")],
    "rerender_force_full_dropped": [
            ("            if (force_full || rwr == WAKE_RENDER_FULL) {",
             "            if (rwr == WAKE_RENDER_FULL) {")],
    # ---- finish_or_break ----
    "tail_break_gate_after_the_render": [
        ("    if (wake_flow_maybe_start_break(now)) {\n"
         "        wake_flow_post_stats_snapshot(); /* break screen painted: release MQTT */\n"
         "        net_apply_finish();              /* drain + apply deferred before sleeping */\n"
         "        enter_deep_sleep(lock_gate_sleep_mode());\n"
         "    }\n"
         "    finish_action_and_render(btn, before, now, selection_changed);",
         "    finish_action_and_render(btn, before, now, selection_changed);\n"
         "    if (wake_flow_maybe_start_break(now)) {\n"
         "        wake_flow_post_stats_snapshot(); /* break screen painted: release MQTT */\n"
         "        net_apply_finish();              /* drain + apply deferred before sleeping */\n"
         "        enter_deep_sleep(lock_gate_sleep_mode());\n"
         "    }")],
    "tail_break_gate_reads_the_live_clock": [
        ("    if (wake_flow_maybe_start_break(now)) {\n"
         "        wake_flow_post_stats_snapshot(); /* break screen painted: release MQTT */",
         "    if (wake_flow_maybe_start_break(hal_time_now())) {\n"
         "        wake_flow_post_stats_snapshot(); /* break screen painted: release MQTT */")],
    "tail_break_gate_deleted": [
        ("    if (wake_flow_maybe_start_break(now)) {\n"
         "        wake_flow_post_stats_snapshot(); /* break screen painted: release MQTT */\n"
         "        net_apply_finish();              /* drain + apply deferred before sleeping */\n"
         "        enter_deep_sleep(lock_gate_sleep_mode());\n"
         "    }",
         "    ;")],
    # ---- the pre-sleep event watch ----
    "watch_branch_inverted": [
        ("    if (timer_get_state() == TIMER_RUNNING) {", "    if (timer_get_state() != TIMER_RUNNING) {")],
    "watch_guaranteed_drain_deleted": [
        ("    wake_flow_break_end_repaint();\n}", "    ;\n}")],
    "watch_guaranteed_drain_before_the_watches": [
        ("    if (timer_get_state() == TIMER_RUNNING) {",
         "    wake_flow_break_end_repaint();\n    if (timer_get_state() == TIMER_RUNNING) {"),
        ("    wake_flow_break_end_repaint();\n}", "    ;\n}")],
    "watch_final_minute_for_break_too": [
        ("        wake_flow_watch_break_end(); /* a no-op unless a break ends inside the window */",
         "        wake_flow_watch_final_minute();")],
    # ---- the tick handler's ordering and its latch drain ----
    "tick_rollover_after_the_bedtime_gate": [
        ("    wake_flow_handle_day_rollover(&now);\n    lock_gate_check_bedtime(now);",
         "    lock_gate_check_bedtime(now);\n    wake_flow_handle_day_rollover(&now);")],
    "tick_break_drain_before_the_bedtime_gate": [
        ("    lock_gate_check_bedtime(now); /* may not return; before the sync block so a",
         "    wake_flow_break_end();\n    lock_gate_check_bedtime(now); /* may not return; before the sync block so a"),
        ("       break-ended promotion is what forces the full refresh. */\n    wake_flow_break_end();",
         "       break-ended promotion is what forces the full refresh. */\n    ;")],
    "tick_sync_flag_inverted": [
        ("wake_flow_dispatch_button_action((button_id_t)pick, &now, painted, !synced_this_wake, &swapped)",
         "wake_flow_dispatch_button_action((button_id_t)pick, &now, painted, synced_this_wake, &swapped)")],
    "tick_sync_never_re_reads_the_clock": [
        ("        net_apply_try_window();\n        now = hal_time_now();\n        synced_this_wake = true;",
         "        net_apply_try_window();\n        synced_this_wake = true;")],
    "tick_pick_mask_gains_d": [
        ("    int pick = button_latch_pick(buttons_take_pressed(), (1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C));",
         "    int pick = button_latch_pick(buttons_take_pressed(),\n"
         "                                 (1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C) | (1u << BTN_D));")],
    "tick_pick_mask_loses_b": [
        ("(1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C));", "(1u << BTN_A) | (1u << BTN_C));")],
    "tick_pick_boundary_minus1": [("    if (pick >= 0) {", "    if (pick > 0) {")],
    "tick_latch_drain_after_the_event_watch": [
        ("    int pick = button_latch_pick(buttons_take_pressed(), (1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C));\n"
         "    if (pick >= 0) {\n"
         "        timer_state_t painted = timer_get_state();\n"
         "        bool swapped = false;\n"
         "        if (wake_flow_dispatch_button_action((button_id_t)pick, &now, painted, !synced_this_wake,"
         " &swapped)) {\n"
         "            status_led_show_timer_state();\n"
         "            finish_or_break((button_id_t)pick, painted, now, swapped);\n"
         "        }\n"
         "    }\n"
         "    maybe_wait_for_event();",
         "    maybe_wait_for_event();\n"
         "    int pick = button_latch_pick(buttons_take_pressed(), (1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C));\n"
         "    if (pick >= 0) {\n"
         "        timer_state_t painted = timer_get_state();\n"
         "        bool swapped = false;\n"
         "        if (wake_flow_dispatch_button_action((button_id_t)pick, &now, painted, !synced_this_wake,"
         " &swapped)) {\n"
         "            status_led_show_timer_state();\n"
         "            finish_or_break((button_id_t)pick, painted, now, swapped);\n"
         "        }\n"
         "    }")],
    "tick_dispatch_result_ignored": [
        ("        if (wake_flow_dispatch_button_action((button_id_t)pick, &now, painted, !synced_this_wake,"
         " &swapped)) {\n"
         "            status_led_show_timer_state();\n"
         "            finish_or_break((button_id_t)pick, painted, now, swapped);\n"
         "        }",
         "        wake_flow_dispatch_button_action((button_id_t)pick, &now, painted, !synced_this_wake, &swapped);\n"
         "        status_led_show_timer_state();\n"
         "        finish_or_break((button_id_t)pick, painted, now, swapped);")],
    # ---- the snap, and its two guards ----
    "tick_snap_guard_dropped": [
        ("    int32_t shown = remaining;\n"
         "    if (timer_get_state() == TIMER_RUNNING) {\n"
         "        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC);\n"
         "    }",
         "    int32_t shown = wake_policy_snap_minute(remaining, SLEEP_PLAN_WATCH_SEC);")],
    "tick_snap_never_applied": [
        ("    int32_t shown = remaining;\n"
         "    if (timer_get_state() == TIMER_RUNNING) {\n"
         "        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC);\n"
         "    }",
         "    int32_t shown = remaining;")],
    # +-1 on EITHER snap threshold is an equivalent mutant, and provably so
    # rather than merely unobserved: wake_policy_snap_minute only alters a
    # value whose second-of-minute residue is <= 2 or >= 58, so a threshold
    # shift is visible only if some v with T_old < v <= T_new has such a
    # residue. SLEEP_PLAN_WATCH_SEC is 75, and the only candidates are
    # v = 75 (residue 15) and v = 76 (residue 16); neither snaps, for ANY
    # value the tick can return. So the shipped perturbations widen the
    # threshold past a value that does snap, and the +-1 lands on the
    # operand instead, where it is reachable.
    "tick_snap_threshold_widened": [
        ("        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC);",
         "        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC * 100);")],
    "tick_snap_operand_plus1": [
        ("        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC);",
         "        shown = wake_policy_snap_minute(shown + 1, SLEEP_PLAN_WATCH_SEC);")],
    "tick_snap_operand_minus1": [
        ("        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC);",
         "        shown = wake_policy_snap_minute(shown - 1, SLEEP_PLAN_WATCH_SEC);")],
    "tick_break_snap_guard_loses_the_banner": [
        ("    if (st.timer_state == TIMER_BREAK || st.break_banner) {",
         "    if (st.timer_state == TIMER_BREAK) {")],
    "tick_break_snap_guard_loses_the_screen": [
        ("    if (st.timer_state == TIMER_BREAK || st.break_banner) {", "    if (st.break_banner) {")],
    "tick_break_snap_threshold_widened": [
        ("        st.break_remaining_sec = wake_policy_snap_minute(st.break_remaining_sec, SLEEP_PLAN_WATCH_SEC);",
         "        st.break_remaining_sec = wake_policy_snap_minute(st.break_remaining_sec,"
         " SLEEP_PLAN_WATCH_SEC * 100);")],
    "tick_break_snap_operand_plus1": [
        ("        st.break_remaining_sec = wake_policy_snap_minute(st.break_remaining_sec, SLEEP_PLAN_WATCH_SEC);",
         "        st.break_remaining_sec = wake_policy_snap_minute(st.break_remaining_sec + 1,"
         " SLEEP_PLAN_WATCH_SEC);")],
    "tick_state_assembled_before_the_snap": [
        ("    int32_t shown = remaining;\n"
         "    if (timer_get_state() == TIMER_RUNNING) {\n"
         "        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC);\n"
         "    }\n"
         "    display_state_t st = make_display_state(shown, now);",
         "    display_state_t st = make_display_state(remaining, now);\n"
         "    int32_t shown = remaining;\n"
         "    if (timer_get_state() == TIMER_RUNNING) {\n"
         "        shown = wake_policy_snap_minute(shown, SLEEP_PLAN_WATCH_SEC);\n"
         "    }\n"
         "    (void)shown;")],
    # ---- the tick handler's render choice ----
    "tick_render_promotion_dropped": [
        ("    wr = lock_gate_promote_render(wr); /* a lock released this wake owes the panel a full one */",
         "    ;")],
    "tick_render_button_wake_flag_flipped": [
        ("    wake_render_t wr = wake_policy_render(before, timer_get_state(), false,"
         " wake_flow_break_ended_this_wake(), false);",
         "    wake_render_t wr = wake_policy_render(before, timer_get_state(), true,"
         " wake_flow_break_ended_this_wake(), false);")],
    "tick_render_break_ended_flag_dropped": [
        ("    wake_render_t wr = wake_policy_render(before, timer_get_state(), false,"
         " wake_flow_break_ended_this_wake(), false);",
         "    wake_render_t wr = wake_policy_render(before, timer_get_state(), false, false, false);")],
    "tick_full_and_partial_swapped": [
        ("        case WAKE_RENDER_FULL:\n            display_full_refresh(&st);\n            break;\n"
         "        default:\n            display_update(&st); /* partial; policy promotes every 5th to full */\n"
         "            break;",
         "        case WAKE_RENDER_FULL:\n            display_update(&st);\n            break;\n"
         "        default:\n            display_full_refresh(&st);\n            break;")],
    # ---- the button handler ----
    "button_ack_after_the_rollover": [
        ("    status_led_show_timer_state();\n\n    time_t now = hal_time_now();\n"
         "    wake_flow_handle_day_rollover(&now);",
         "    time_t now = hal_time_now();\n    wake_flow_handle_day_rollover(&now);\n"
         "    status_led_show_timer_state();")],
    "button_before_captured_before_the_drain": [
        ("    wake_flow_break_end();\n    timer_state_t before = timer_get_state();",
         "    timer_state_t before = timer_get_state();\n    wake_flow_break_end();")],
    "button_release_drain_deleted": [
        ("    buttons_take_pressed();\n\n    finish_or_break(btn, before, now, swapped);",
         "    finish_or_break(btn, before, now, swapped);")],
    "button_release_drain_after_the_tail": [
        ("    buttons_take_pressed();\n\n    finish_or_break(btn, before, now, swapped);",
         "    finish_or_break(btn, before, now, swapped);\n    buttons_take_pressed();")],
    "button_d_falls_into_the_dispatch": [
        ("        case BTN_D:\n"
         "            /* NTP-gated paint, same as BTN A: sync now, MQTT after paint */\n"
         "            if (net_apply_open()) {\n"
         "                net_window_wait_ntp();\n"
         "            }\n"
         "            now = hal_time_now();\n"
         "            break;",
         "        case BTN_D:\n"
         "            wake_flow_dispatch_button_action(btn, &now, before, true, &swapped);\n"
         "            break;")],
    "button_d_never_re_reads_the_clock": [
        ("            if (net_apply_open()) {\n"
         "                net_window_wait_ntp();\n"
         "            }\n"
         "            now = hal_time_now();\n"
         "            break;",
         "            if (net_apply_open()) {\n"
         "                net_window_wait_ntp();\n"
         "            }\n"
         "            break;")],
    "button_d_window_guard_dropped": [
        ("            if (net_apply_open()) {\n                net_window_wait_ntp();\n            }",
         "            net_apply_open();\n            net_window_wait_ntp();")],
    "button_dispatch_denied_its_window": [
        ("            wake_flow_dispatch_button_action(btn, &now, before, true, &swapped);",
         "            wake_flow_dispatch_button_action(btn, &now, before, false, &swapped);")],
    "button_tail_skipped_when_the_dispatch_refused": [
        ("    finish_or_break(btn, before, now, swapped); /* e.g. resume with accrual already past the interval */",
         "    if (swapped) {\n"
         "        finish_or_break(btn, before, now, swapped);\n    }")],
    "button_event_watch_deleted": [
        ("    finish_or_break(btn, before, now, swapped); /* e.g. resume with accrual already past the interval */\n"
         "    maybe_wait_for_event();",
         "    finish_or_break(btn, before, now, swapped); /* e.g. resume with accrual already past the interval */")],
}


def apply_mutant(bodies, seamsrc, edits):
    """Apply each edit to whichever of the two NEW-side sources holds it.
    Missing anchors are fatal: a mutant that silently did nothing would
    read as an equivalence."""
    for a, b in edits:
        if a in bodies:
            bodies = bodies.replace(a, b, 1)
        elif a in seamsrc:
            seamsrc = seamsrc.replace(a, b, 1)
        else:
            sys.exit(f"mutant anchor not found:\n{a}")
    return bodies, seamsrc


PRE = r"""
/* Differential sweep harness - generated, never committed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>

#include "app_state.h"
#include "buttons.h"
#include "button_latch.h"
#include "display.h"
#include "hal_time.h"
#include "lock_gate.h"
#include "net_apply.h"
#include "net_window.h"
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "sleep_plan.h"
#include "stats_json.h"
#include "status_led.h"
#include "timer.h"
#include "wake_flow.h"
#include "wake_policy.h"

/* The real policies, not models: the render choice, the minute snap, the
   NTP cadence and the press priority are all decisions both sides delegate
   to these, and a stub would be a second implementation to keep in sync. */
#include "wake_policy.c"
#include "button_latch.c"

/* Both sides get log macros that EVALUATE their arguments, exactly as the
   real ESP_LOG* do on device. wake_flow.c's NATIVE block discards them, so
   anything smuggled into a log vararg is invisible to the committed host
   suite - here it is not. This cycle MOVES one such call site: the
   post-action render's "state %d -> %d" line calls timer_get_state()
   inside its argument list. That call runs on device and is invisible to
   every host test; here it lands in the trace on both sides. */
static void diff_logv(const char *tag, const char *fmt, ...);
#define ESP_LOGI(tag, ...) diff_logv(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) diff_logv(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) diff_logv(tag, __VA_ARGS__)
static const char *TAG = "diff";

/* The extracted state-assembly seam reads it; wake_flow.c derives it from
   the Kconfig bool and only the function body is extracted. false is the
   shipping configuration, and both sides reach the seam, so the value is
   the same on either side of the comparison whatever it is. */
#define PARENT_TESTING false

/* The sync cadence macro. Device-side it comes from sdkconfig.h; both
   sides read the same name, so one definition here keeps the comparison
   about the refactor rather than about a build setting. */
#undef IDLE_SYNC_INTERVAL_SEC
#define IDLE_SYNC_INTERVAL_SEC (60 * 60)

/* ---- the ordered effect trace ------------------------------------------ */
enum {
    E_CLOCK = 1, E_DELAY, E_GET_STATE, E_EXPIRY_WALL, E_BREAK_REM, E_BREAK_ACTIVE,
    E_EXTRA_RUNNING, E_NEEDS_SYNC, E_LAST_SYNC, E_TRY_WINDOW, E_BREAK_DUE,
    E_MAYBE_BREAK, E_EXPIRY_ALERT, E_BREAK_END, E_BREAK_ENDED_FLAG, E_BREAK_REPAINT,
    E_ROLLOVER, E_BEDTIME, E_PROMOTE, E_SLEEP_MODE, E_SLEEP,
    E_LED, E_PARTIAL, E_FULL_REFRESH, E_TICK, E_TAKE_PRESSED, E_TAKE_MASKED,
    E_WAKEUP_BTN, E_RESET_REASON, E_DISPATCH, E_GRID_WAIT, E_WATCH_FINAL,
    E_WATCH_BREAK, E_NET_OPEN, E_WAIT_NTP, E_NET_FINISH, E_WIN_ACTIVE,
    E_STATS_COLLECT, E_STATS_POST, E_BATT, E_MAKE_STATE, E_LOGLINE,
};

#define TRACE_MAX 8192
typedef struct { int ev; long long a, b, c; } tev_t;
static tev_t g_tr[TRACE_MAX];
static int g_tr_n;
static long g_overflow; /* a truncated trace could hide a divergence */
static void tr(int ev, long long a, long long b, long long c) {
    if (g_tr_n < TRACE_MAX) { g_tr[g_tr_n].ev = ev; g_tr[g_tr_n].a = a;
        g_tr[g_tr_n].b = b; g_tr[g_tr_n].c = c; g_tr_n++; }
    else { g_overflow++; }
}
static void diff_logv(const char *tag, const char *fmt, ...) {
    /* Evaluates the varargs (that is the point) but records only that a
       line happened - the log TEXT is not part of the device contract we
       are pinning, the CALLS inside the argument list are. */
    va_list ap; va_start(ap, fmt); (void)tag; (void)fmt; va_end(ap);
    tr(E_LOGLINE, 0, 0, 0);
}

/* ---- the fake clock ----------------------------------------------------- */
/* Reads ADVANCE it (so an extra or a missing read is visible even when
   nothing else moved), and hal_delay_ms advances it exactly the way the
   committed host mock does - carrying the sub-second remainder. */
static time_t g_clock;
static int g_clock_step;
static uint32_t g_delay_carry;
time_t hal_time_now(void) {
    g_clock += g_clock_step;
    tr(E_CLOCK, (long long)g_clock, 0, 0);
    return g_clock;
}
void hal_delay_ms(uint32_t ms) {
    tr(E_DELAY, (long long)ms, 0, 0);
    g_delay_carry += ms;
    g_clock += (time_t)(g_delay_carry / 1000u);
    g_delay_carry %= 1000u;
}

/* ---- the held-through-sleep RTC pair ------------------------------------
   ONE definition, read by BOTH sides. These arrive from RTC memory, not
   from a caller, which is precisely what makes row 7 an axis this sweep
   can see: the guard's operands survive a sleep the harness never runs. */
static uint8_t s_held_mask_at_sleep;
static int64_t s_sleep_entry_time;

/* ---- the injected model -------------------------------------------------- */
typedef struct {
    int state;                /* timer_state_t */
    int state_after_tick;     /* -1 = the tick leaves the state alone */
    int state_after_grid;     /* -1 = the grid wait leaves the state alone */
    long long expiry_off;     /* expiry wall, relative to the clock seed */
    int break_active, extra_running;
    long long break_off;
    int needs_sync;
    long long last_sync_off;  /* -1 = never synced */
    int reset_reason;
    int wake_btn;             /* button_id_t from the EXT1 decode */
    unsigned held_mask;       /* RTC: what was held at sleep entry */
    long long sleep_gap;      /* RTC: seconds between sleep entry and this wake */
    int rollover;             /* the day rollover fires */
    int win_active;           /* net_window_active() */
    int net_open, ntp_ok;
    int net_finish;           /* net_finish_t */
    int state_after_finish;   /* -1 = the join leaves the state alone */
    long long due_from_off;   /* -1 = never due */
    int break_starts;
    int dispatch_ok;          /* the guard matrix accepted the press */
    int dispatch_swapped;
    int dispatch_state;       /* -1 = the dispatch leaves the state alone */
    int break_end_drains;     /* wake_flow_break_end() finds an edge */
    int break_ended_flag;
    int promote;              /* -1 = pass through */
    int latched;              /* the ISR latch contents at the drain */
    int made_kind;            /* display_state_t.timer_state from the seam */
    int made_banner;
    int made_break_rem;
    long tick_ret;
    int batt_mv;
    int clock_step;
    int sleep_mode;
    time_t clock_seed;
} params_t;
static params_t P;

static int m_state;
static int64_t m_expiry_wall;
static int m_break_active, m_extra_running;
static time_t m_break_wall_end, m_due_from;
static int m_latched;
static jmp_buf g_sleep_jmp;

static void reset_run(void) {
    g_clock = P.clock_seed;
    g_clock_step = P.clock_step;
    g_delay_carry = 0;
    g_tr_n = 0;
    m_state = P.state;
    m_expiry_wall = (int64_t)P.clock_seed + P.expiry_off;
    m_break_active = P.break_active;
    m_extra_running = P.extra_running;
    m_break_wall_end = P.clock_seed + (time_t)P.break_off;
    m_due_from = (P.due_from_off < 0) ? 0 : P.clock_seed + (time_t)P.due_from_off;
    m_latched = P.latched;
    s_held_mask_at_sleep = (uint8_t)P.held_mask;
    s_sleep_entry_time = (int64_t)P.clock_seed - P.sleep_gap;
}

timer_state_t timer_get_state(void) {
    tr(E_GET_STATE, m_state, 0, 0);
    return (timer_state_t)m_state;
}
int64_t timer_expiry_wall(void) { tr(E_EXPIRY_WALL, (long long)m_expiry_wall, 0, 0); return m_expiry_wall; }
int32_t timer_break_remaining(time_t now) {
    int32_t r = 0;
    if (m_break_active) {
        int64_t left = (int64_t)m_break_wall_end - (int64_t)now;
        r = left > 0 ? (int32_t)left : 0;
    }
    tr(E_BREAK_REM, (long long)now, r, 0);
    return r;
}
bool timer_break_active(void) { tr(E_BREAK_ACTIVE, m_break_active, 0, 0); return m_break_active != 0; }
bool timer_any_extra_running(void) { tr(E_EXTRA_RUNNING, m_extra_running, 0, 0); return m_extra_running != 0; }
bool timer_needs_ntp_sync(time_t now) { tr(E_NEEDS_SYNC, (long long)now, P.needs_sync, 0); return P.needs_sync != 0; }
time_t timer_last_ntp_sync(void) {
    time_t v = (P.last_sync_off < 0) ? (time_t)0 : P.clock_seed + (time_t)P.last_sync_off;
    tr(E_LAST_SYNC, (long long)v, 0, 0);
    return v;
}
esp_err_t net_apply_try_window(void) { tr(E_TRY_WINDOW, 0, 0, 0); return ESP_OK; }
bool timer_break_due(time_t now, int32_t interval_sec) {
    int due = (m_due_from != 0 && now >= m_due_from);
    tr(E_BREAK_DUE, (long long)now, (long long)interval_sec, due);
    return due != 0;
}
bool wake_flow_maybe_start_break(time_t now) {
    tr(E_MAYBE_BREAK, (long long)now, P.break_starts, 0);
    return P.break_starts != 0;
}
void wake_flow_fire_expiry_alert(void) {
    tr(E_EXPIRY_ALERT, 0, 0, 0);
    /* the alarm holds the CPU for ~15 s on device: the post-render break
       gate re-reads the clock precisely because of it */
    g_clock += 15;
}
bool wake_flow_break_end(void) { tr(E_BREAK_END, P.break_end_drains, 0, 0); return P.break_end_drains != 0; }
bool wake_flow_break_ended_this_wake(void) { tr(E_BREAK_ENDED_FLAG, P.break_ended_flag, 0, 0); return P.break_ended_flag != 0; }
bool wake_flow_break_end_repaint(void) { tr(E_BREAK_REPAINT, 0, 0, 0); return true; }
void wake_flow_handle_day_rollover(time_t *now) {
    tr(E_ROLLOVER, (long long)*now, P.rollover, 0);
    if (P.rollover) *now = *now + 30; /* the rollover's own window corrects the clock */
}
void lock_gate_check_bedtime(time_t now) { tr(E_BEDTIME, (long long)now, 0, 0); }
wake_render_t lock_gate_promote_render(wake_render_t wr) {
    tr(E_PROMOTE, wr, P.promote, 0);
    return P.promote < 0 ? wr : (wake_render_t)P.promote;
}
wake_sleep_mode_t lock_gate_sleep_mode(void) { tr(E_SLEEP_MODE, P.sleep_mode, 0, 0); return (wake_sleep_mode_t)P.sleep_mode; }
void enter_deep_sleep(wake_sleep_mode_t mode) { tr(E_SLEEP, mode, 0, 0); longjmp(g_sleep_jmp, 1); }
/* The grid wait polls the pause button on device, so a press inside it
   moves the timer between `before` and the render's own state read. That
   is the ONLY way the two can differ by something other than an EXPIRED
   (which the alert arm swallows first), and therefore the only way the
   render's button_wake argument is observable at all. */
void wake_flow_wait_for_render_grid(int max_wait_sec) {
    tr(E_GRID_WAIT, max_wait_sec, 0, 0);
    if (P.state_after_grid >= 0) m_state = P.state_after_grid;
}
void wake_flow_watch_final_minute(void) { tr(E_WATCH_FINAL, 0, 0, 0); }
void wake_flow_watch_break_end(void) { tr(E_WATCH_BREAK, 0, 0, 0); }
bool wake_flow_dispatch_button_action(button_id_t btn, time_t *now, timer_state_t before, bool allow_net_window,
                                      bool *selection_changed) {
    tr(E_DISPATCH, btn, before, allow_net_window);
    *selection_changed = P.dispatch_swapped != 0;
    if (P.dispatch_state >= 0) m_state = P.dispatch_state;
    *now = *now + 1; /* the action leaves a clock behind (the 250 ms ack hold) */
    return P.dispatch_ok != 0;
}
bool net_apply_open(void) { tr(E_NET_OPEN, P.net_open, 0, 0); return P.net_open != 0; }
bool net_window_wait_ntp(void) { tr(E_WAIT_NTP, P.ntp_ok, 0, 0); return P.ntp_ok != 0; }
net_finish_t net_apply_finish(void) {
    tr(E_NET_FINISH, P.net_finish, 0, 0);
    if (P.state_after_finish >= 0) m_state = P.state_after_finish;
    return (net_finish_t)P.net_finish;
}
bool net_window_active(void) { tr(E_WIN_ACTIVE, P.win_active, 0, 0); return P.win_active != 0; }
void stats_collect(stats_snapshot_t *out) { tr(E_STATS_COLLECT, 0, 0, 0); memset(out, 0, sizeof *out); }
void net_window_post_snapshot(const stats_snapshot_t *snap) { (void)snap; tr(E_STATS_POST, 0, 0, 0); }
void status_led_show_timer_state(void) { tr(E_LED, 0, 0, 0); }
void display_update(const display_state_t *st) {
    tr(E_PARTIAL, (long long)st->remaining_sec, (long long)st->wall_time, (long long)st->break_remaining_sec);
}
void display_full_refresh(const display_state_t *st) {
    tr(E_FULL_REFRESH, (long long)st->remaining_sec, (long long)st->wall_time, (long long)st->break_remaining_sec);
}
int32_t timer_tick(time_t now) {
    tr(E_TICK, (long long)now, 0, 0);
    if (P.state_after_tick >= 0) m_state = P.state_after_tick;
    return (int32_t)P.tick_ret;
}
uint8_t buttons_take_pressed(void) {
    int v = m_latched; m_latched = 0;
    tr(E_TAKE_PRESSED, v, 0, 0);
    return (uint8_t)v;
}
uint8_t buttons_take_pressed_mask(uint8_t mask) {
    int v = m_latched & mask; m_latched &= (int)(unsigned)~mask;
    tr(E_TAKE_MASKED, mask, v, 0);
    return (uint8_t)v;
}
button_id_t buttons_get_wakeup_button(void) { tr(E_WAKEUP_BTN, P.wake_btn, 0, 0); return (button_id_t)P.wake_btn; }
esp_reset_reason_t esp_reset_reason(void) { tr(E_RESET_REASON, P.reset_reason, 0, 0); return (esp_reset_reason_t)P.reset_reason; }
int battery_read_mv(void) { tr(E_BATT, P.batt_mv, 0, 0); return P.batt_mv; }
int battery_percent_from_mv(int mv) { return mv / 40; }

/* app_state.c's assembly, modelled. Both sides now reach it through the
   SHIPPING make_display_state, extracted from wake_flow.c below - the ADC
   read and the log line are in that extracted body, so what is left to
   model here is the assembly itself. The three break fields are what the
   tick handler SNAPS after assembly, so they are part of what the paint
   stubs report. `in` is read so a seam that stopped filling it would not
   pass silently.

   There used to be a hand-written make_state here, back when the seam was
   a main.c thunk over it. It is gone: with the real body compiled in, a
   model of it would be a second implementation of the same three lines. */
display_state_t app_state_display(const app_state_in_t *in, int32_t remaining, time_t now) {
    display_state_t st;
    memset(&st, 0, sizeof st);
    st.remaining_sec = remaining;
    st.wall_time = now;
    st.timer_state = (timer_state_t)P.made_kind;
    st.break_banner = P.made_banner != 0;
    st.break_remaining_sec = P.made_break_rem;
    (void)in->batt_mv;
    (void)in->parent_testing;
    tr(E_MAKE_STATE, (long long)remaining, (long long)now, 0);
    return st;
}
"""

DRIVER = r"""
/* ---- the driver -------------------------------------------------------- */

#define DAY_BASE 1785283200 /* 2026-07-29 00:00:00 UTC, exactly on a minute */

typedef struct { tev_t *ev; int n; int cap; } run_t;
static run_t o, n;

static void ensure(run_t *r) {
    if (r->cap == 0) { r->cap = TRACE_MAX; r->ev = malloc(sizeof(tev_t) * (size_t)r->cap); }
}
static void snap(run_t *r) {
    ensure(r);
    r->n = g_tr_n;
    memcpy(r->ev, g_tr, sizeof(tev_t) * (size_t)g_tr_n);
}

static int same(const run_t *a, const run_t *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) {
        if (a->ev[i].ev != b->ev[i].ev || a->ev[i].a != b->ev[i].a ||
            a->ev[i].b != b->ev[i].b || a->ev[i].c != b->ev[i].c) return 0;
    }
    return 1;
}

static long g_cases, g_diverge;
static char g_first[512];

static void report(const char *what) {
    g_diverge++;
    if (g_first[0]) return;
    int k = -1;
    int lim = o.n < n.n ? o.n : n.n;
    for (int i = 0; i < lim; i++)
        if (o.ev[i].ev != n.ev[i].ev || o.ev[i].a != n.ev[i].a ||
            o.ev[i].b != n.ev[i].b || o.ev[i].c != n.ev[i].c) { k = i; break; }
    if (k < 0) k = lim;
    snprintf(g_first, sizeof g_first,
             "%s: n_old=%d n_new=%d first_diff@%d old=(%d,%lld,%lld,%lld) new=(%d,%lld,%lld,%lld)",
             what, o.n, n.n, k,
             k < o.n ? o.ev[k].ev : -1, k < o.n ? o.ev[k].a : -1,
             k < o.n ? o.ev[k].b : -1, k < o.n ? o.ev[k].c : -1,
             k < n.n ? n.ev[k].ev : -1, k < n.n ? n.ev[k].a : -1,
             k < n.n ? n.ev[k].b : -1, k < n.n ? n.ev[k].c : -1);
}

static void defaults(void) {
    memset(&P, 0, sizeof P);
    P.state = TIMER_IDLE;
    P.state_after_tick = -1;
    P.state_after_grid = -1;
    P.state_after_finish = -1;
    P.dispatch_state = -1;
    P.due_from_off = -1;
    P.last_sync_off = -1;
    P.promote = -1;
    P.reset_reason = ESP_RST_DEEPSLEEP;
    P.wake_btn = BTN_NONE;
    P.net_finish = NET_FINISH_IDLE;
    P.tick_ret = 777;
    P.batt_mv = 3900;
    P.made_kind = TIMER_IDLE;
    P.clock_seed = (time_t)DAY_BASE;
    P.sleep_gap = 100;
}

/* Both handlers end in enter_deep_sleep, which longjmps. The two runners
   below are what make "the wake stopped here" a comparable trace rather
   than an abort. */
#define RUN_OLD(call) do { reset_run(); if (setjmp(g_sleep_jmp) == 0) { call; } snap(&o); } while (0)
#define RUN_NEW(call) do { reset_run(); if (setjmp(g_sleep_jmp) == 0) { call; } snap(&n); } while (0)

int main(void) {
    setenv("TZ", "UTC0", 1);
    tzset();

    static const int STATES[] = {TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_BREAK, TIMER_EXPIRED};
    static const int BTNS[] = {BTN_A, BTN_B, BTN_C, BTN_D, BTN_NONE};
    static const int STEPS[] = {0, 1};

    /* ---- post_stats_snapshot ------------------------------------------ */
    for (int wa = 0; wa < 2; wa++)
    for (unsigned ki = 0; ki < sizeof STEPS / sizeof STEPS[0]; ki++) {
        defaults();
        P.win_active = wa;
        P.clock_step = STEPS[ki];
        RUN_OLD(old_post_stats_snapshot());
        RUN_NEW(new_wake_flow_post_stats_snapshot());
        g_cases++;
        if (!same(&o, &n)) report("post_stats_snapshot");
    }

    /* ---- render_action_result ------------------------------------------ */
    static const int AFTER[] = {TIMER_IDLE, TIMER_RUNNING, TIMER_BREAK, TIMER_EXPIRED};
    for (unsigned bi = 0; bi < sizeof BTNS / sizeof BTNS[0]; bi++)
    for (unsigned si = 0; si < sizeof STATES / sizeof STATES[0]; si++)
    for (unsigned ai = 0; ai < sizeof AFTER / sizeof AFTER[0]; ai++)
    for (int sc = 0; sc < 2; sc++)
    for (int bf = 0; bf < 2; bf++)
    for (int bd = 0; bd < 2; bd++)
    for (unsigned ki = 0; ki < sizeof STEPS / sizeof STEPS[0]; ki++) {
        defaults();
        P.state = AFTER[ai];
        P.break_ended_flag = bf;
        P.break_end_drains = bd;
        P.clock_step = STEPS[ki];
        P.tick_ret = (long)(ai * 61) - 30;
        time_t at = (time_t)DAY_BASE + 17;
        RUN_OLD(old_render_action_result((button_id_t)BTNS[bi], (timer_state_t)STATES[si], at, sc != 0));
        RUN_NEW(new_render_action_result((button_id_t)BTNS[bi], (timer_state_t)STATES[si], at, sc != 0));
        g_cases++;
        if (!same(&o, &n)) report("render_action_result");
    }

    /* ---- finish_action_and_render (row 11) ----------------------------- */
    static const int FINISH[] = {NET_FINISH_IDLE, NET_FINISH_CHANGED, NET_FINISH_ALERTED};
    for (unsigned bi = 0; bi < sizeof BTNS / sizeof BTNS[0]; bi++)
    for (unsigned si = 0; si < sizeof STATES / sizeof STATES[0]; si++)
    for (unsigned ai = 0; ai < sizeof AFTER / sizeof AFTER[0]; ai++)
    for (unsigned fi = 0; fi < sizeof FINISH / sizeof FINISH[0]; fi++)
    for (int saf = -1; saf < (int)(sizeof AFTER / sizeof AFTER[0]); saf++)
    for (int wa = 0; wa < 2; wa++)
    for (int bf = 0; bf < 2; bf++)
    for (unsigned ki = 0; ki < sizeof STEPS / sizeof STEPS[0]; ki++) {
        defaults();
        P.state = AFTER[ai];
        P.net_finish = FINISH[fi];
        P.state_after_finish = (saf < 0) ? -1 : AFTER[saf];
        P.win_active = wa;
        P.break_ended_flag = bf;
        P.clock_step = STEPS[ki];
        P.tick_ret = (long)(fi * 41) - 10;
        time_t at = (time_t)DAY_BASE + 17;
        RUN_OLD(old_finish_action_and_render((button_id_t)BTNS[bi], (timer_state_t)STATES[si], at, false));
        RUN_NEW(new_finish_action_and_render((button_id_t)BTNS[bi], (timer_state_t)STATES[si], at, false));
        g_cases++;
        if (!same(&o, &n)) report("finish_action_and_render");
    }

    /* ---- finish_or_break ----------------------------------------------- */
    for (unsigned bi = 0; bi < sizeof BTNS / sizeof BTNS[0]; bi++)
    for (unsigned si = 0; si < sizeof STATES / sizeof STATES[0]; si++)
    for (int bs = 0; bs < 2; bs++)
    for (int wa = 0; wa < 2; wa++)
    for (unsigned fi = 0; fi < sizeof FINISH / sizeof FINISH[0]; fi++)
    for (int sc = 0; sc < 2; sc++)
    for (unsigned ki = 0; ki < sizeof STEPS / sizeof STEPS[0]; ki++) {
        defaults();
        P.state = STATES[si];
        P.break_starts = bs;
        P.win_active = wa;
        P.net_finish = FINISH[fi];
        P.sleep_mode = (int)(si % 3);
        P.clock_step = STEPS[ki];
        time_t at = (time_t)DAY_BASE + 17;
        RUN_OLD(old_finish_or_break((button_id_t)BTNS[bi], (timer_state_t)STATES[si], at, sc != 0));
        RUN_NEW(new_finish_or_break((button_id_t)BTNS[bi], (timer_state_t)STATES[si], at, sc != 0));
        g_cases++;
        if (!same(&o, &n)) report("finish_or_break");
    }

    /* ---- maybe_wait_for_event ------------------------------------------ */
    for (unsigned si = 0; si < sizeof STATES / sizeof STATES[0]; si++)
    for (int bd = 0; bd < 2; bd++)
    for (unsigned ki = 0; ki < sizeof STEPS / sizeof STEPS[0]; ki++) {
        defaults();
        P.state = STATES[si];
        P.break_end_drains = bd;
        P.clock_step = STEPS[ki];
        RUN_OLD(old_maybe_wait_for_event());
        RUN_NEW(new_maybe_wait_for_event());
        g_cases++;
        if (!same(&o, &n)) report("maybe_wait_for_event");
    }

    /* ---- handle_timer_tick (rows 8 and 10) ----------------------------- */
    static const int REASONS[] = {ESP_RST_DEEPSLEEP, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_PANIC};
    static const long long DUE_FROM[] = {-1, 0, 5, 20};
    static const int LATCH[] = {0, 1u << BTN_A, 1u << BTN_B, 1u << BTN_C, 1u << BTN_D,
                                (1u << BTN_A) | (1u << BTN_D), 0xF};
    static const long long LASTSYNC[] = {-1, -3600, -3599, 0};
    static const int PROMOTE[] = {-1, WAKE_RENDER_PARTIAL, WAKE_RENDER_FULL, WAKE_RENDER_EXPIRY_ALERT};
    for (unsigned si = 0; si < sizeof STATES / sizeof STATES[0]; si++)
    for (unsigned ri = 0; ri < sizeof REASONS / sizeof REASONS[0]; ri++)
    for (unsigned di = 0; di < sizeof DUE_FROM / sizeof DUE_FROM[0]; di++)
    for (unsigned li = 0; li < sizeof LATCH / sizeof LATCH[0]; li++)
    for (unsigned yi = 0; yi < sizeof LASTSYNC / sizeof LASTSYNC[0]; yi++)
    for (unsigned pi = 0; pi < sizeof PROMOTE / sizeof PROMOTE[0]; pi++)
    for (int sat = 0; sat < 2; sat++)
    for (int dok = 0; dok < 2; dok++)
    for (int bs = 0; bs < 2; bs++) {
        defaults();
        P.state = STATES[si];
        P.reset_reason = REASONS[ri];
        P.due_from_off = DUE_FROM[di];
        P.latched = LATCH[li];
        P.last_sync_off = LASTSYNC[yi];
        P.promote = PROMOTE[pi];
        P.state_after_tick = sat ? TIMER_EXPIRED : -1;
        /* A pause press inside the grid wait, which is what makes the
           before/after pair differ by something other than an expiry. */
        P.state_after_grid = (li == 1) ? TIMER_PAUSED : ((li == 2) ? TIMER_BREAK : -1);
        P.dispatch_ok = dok;
        P.break_starts = bs;
        /* Derived rather than more product dimensions: still varied, and a
           paint that carried the wrong one would show up in the trace. */
        P.needs_sync = (int)(yi & 1u);
        P.rollover = (int)(di & 1u);
        P.break_end_drains = (int)(li & 1u);
        P.break_ended_flag = (int)((li >> 1) & 1u);
        P.dispatch_swapped = (int)(dok & 1);
        P.dispatch_state = dok ? TIMER_RUNNING : -1;
        P.made_kind = (int)STATES[si];
        P.made_banner = (int)(pi & 1u);
        P.made_break_rem = (int)((const int[]){119, 75, 120, 61}[di]);
        /* Residues 59, 1, 12 and 58: three of the four are values the
           minute snap actually MOVES, which is what makes the snap's
           threshold and its operand reachable at all. A single residue-12
           value leaves every snap mutant equivalent by accident. */
        P.tick_ret = (long)((const int[]){3659, 3661, 3672, 3718}[ri]);
        P.clock_seed = (time_t)DAY_BASE + (time_t)(si * 7 + ri * 11);
        P.sleep_mode = (int)(ri % 3);
        RUN_OLD(old_handle_timer_tick());
        RUN_NEW(new_wake_flow_handle_timer_tick());
        g_cases++;
        if (!same(&o, &n)) report("handle_timer_tick");
    }

    /* ---- handle_button_wake (row 7) ------------------------------------ */
    static const unsigned HELD[] = {0, 1u << BTN_A, 1u << BTN_B, 0xF, 0xFF};
    static const long long GAPS[] = {0, 1, 2, 3, 100};
    for (unsigned bi = 0; bi < sizeof BTNS / sizeof BTNS[0]; bi++)
    for (unsigned hi = 0; hi < sizeof HELD / sizeof HELD[0]; hi++)
    for (unsigned gi = 0; gi < sizeof GAPS / sizeof GAPS[0]; gi++)
    for (unsigned si = 0; si < sizeof STATES / sizeof STATES[0]; si++)
    for (int dok = 0; dok < 2; dok++)
    for (int no = 0; no < 2; no++)
    for (int bs = 0; bs < 2; bs++)
    for (unsigned li = 0; li < sizeof LATCH / sizeof LATCH[0]; li++) {
        defaults();
        P.wake_btn = BTNS[bi];
        P.held_mask = HELD[hi];
        P.sleep_gap = GAPS[gi];
        P.state = STATES[si];
        P.dispatch_ok = dok;
        P.net_open = no;
        P.break_starts = bs;
        P.latched = LATCH[li];
        P.ntp_ok = (int)(gi & 1u);
        P.rollover = (int)(hi & 1u);
        P.dispatch_swapped = (int)(si & 1u);
        P.dispatch_state = dok ? TIMER_RUNNING : -1;
        P.break_end_drains = (int)(li & 1u);
        P.break_ended_flag = (int)(bi & 1u);
        P.win_active = (int)(dok & 1);
        P.net_finish = (int)(si % 3);
        P.made_kind = (int)STATES[si];
        P.tick_ret = (long)(400 + (long)bi * 9);
        P.clock_seed = (time_t)DAY_BASE + (time_t)(bi * 5 + gi * 13);
        P.sleep_mode = (int)(hi % 3);
        RUN_OLD(old_handle_button_wake());
        RUN_NEW(new_wake_flow_handle_button_wake());
        g_cases++;
        if (!same(&o, &n)) report("handle_button_wake");
    }

    printf("cases=%ld divergences=%ld overflow=%ld\n", g_cases, g_diverge, g_overflow);
    if (g_first[0]) printf("first: %s\n", g_first);
    return g_diverge ? 1 : 0;
}
"""

for mname, edits in MUTANTS.items():
    nb, sm = apply_mutant(new_bodies, seams, edits)
    nb = namespace(nb, "new_", NEW_NAMES)
    with open(os.path.join(OUT, f"harness_{mname}.c"), "w") as f:
        f.write(PRE)
        f.write("\n/* ---- wake_flow.c state-assembly seam (extracted verbatim) ---- */\n")
        f.write(sm)
        f.write(f"\n\n/* ---- OLD implementations ({BASE} main.c) ---- */\n")
        f.write(old_bodies)
        f.write("\n\n/* ---- NEW implementations (wake_flow.c) ---- */\n")
        f.write(nb)
        f.write("\n")
        f.write(DRIVER)
print("generated", len(MUTANTS), "harnesses")
