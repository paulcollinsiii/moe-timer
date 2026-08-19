# Defect register — open work, and what is waiting on hardware

The working list of firmware defects. It began as the register of things found
while executing `docs/planning/implemented/20260729.refactormain.plan.md` and
deliberately **not** fixed during it. That refactor landed and merged (`5d837a6`
on `integration`), so the register became the task list for the phase after it.

**Cut back on 2026-08-18.** The closed and implemented entries, and the
reasoning that produced them, moved to
`docs/planning/implemented/20260818.bugregister.closed.md`. Two things are left
here:

1. **Implemented — awaiting hardware smoke test.** Fixes that are merged and
   host-verified but have never run on the device. A fix in this state is not
   finished. It is a well-evidenced claim, and the evidence stops at the host
   toolchain.
2. **Open defects.** Not fixed.

A row leaves section 1 when someone confirms it on hardware; the entry then
moves to the archive carrying the result. A row that fails its smoke test comes
back as an open defect rather than being amended in place, so the failure is
visible in the history.

The standing rules are at the top because they are the part worth reading before
touching anything below, and because everything else on this page is meant to
leave it.

Each open firmware defect below is still pinned by a test asserting the CURRENT,
WRONG behaviour, and **each fix must flip its pinning test deliberately, not
silently** — a fix that leaves its pin passing has not been demonstrated.

| Status | Meaning |
|---|---|
| SMOKE | Fixed and merged; host-verified only, never run on hardware |
| OPEN | Reproduced, not yet fixed |
| HYPOTHESIS | Observed on hardware, root cause not yet confirmed in code |
| PARKED | Real, but settling it needs hardware time rather than engineering time |
| CONDITIONAL | Not a bug today; becomes one if a named change lands |
| HAZARD | Not wrong today (verified); a named future change makes it wrong |

---

## Standing rules

These are the durable lessons, kept at the top because **everything below them
is meant to leave this file as it lands, and these are not.** Promote them into
`docs/home_assistant.md` (R1, R2) and the project conventions (R3, R4) before
this file is consumed.

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

**R3 — no function call at all in a log-statement argument.** Log arguments
are evaluated conditionally at compile time on device as well as host, so a
side effect placed there is a Kconfig value away from disappearing. The rule is
deliberately stronger than "no *side-effecting* call": purity is a judgement,
and a judgement made once per call site is the thing that failed here. It is
enforced by `scripts/check-log-args.py` from pre-commit, which also carries the
allowlist of pure formatters and the argument for each entry. See HAZ-1 in the
closed archive.

**R4 — a `TIMER_DEFS_BLOB_VERSION` bump must ship its migration in the same
firmware image.** This is not a style preference; it is what makes a migration
reachable at all. The first network window after a bump **destroys** the old
bytes: the table reads as unreadable, `config_apply`'s rebuild branch fires,
menuconfig answers every slot, and the result is written over the old data. A
migration's entire premise is reading those bytes and carrying the values
across, so bump first and migrate later and there is nothing left to migrate by
the time the migration ships — the fix for the data loss would arrive after the
data loss, on every device that took a network window in between. Shipping the
bump and the migration together is the only ordering in which the migration
does anything.

R4 is latent today; nothing in flight bumps the version. It is written down
because the branch that makes it true was added as a *fix* — recovering a
device whose per-timer controls were permanently dead — and does not look like
a hazard from the call site.

---

## Implemented — awaiting hardware smoke test

Merged and host-verified. **Never run on the device.** Until each row below is
confirmed on hardware it is a claim, not a result.

| # | What | Landed | Host evidence | What hardware has to settle |
|---|---|---|---|---|
| **S1** | BUG-8 — losing the timer-defs blob no longer cements a Kconfig `break_eligible`, and the per-timer controls stop refusing forever. Carries BUG-5 items 1–4. | `ad62dff` `858d41e` `de21436` `10745de` | 40/40 suites; firmware links at 1,496,784 B (81.6 % of the app slot); the padding-reuse premise replayed against the pre-fix boot sequence at `-O2` | Checks S1.1–S1.6 below |
| **S2** | The OTA update chain | `677f667` and the series under it | Suites green; size guard armed at 85 % | The 16-item list under **Hardware smoke test** in `docs/planning/ota.plan.md` — not duplicated here |

S1 and S2 are coupled in practice: S1 is intended to reach the device *as* the
first real OTA payload, so a failure is ambiguous until S2's item 4 has passed
at least once. Flash the base image over USB and confirm a plain OTA works
before reading anything into S1's results.

**Field status 2026-08-19 — the coupling is discharged.** The BUG-8 fix was
delivered to the device *by OTA* and is running correctly so far. That single
event settles the ambiguity above: the OTA chain carried a real payload
end-to-end, so S1's results can now be read at face value. It is not a pass for
either row yet — S2 still owes the rest of its 16-item list, and S1's decisive
check has not happened. **What is outstanding is the day rollover**, which is
when an HA-configured table either stays set or reverts; that is S1.1 and S1.3
observed in the field rather than provoked on a bench. Until that rollover is
seen, "working correctly" means nothing has gone visibly wrong, which is the
expected reading of a device that has not yet crossed the boundary the defect
lived on.

### S1 — the checks, most dangerous first

**S1.1 — an existing blob still reads.** This is the one that would be
catastrophic and silent, so do it first and on a device that already has an
HA-configured table. Boot the new image once and confirm the timer names,
minutes and break-eligible switches are **unchanged**. The `defined` byte was
placed in bytes that were previously implicit padding specifically so that
`sizeof` and every offset stay put; that was proven at the host and against the
original boot-write path, but only a device carrying a blob written by an older
image proves it end to end. A wrong answer here looks like every timer
reverting to menuconfig at once.

**S1.2 — boot writes nothing.** Lose the blob (erase NVS, or reflash with a
deliberate layout change) and boot. The log must carry
`no timer-defs blob stored; running on the compile-time table (not persisted)`
from `timer_defs.c`, the timers must run for that wake, and **a second boot must
log the same line again** — if the second boot is silent, something persisted
the table and the fix did not take.

**S1.3 — the canary.** The reported symptom. Set `break_eligible` OFF on two
timers from HA, then provoke a loss (a panic, or an NVS erase). Confirm the
switches do **not** silently come back ON. With a retained `timers` document
they should return to OFF; without one they should read as never-set rather
than as a Kconfig `n` presented as an operator choice.

**S1.4 — the controls stop refusing.** On a device with **no** blob at all —
one driven only from the HA per-timer controls, which is the operator profile
BUG-8 was reported from — move one control. Before the fix this was rejected
with `nodefs` permanently and invisibly, because nothing publishes the
`ha_config_set` ack. Confirm the value takes and survives a sleep/wake.

**S1.5 — the two warnings are distinguishable.** Provoke each and confirm they
differ: no blob at all (`no timer-defs blob stored`) versus a blob that fails
the version/size check (`timer-defs blob present but UNREADABLE (<esp_err>)`).
Both `timer_defs.c:167,170` and `ha_config.c:379,382` carry their own pair;
seeing only one pair is the expected result for a given wake, seeing neither is
not.

**S1.6 — the accepted edge, so it is not later reported as a regression.**
After S1.4's single control edit, change a menuconfig name or duration for a
**different** slot and reflash. The change should **not** take. The write-path
seed fills a name for every slot menuconfig names, so the first control edit
provisions the whole table and those bystanders read as already-defined
afterwards. Only a slot menuconfig leaves unnamed still falls through to the
compile-time rung. This is documented, not fixed — closing it needs a
blob-level "provenance is meaningful" flag in the one remaining reserve byte,
which is a second semantic byte and a design change nobody has asked for.

**Not settled by any of the above, and worth a number while the device is on
the bench:** NVS headroom. The `nvs` partition is `0x6000` — six 4 KB pages —
shared with the WiFi stack, and `timer_persist_save()` rewrites a blob from
four call sites on a device that wakes many times a day. The fix makes blob
loss *survivable*; it does nothing to make it rarer, and the loss rate has
never been measured. `nvs_get_stats()` would settle it.

---

## Open defects

### Order of work

Ordered by dependency first, then by cost. Only one genuine sequencing
constraint remains; everything else is independent and can be reordered freely.

| # | Item | Why here | Blocked by |
|---|---|---|---|
| 0 | **S1 + S2 smoke tests** | The only item that needs the device. Two merged fixes stay unconfirmed until it happens, and everything below is engineering time that can proceed in parallel | a USB flash, then an OTA |
| 1 | **BUG-7** — a RUNNING slot outliving its own definition | State-machine change to an uncovered path; independent | — |
| 2 | **BUG-2**, then **BUG-3** | Same latch/mask surface — fix together so each is checked against the other. Both need a re-baselined sweep to show the fix changed *only* the intended cases. | — |
| 3 | **BUG-5** — the v1→v2 migration | Only bites on a version bump, and **R4** means it has to be written *before* one rather than after. Nothing in flight bumps the version, which is why it sits last. | — |
| — | **BUG-1** | **Parked 2026-08-07.** Settling its fork needs an instrumented build run on hardware, which is reporter time rather than engineering time. Revisit after item 2: BUG-2's fix touches the same latch surface and may move the ground under it. | — |

**Constraint — BUG-2 and BUG-3 together.** They share a root shape and both
touch button-latch masks; a fix for either must be checked against the other
rather than applied in isolation.

*The earlier constraint "BUG-4 before BUG-2/BUG-3" was discharged on 2026-08-10
and is gone. The sweeps now derive the repository from their own location and
are pinned to `difftest-base/*` tags, so a pinned bug's fix can be re-baselined
and re-verified from any checkout.*

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

## BUG-5 — a timer-defs blob version bump still discards the user's table

**Status:** OPEN, narrowed · **Found:** 2026-08-07, hardware, build `59c3afa`
**Severity:** user-visible data loss, **one-shot per version bump** — an
HA-configured timer table reverts to compile-time defaults.

Three of the four pieces landed and are archived in
`docs/planning/implemented/20260818.bugregister.closed.md`: the boot overwrite
is gone, the silence is gone, and the layout is now pinned by `_Static_assert`s
that leave no implicit padding for a new field to land in. The sequencing
constraint this entry used to carry was promoted to **R4**, because it binds
anyone who bumps the version whether or not they have read this entry.

What is left is the migration, and the two things a fix for it has to respect:
where the durable copies actually live, and the reporter's stated policy — which
is stated over a *different* version axis than the one this defect is on.

**Still open:** the v1→v2 migration decided on 2026-08-07 (read a v1 blob, copy
the common prefix, default `break_eligible` from the Kconfig value, write back
as v2). Until it exists, a version bump still makes the stored table
unreachable — loud and recoverable-from-HA rather than silent and permanent,
but the values are not carried across. Note the word dropped from an earlier
revision of this sentence: the bump is **not** "non-destructive". See the
archive's *What landed* item 1 for why: the rebuild that makes a stranded
device recoverable does so by writing **over** the unreadable bytes, which is
what R4 exists to stop.

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


### Fix constraints

* **Migrate rather than discard. Decided 2026-08-07 (reporter).** Read a v1
  blob, copy the common prefix, default `break_eligible` from the Kconfig value
  for that slot, write back as v2. This is what makes the stated policy's
  second clause ("bug fixes land without changing the HA set config") actually
  hold.
* **Consequence of that decision, flagged deliberately — now obsolete:** the
  silent re-seed used to be the *only* mechanism by which device defaults ever
  beat the HA document. It was removed by `ad62dff` (BUG-8), so migrating no
  longer removes anything. `config_apply` skips when `cfg_ver` matches and
  applies when it does not (`main/config_apply.c`, the `strcmp(stored,
  ver_str)` branch — which since this entry's fix also rebuilds an unreadable
  timer table on a match), and never prefers its own defaults. "Device defaults
  win" therefore has **no** implementation at all today; menuconfig is a
  default consulted in place, never a stored value. If that clause of the
  stated policy is wanted for real, it is new work and must be built
  deliberately.
* **Whatever the policy, log it. DONE.** "Stale blob, config reset" and "no
  blob yet, seeding" are different events and must not share a silent code
  path. This constraint was unconditional and worth landing on its own even
  with the migration deferred, and that is exactly how it landed — see *What
  landed* (2).
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

## Closed — moved to the archive

Full detail, and the reasoning behind each, is in
`docs/planning/implemented/20260818.bugregister.closed.md`. These one-liners
stay here only because source comments still name the numbers; delete a line
once its comments are reworded.

* **BUG-4** — the checked-in sweeps derive the repository from their own
  location instead of a hardcoded path, and are pinned to `difftest-base/*`
  tags. `c9e62c8`. Named by `docs/planning/20260807.propertychecker.plan.md:117`.
* **HAZ-2** — cycle09 reports trace overflow. `c9e62c8`.
* **BUG-6** — an omitted optional timer flag no longer clears an HA-set value.
  `733e86e`. Named by `main/config_apply.c:477` and
  `test/test_config_apply/test_config_apply.c:334`. The durable lesson is **R2**.
* **Build hardening** — `-Werror=unused-function` re-armed per-target on `main`
  and `components/ssd1680`. `1865a3f`.
* **HAZ-1** — a function call in an `ESP_LOGx` argument list is now refused by
  `scripts/check-log-args.py` at pre-commit, and the sixteen that existed were
  hoisted to locals. `68121a8`. Named by the anchor comments on the three
  allowlisted formatters (`main/ota_policy.c`, `main/ota_url.c`,
  `main/wake_flow.c`) and by the hoist comments in seven files. The durable
  lesson is **R3**.
* **BUG-8** — losing the timer-defs blob no longer cements a Kconfig
  `break_eligible`. `ad62dff` `858d41e` `de21436` `10745de`. Named by
  `main/main.c:309` and `docs/planning/ota.plan.md:2070`. **Not finished:
  awaiting its hardware smoke test — see S1 above.**

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
