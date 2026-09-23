#!/usr/bin/env python3
"""The ack hold's Kconfig default and its host fallback must be the same number.

WHY THIS EXISTS
---------------
`STATUS_LED_ACK_HOLD_MS` (include/status_led.h) is the NeoPixel pre-press
hold, and it is a menuconfig knob so a board can sweep it.  (It was also the
chore-ack coalescing window until M2-T15 split that off as
CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS.  The window has no host fallback -- the
host build supplies it through EXTRA_DEFS -- so it has no second home and
nothing here to check; its Kconfig block sits directly below the hold's and
is one of the neighbours this gate must not read.)  The firmware takes
`CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS` from sdkconfig.h.  The host build has
no sdkconfig.h, so it takes a literal written into the `#else` arm.  Two
places, one number.

status_led.h stated "THE FALLBACK AND THE KCONFIG DEFAULT MUST AGREE" as
though something enforced it.  NOTHING DID.  Review proved it twice over:

  * Mutating the `#else` literal to 250, and again to 0, changed no host
    binary at all and failed no test.  The only host suite that expands the
    macro is test_wake_flow, and test/CMakeLists.txt builds that one with
    CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS=170 DEFINED -- so it takes the
    `#ifdef` arm and never sees the fallback.  Every other suite compiles the
    `#else` and never mentions the macro.
  * So the MUST was not merely unenforced, its stated consequence ("the host
    suites and the firmware are timing two different devices") could not
    occur either.  An invariant nothing holds, described as though something
    does, is the defect this milestone kept finding.

The hazard is real but it is in the FUTURE, which is exactly why a gate and
not a test: the next suite to measure this figure without an EXTRA_DEFS
override would time the fallback while the firmware timed the Kconfig
default, and it would pass green.  A test cannot be written for a suite that
does not exist yet.  This can, and it fails the moment the two numbers part.

M2-HW-FIX raised the pair 250 -> 400 and the raise landed in both places
correctly.  This gate is not about that edit; it is so the NEXT one cannot
land in only one.  (The same change left "250" behind in a test/CMakeLists.txt
comment and "a quarter-second" in two wake_flow.c comments, which is the shape
this guards against, one level down in prose.)

WHAT IT CHECKS
--------------
Exactly one thing, and it is deliberately narrow: the integer after `default`
inside the `config MAGTAG_STATUS_LED_ACK_HOLD_MS` block of
main/Kconfig.projbuild equals the integer the `#else` arm of the
`CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS` guard in include/status_led.h defines
`STATUS_LED_ACK_HOLD_MS` to be.

Both files carry long prose about this figure, mentioning 250, 400 and 170 by
name many times.  Nothing here reads prose: the Kconfig side keys on the
symbol's own block and stops at the next `config`, and the C side keys on the
`#ifdef`/`#else`/`#endif` structure around the one macro.

LIMITATIONS, recorded so the next reader knows the gate's edges:

  * It compares the DEFAULT, not the value in a local `sdkconfig`, which is
    gitignored and is a board's business.  A board that sweeps the knob in
    menuconfig is doing the supported thing; this only refuses a committed
    default that the host build would not share.
  * A Kconfig `default` with a condition (`default 400 if FOO`) is not
    understood and is reported as unparseable rather than guessed at.  There
    is no such default today and one would be a design change.
  * Either file missing, or the symbol missing from it, is a FAILURE and not
    a pass -- deleting or renaming a side cannot quietly retire the gate.
    That hole is the specific way its two predecessors shipped broken.
"""

import re
import sys

KCONFIG = "main/Kconfig.projbuild"
HEADER = "include/status_led.h"

SYMBOL = "MAGTAG_STATUS_LED_ACK_HOLD_MS"
CONFIG_SYMBOL = "CONFIG_" + SYMBOL
MACRO = "STATUS_LED_ACK_HOLD_MS"


class Unparseable(Exception):
    """The gate could not find what it is about. Always a failure, never a pass."""


def read(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return fh.read()
    except OSError as exc:
        raise Unparseable(f"{path}: cannot read the file this gate is about: {exc}")


def kconfig_default(src):
    """The `default N` of the SYMBOL block, as (value, line number).

    The block runs from its own `config` line to the next `config` line at
    any indentation, which is how Kconfig itself delimits it. Scoping the
    search that way is what keeps the many other `default` lines in the file
    -- and the figures quoted in this symbol's own help prose -- out of it.
    """
    start = None
    lines = src.splitlines()
    for i, line in enumerate(lines):
        if re.match(r"^\s*config\s+" + re.escape(SYMBOL) + r"\s*$", line):
            start = i
            break
    if start is None:
        raise Unparseable(f"{KCONFIG}: no `config {SYMBOL}` block -- was the symbol renamed or removed?")

    end = len(lines)
    for i in range(start + 1, len(lines)):
        if re.match(r"^\s*config\s+\w+\s*$", lines[i]):
            end = i
            break

    found = []
    for i in range(start, end):
        m = re.match(r"^\s*default\s+(\S+)\s*$", lines[i])
        if m:
            found.append((m.group(1), i + 1))
    if not found:
        raise Unparseable(f"{KCONFIG}:{start + 1}: `config {SYMBOL}` has no unconditional `default`")
    if len(found) > 1:
        where = ", ".join(f"line {ln}" for _, ln in found)
        raise Unparseable(f"{KCONFIG}: `config {SYMBOL}` has {len(found)} `default` lines ({where})")

    text, lineno = found[0]
    if not re.fullmatch(r"-?\d+", text):
        raise Unparseable(f"{KCONFIG}:{lineno}: `default {text}` is not a plain integer this gate can compare")
    return int(text), lineno


def header_fallback(src):
    """The literal MACRO takes in the #else arm, as (value, line number).

    Keyed on the #ifdef/#else/#endif structure rather than on "the second
    #define", so an added branch or a reordering is reported instead of
    silently changing which line is read.
    """
    lines = src.splitlines()
    open_at = None
    for i, line in enumerate(lines):
        if re.match(r"^\s*#\s*ifdef\s+" + re.escape(CONFIG_SYMBOL) + r"\s*$", line):
            open_at = i
            break
    if open_at is None:
        raise Unparseable(f"{HEADER}: no `#ifdef {CONFIG_SYMBOL}` -- the host fallback's guard is gone")

    else_at = endif_at = None
    depth = 0
    for i in range(open_at + 1, len(lines)):
        line = lines[i]
        if re.match(r"^\s*#\s*(if|ifdef|ifndef)\b", line):
            depth += 1
        elif re.match(r"^\s*#\s*endif\b", line):
            if depth == 0:
                endif_at = i
                break
            depth -= 1
        elif depth == 0 and re.match(r"^\s*#\s*else\b", line):
            if else_at is not None:
                raise Unparseable(f"{HEADER}:{i + 1}: a second `#else` in the {CONFIG_SYMBOL} guard")
            else_at = i
    if else_at is None or endif_at is None:
        raise Unparseable(f"{HEADER}:{open_at + 1}: the `#ifdef {CONFIG_SYMBOL}` guard has no `#else`/`#endif` pair")

    found = []
    for i in range(else_at + 1, endif_at):
        m = re.match(r"^\s*#\s*define\s+" + re.escape(MACRO) + r"\s+(\S+)\s*$", lines[i])
        if m:
            found.append((m.group(1), i + 1))
    if not found:
        raise Unparseable(f"{HEADER}:{else_at + 1}: the `#else` arm does not `#define {MACRO}`")
    if len(found) > 1:
        where = ", ".join(f"line {ln}" for _, ln in found)
        raise Unparseable(f"{HEADER}: the `#else` arm defines {MACRO} {len(found)} times ({where})")

    text, lineno = found[0]
    if not re.fullmatch(r"-?\d+", text):
        raise Unparseable(f"{HEADER}:{lineno}: the fallback `{text}` is not a plain integer this gate can compare")
    return int(text), lineno


def main(argv):
    # pre-commit passes the staged files it matched; this invariant spans two
    # named files, so the arguments only decide WHETHER to look, never where.
    del argv
    try:
        default, kline = kconfig_default(read(KCONFIG))
        fallback, hline = header_fallback(read(HEADER))
    except Unparseable as exc:
        print(str(exc))
        print(
            "\nThis gate holds the ack hold's Kconfig default equal to its host\n"
            "fallback. It could not find one of the two, which is a failure and\n"
            "not a pass: a gate that cannot see its subject would otherwise go\n"
            "green forever. Fix the file or retire the gate deliberately."
        )
        return 1

    if default == fallback:
        return 0

    print(f"{MACRO}: the Kconfig default and the host fallback disagree")
    print(f"  {KCONFIG}:{kline}: default {default}")
    print(f"  {HEADER}:{hline}: #define {MACRO} {fallback}")
    print(
        f"\nOne figure (the NeoPixel pre-press hold), read from sdkconfig.h on\n"
        f"the device and from the\n"
        f"literal above on the host. A suite that measures it without an\n"
        f"EXTRA_DEFS override times the fallback while the firmware times the\n"
        f"default, and passes green while the two devices differ.\n"
        f"Raise or lower BOTH."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
