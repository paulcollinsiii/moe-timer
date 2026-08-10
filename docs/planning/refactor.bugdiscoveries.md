# Open defects and hazards — the working list

This began as the register of defects found while executing
`docs/planning/implemented/20260729.refactormain.plan.md` and deliberately
**not** fixed during it. That
refactor has landed and merged (`5d837a6` on `integration`), so the register is
now simply **the task list for the next phase**.

The original deferral rule no longer binds. It existed because the refactor's
whole value was the claim that behaviour did not change, proven by differential
sweeps against a baseline commit; fixing a bug mid-move would have made an
intended fix indistinguishable from an accidental regression. That proof is
banked. Each firmware defect below is still pinned by a test asserting the
CURRENT, WRONG behaviour, and **each fix must flip its pinning test
deliberately, not silently** — a fix that leaves its pin passing has not been
demonstrated.

Entries are **deleted from this file when they land**, not marked done. A closed
item keeps a one-line tombstone only where source comments name it, so those
references still resolve. Durable lessons do not live here at all — see
**Standing rules** at the foot, which exist to be promoted out of this file
before it is consumed.

| Status | Meaning |
|---|---|
| OPEN | Reproduced, not yet fixed |
| HYPOTHESIS | Observed on hardware, root cause not yet confirmed in code |
| CONDITIONAL | Not a bug today; becomes one if a named change lands |
| HAZARD | Not wrong today (verified); a named future change makes it wrong |

Two entries were defects in the verification harness rather than the firmware —
**BUG-4** and **HAZ-2**. They were filed here rather than as chores because the
sweeps are what every "deliberately preserved" claim in this file rests on. Both
are now closed; the sweeps run from any checkout and cycle09 reports trace
overflow, so the rest of this list can be verified by anyone.

---

## Order of work

Ordered by dependency first, then by cost. The two genuine sequencing
constraints are called out; everything else is independent and can be reordered
freely.

| # | Item | Why here | Blocked by |
|---|---|---|---|
| ~~1~~ | ~~**BUG-4** + **HAZ-2**~~ | **Done** — see Closed. The sweeps now derive the repository from their own location and are pinned to `difftest-base/*` tags, so everything below can be re-verified from any checkout. | — |
| 1 | **HAZ-1** — calls inside log-statement arguments | Mechanical, closes a whole class, touches nothing else | — |
| 2 | **BUG-5** — timer-defs blob drift discards the user's table | User-data policy; independent of the button work | — |
| 3 | **BUG-7** — a RUNNING slot outliving its own definition | State-machine change to an uncovered path; independent | — |
| 4 | **BUG-2**, then **BUG-3** | Same latch/mask surface — fix together so each is checked against the other. Both need a re-baselined sweep to show the fix changed *only* the intended cases. | — (was blocked on BUG-4) |
| — | **BUG-1** | **Parked 2026-08-07.** Settling its fork needs an instrumented build run on hardware, which is reporter time rather than engineering time. Revisit after item 5: BUG-2's fix touches the same latch surface and may move the ground under it. | — |

**Constraint 1 — BUG-4 before BUG-2/BUG-3. Discharged 2026-08-10.** Fixing a
pinned bug makes the sweep's control diverge by design, which is only
informative if the sweep can be re-baselined and re-run from an arbitrary
checkout. BUG-4 prevented that; it is now fixed, so BUG-2/BUG-3 are unblocked.

**Constraint 2 — BUG-2 and BUG-3 together.** They share a root shape and both
touch button-latch masks; a fix for either must be checked against the other
rather than applied in isolation.

---

## BUG-1 — Button presses are swallowed during the hourly NTP-sync wake

**Status:** PARKED 2026-08-07 (was HYPOTHESIS) · **Found:** 2026-08-04, hardware,
build `136cb06`
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

## BUG-5 — a timer-defs blob version bump silently discards the user's table

**Status:** OPEN · **Found:** 2026-08-07, hardware, build `59c3afa`
**Severity:** user-visible data loss, **one-shot per version bump** — an
HA-configured timer table reverts to compile-time defaults with no log line and
no indication anything happened.

> **Rescoped 2026-08-07.** This entry was originally titled *"a stale timer-defs
> blob silently resets every HA timer edit"* and was written as the explanation
> for the reported break-eligible reverts. It is not that. The recurring cause
> was the bulk config document (the entry formerly numbered BUG-6, fixed in
> `733e86e`); the ON, ON, OFF, OFF pattern the reporter saw followed from that,
> not from this. What remains here is a real but **narrower** defect: a blob
> version bump discards user configuration once, silently. The diagnostic
> scaffolding that existed to tell the two apart has been dropped as spent.

### The defect

`nvs_config_get_timer_defs()` (`main/nvs_config.c:248`) returns
`ESP_ERR_INVALID_VERSION` on **any** size or version drift.
`timer_defs_install()` (`main/timer_defs.c:76`) does not distinguish that from
"never configured" — both take the same branch, whose comment reads *"No
HA-managed blob yet"*, and it re-seeds from the Kconfig table and writes the
result back over the user's table.

There is **no migration and no log line at all** on that path. A user's entire
timer configuration can be discarded without a trace.

`TIMER_DEFS_BLOB_VERSION` went to 2 in `6fab99d feat(timer)!: add
break_eligible to the timer definition`. Any device carrying a pre-`6fab99d`
blob — or one whose NVS was erased — hits this on first boot of a newer build.
The `!` marks the break, but a breaking schema change that silently eats
configuration is still a defect: v1's fields are a strict prefix of v2's, so a
real migration is available and would preserve names, minutes and reload while
defaulting only the new field.

### Who is canonical — corrected

An earlier revision of this file asserted *"the device's NVS blob is canonical;
HA holds no durable copy of a timer definition."* **The second half is wrong**
and the correction matters, because it changes what a fix may rely on. Two
durable copies exist off-device:

1. **The retained bulk `config` document.** Fully durable on the broker;
   reapplied when its `ver` differs from the stored `cfg_ver`
   (`main/config_apply.c:233`, recorded at `:265`).
2. **The retained per-entity `set/` commands.** HA's config entities advertise
   `"retain":true` on their command topic (`main/ha_config.c:358`), so the
   broker does hold each value — but `apply_sets()` **deletes each one once
   applied** (`main/mqtt_ha.c:312-323`), deliberately, so a stale set cannot
   re-override the bulk document every window. So this copy is durable only
   until first consumption.

The blob is the *runtime* source of truth and is what `ha_config_state_json()`
publishes, so HA's UI mirrors it — "the publish is toggling the flag" is the
device telling HA what it now believes. But the blob is not the only durable
store, and copy 2's consume-on-apply is precisely why the original report never
self-healed.

### The stated policy (reporter, 2026-08-07)

> * defaults are loaded at flash
> * device does a full sync and sees the existing document from HA
> * **if the document versions are the same** (the config structure has not
>   changed) **the HA doc is canonical**
> * **if the doc versions have changed, the device defaults win**
>
> "That allows for upgrades to happen cleanly, while bug fixes land without
> changing the HA set config."

The version meant is `DISC_SCHEMA_VER` (`main/mqtt_ha.c:25`, currently 16).

**Note the axis mismatch, which the fix must resolve deliberately.** That policy
is stated over the *discovery/document* schema; this defect is on
`TIMER_DEFS_BLOB_VERSION`, the *NVS blob* schema. They are different versions
that move for different reasons, and today nothing ties them together. A blob
bump with no accompanying document change currently means "device defaults win"
by accident rather than by decision.

### Measured: the size check cannot replace the version field

Relevant because dropping `TIMER_DEFS_BLOB_VERSION` and leaning on the existing
`len != sizeof(*out)` test is the obvious simplification, and it does not work.
`nvs_timer_def_t` is `char name[16]; int32_t min; uint8_t reload; uint8_t
break_eligible;` — 22 bytes of content in a 24-byte struct, so it carries **two
spare padding bytes**. Compiling the v1 (pre-`6fab99d`), v2 (current) and a
hypothetical v3 layout with one more `uint8_t` flag:

| Layout | `sizeof(def)` | `sizeof(blob)` | size check catches drift? |
|---|---|---|---|
| v1 — no `break_eligible` | 24 | 100 | — |
| v2 — current | 24 | 100 | **no** |
| v3 — one more `uint8_t` flag | 24 | 100 | **no** |

All three are byte-identical in size. The size test would not have caught the
v1→v2 bump that created this defect, and will not catch the next flag either,
because the next two `uint8_t` fields land in existing padding for free. **The
version field is the only thing that has ever detected drift here.**

Both writers `memset` before filling, so a v1 blob misread as v2 would today
yield `break_eligible = 0` everywhere rather than garbage — silently wrong, not
random. The sharper hazard is any future change that *reuses* a padding byte or
*reorders* fields: same size, different meaning, old bytes read as valid new
values.

### Measured: a layout digest cannot replace the padding it hides in

A compile-time checksum over the struct layout was proposed as a stronger guard
than a bare `sizeof` assert — one constant catching reorders, additions and
renames at once. Measured against the case that actually produced this defect,
it does not:

| Guard | Field reordered | Field added into trailing padding | Field renamed |
|---|---|---|---|
| `sizeof` assert | no | **no** | no |
| `offsetof` asserts per field | yes | **no** | no |
| Layout digest over enumerated fields | yes | **no** | no |
| Designated-initializer canary | — | **no** (see below) | no |

Both the digest and the per-field asserts are built from the *enumerated*
fields, so a field nobody enumerated is invisible to both. Adding a `uint8_t`
into the two trailing padding bytes leaves `sizeof` at 24 and every existing
offset unchanged — the digest comes out **identical** (verified: 24574019 either
way). That is exactly the shape of the v1→v2 `break_eligible` addition.

The designated-initializer canary — a `static const` naming every field, relying
on `-Wmissing-field-initializers` to flag a new one — was also tested and **does
not warn**: GCC does not apply that diagnostic to designated initializers, with
either `-Wextra` or the flag named explicitly.

**Field names are invisible to the compiler**, so no compile-time mechanism can
catch a rename. That is acceptable: a rename that keeps type and position does
not change the stored bytes. It only matters if it signals a *semantic* change,
which no layout guard can see.

### The guard that does work: remove the hiding place

Make the implicit padding an explicit field, then assert that no implicit
padding remains. A new field then has nowhere to land silently — it must either
grow the struct (caught) or visibly consume the named reserve, which is an edit
sitting directly beneath the version constant.

```c
typedef struct {
    char    name[16];       /* "" = slot disabled */
    int32_t min;
    uint8_t reload;
    uint8_t break_eligible;
    uint8_t rsvd[2];        /* was implicit padding; named so nothing can hide */
} nvs_timer_def_t;

_Static_assert(sizeof(nvs_timer_def_t) == 24, "layout grew: migrate or bump BLOB_VERSION");
_Static_assert(offsetof(nvs_timer_def_t, min) == 16, "fields reordered");
_Static_assert(offsetof(nvs_timer_def_t, reload) == 20, "fields reordered");
_Static_assert(offsetof(nvs_timer_def_t, break_eligible) == 21, "fields reordered");
_Static_assert(offsetof(nvs_timer_def_t, rsvd) == 22, "fields reordered");
_Static_assert(16 + 4 + 1 + 1 + 2 == sizeof(nvs_timer_def_t),
               "implicit padding reappeared: a new field could hide in it");
_Static_assert(sizeof(nvs_timer_defs_blob_t) == 100, "blob layout changed");
```

The blob header needs the same treatment — `uint8_t version` followed by a
4-byte-aligned array carries 3 implicit padding bytes, so it becomes
`uint8_t version; uint8_t rsvd[3];`.

**This change is free to deploy.** Verified: naming the padding leaves
`sizeof(def) == 24`, `sizeof(blob) == 100` and `offsetof(blob, defs) == 4`
exactly as they are today, so it is byte-identical to blobs already on devices
and needs no migration of its own. Both writers already `memset` before
filling, so the reserve stays zeroed and is usable by a future field.

Prefer the individual asserts over a single digest: identical detection power,
but a digest reports one opaque number where these name the field that moved.

### Fix constraints

* **Migrate rather than discard. Decided 2026-08-07 (reporter).** Read a v1
  blob, copy the common prefix, default `break_eligible` from the Kconfig value
  for that slot, write back as v2. This is what makes the stated policy's
  second clause ("bug fixes land without changing the HA set config") actually
  hold.
* **Consequence of that decision, flagged deliberately:** the silent re-seed is
  currently the *only* mechanism by which device defaults ever beat the HA
  document. `config_apply` has no such branch — it skips when `cfg_ver` matches
  and applies when it does not (`main/config_apply.c:233`), and never prefers
  its own defaults. So migrating does not merely fix a bug, it **removes the
  only implementation of "device defaults win"** that exists. If that clause of
  the stated policy is wanted for real, it is new work and must be built
  deliberately.
* **Whatever the policy, log it.** "Stale blob, config reset" and "no blob yet,
  seeding" are different events and must not share a silent code path. This
  constraint is unconditional and is arguably worth landing on its own even if
  the migration is deferred.
* **Keep `TIMER_DEFS_BLOB_VERSION`, independent of `DISC_SCHEMA_VER`, and
  guard the layout mechanically. Decided 2026-08-07.** Two alternatives were
  rejected with reasons:
  * *Tie it to `DISC_SCHEMA_VER`* — no. That constant bumps for cosmetic entity
    changes (v16 was "text entities advertise their max length"), so keying the
    blob on it would make every cosmetic HA tweak invalidate the stored timer
    table. Strictly worse than today. The two versions answer different
    questions and the code should say so.
  * *Store the defs as JSON and retire layout versioning entirely* — no. It
    would work, and it would unify the stored form with the wire form, but it
    puts a cJSON parse at `timer_defs_install()` time. That is early boot,
    where today cJSON only runs inside the network window.
* **The version's role changes** from an equality tripwire that discards to a
  migration-ladder input (`if (v < CURRENT) migrate_up()`). That is what the
  migrate decision above actually means, and it deletes the silent-discard
  branch rather than making it quieter.
* Fixing this must not resurrect the defect formerly filed as BUG-6: a re-seed
  writes Kconfig names into every slot, which makes each slot look *existing*
  to `apply_timers()`, so a subsequent document that omits `break` will now
  preserve the **Kconfig** value rather than the user's. Migration avoids this;
  reset-and-log does not.

---

## BUG-7 — a RUNNING slot outlives its own definition

**Status:** OPEN · **Found:** during `feature/break-eligible` review · **Severity:**
user-visible; corrupts the screen-exposure balance and breaks two documented
state invariants

Folded in from `docs/planning/20260729.orphaned-running-slot.note.md`, which is
deleted; it was already confirmed out of scope for the branch that found it and
is an uncovered corner case, which is what this list is for. It **predates** the
refactor: present unchanged at `5154a04` and at every commit back through the
introduction of `timer_ensure_active_slot_enabled`.

### The defect

```c
void timer_ensure_active_slot_enabled(void) {
    if (!slot_enabled(g_rtc_state.active_slot))
        g_rtc_state.active_slot = 0;
}
```

`timer_ensure_active_slot_enabled()` (`main/timer.c:57`) moves the selection off
a slot whose definition is gone. It does **not** check whether that slot is
`RUNNING`, and `timer_restore_snapshot()` calls it unconditionally at the end
(`main/timer.c:~860`). The slot's own state is restored first and left alone —
the comment there says so explicitly (*"its state stays restored; only the
selection moves"*).

So after restoring a snapshot whose active slot was RUNNING and whose
`MAGTAG_TIMER<n>_NAME` was emptied by a reflash, the device is left with a
RUNNING slot that is not the active slot.

### Repro

1. Configure an extra timer — say slot 1, "Piano".
2. Start it. Let a snapshot be written (any `enter_deep_sleep`).
3. Reflash with `MAGTAG_TIMER1_NAME=""`, or delete the timer from Home
   Assistant, which empties the name in the defs blob.
4. Boot the same day, so the snapshot restores.

Slot 1 comes back RUNNING; the selection is forced to slot 0.

### Invariants broken

From `include/timer.h` and `test/test_timer`'s `assert_state_legal()`:

* **I1** — only the ACTIVE slot may be RUNNING.
* **I3** — a RUNNING slot is always the active slot.
* **I2** becomes reachable too (more than one RUNNING slot) as soon as the user
  presses A on the now-selected Screen: nothing pauses the orphan, so both slots
  end up RUNNING.

`test_timer`'s teardown *would* trip on this, but **no test constructs it** —
the existing coverage
(`test_snapshot_restore_falls_back_when_active_slot_disabled`) pauses the slot
before snapshotting, so nothing is RUNNING.

### Downstream effect on the exposure balance

With the screen-exposure balance (v1.5) this stopped being a tidiness problem.
The live run segment lives on slot 0 and is signed by
`rtc_state_t.run_segment_slot` — the slot that armed it. After the restore that
slot is disabled, so `timer_slot_break_eligible()` returns false for it (a slot
with no definition cannot claim to be a break activity).

Pressing A on Screen then folds the orphan's segment as **positive accrual**,
even though it was a break-eligible timer draining the balance. The wall-clock
gap since the reflash is counted as screen exposure, and the device is left with
two RUNNING slots.

This is **not** a defect in the sign rule: once the definition is gone the
eligibility is genuinely unrecoverable, because the flag lived in the defs table
that was reflashed. The defect is that the run was allowed to survive its own
definition at all.

### Why it was not fixed where it was found

The obvious fix — fold the orphan's segment out and reset the slot, the same
contract `timer_reconcile_def` already applies to a rename/disable — needs a
`now` to fold at, and `timer_ensure_active_slot_enabled()` takes no arguments.
Adding one ripples to its callers. That is a state-machine change to a path with
no existing coverage, and doing it inside a review-fix round on an unrelated
feature was the wrong trade.

### Suggested shape

Give the function a `now` and treat a disabled RUNNING slot the way a mid-window
disable is already treated:

* fold the live segment **as non-eligible** — i.e. as screen exposure.
  **Decided 2026-08-07 (reporter):** the sign is genuinely unrecoverable, so
  take the conservative direction, which errs toward *more* eye rest rather
  than less. The cost is that a break activity interrupted by a reflash counts
  against the allowance; accepted on the grounds that dropping config this way
  is unlikely to happen mid-run. Note this is the same direction the code takes
  today by accident — the defect being fixed is the *powered-off gap* being
  swept in with it, not the sign itself.
* reset the slot like `timer_reload` does — `memset`, keep `completions`;
* then move the selection.

Add the missing test: snapshot a RUNNING extra, install a defs table without it,
restore, assert `assert_state_legal()` passes and the balance did not gain the
powered-off gap.

---

## HAZ-1 — function calls inside log-statement arguments

**Status:** HAZARD — no defect today (all call sites verified pure) · **Found:**
2026-08-07 · **Rescoped 2026-08-07** after the device-side behaviour was checked

Originally filed as a host-build artifact: the host log stub discards its
varargs, so a call inside a log argument is not evaluated on host. That framing
was too narrow and let the risk read as a testing quirk. It is not.

### The actual rule: log arguments are conditionally evaluated *on device too*

`ESP_LOGx` expands through `ESP_LOG_LEVEL_LOCAL`, which wraps the entire call —
**arguments included** — in a compile-time conditional:

```c
#define ESP_LOG_LEVEL_LOCAL(configs, tag, format, ...) \
    do { if (ESP_LOG_ENABLED(configs)) { ESP_LOG_LEVEL(...); } } while(0)
/* esp_log.h:157 */

#define ESP_LOG_ENABLED(configs) (LOG_LOCAL_LEVEL >= ESP_LOG_GET_LEVEL(configs))
/* esp_log_level.h:73 — a compile-time constant */
```

When the level is disabled the guard is `if (0)` and **the argument expressions
are never evaluated on the device**. A call placed there is not "a call that
might get optimised out one day" — it is a call whose execution is a function of
a Kconfig value.

### This is already active, not hypothetical

`sdkconfig` sets `CONFIG_LOG_MAXIMUM_LEVEL=3` (INFO), so `ESP_LOGD` and
`ESP_LOGV` are compiled out **today**. `main/wake_flow.c:164` is an `ESP_LOGD`
whose argument list calls `battery_percent_from_mv(mv)` — **that call does not
happen on the device as shipped.** It is pure, so nothing is currently wrong;
but the mechanism is live, not waiting for anyone to change a flag.

Lowering `CONFIG_LOG_MAXIMUM_LEVEL` to WARN — an entirely routine size/power
change on a battery device — silently extends the same treatment to all five
`ESP_LOGI` sites below. Lowering it to ERROR takes the four `ESP_LOGW` sites too.

### Why nothing would catch it

The host stubs are of the form

```c
#define ESP_LOGI(tag, ...) ((void)(tag))
```

which discards the varargs, so host and device happen to **agree** — by
accident, and only while the level is disabled. Worse, the differential sweep
cannot see the difference either, because both sides of the comparison are host
builds. A side-effecting call in a log argument is a divergence with **no
detector anywhere in the project**.

### Census — verified mechanically

Re-counted with comments and string literals stripped, so format-string words
like `"...unavailable("` do not register as calls.

Stubs are defined in **five** files: `main/wake_flow.c:85-87`,
`main/alerts.c:41-42`, `main/net_apply.c:18-19`, `main/timer_persist.c:16`,
`main/lock_gate.c:21`. (`alerts.c` and `net_apply.c` carry stubs but no
calls-in-arguments, so they are exposure-free today.)

**Ten** call sites, **six** distinct callees, all verified pure reads:

| Site | Level | Call | Evaluated on device today? |
|---|---|---|---|
| `main/wake_flow.c:164` | D | `battery_percent_from_mv(mv)` | **no — already compiled out** |
| `main/wake_flow.c:267` | I | `timer_active_slot()` | yes |
| `main/wake_flow.c:378` | I | `timer_active_slot()` | yes |
| `main/wake_flow.c:431` | I | `timer_get_state()` | yes |
| `main/wake_flow.c:497` | I | `timer_run_accum(now)` | yes |
| `main/wake_flow.c:543` | W | `timer_current_date()` | yes |
| `main/wake_flow.c:766` | I | `timer_get_state()` | yes |
| `main/timer_persist.c:33` | W | `esp_err_to_name(ret)` | yes |
| `main/timer_persist.c:45` | W | `timer_get_state()` | yes |
| `main/lock_gate.c:87` | W | `timer_get_state()` | yes |

`wake_flow.c` carries a maintained census comment for exactly this reason and it
is accurate. `timer_persist.c` and `lock_gate.c` carry **none** — and that is the
real finding here. The discipline protecting against this exists in one of the
three files that need it, which means it is not a discipline, it is a habit that
did not propagate.

### Recommended fix — a bright line, not a better census

**Rule: no function call in a log-statement argument list, except an
allowlisted pure formatter (`esp_err_to_name` and friends).** Hoist the rest to
a local computed before the log statement.

Enforce it with a checked-in scanner run from pre-commit — the
comment-and-string-stripping scanner used for the census above is most of it
already. Cost is roughly nine mechanical edits and one hook.

The alternative — keep the calls, extend `wake_flow.c`'s census comment to the
other two files, and require a purity judgement per addition — is cheaper now
and is what the code does today. It is not recommended: it has already been
tried implicitly and failed to reach two of three files, and it asks every
future contributor to reason about compile-time argument evaluation correctly.
A bright line asks nobody to reason about anything.

**Wrinkle the fix must handle:** hoisting a value consumed only by a
compiled-out log makes it set-but-unused, which `-Wall` will flag —
`main/wake_flow.c:164`'s `ESP_LOGD` is exactly this case. Pair each hoist with a
`(void)` or scope it to levels that are not compiled out; do not discover this
halfway through and quietly revert the rule.

---

## Standing rules

These are the durable lessons, kept separate because **everything above is meant
to be deleted as it lands and these are not.** Promote them into
`docs/home_assistant.md` (R1, R2) and the project conventions (R3) before this
file is consumed.

**R1 — new timer config is durable config, and durable config lives in HA.**
When adding a field to timers 1–4, ask *"is this durable config that should be
in HA?"* The answer is almost always yes. If it is, the field must be:
1. expressible in the bulk `config` document,
2. documented in `docs/home_assistant.md`,
3. published in the state JSON so HA's UI reflects the device's actual belief.

Missing (1) is what caused the break-eligible reverts; missing (2) is what made
it invisible for so long.

**R2 — anything settable from an HA entity must be expressible in the bulk
document.** `apply_sets()` deletes the retained `set/` command once applied
(`main/mqtt_ha.c:312-323`), so an entity-only field has **no durable store
anywhere** and will be destroyed by the next application of the bulk document.
R2 is the mechanical reason R1 is not merely tidiness.

*Known asymmetry, accepted deliberately:* the two config channels are still
asymmetric — a per-field set is one-shot by design. That is sound for the timer
fields today, because the document can now express all of them and no longer
overrides by omission. R2 is what keeps it sound for the next field.

**R3 — no side-effecting call in a log-statement argument.** Log arguments are
evaluated conditionally at compile time on device as well as host (see HAZ-1),
so a side effect placed there is a Kconfig value away from disappearing.

---

## Closed

Kept as one-liners only because source comments name them; delete these once the
comments are reworded.

* **BUG-4 — the checked-in sweeps tested a hardcoded path, not the caller's
  tree.** The three cycle generators pinned `REPO` to this branch's worktree, so
  a sweep run from anywhere else silently swept that tree and reported a green
  control about code the reader was not looking at — and would have become
  permanently unrunnable the day the worktree was deleted. Fixed by deriving
  `REPO` from each generator's own location, the way `run.sh` already did, plus
  a `preflight()` that refuses to run when the derived path is not the root of a
  git working tree. Verified two-sided: all three sweeps still pass with
  unchanged case counts (29592 / 15032 / 121504), and a generator copied outside
  a repository now exits 1 with `is not a git working tree` where the old code
  would have swept the hardcoded path regardless. `BASE` is pinned to the
  annotated tags `difftest-base/cycle09|10|11` rather than bare short SHAs, so
  the baselines survive a branch deletion or a history rewrite; `preflight()`
  prints the exact `git tag -a` restore command if one goes missing.
* **HAZ-2 — cycle09 could not report trace truncation.** Closed in the same
  commit: cycle09 now carries the `g_overflow` counter cycles 10 and 11 have and
  prints `overflow=` in its result line, so `run.sh` no longer annotates every
  cycle09 line with `[overflow not reported]`. `TRACE_MAX` stays at 256 — the
  measured high-water mark over all 29592 cases is 18 events — and now carries a
  comment recording that the number is measured, and that the trace lives inline
  in `run_t` rather than being malloc'd, which is why it is smaller than the
  other two cycles' 8192.
* **BUG-6 — the bulk config document destroyed `break_eligible` on every
  apply.** Fixed in `733e86e`: `apply_timers()` starts from the stored table, so
  an absent optional key leaves an existing slot unchanged and means false only
  for a slot being defined for the first time; `docs/home_assistant.md`
  documents `break` and the optional-key rule. Named by `main/config_apply.c:192`
  and `test/test_config_apply/test_config_apply.c:334`. The durable lesson is
  **R2**.
* **Build hardening — `-Werror=unused-function` disarmed by IDF.** Closed in
  `1865a3f`, re-armed per-target on `main` and `components/ssd1680` so IDF's own
  components and managed dependencies are unaffected. Verified two-sided: the
  build passes as-is and fails when the `alerts_set_extend_awake()` install is
  deleted. Caveat: the backstop holds only while `extend_awake_failsafe` has
  exactly one reference.

---

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
