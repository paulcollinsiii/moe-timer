#!/usr/bin/env python3
"""Exactly one status_led_show_timer_state() call in main/wake_flow.c.

WHY THIS EXISTS
---------------
status_led.c maps the chore checklist's GATE to pixel 0, which is
NP_STATE_PIXEL -- the pixel `status_led_show_timer_state()` writes.  The two
painters therefore collide, and `status_led.h` states "not alongside
status_led_show_timer_state()" as a caller contract that `chores_led_show()`
cannot enforce for itself.

`wake_flow.c` enforces it by funnelling every status paint through one
wrapper, `wake_flow_show_status_leds()`, which repaints the checklist instead
on any wake that claimed the strip.  Twelve call sites go through it and
exactly one raw call remains -- the one inside the wrapper.

That is the whole guarantee, and until this script existed NOTHING CHECKED
IT.  An adversarial review of M2-T8 reverted each of the twelve sites to a
raw `status_led_show_timer_state()` in turn and **ten of the twelve
survived** the entire host suite: the wrapper was correct, and nothing would
have noticed it becoming incorrect.  Two of the ten survivors were the LAST
LED paint before deep sleep on a chore-reachable path, so a wrong colour
would have stayed lit on the device until the next wake.

Twelve tests would be the other way to close that, and a worse one: each
would pin one site by its effects, none would say what the invariant IS, and
a thirteenth site added later would arrive unpinned.  This says the
invariant, and a new site is caught the moment it is written.

The wrapper's own comment already claimed "it is checkable by grep, which a
list of 'these sites are unreachable in chore mode' arguments is not".  This
is that grep, so the claim is enforced rather than merely made.

WHAT COUNTS AS A CALL
---------------------
The bare function name followed by `(`.  Comments and string literals are
stripped first, so the many prose mentions of the function in this file's
comments -- and there are a dozen -- do not count, and a call cannot hide
inside a comment either.  Declarations do not appear in a .c file for a
function declared in a header, so there is nothing to exempt.

LIMITATIONS, recorded so the next reader knows the gate's edges:

  * A call reached through a function pointer or a macro alias is invisible.
    Neither exists in the tree; both would be a strange way to write this.
  * The check is per-file and keyed on a path, so MOVING the wrapper to
    another translation unit would pass this gate while breaking the
    invariant.  That is deliberate: the invariant is about wake_flow.c
    specifically, because that is the file with twelve paint sites in it,
    and a move is a design change that should be reviewed rather than
    silently blessed.  The file must exist, though -- a missing target is a
    failure, not a pass, so deleting or renaming it cannot quietly retire
    the gate.
"""

import re
import sys

# The file the invariant is about, and the count it must have. Relative to
# the repository root, which is where pre-commit runs its hooks.
TARGET = "main/wake_flow.c"
FUNC = "status_led_show_timer_state"
EXPECTED = 1

# Why exactly one: the single raw call lives inside the wrapper
# wake_flow_show_status_leds(), which is what every other site calls.
WRAPPER = "wake_flow_show_status_leds"


def strip_comments_and_strings(src):
    """Blank out comments and string/char literals, preserving line count.

    Written as one pass over the characters rather than as a regex: a regex
    for "a comment, unless it is inside a string" is the classic way to get
    this subtly wrong, and the file being checked is full of comments that
    mention the function by name.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and src[i] != "\n":
                i += 1
        elif c == "/" and nxt == "*":
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                if src[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        elif c in ('"', "'"):
            quote = c
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\":
                    i += 1
                if i < n and src[i] == "\n":
                    out.append("\n")
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def find_calls(src):
    """Line numbers of every call to FUNC in already-stripped source."""
    pattern = re.compile(r"\b" + re.escape(FUNC) + r"\s*\(")
    return [src.count("\n", 0, m.start()) + 1 for m in pattern.finditer(src)]


def check(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            raw = fh.read()
    except OSError as exc:
        print(f"{path}: cannot read the file this gate is about: {exc}")
        return 1

    lines = find_calls(strip_comments_and_strings(raw))
    if len(lines) == EXPECTED:
        return 0

    print(f"{path}: expected exactly {EXPECTED} call to {FUNC}(), found {len(lines)}")
    for ln in lines:
        print(f"  {path}:{ln}")
    if len(lines) > EXPECTED:
        print(
            f"\n{FUNC}() writes NP_STATE_PIXEL, which is the chore checklist's\n"
            f"gate pixel. Call {WRAPPER}() instead: on a wake the checklist has\n"
            f"claimed it repaints the strip, so the gate cannot be overwritten\n"
            f"with a timer colour that means nothing on that screen.\n"
            f"The one permitted call is the one inside {WRAPPER}() itself."
        )
    else:
        print(
            f"\nThe single raw call belongs inside {WRAPPER}(). If the wrapper has\n"
            f"moved or been removed, the caller contract in status_led.h ('not\n"
            f"alongside {FUNC}()') no longer has an enforcement point."
        )
    return 1


def main(argv):
    # pre-commit passes the staged files it matched; the invariant is about
    # one file, so the arguments only decide WHETHER to look, never where.
    # Run unconditionally when invoked with no arguments (the --all-files
    # case reaches us with the file in the list anyway).
    del argv
    return check(TARGET)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
