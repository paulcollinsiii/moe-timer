#!/usr/bin/env python3
"""Exactly one status_led_show_timer_state() call in main/wake_flow.c, and
Button C's count display released on every path out of its gesture.

(The second invariant is at the bottom of this docstring's WHY, under "THE
COUNT DISPLAY"; the first is what the rest of it is about.)

WHY THIS EXISTS
---------------
status_led.c maps CHORE SLOT 2 -- button D's row -- to pixel 0, which is
NP_STATE_PIXEL, the pixel `status_led_show_timer_state()` writes.  The two
painters therefore collide.  (Pixel 0 held the checklist's GATE until M2-HW2
inverted the strip on 2026-09-22; the gate is pixel 3 now.  That changed
nothing here: `k_chore_pixel` is a permutation of the whole strip, so pixel 0
always belongs to SOME slot and the two painters overlap either way.)
`status_led.h` states "not alongside
status_led_show_timer_state()" as a caller contract that `chores_led_show()`
cannot enforce for itself.

`wake_flow.c` enforces it by funnelling every status paint through one
wrapper, `wake_flow_show_status_leds()`, which repaints the checklist instead
on any wake that claimed the strip.  Thirteen call sites go through it and
exactly one raw call remains -- the one inside the wrapper.

That is the whole guarantee, and until this script existed NOTHING CHECKED
IT.  An adversarial review of M2-T8 reverted each of the twelve sites the file
had then to a raw `status_led_show_timer_state()` in turn and **ten of the
twelve survived** the entire host suite: the wrapper was correct, and nothing
would have noticed it becoming incorrect.  Two of the ten survivors were the
LAST LED paint before deep sleep on a chore-reachable path, so a wrong colour
would have stayed lit on the device until the next wake.

One test per site would be the other way to close that, and a worse one: each
would pin one site by its effects, none would say what the invariant IS, and
a site added later would arrive unpinned.  This says the invariant, and a new
site is caught the moment it is written.

The wrapper's own comment already claimed "it is checkable by grep, which a
list of 'these sites are unreachable in chore mode' arguments is not".  This
is that grep, so the claim is enforced rather than merely made.

THE COUNT DISPLAY (the sixth claimant, status_led.h)
----------------------------------------------------
status_led_swap_count_show() paints all four pixels for Button C's
counted-clicks gesture, and pixels 1-3 have no state painter to overwrite
them. So it may be called only from wake_flow_swap_count_gesture(), and a
path that shows the count and leaves would leave magenta lit until sleep.
What is checked, exactly: after the LAST count frame, in that function's
top-level statement sequence, comes neopixel_stop() and then the wrapper, each
an unconditional statement of its own (not inside a nested block, not guarded
by a brace-less if/else), in that order, with no `return` between the last
frame and the release. NOT checked: a goto, a longjmp, or a `break` out of an
enclosing construct, and that every path reaches the release; the host test
pins the behaviour. A file that never calls the count display is fine (the
fixtures in the test suite are such files); one that calls it with no owning
function, or from anywhere else, fails.

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
    specifically, because that is the file with thirteen paint sites in it,
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


# THE SIXTH CLAIMANT (status_led.h): Button C's count display takes the WHOLE
# strip, including pixels 1-3 that no state painter owns, so a path that shows
# the count and then leaves without releasing it leaves magenta pixels lit
# until sleep. The release is a wipe (neopixel_stop) followed by the one status
# wrapper, and it has to be in the same function as the show, after the last
# one, so a path out of the gesture cannot skip it.
COUNT_FUNC = "status_led_swap_count_show"
GESTURE = "wake_flow_swap_count_gesture"
RELEASE_CALLS = ("neopixel_stop", WRAPPER)


def call_positions(src, name):
    pattern = re.compile(r"\b" + re.escape(name) + r"\s*\(")
    return [m.start() for m in pattern.finditer(src)]


def function_span(src, name):
    """(start, end) of the definition of `name`, braces matched; None if absent."""
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", src)
    if not m:
        return None
    depth = 0
    for i in range(m.end() - 1, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return (m.start(), i + 1)
    return None


def count_display_problems(src):
    """What is wrong with the count display's use, as human sentences (empty = fine)."""
    shows = call_positions(src, COUNT_FUNC)
    if not shows:
        return []  # not in this file: nothing to hold
    span = function_span(src, GESTURE)
    if span is None:
        return [f"{COUNT_FUNC}() is called but {GESTURE}() is not defined: the count display has no owner"]
    problems = []
    for pos in shows:
        if not (span[0] <= pos < span[1]):
            line = src.count("\n", 0, pos) + 1
            problems.append(f"line {line}: {COUNT_FUNC}() is called outside {GESTURE}()")
    inside = [p for p in shows if span[0] <= p < span[1]]
    if inside:
        problems.extend(release_problems(src, span, max(inside)))
    return problems


def brace_depth(src, span, pos):
    """Brace depth at `pos` inside the function: 1 is its top-level statement sequence."""
    open_at = src.index("{", span[0])
    depth = 0
    for i in range(open_at, pos):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
    return depth


def starts_a_statement(src, pos):
    """True when the call at `pos` begins a statement: the previous significant
    character ends one (`;`) or opens/closes a block. `if (x) call();` has a
    `)` before it and `else call();` an `e`, so neither passes."""
    i = pos - 1
    while i >= 0 and src[i].isspace():
        i -= 1
    return i >= 0 and src[i] in ";{}"


def release_problems(src, span, last_show):
    """The release after the last count frame, as human sentences.

    What this checks, and nothing more: after the last show, in the function's
    top-level statement sequence, comes neopixel_stop() and then the wrapper,
    each as an unconditional statement of its own (not inside a nested block,
    not guarded by a brace-less `if`/`else`), in that order, with no `return`
    between the last show and the release. It does not follow a goto, a
    longjmp or a `break` out of an enclosing construct, and it does not prove
    that every path reaches the release; the host test pins the behaviour.
    """
    problems = []
    found = {}
    for name in RELEASE_CALLS:
        after = [p for p in call_positions(src, name) if last_show < p < span[1]]
        if not after:
            problems.append(f"{GESTURE}() has no {name}() after its last {COUNT_FUNC}(): the strip is never released")
            continue
        pos = after[0]
        found[name] = pos
        if brace_depth(src, span, pos) != 1 or not starts_a_statement(src, pos):
            line = src.count("\n", 0, pos) + 1
            problems.append(
                f"line {line}: {name}() after the last {COUNT_FUNC}() is not an unconditional statement of "
                f"{GESTURE}(): a path can skip the release"
            )
    if len(found) == len(RELEASE_CALLS):
        wipe, wrapper = found[RELEASE_CALLS[0]], found[RELEASE_CALLS[1]]
        if wrapper < wipe:
            problems.append(
                f"{WRAPPER}() is called before {RELEASE_CALLS[0]}(): the wipe darkens the state pixel it just painted"
            )
        for m in re.finditer(r"\breturn\b", src[last_show : max(wipe, wrapper)]):
            line = src.count("\n", 0, last_show + m.start()) + 1
            problems.append(f"line {line}: a return between the last {COUNT_FUNC}() and the release leaves the strip lit")
            break
    return problems


def check(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            raw = fh.read()
    except OSError as exc:
        print(f"{path}: cannot read the file this gate is about: {exc}")
        return 1

    stripped = strip_comments_and_strings(raw)
    count_problems = count_display_problems(stripped)
    if count_problems:
        print(f"{path}: the count display's strip contract is broken (status_led.h, the sixth claimant):")
        for p in count_problems:
            print(f"  {p}")
        print(
            f"\nButton C's count display writes all four pixels. Show it only from {GESTURE}(), and end\n"
            f"that function with neopixel_stop() followed by {WRAPPER}() so pixels 1-3 are wiped\n"
            f"and the state pixel is repainted on every path out."
        )
        return 1

    lines = find_calls(stripped)
    if len(lines) == EXPECTED:
        return 0

    print(f"{path}: expected exactly {EXPECTED} call to {FUNC}(), found {len(lines)}")
    for ln in lines:
        print(f"  {path}:{ln}")
    if len(lines) > EXPECTED:
        print(
            f"\n{FUNC}() writes NP_STATE_PIXEL -- pixel 0, which is chore slot 2's\n"
            f"row (button D's) on the checklist. Call {WRAPPER}() instead: on a\n"
            f"wake the checklist has claimed it repaints the strip, so that row\n"
            f"cannot be overwritten with a timer colour that means nothing on\n"
            f"that screen.\n"
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
