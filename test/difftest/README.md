# Differential sweep

The unit suites pin what the code *should* do. This pins something narrower and
harder: that a refactor changed **nothing at all**.

For a given cycle it extracts the OLD function bodies from a baseline git rev
and the NEW ones from the working tree, compiles both into one binary against a
single shared set of stubs, drives them over a cartesian product of corner-case
inputs, and compares the **full ordered effect trace** — every call, every
argument, in order. Not a return value, not a multiset: the sequence.

```sh
test/difftest/run.sh cycle09_rollover
```

Exit status is 0 only if every check below passes.

## The two-sided invariant

A sweep that reports "0 divergences" is worthless on its own — a harness that
observes nothing also reports 0. So every run checks both directions:

| harness | required | meaning |
|---|---|---|
| `harness_control` | divergences **== 0** | OLD and NEW are indistinguishable. The refactor claim. |
| every `harness_*` perturbation | divergences **> 0** | The sweep can actually see a change on that axis. |

A perturbation reporting 0 is a **failure**, not a pass: it means the sweep is
blind along that axis, and the control's 0 proves nothing there. The
perturbations are deliberate small wrongs — reorder two calls, delete a call,
move the clock sample, shift a boundary by one — injected into a copy of NEW.

Cycle 9's numbers, for reference: 29,592 cases, control 0, perturbations
11,088 / 648 / 2,016 / 324 / 432.

## Why bodies are extracted, never transcribed

`extract()` pulls functions out of both sources by brace matching. Nothing is
hand-copied, because a hand-copied "OLD" body is just a second chance to write
the same bug twice and call the agreement proof.

Only two rewrites are permitted, and both are changes the refactor explicitly
claims are device-identical:

* `time(NULL)` → `hal_time_now()` — `hal_time.c` is a one-line
  `return time(NULL);`. Note this is a *textual* substitution applied to the
  OLD body before comparison, so it makes the two sides agree by construction
  on the *identity* of the clock call. What it still pins is the **count and
  position** of clock reads, which is the part that actually matters.
* namespacing into `old_` / `new_` so both link into one binary.

## What this does NOT prove

Worth being blunt, because the gap here has already bitten once:

* **It is per-cycle and static.** Each cycle script names a baseline rev. Once
  that cycle lands, its sweep is a regression test against a historical rev,
  not against whatever is currently in `main`.
* **It only covers the functions named in the cycle script.** Anything outside
  `OLD_NAMES` / `NEW_NAMES` is not compared.
* **Stub fidelity is an assumption.** Both sides call the same stubs, so a
  wrong stub cannot cause a divergence — it just makes both sides equally
  wrong. The sweep proves OLD ≡ NEW, never that either is correct.
* **The corner-case arrays are a choice.** Coverage is only as good as the
  inputs enumerated in the driver.

The reason this harness is checked in at all: it uniquely covers things the
unit suites cannot see — clock-read count and position, argument-level ordering,
and operands that only appear inside log varargs (which the host build discards
entirely). While it lived in a scratch directory, everything in that list was
unprotected the moment the directory was cleaned up. A real escape landed in
exactly that gap during cycle 9: recording the rollover date from a fresh clock
read instead of the corrected value survived all 133 unit tests, because the
mock clock is frozen and nothing downstream could tell reuse from re-read.

## Adding a cycle

Copy `cycles/cycle09_rollover.py` and change:

* `BASE` — the baseline rev, normally the commit before this cycle's work.
* `OLD_NAMES` / `NEW_NAMES` — the functions moved, in matching order.
* `MUTANTS` — at least one perturbation per behaviour the cycle must preserve.
  Bias toward ±1 on every boundary and every operand; off-by-one at equality
  boundaries is this codebase's actual defect class.
* `PRE` / `DRIVER` — the shared stubs and the input product.

The generated `harness_*.c` are build artifacts: git-ignored, and deleted by
`run.sh` on the way out. The generator is the source of truth.
