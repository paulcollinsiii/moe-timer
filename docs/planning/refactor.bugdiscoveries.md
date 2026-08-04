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

### Candidate root cause — NOT yet confirmed

The awake-press latch is ISR-fed and armed for the whole awake window, so the
press is very likely *recorded*. The problem is that nothing on this wake path
appears to *consume* it, and the latch does not survive to the next wake:

* `buttons_watch_begin()` (`main/buttons.c:41`) calls `button_latch_reset()`,
  and is invoked from `buttons_init()` (`main/buttons.c:99`) — which runs on
  **every boot**. So anything still latched at sleep entry is destroyed on the
  next wake.
* `buttons_watch_end()` (`main/buttons.c:57`) detaches the ISR before
  `buttons_configure_wakeup_if()` moves the pads to the RTC mux, so the latch
  also stops recording before sleep.
* An NTP-sync wake is a *timer* wake, so it runs the tick path, not
  `handle_button_wake`. If that path opens the network window and returns to
  sleep without polling the latch, the press is latched, never dispatched, and
  then cleared at the next `buttons_init()`.

The press also cannot be recovered via EXT1: the device is already awake when
it happens, EXT1 only arms at sleep entry, and if the button is *still held* at
sleep entry the held-through-sleep guard (behaviour row 7) deliberately ignores
it. Both paths therefore drop it.

### To confirm

Establish whether the sync/idle cadence path calls `buttons_take_pressed()` (or
the masked variant) after the network window closes and before sleep. If it
does not, the hypothesis holds. The network window can run for several seconds,
so the deaf interval is long enough to be hit routinely in normal use.

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

These are probably **not** the same defect: BUG-2 takes the press from the latch
and drops it in a specific state, whereas BUG-1 appears never to poll at all on
that wake path. But they share a root shape — *a press is latched during a long
awake window and no owner services it* — and a fix for either should be checked
against the other rather than applied in isolation.

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

---

## Not tracked here

Documentation staleness found during the refactor — `docs/architecture.md`
lines 128–129, and the stale claim that `handle_break_end()` "lives in main.c" —
is not a behaviour defect and belongs to **task 14 (docs)** of the refactor
plan, not to this list.
