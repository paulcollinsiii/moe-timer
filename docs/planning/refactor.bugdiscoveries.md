# Bug discoveries deferred during the main.c refactor

Defects found while executing `20260729.refactormain.plan.md`, deliberately
**not** fixed during it.

The refactor's whole value is the claim that behaviour did not change — proven
by differential sweeps comparing full ordered effect traces against a baseline
commit. Fixing a bug mid-move destroys that proof for the function it touches:
the control harness would diverge, and there would be no way to tell an
intended fix from an accidental regression. So each of these is pinned by a
test that asserts the CURRENT, WRONG behaviour, and gets its own commit after
the refactor lands.

Close these out before declaring the refactor finished. Each fix must flip its
pinning test deliberately, not silently.

| Status | Meaning |
|---|---|
| OPEN | Reproduced, not yet fixed |
| HYPOTHESIS | Observed on hardware, root cause not yet confirmed in code |
| CONDITIONAL | Not a bug today; becomes one if a named change lands |

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
  mask. (Now `main/wake_flow.c:815` after the cycle-11 move; structurally
  identical.)
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

`allow_net_window` was also checked and cleared: per `include/wake_flow.h:101`
it only suppresses a *redundant second* NTP window on a start/resume. It does
not refuse the press, so `!synced_this_wake` being false on a sync wake is not
the mechanism either.

### Still open — narrowed candidates

The mechanism is genuinely not identified yet.

**Nothing eats a latched C before the drain.** Every latch consumer reachable
on an IDLE sync tick was enumerated: `main/wake_flow.c:272` and `:282` are both
**masked-A** takes, and `button_latch_take_masked()` clears only the masked
bits, so a latched C passes straight through them. `main/wake_flow.c:467` (the
final-minute watch's unmasked entry discard) is **not reached at all** —
`maybe_wait_for_event()` at `main/wake_flow.c:652` branches on
`timer_get_state() == TIMER_RUNNING`, and an IDLE sync wake takes the
`watch_break_end` arm. `alerts.c`'s unmasked takes sit inside `run_alert()`,
unreachable with no expiry. So a C press made during the window is picked at
`main/wake_flow.c:815` and dispatched, exactly as intended.

*(An earlier revision of this file named `:467` as the leading candidate. That
was wrong for the reported path — recorded here so the dead lead is not
re-followed.)*

What survives:

1. **The press landed in the post-drain window** — after `main/wake_flow.c:815`
   and before the ISR detaches. That covers `maybe_wait_for_event()`,
   `enter_deep_sleep`'s prologue, and the bounded release-wait at
   `main/main.c:70`, which spins up to **3 s** on `buttons_scan_held()` — a
   *level* read, not a latch read. A press made and released inside it is
   latched, never consumed, and then destroyed by `buttons_watch_end()`.
2. **Something in the network window suppresses the ISR itself** — the window
   runs on a dedicated `net_win` task; if the press is never latched, every
   downstream poll is correctly finding nothing.

**Neither candidate explains the sync-wake specificity**, which is the part
that actually needs explaining: the window in candidate 1 is the same width on
every wake, sync or not, so it does not predict that C works normally right
after the 1-minute update and fails right after the hourly sync. Ruled out as
the amplifier: `net_window_join(15000)` at `main/main.c:65` — `net_apply_finish`
has already joined and cleared `s_active` (`main/net_window.c:180`), so the
sleep-path join returns immediately.

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

## Not tracked here

Documentation staleness found during the refactor — `docs/architecture.md`
lines 128–129, and the stale claim that `handle_break_end()` "lives in main.c" —
is not a behaviour defect and belongs to **task 14 (docs)** of the refactor
plan, not to this list.
