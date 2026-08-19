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

USAGE
-----
    scripts/check-log-args.py [FILE ...]

With no arguments the default roots (main/ components/ include/ test/) are
scanned.  pre-commit passes the changed files.  Exit status 1 on any finding.
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

# Allowlist entries defined in this repository. Their definitions are
# re-verified by check_definitions() whenever the defining file is scanned.
REPO_LOCAL = {
    "ota_policy_reason_str",
    "ota_url_redirect_str",
    "wake_flow_reset_reason_str",
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
    "__typeof__", "alignof", "_Alignof", "offsetof",
}

# The whole ESP log family: the plain levels, the early/DRAM variants used
# from ISR and pre-heap contexts, and the two level-parameterised forms every
# one of them expands through.
LOG_RE = re.compile(
    r"\bESP_(?:EARLY_|DRAM_)?LOG(?:[EWIDV]|_LEVEL(?:_LOCAL)?|_BUFFER_HEX(?:DUMP)?"
    r"(?:_LEVEL)?|_BUFFER_CHAR(?:_LEVEL)?)\s*\("
)
CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")

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


def line_of(src, pos):
    return src.count("\n", 0, pos) + 1


def check_calls(path, src, stripped):
    """Findings for calls inside log argument lists."""
    findings = []
    for m in LOG_RE.finditer(stripped):
        macro = stripped[m.start():m.end() - 1].strip()
        args, _ = balanced(stripped, m.end() - 1)
        for c in CALL_RE.finditer(args):
            name = c.group(1)
            if name in C_KEYWORDS or name in ALLOWLIST:
                continue
            findings.append((line_of(src, m.start()), macro, name))
    return findings


ASSIGN_RE = re.compile(r"\b([sg]_[A-Za-z0-9_]*)\s*(?:\+\+|--|[-+*/%|&^]?=(?!=))")
INCDEC_RE = re.compile(r"(\+\+|--)")


def check_definitions(path, src, stripped):
    """Re-verify that repo-local allowlist entries are still pure formatters.

    A cheap tripwire, not a proof: it fires whenever the defining file is
    touched, which is the moment a side effect would be introduced. A body
    that calls nothing, increments nothing and assigns to no file-scope
    (`s_`/`g_`) object cannot do I/O, cannot mutate module state, and cannot
    recurse into something that does.
    """
    findings = []
    for name in sorted(REPO_LOCAL):
        for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", stripped):
            _, rparen = balanced(stripped, m.end() - 1)
            tail = stripped[rparen + 1:rparen + 40]
            if not tail.lstrip().startswith("{"):
                continue  # a call or a prototype, not a definition
            brace = rparen + 1 + len(tail) - len(tail.lstrip())
            body, _ = balanced(stripped, brace)
            bad = []
            for c in CALL_RE.finditer(body):
                if c.group(1) not in C_KEYWORDS:
                    bad.append("calls %s()" % c.group(1))
            if INCDEC_RE.search(body):
                bad.append("uses ++/--")
            for a in ASSIGN_RE.finditer(body):
                bad.append("assigns to %s" % a.group(1))
            if bad:
                findings.append((line_of(src, m.start()), name, sorted(set(bad))))
    return findings


CALL_HELP = """\
    ESP_LOGx wraps its ARGUMENTS, not just its body, in a compile-time
    `if (ESP_LOG_ENABLED(...))`. At a log level where this statement is
    compiled out the guard is `if (0)` and the argument is NEVER EVALUATED --
    on the device, not only on host. Whether this call runs is therefore a
    Kconfig value (CONFIG_LOG_MAXIMUM_LEVEL), and nothing in the suite or the
    differential sweep can see the difference.

    Fix: hoist it to a local computed BEFORE the log statement.

        timer_state_t st = timer_get_state();
        ESP_LOGI(TAG, "state %d", (int)st);

    If the local ends up consumed only by the log, add `(void)st;` -- the
    host log stubs discard their varargs, so it would otherwise be
    set-but-unused under -Wall.

    If the callee is a TOTAL, SIDE-EFFECT-FREE FORMATTER (an enum -> string
    mapper, like esp_err_to_name), add it to ALLOWLIST in
    scripts/check-log-args.py together with a written justification."""

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


def main(argv):
    paths = argv[1:] or sorted(iter_default_files())
    total = 0
    for path in paths:
        if not path.endswith((".c", ".h")):
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                src = fh.read()
        except OSError as exc:
            print("%s: cannot read (%s)" % (path, exc), file=sys.stderr)
            total += 1
            continue
        stripped = strip_noise(src)

        for line, macro, name in check_calls(path, src, stripped):
            total += 1
            print("\n%s:%d: function call `%s()` inside an %s argument list"
                  % (path, line, name, macro))
            print(CALL_HELP)

        for line, name, bad in check_definitions(path, src, stripped):
            total += 1
            print("\n%s:%d: allowlisted log formatter `%s()` is no longer pure: %s"
                  % (path, line, name, "; ".join(bad)))
            print(DEF_HELP)

    if total:
        print("\n%d finding(s). See HAZ-1 in docs/planning/refactor.bugdiscoveries.md."
              % total)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
