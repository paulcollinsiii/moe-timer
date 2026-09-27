#!/usr/bin/env python3
"""HAZ-1: refuse a function call inside an ESP_LOGx argument list.

WHY THIS EXISTS
---------------
`ESP_LOGx` expands through `ESP_LOG_LEVEL_LOCAL`, which wraps the whole
statement -- *arguments included* -- in a compile-time conditional:

    #define ESP_LOG_LEVEL_LOCAL(configs, tag, format, ...) \\
        do { if (ESP_LOG_ENABLED(configs)) { ESP_LOG_LEVEL(...); } } while(0)

    #define ESP_LOG_ENABLED(configs) (LOG_LOCAL_LEVEL >= ESP_LOG_GET_LEVEL(configs))

`ESP_LOG_ENABLED` is a compile-time constant.  When the level is disabled the
guard is `if (0)` and **the argument expressions are never evaluated on the
device**.  So a call placed in a log argument is not "a call that might be
optimised away one day" -- it is a call whose execution is a function of a
Kconfig value.

This is live, not hypothetical.  `sdkconfig` sets `CONFIG_LOG_MAXIMUM_LEVEL=3`
(INFO), so every `ESP_LOGD`/`ESP_LOGV` argument already does not run on the
device.  Lowering the level to WARN -- a routine size/power change on a battery
device -- silently extends that to every `ESP_LOGI` site.

Nothing else in this project detects it.  The host log stubs
(`#define ESP_LOGI(tag, ...) ((void)(tag))`) discard their varargs too, so host
and device agree *by accident*, and the differential sweep compares two host
builds so it cannot see the difference either.

Hence a bright line rather than a maintained census: no call in a log argument,
except an allowlisted total formatter.  Hoist the rest to a local.

The `esp_check.h` family (`ESP_RETURN_ON_ERROR` and friends) is covered too:
those macros forward their variadic format arguments straight into `ESP_LOGE`,
so an argument there carries the identical hazard.  Their leading non-format
arguments -- the condition, the error code, the goto label -- are evaluated
outside the log and are skipped; see LOG_MACROS.

KNOWN LIMITATIONS
-----------------
This is a lexer, not a preprocessor, and the following are invisible to it.
They are recorded so the next reader knows the gate's edges rather than
assuming it has none.  Each needs real preprocessing to close, which is a far
larger tool than this one; the tree was checked and none of them occurs today.

  * An object-like macro that expands to a call:
        #define STACK_FREE uxTaskGetStackHighWaterMark(NULL)
        ESP_LOGI(TAG, "%u", (unsigned)STACK_FREE);
    There is no `(` at the use site, so nothing looks like a call.
  * A wrapper macro around ESP_LOGx (`#define MY_LOG(...) ESP_LOGI(TAG, ...)`).
    Uses of the wrapper are not scanned; the *definition* is, so a call written
    inside the definition is still caught.
  * A function-like ALL_CAPS macro that expands to a call.  ALL_CAPS names are
    exempted as macros (see CALL_RE), which is what stops MIN/BIT/pdMS_TO_TICKS
    from being reported as calls; a macro that hides a call behind that
    spelling is the price.
  * A call through a dereferenced function pointer, `(*s_ops->get)()`.  The
    ordinary `s_ops->get()` form IS caught.
  * A macro or callee name split by a line continuation (`ESP_LO\\<newline>GI`).
  * Code inside `#if 0` is scanned and can be reported, because it is not
    preprocessed away.

USAGE
-----
    scripts/check-log-args.py [FILE ...]

With no arguments the default roots (main/ components/ include/ test/) are
scanned.  pre-commit passes the changed files.  Exit status 1 on any finding.
Its own test suite is test/test_check_log_args/test_check_log_args.py, run by
ctest with the C suites.
"""

import os
import re
import sys

# ---------------------------------------------------------------------------
# THE ALLOWLIST -- the load-bearing decision in this file.
#
# An entry is a permanent hole in the rule, so the bar is deliberately high.
# A callee earns a place only if BOTH hold:
#
#   1. It is a TOTAL PURE FORMATTER: it takes a value, returns a display
#      string, and touches nothing else.  Not "is pure today" -- it must be
#      obviously and permanently a formatter, so that adding a side effect to
#      it would be a visibly strange thing to write.
#   2. Hoisting it would make the code WORSE, not just longer -- i.e. it is
#      common enough that a mandatory local at every site would bury the log
#      line it decorates.
#
# A repo-local function is a weaker candidate than a vendor one precisely
# because it is ours to change.  Repo-local entries therefore pay an extra
# price: they are listed in REPO_LOCAL below and this scanner re-checks their
# DEFINITION every time the defining file is touched (see check_definitions).
# That turns "trust the justification" into a tripwire.
#
# Adding an entry means writing the justification.  An entry without one is
# not an allowlist, it is a hole.
# ---------------------------------------------------------------------------
ALLOWLIST = {
    # -- vendor ------------------------------------------------------------
    "esp_err_to_name": (
        "ESP-IDF. Maps an esp_err_t onto a static string table and returns a "
        "pointer into it; no state, no allocation, total for any input. "
        "Vendor code, so it is not ours to grow a side effect in, and it is "
        "by far the most common log argument in the tree (32 sites). A "
        "mandatory local at every error path would bury the errors."
    ),
    # -- repo-local, enum -> string-literal formatters ---------------------
    # All three are the same construct as esp_err_to_name: a switch or a
    # const table whose every arm returns a string literal, total over the
    # domain (each guards its out-of-range and NULL cases), no I/O, no state.
    # They exist for exactly one purpose -- rendering an enum for a human --
    # and are anchored by a comment at their definition naming this file.
    "ota_policy_reason_str": (
        "main/ota_policy.c. ota_reason_t -> const string literal via a const "
        "table, range-guarded and NULL-guarded. Six log sites, all in "
        "ota_flow.c, which is host-built with vararg-discarding stubs -- so "
        "hoisting would cost six locals AND six (void) casts to stay "
        "warning-clean, purely to restate a table lookup."
    ),
    "ota_url_redirect_str": (
        "main/ota_url.c. ota_redirect_t -> const string literal, pure switch "
        "with a default arm. Same construct as the entry above; listed with "
        "it so that the two OTA reason formatters are not treated "
        "differently for no reason a reader could predict."
    ),
    "wake_flow_reset_reason_str": (
        "main/wake_flow.c. esp_reset_reason_t -> const string literal, pure "
        "switch with a default arm. Same construct again. Note this does NOT "
        "allowlist esp_reset_reason() itself, which is a read of a hardware "
        "register and is hoisted at both of its log sites."
    ),
}

# Allowlist entries defined in this repository, mapped to the file that is
# expected to define them.  Two things hang off this:
#   * check_definitions() re-verifies the body whenever a file defining one is
#     scanned, so a side effect is caught at the moment it is introduced;
#   * if the named file is scanned and the definition is NOT located, that is
#     itself a finding.  A body check that silently finds nothing is
#     indistinguishable from a body check that passed, which would make this
#     whole mechanism decorative.
REPO_LOCAL = {
    "ota_policy_reason_str": "main/ota_policy.c",
    "ota_url_redirect_str": "main/ota_url.c",
    "wake_flow_reset_reason_str": "main/wake_flow.c",
}

# Deliberately NOT allowlisted, recorded so the argument is not re-litigated:
#
#   esp_reset_reason, esp_get_free_heap_size, esp_get_minimum_free_heap_size,
#   uxTaskGetStackHighWaterMark, esp_https_ota_get_image_size
#       Pure reads, but reads of *the world*, not formatters. Their value is
#       the point of the log line, so where they are read matters; leaving
#       them inline means the read silently stops happening at a lower log
#       level, which is the exact failure this rule exists to prevent.
#
#   timer_get_state, timer_active_slot, timer_run_accum, timer_current_date,
#   battery_percent_from_mv
#       Module state accessors, not formatters. timer_run_accum() in
#       particular takes `now` and computes against live state. These are the
#       ones a future maintainer is most likely to grow a side effect in.

C_KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "defined", "do",
    "else", "case", "goto", "_Static_assert", "static_assert", "typeof",
    "__typeof__", "alignof", "_Alignof", "offsetof", "__attribute__",
}

# Log-family macros, mapped to the number of LEADING arguments that are NOT
# part of the log statement and are therefore evaluated unconditionally.
#
# The esp_check.h macros expand to (roughly)
#     do { if (unlikely((err_rc_ = (x)) != ESP_OK)) { ESP_LOGE(log_tag, format, ...); ... } }
# so `x` / `a` are assigned and tested outside the log, `err_code` sits in the
# return statement and `goto_tag` is a label -- none of them is conditionally
# evaluated by the *level*.  Everything after them lands in ESP_LOGE's argument
# list and carries the hazard, including the tag.
LOG_MACROS = {}
for _lvl in ("E", "W", "I", "D", "V"):
    LOG_MACROS["ESP_LOG" + _lvl] = 0
    LOG_MACROS["ESP_EARLY_LOG" + _lvl] = 0
    LOG_MACROS["ESP_DRAM_LOG" + _lvl] = 0
LOG_MACROS["ESP_LOG_LEVEL"] = 0
LOG_MACROS["ESP_LOG_LEVEL_LOCAL"] = 0
for _b in ("ESP_LOG_BUFFER_HEX", "ESP_LOG_BUFFER_CHAR"):
    LOG_MACROS[_b] = 0
    LOG_MACROS[_b + "_LEVEL"] = 0
LOG_MACROS["ESP_LOG_BUFFER_HEXDUMP"] = 0
for _s in ("", "_ISR"):
    LOG_MACROS["ESP_RETURN_ON_ERROR" + _s] = 1      # (x, log_tag, format, ...)
    LOG_MACROS["ESP_GOTO_ON_ERROR" + _s] = 2        # (x, goto_tag, log_tag, format, ...)
    LOG_MACROS["ESP_RETURN_ON_FALSE" + _s] = 2      # (a, err_code, log_tag, format, ...)
    LOG_MACROS["ESP_GOTO_ON_FALSE" + _s] = 3        # (a, err_code, goto_tag, log_tag, format, ...)

LOG_RE = re.compile(r"\b(" + "|".join(sorted(LOG_MACROS, key=len, reverse=True)) + r")\s*\(")

CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")

# An ALL_CAPS identifier is a macro by universal C convention, and a macro that
# expands to a pure expression is evaluated -- or not -- exactly like any other
# expression, so it carries no hazard.  Rejecting MIN(), BIT(), pdMS_TO_TICKS()
# and friends would block legitimate commits and push authors towards the
# ALLOWLIST, whose stated criterion ("returns a display string") fits none of
# them -- diluting the one list that has to stay meaningful.
#
# A short lower-case prefix is allowed because that is how FreeRTOS and IDF
# spell their macros (pdMS_TO_TICKS, portTICK_PERIOD_MS).  Real functions in
# this tree are lower_snake_case or camelCase and never match.
MACRO_RE = re.compile(r"^[a-z]{0,4}[A-Z][A-Z0-9_]*$")

# Assignment operators, excluding the comparisons ==, !=, <=, >= that share the
# `=` character.  <<= and >>= must be listed explicitly: they were missing from
# the first version of this file and a `s_flags <<= 1;` walked straight through.
ASSIGN_RE = re.compile(r"(<<=|>>=|[-+*/%&|^]=(?!=)|(?<![=!<>+\-*/%&|^])=(?!=))")
INCDEC_RE = re.compile(r"(\+\+|--)")

# A statement that starts with one of these declares something; its `=` is an
# initialiser, not a write to existing state.  Anything else with an `=` in it
# is assigning to an object that already exists, which a pure formatter has no
# reason to do.
TYPE_STARTERS = {
    "const", "volatile", "register", "char", "int", "short", "long",
    "unsigned", "signed", "float", "double", "bool", "void", "struct",
    "union", "enum", "auto", "_Bool",
}
TYPEDEF_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*_t$")

DEFAULT_ROOTS = ("main", "components", "include", "test")
SKIP_DIRS = {"build", ".pio", "unity", "cJSON", "managed_components", ".git"}


def strip_noise(src):
    """Blank out comments and the CONTENTS of string/char literals.

    Offsets are preserved (blanks replace the removed text, newlines kept), so
    reported line numbers match the original file. Without this, a format
    string such as "button C swap unavailable (state %d)" registers a call to
    `unavailable`.
    """
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in src[i:j]))
            i = j
        elif c in ('"', "'"):
            q = c
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == q or src[j] == "\n":
                    break
                j += 1
            out.append(q + "".join(ch if ch == "\n" else " " for ch in src[i + 1:j]))
            if j < n and src[j] == q:
                out.append(q)
                j += 1
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def balanced(s, lparen):
    """Return (body, end_index) for the parens/braces opening at `lparen`."""
    open_ch = s[lparen]
    close_ch = ")" if open_ch == "(" else "}"
    depth = 0
    i = lparen
    while i < len(s):
        if s[i] == open_ch:
            depth += 1
        elif s[i] == close_ch:
            depth -= 1
            if depth == 0:
                return s[lparen + 1:i], i
        i += 1
    return "", len(s)


def split_args(args):
    """Split an argument list on top-level commas."""
    out = []
    depth = 0
    cur = []
    for ch in args:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return out


def line_of(src, pos):
    return src.count("\n", 0, pos) + 1


def brace_depths(s):
    """Depth *before* each character, so a file-scope `{` sits at depth 0."""
    depths = []
    d = 0
    for ch in s:
        depths.append(d)
        if ch == "{":
            d += 1
        elif ch == "}":
            d = max(0, d - 1)
    depths.append(d)
    return depths


# ---------------------------------------------------------------------------
# Rule 1: no call inside a log argument list
# ---------------------------------------------------------------------------
def check_calls(src, stripped):
    findings = []
    for m in LOG_RE.finditer(stripped):
        macro = m.group(1)
        args, _ = balanced(stripped, m.end() - 1)
        parts = split_args(args)[LOG_MACROS[macro]:]
        for c in CALL_RE.finditer(",".join(parts)):
            name = c.group(1)
            if name in C_KEYWORDS or name in ALLOWLIST or MACRO_RE.match(name):
                continue
            findings.append((line_of(src, m.start()), macro, name))
    return findings


# ---------------------------------------------------------------------------
# Rule 2: an allowlisted repo-local formatter must stay a pure formatter
# ---------------------------------------------------------------------------
ATTR_ONLY_RE = re.compile(r"^\s*(?:__attribute__\s*\(\(.*?\)\)\s*)*$", re.S)


def classify(stripped, rparen):
    """What follows a parameter list: 'definition', 'prototype', or None.

    None means "this looked like a file-scope signature but could not be
    classified" -- which is reported rather than skipped.  The first version of
    this file looked for `{` in a fixed 40-character window and silently
    treated anything else as a call; a trailing comment longer than forty
    characters was enough to make a poisoned definition pass.
    """
    brace = stripped.find("{", rparen + 1)
    semi = stripped.find(";", rparen + 1)
    if semi >= 0 and (brace < 0 or semi < brace):
        return "prototype", -1
    if brace < 0:
        return None, -1
    if not ATTR_ONLY_RE.match(stripped[rparen + 1:brace]):
        return None, -1
    return "definition", brace


def statements(body):
    """Split a function body into statements, keeping each one's offset."""
    out = []
    start = 0
    for i, ch in enumerate(body):
        if ch in ";{}":
            out.append((start, body[start:i]))
            start = i + 1
    out.append((start, body[start:]))
    return [(o, t) for o, t in out if t.strip()]


def lvalue_of(stmt, op_start):
    """The assignment target: back to the previous top-level comma."""
    depth = 0
    cut = 0
    for i, ch in enumerate(stmt[:op_start]):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            cut = i + 1
    return stmt[cut:op_start]


def purity_violations(body):
    """Everything in `body` that a total pure formatter cannot contain."""
    bad = []
    for c in CALL_RE.finditer(body):
        if c.group(1) not in C_KEYWORDS:
            bad.append("calls %s()" % c.group(1))
    if INCDEC_RE.search(body):
        bad.append("uses ++/--")
    for _, stmt in statements(body):
        tokens = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", stmt)
        first = tokens[0] if tokens else ""
        if "static" in tokens:
            bad.append("declares a function-local static")
        is_decl = first in TYPE_STARTERS or bool(TYPEDEF_RE.match(first))
        for op in ASSIGN_RE.finditer(stmt):
            lv = lvalue_of(stmt, op.start()).strip()
            if "[" in lv or "." in lv or "->" in lv or lv.startswith("*"):
                # An indexed, member or pointer write. This is the shape the
                # naming-convention check used to miss, and it is the single
                # most likely way one of these functions stops being pure:
                # "format into a buffer and return a pointer to it".
                bad.append("writes through `%s`" % " ".join(lv.split()))
            elif not is_decl:
                bad.append("assigns to `%s`" % " ".join(lv.split()))
    return sorted(set(bad))


def check_definitions(path, src, stripped):
    findings = []
    depths = brace_depths(stripped)
    seen = set()
    for name in sorted(REPO_LOCAL):
        for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", stripped):
            if depths[m.start()] != 0:
                continue  # inside a function body: a call, not a signature
            _, rparen = balanced(stripped, m.end() - 1)
            kind, brace = classify(stripped, rparen)
            if kind == "prototype":
                seen.add(name)
                continue
            if kind is None:
                findings.append((
                    line_of(src, m.start()), name,
                    ["could not be classified as a definition or a prototype, "
                     "so its body was NEVER CHECKED"],
                ))
                seen.add(name)
                continue
            seen.add(name)
            body, _ = balanced(stripped, brace)
            bad = purity_violations(body)
            if bad:
                findings.append((line_of(src, m.start()), name, bad))

    # The backstop: if this is the file that is supposed to define one of them
    # and no signature was found at file scope at all, say so.
    norm = os.path.normpath(path).replace(os.sep, "/")
    for name, expected in REPO_LOCAL.items():
        if norm.endswith(expected) and name not in seen:
            findings.append((
                1, name,
                ["is allowlisted and recorded as defined in %s, but no "
                 "file-scope definition was found there" % expected],
            ))
    return findings


CALL_HELP = """\
    ESP_LOGx wraps its ARGUMENTS, not just its body, in a compile-time
    `if (ESP_LOG_ENABLED(...))`. At a log level where this statement is
    compiled out the guard is `if (0)` and the argument is NEVER EVALUATED --
    on the device, not only on host. Whether this call runs is therefore a
    Kconfig value (CONFIG_LOG_MAXIMUM_LEVEL), and nothing in the suite or the
    differential sweep can see the difference. The esp_check.h macros forward
    their format arguments into ESP_LOGE and behave the same way.

    Fix: hoist it to a local computed BEFORE the log statement.

        timer_state_t st = timer_get_state();
        ESP_LOGI(TAG, "state %d", (int)st);

    If the local ends up consumed only by the log, add `(void)st;` -- the
    host log stubs discard their varargs, so it would otherwise be an
    unused variable under -Wall.

    If the callee is a TOTAL, SIDE-EFFECT-FREE FORMATTER (an enum -> string
    mapper, like esp_err_to_name), add it to ALLOWLIST in
    scripts/check-log-args.py together with a written justification. Do NOT
    add anything else there: a pure MACRO needs no entry (ALL_CAPS names are
    already exempt), and a state read belongs in a local."""

DEF_HELP = """\
    This function is on the ALLOWLIST in scripts/check-log-args.py, which lets
    it be called directly inside ESP_LOGx arguments -- where it may not be
    evaluated at all. That is only safe while it stays a pure formatter.

    Fix: either revert the side effect, or remove the function from ALLOWLIST
    and hoist it to a local at every log site that calls it."""


def iter_default_files():
    for root in DEFAULT_ROOTS:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in sorted(filenames):
                if fn.endswith((".c", ".h")):
                    yield os.path.join(dirpath, fn)


def scan(path):
    """Findings for one file, as printable blocks."""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    stripped = strip_noise(src)
    blocks = []
    for line, macro, name in check_calls(src, stripped):
        blocks.append("%s:%d: function call `%s()` inside an %s argument list\n%s"
                      % (path, line, name, macro, CALL_HELP))
    for line, name, bad in check_definitions(path, src, stripped):
        blocks.append("%s:%d: allowlisted log formatter `%s()` %s\n%s"
                      % (path, line, name, "; ".join(bad), DEF_HELP))
    return blocks


def main(argv):
    paths = argv[1:] or sorted(iter_default_files())
    total = 0
    for path in paths:
        if not path.endswith((".c", ".h")):
            continue
        try:
            blocks = scan(path)
        except OSError as exc:
            print("%s: cannot read (%s)" % (path, exc), file=sys.stderr)
            total += 1
            continue
        for b in blocks:
            total += 1
            print("\n" + b)

    if total:
        print("\n%d finding(s). See HAZ-1 in "
              "docs/planning/implemented/20260818.bugregister.closed.md." % total)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
