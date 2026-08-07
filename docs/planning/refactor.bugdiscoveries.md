# Bug discoveries deferred during the main.c refactor

Defects found while executing `20260729.refactormain.plan.md`, deliberately
**not** fixed during it.

The refactor's whole value is the claim that behaviour did not change — proven
by differential sweeps comparing full ordered effect traces against a baseline
commit. Fixing a bug mid-move destroys that proof for the function it touches:
the control harness would diverge, and there would be no way to tell an
intended fix from an accidental regression. So each FIRMWARE defect here is
pinned by a test that asserts the CURRENT, WRONG behaviour, and gets its own
commit after the refactor lands.

BUG-4 is the exception and is marked as such: it is a defect in the
verification harness itself, not in firmware, so there is no device behaviour
to pin. It is filed here rather than as a chore because it makes the sweeps
report a false PASS, and the sweeps are what every other entry's "deliberately
preserved" claim rests on.

Close these out before declaring the refactor finished. Each firmware fix must
flip its pinning test deliberately, not silently.

| Status | Meaning |
|---|---|
| OPEN | Reproduced, not yet fixed |
| HYPOTHESIS | Observed on hardware, root cause not yet confirmed in code |
| CONDITIONAL | Not a bug today; becomes one if a named change lands |
| hazard | Not wrong today (verified); a named future change makes it wrong |

---

## BUG-1 — Button presses are swallowed during the hourly NTP-sync wake

**Status:** HYPOTHESIS · **Found:** 2026-08-04, hardware, build `136cb06`
**Severity:** user-visible; the device appears dead to input for several seconds

### Observed

1. The 1-hour idle cadence fires for NTP sync; the sync completes.
2. Button C is pressed to change the selected timer — **ignored**.
3. The 1-minute screen update fires.
4. Button C works normally again.

### Why this is probably not a display artifact

The press produced no effect at all, not merely a late repaint, and normal
service resumed only after an unrelated scheduled wake. That points at the
press being discarded rather than deferred.

### Original candidate root cause — REFUTED 2026-08-04

The first hypothesis was that the tick path opens the network window and
returns to sleep **without ever polling the latch**, so the press is recorded
and then destroyed by the next boot's `button_latch_reset()`. That was checked
directly against `136cb06` — the exact build the bug was observed on — and it
does not hold. Two of its three legs are wrong:

* **The tick path does poll the latch.** `136cb06:main/main.c:697` runs
  `button_latch_pick(buttons_take_pressed(), (1u<<BTN_A)|(1u<<BTN_B)|(1u<<BTN_C))`
  unconditionally, and its own comment names this case: *"a press that landed
  while this wake was awake (sync, grid wait, e-ink flush)"*. BTN_C is in the
  mask. (Now `main/wake_flow.c:1009`, inside `wake_flow_handle_timer_tick()`,
  after the cycle-11 move and the task-13 audit; structurally identical.)
* **The ISR is armed for the entire awake window.** `buttons_watch_begin()` is
  reachable only from `buttons_init()` (`main/buttons.c:99`, every boot) and
  `buttons_watch_end()` only from sleep entry (`main/buttons.c:125`). There is
  no interval mid-wake where presses stop being latched. The
  `button_latch_reset()` observation is true but irrelevant: it fires at boot,
  long before the press.
* **No early sleep sits between the sync block and the poll.** The only
  `enter_deep_sleep()` calls in between are the two break-start exits, and
  `lock_gate_check_bedtime()` runs *before* the sync block. An ordinary hourly
  NTP-sync wake reaches the poll.

`allow_net_window` was also checked and cleared: per `include/wake_flow.h:127`
it only suppresses a *redundant second* NTP window on a start/resume. It does
not refuse the press, so `!synced_this_wake` being false on a sync wake is not
the mechanism either.

### Still open — narrowed candidates

The mechanism is genuinely not identified yet.

**Nothing eats a latched C before the drain.** Every latch consumer reachable
on an IDLE sync tick was enumerated: `main/wake_flow.c:411` and `:421` are both
**masked-A** takes (`buttons_take_pressed_mask(1u << BTN_A)`, which reaches
`button_latch_take_masked()` via `main/buttons.c:70`), and that clears only the
masked bits, so a latched C passes straight through them.
`main/wake_flow.c:629` (the final-minute watch's unmasked entry discard) is
**not reached at all** — `maybe_wait_for_event()` at `main/wake_flow.c:845`
branches on `timer_get_state() == TIMER_RUNNING`, and an IDLE sync wake takes
the `watch_break_end` arm. `alerts.c`'s unmasked takes sit inside `run_alert()`,
unreachable with no expiry. So a C press made during the window is picked at
`main/wake_flow.c:1009` and dispatched, exactly as intended.

*(An earlier revision of this file named the final-minute watch's entry discard
— then at `:467`, now `:629` — as the leading candidate. That was wrong for the
reported path — recorded here so the dead lead is not re-followed.)*

What survives:

1. **The press landed in the post-drain window** — after `main/wake_flow.c:1009`
   and before the ISR detaches. That covers `maybe_wait_for_event()`,
   `enter_deep_sleep`'s prologue, and the bounded release-wait at
   `main/main.c:122`, which spins up to **3 s** on `buttons_scan_held()` — a
   *level* read, not a latch read. A press made and released inside it is
   latched, never consumed, and then destroyed by `buttons_watch_end()`.
2. **Something in the network window suppresses the ISR itself** — the window
   runs on a dedicated `net_win` task; if the press is never latched, every
   downstream poll is correctly finding nothing.

**Neither candidate explains the sync-wake specificity**, which is the part
that actually needs explaining: the window in candidate 1 is the same width on
every wake, sync or not, so it does not predict that C works normally right
after the 1-minute update and fails right after the hourly sync. Ruled out as
the amplifier: `net_window_join(15000, NULL)` at `main/main.c:105` —
`net_apply_finish` has already joined and cleared `s_active`
(`main/net_window.c:177`), so the sleep-path join returns immediately.

### To confirm

Distinguish 1 from 2 by determining whether the press is *latched at all*
during the network window, rather than assuming it is. That is the fork; until
it is settled neither candidate should be fixed.

Note that BUG-2's eat also occurs on this path — the grid wait runs here and
takes the A bit while not RUNNING — and is pinned by
`test_row6_the_bug2_eat_also_happens_on_the_idle_sync_path_KNOWN_BUG`. That
test documents **BUG-2 on the sync path, not BUG-1**: it stages an A press, and
it dies to a BUG-2 fix. Do not read it as closing this entry.

### Fix constraint

Whatever the fix, it must not let a press queued during the sync window fire a
*stale* action: by the time it is serviced the clock may have stepped (NTP), and
`timer_shift_expiry(net_window_take_clock_step())` may already have moved
expiry. Any press replayed across that boundary needs its `now` re-read after
the step, not before.

---

## BUG-2 — Button A press eaten during the render-grid wait while not RUNNING

**Status:** OPEN · **Found:** during cycle 10 planning · **Severity:** user-visible

`poll_pause_button` consumes a Button A press while the timer is **not** in the
RUNNING state, during a `wait_for_render_grid` window of up to **25 seconds**.
The press is taken from the latch and then discarded, because pause is only
meaningful while RUNNING — so the user's press does nothing and is gone.

Confirmed and deliberately preserved through cycle 10. Pinned by
`test_row6_a_press_while_not_running_is_eaten_KNOWN_BUG` in
`test/test_wake_flow/test_wake_flow.c`, named to make clear it documents a
defect rather than blessing it; that test must fail loudly when the bug is
fixed, and the fixing commit must rewrite it deliberately rather than delete
it. The declaration comment on `wake_flow_wait_for_render_grid()` and the long
comment in `wake_flow_poll_pause_button()` both point back here.

The 25-second window makes this materially more likely to be hit than the
duration alone suggests, since it overlaps the e-paper full refresh — exactly
when a user who sees a stale panel is most likely to press something.

### Note on the relationship to BUG-1

This was originally written as "probably **not** the same defect", on the
grounds that BUG-1 never polled at all. That grounds was refuted on 2026-08-04
— the tick path *does* poll. A follow-up guess that BUG-1 was simply BUG-2 on
the sync path was then **also** refuted: BUG-2 takes the **A** bit only, and a
masked take clears only the bits it names, so it cannot consume the **C** press
BUG-1 reported.

So the original verdict stands, for a better reason than it was first given:
these are **not** the same defect. BUG-2 does occur on the sync path — pinned by
`test_row6_the_bug2_eat_also_happens_on_the_idle_sync_path_KNOWN_BUG` — but
fixing it will not fix BUG-1.

They still share a root shape (*a press is latched during a long awake window
and no owner services it*), so a fix for either must be checked against the
other rather than applied in isolation.

---

## BUG-3 — Break-tail pick mask excludes BTN_D without a test

**Status:** CONDITIONAL · **Severity:** none today

The break-tail button pick uses an explicit mask:

```c
button_latch_pick(buttons_take_pressed(), (1u << BTN_A) | (1u << BTN_B) | (1u << BTN_C));
```

BTN_D is excluded. This is currently harmless because BTN_D has no dispatch arm,
so the exclusion is unobservable and no test covers it.

**The moment BTN_D gains a dispatch arm, this exclusion becomes load-bearing**
and silently suppresses it in the break tail. Whoever adds that arm must decide
deliberately whether D belongs in this mask, and add a test either way.

**Scope correction (cycle 11):** there are TWO of these masks, not one. The
tick-wake latch drain in `wake_flow_handle_timer_tick()` carries the identical
exclusion, for the identical reason, and is equally unobservable today —
mutation testing in cycle 11 confirmed that adding `(1u << BTN_D)` to it is the
one mutant of that cycle's 81 that the host suite cannot kill. The equivalence
holds because D, winning the pick alone, reaches the dispatch's outer `default`
arm, which writes `selection_changed = false` and returns false; the only
residual difference on device is one extra side-effect-free `timer_get_state()`
read. Both call sites need a deliberate decision and a test when D gains an arm.
Recorded on `test_a_latched_d_press_is_never_dispatched_by_the_tick_drain`.

---

## BUG-4 — the checked-in sweeps test a hardcoded path, not the caller's tree

**Status:** OPEN · **Found:** 2026-08-07, during the stat-gather move
**Severity:** high, but scoped to the VERIFICATION HARNESS — no firmware
defect. It makes the sweep report a false PASS, which is worse than a
crash, because the sweep is the whole behaviour-preservation argument.

`test/difftest/run.sh` derives the repo from its own location
(`BASH_SOURCE`, line 17-18) and uses it for the include paths and
`EXTRA_SRC`. The three cycle generators ignore that and pin their own
absolute constant:

```python
REPO = "/workspaces/magtag-espidf/.claude/worktrees/refactor-main-impl"
```

(`cycles/cycle09_rollover.py:36`, `cycle10_watches.py:39`,
`cycle11_handlers.py:53`.) `REPO` is what supplies both sides of the
comparison — `git -C REPO show <BASE>:main/main.c` for the baseline and
`open(REPO/main/wake_flow.c)` for the current code. So the code actually
swept is whatever lives at that absolute path, regardless of where the
sweep was invoked from.

### Demonstrated, not inferred

The whole tree was copied to a scratch directory, the COPY's
`main/wake_flow.c` had `wake_flow_handle_timer_tick` renamed so that
`extract()` could not possibly find it, and the COPY's `run.sh` was run.
A sweep reading its own checkout must die with `could not find definition
of wake_flow_handle_timer_tick`. Instead it completed and reported

```
harness_control  cases=121504  divergences=0  OK  (identical)
```

with exit 0 — it had read the worktree's file and never looked at the
copy's.

### Why this matters after the branch merges

Two failure modes, and the safe one is the one that goes away:

1. **While this worktree exists** — running the sweep from the main
   checkout silently sweeps the WORKTREE and reports OK. Anyone
   re-verifying the refactor from main gets a green result that says
   nothing about the code in front of them.
2. **Once the worktree is deleted**, which is the normal end state —
   `git -C <gone> show` raises, `run.sh` prints `generate FAILED` and
   exits 1. Loud, but the sweeps are then permanently unrunnable for
   everyone, which defeats the point of `a7050d4` checking them in.

Note the results in the commits on this branch are NOT affected: every
run was invoked from inside the worktree, where `REPO` and the caller's
tree are the same directory. The claim they make is sound; what is broken
is anyone else's ability to re-make it.

### Fix constraint

`REPO` must come from the script's own location the way `run.sh` already
does it (`os.path.dirname(os.path.abspath(__file__))` walked up two
levels), not from a constant. Whatever replaces it needs a test that
*fails* when the sweep is pointed at a tree it did not come from —
otherwise the next copy of this defect is invisible again.

Related: `BASE` is a bare SHA on this branch (`7aab085` and friends). If
this branch is squash-merged those objects become unreachable and the
baseline read fails for the same reason. Belongs to **task 15**, which
is where the sweep gets generalised anyway.

---

## Log-stub vararg landmine — censused in one file, unrecorded in two

**Status:** hazard, not a defect today (verified) · **Found:** 2026-08-07

Every host-built module that logs defines a stub of the form

```c
#define ESP_LOGI(tag, ...) ((void)(tag))
```

which **discards the varargs**, so any function call sitting inside a log
argument is never evaluated on host. If such a call ever has a side
effect, the device does it and the host does not — and neither the host
suite nor the differential sweep can see the difference, because both are
host builds. That is a divergence with no detector.

`wake_flow.c` carries a maintained census comment for exactly this reason.
It was re-counted mechanically (comments and string literals stripped, so
format-string words like `"...unavailable("` do not register as calls) and
it is accurate: **seven** sites, at `:164`, `:267`, `:378`, `:431`,
`:497`, `:543`, `:766`.

The same scan found sites in two files that carry **no census and no
warning**:

* `main/timer_persist.c:33` (`esp_err_to_name`) and `:45`
  (`timer_get_state`)
* `main/lock_gate.c:87` (`timer_get_state`)

All six distinct callees across all three files were checked for writes to
module state and all are pure reads, so **nothing diverges today**. The
hazard is that the discipline protecting against it exists in one file and
not in the other two that need it. Either give those files the same census
comment, or replace the stubs with a variadic-consuming one
(`(void)sizeof(printf(__VA_ARGS__))` or similar) so the arguments are
type-checked and evaluated on host too — the second closes the class
rather than documenting it.

## Sweep trace cap — cycle09 cannot report truncation

**Status:** latent, measured · **Found:** 2026-08-07

`run.sh` refuses a run whose trace overflowed, because the comparison
tests lengths before contents and a divergence past the cap would be
invisible. Cycles 10 and 11 supply the counter (`g_overflow`) and a cap of
8192. **Cycle 09 has neither** — `TRACE_MAX` is 256 and the recorder drops
events with no `else` arm — which is why every cycle09 line prints
`[overflow not reported]` and its control's 0 proves less than the other
two cycles' do.

Measured rather than assumed: the control was instrumented with a
high-water mark and the longest trace cycle09 produces over all 29592
cases is **18 events against a cap of 256** — a 14x margin. So this is a
latent gap, not an active blind spot, and it is disclosed on every line of
output. Close it in **task 15** by giving cycle09 the counter the other
two have.

---

## Build hardening — not a defect, but a hazard task 12 introduced

Task 12 replaced `.on_locate = run_locate_alarm` (a static initializer that
bound the awake-failsafe extender and therefore **could not** be forgotten)
with a runtime `alerts_set_extend_awake()` call from `app_main`, which can be.
A forgotten install is not a crash — `alert_run_locate` NULL-guards and logs a
warning — but the locate alarm then runs without pushing the failsafe out and
can be cut short by the awake cap.

The structural backstop is that `extend_awake_failsafe` is `static` and the
install is its only remaining reference, so dropping the install makes it
unused. **That is currently only a warning.** IDF passes
`-Wall -Werror -Wno-error=unused-function` to every TU, deliberately disarming
exactly this diagnostic; the build exits 0 with the install deleted (verified).

**CLOSED 2026-08-07.** `-Werror=unused-function` is re-armed via
`target_compile_options(${COMPONENT_LIB} PRIVATE ...)` at the foot of
`main/CMakeLists.txt` and `components/ssd1680/CMakeLists.txt`. Scoped to our
own targets rather than set globally, because a global re-arm also covers IDF's
components and the managed dependencies, whose unused statics need triage that
has nothing to do with this project — that scoping is what made it safe to do
now rather than "worth its own task".

The last-flag-wins reasoning held: the emitted command line ends
`... -Wno-error=unused-function ... -Werror=unused-function -MD ...`, ours
last.

Verified two-sided, which is the only verification that means anything here:
the build passes as-is, AND deleting the `alerts_set_extend_awake()` install
from `app_main` now fails with

```
main/main.c:267:13: error: 'extend_awake_failsafe' defined but not used
                          [-Werror=unused-function]
```

at exit 2, where before it exited 0. The backstop is live.

Caveat: the backstop holds only while `extend_awake_failsafe` has exactly one
reference. A second caller in main.c makes a dropped install silent again.

## Not tracked here

Documentation staleness found during the refactor — `docs/architecture.md`
lines 128–129, and the stale claim that `handle_break_end()` "lives in main.c" —
is not a behaviour defect and belonged to **task 14 (docs)** of the refactor
plan, not to this list. Task 14 has since landed and both are fixed.

One documentation hazard is still open and is recorded here only because it can
mislead a future reader of *this* file: `docs/ProductOverview.md`'s
"Implementation Notes for Coding Agents" still instructs agents to use LovyanGFX
and `display.cpp` and cites removed IDF APIs. It carries a **Superseded** marker
as of task 14 but was deliberately not rewritten, because it is the pre-build
design record. Read the marker before the instructions.
