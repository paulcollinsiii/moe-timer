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

**Field result 2026-08-19 — S1.1 answered, in the affirmative.** The device
crossed genuine day rollovers at 01:28:46 EDT on 08-18 and 01:29:18 EDT on
08-19 (`active timer -> Screen` is `timer_reset()`), carrying an HA-configured
table written by an older image, and the timer names, minutes and
break-eligible switches were unchanged across both. HA logs state *changes*,
so the flags' absence from the activity stream across those boundaries is the
evidence: the padding reuse read correctly on a blob it did not write. The
rollover was the check this row existed for, and it passed twice.

S1.3 is **still owed** — see BUG-10 for the retraction of a first reading that
mistook an HA automation for a device-side revert. Nothing has yet provoked a
real blob loss on this device. S1.2, S1.4, S1.5 and S1.6 are also still owed.

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
| 0.5 | **BUG-10** — recurring PANIC resets on an idle device | A device that reboots itself several times a day is the most serious thing on this page, and the cause is unknown. Diagnostics first: inference from an HA activity stream has already produced one retracted answer, so the device needs to report what it was doing when it died | — |
| 1 | **BUG-7** — a RUNNING slot outliving its own definition | State-machine change to an uncovered path; independent | — |
| 2 | **BUG-2** | **BUG-3 is RESOLVED** (2026-09-16, M2-T4a/T4b) and is no longer part of this item — its condition fired when Button D gained a chore-ack arm, and the decision it was waiting for was made there with cases at both call sites. BUG-2 stands alone now, and still needs a re-baselined sweep to show the fix changed *only* the intended cases. | — |
| 3 | **BUG-5** — the v1→v2 migration | Only bites on a version bump, and **R4** means it has to be written *before* one rather than after. Nothing in flight bumps the version, which is why it sits last. | — |
| 4 | **BUG-11** — bed time is evaluated against an unvalidated clock | **No longer blocked, and no longer 0.6.** Both were derived from a mechanism review refuted on 2026-08-21: a panic does *not* clear the wall clock, so this is not downstream of BUG-10. The real trigger is a power-on reset alone, which is rare, and the fix needs a design decision rather than a patch. Settle the OPEN QUESTION in the entry before touching the code — it lives on the same path | — |
| 5 | **BUG-12** — a network window that ends before MQTT leaves the net phase slot stale | Found reviewing the BOOT subdivision. Cheap, but it sequences the window the panic measurement is being read from, so it wants its own pass rather than a ride-along | — |
| — | **BUG-1** | **Parked 2026-08-07.** Settling its fork needs an instrumented build run on hardware, which is reporter time rather than engineering time. Revisit after item 2: BUG-2's fix touches the same latch surface and may move the ground under it. | — |

*The constraint "BUG-2 and BUG-3 together" was discharged on 2026-09-16. BUG-3
was resolved on its own, by M2-T4b, and the check it asked for was made in the
direction that mattered: the latch pick is now a single shared helper
(`wake_flow_pick_latched_press`), so a later BUG-2 fix cannot move one mask
without moving the other. BUG-2 remains open and no longer waits on anything.*

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
`test_row6_b_press_while_not_running_is_eaten_KNOWN_BUG` in
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

**Status:** RESOLVED 2026-09-16 (M2-T4a/T4b) · **Severity:** none today, and the
condition below fired exactly as written — see the closing note at the end of
this entry before reading the rest of it as current.

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

**Closed (2026-09-16, M2-T4a/T4b).** The condition fired: M2-T4a bound BTN_D to
checkbox 3 of the chore checklist. The decision this entry asked for was made
deliberately and is **admit D to both picks, and only while the mode byte says
`APP_MODE_CHORES`** — `wake_flow_pick_latched_press()` is the single place both
call sites now get their mask from, so the two can no longer drift apart. The
shipped defect in between is the one this entry predicted: for the length of
M2-T4a, ✓1 and ✓2 were honoured from both drains and ✓3 was silently dropped,
including through `wake_flow_watch_break_end`'s ~250 ms tail poll — the break,
which is the window design §2.6 most wants the checklist live in.

One prediction did **not** hold, and it is worth recording because it was the
whole reason the entry stayed open: *"both masks become load-bearing"*. They did
not. D's TIMER action still lives only in the EXT1 decode and the shared
dispatch still has no `BTN_D` arm, so a D that leaks into the candidates in
Timers mode routes to `button_chore_ack_apply()`, whose first line refuses on
the same mode byte — inert, exactly as the dispatch's default arm was. What the
masks now decide is whether ✓3 *works*, not whether anything unsafe happens.

Cases, both call sites, per the entry's "a test either way":
`test_c4b_a_latched_d_press_in_the_break_tail_acks_row_3`,
`test_c4b_a_latched_d_press_in_chore_mode_acks_row_3`,
`test_c4b_a_latched_d_press_outside_chore_mode_is_still_dropped`,
`test_c4b_a_latched_b_press_outranks_a_latched_d_in_the_break_tail`, and the
renamed timer-leg case
`test_a_latched_d_press_in_timers_mode_is_never_dispatched_by_the_tick_drain`
(the old name asserted more than it tested once D gained an arm). Mutants
W17–W22 cover the mask, the mode gate, the routing, the row mapping and the
priority; all six are killed by the case named above as their killer.

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

## BUG-10 — recurring PANIC resets on an idle device

**Reported 2026-08-19 from the testing device, with an HA activity-stream
export.** The device panics on its own, unattended, several times a day.

| Panic (EDT) | Gap from previous |
|---|---|
| 08-18 01:49:04 | — |
| 08-18 22:47:54 | 20 h 58 m 49 s |
| 08-19 06:46:05 | 7 h 58 m 11 s |

Reset reason is `ESP_RST_PANIC` — an abort or a CPU exception. Not a watchdog
and not a brownout: `wake_flow.c:250-268` gives `ESP_RST_TASK_WDT`,
`ESP_RST_INT_WDT` and `ESP_RST_BROWNOUT` their own strings, and
`CONFIG_ESP_TASK_WDT_PANIC` is off.

**Not the OTA.** The OTA landed 08-18 16:02 UTC (`last_reset` -> `SW`,
`update_target` 1.5.1). The 08-18 05:49 UTC panic predates it, so the panics
are not something the new image introduced.

**Not deterministic on the rollover either.** The 08-18 panic followed that
day's 01:28:46 rollover by 20 minutes, but the 08-19 rollover at 01:29:18 was
followed by five clean hours. Three points is not a period; the gaps are 21 h
and 8 h, and the second device's stream may or may not agree.

### Retracted: the "silent NVS self-erase" reading of 2026-08-19

The first version of this entry concluded that the device had erased its own
NVS at 05:12 EDT via `main.c:410`, on the strength of four settings reverting
at once across two unrelated storage mechanisms. **That was wrong, and the
activity-stream CSV disproves it.** Recorded rather than deleted, because the
reasoning was sound given what it had and someone will otherwise re-derive it.

The four rows carry `context_event_type=call_service`,
`context_domain=homeassistant`, `context_service=turn_off`. Every state the
*device* publishes in the same export has empty context columns. A Home
Assistant automation or scene turned those switches off; the firmware never
published them. The same call recurs 24 h earlier at 08-18 09:10:51 UTC
against `ota_check_on_sync` alone — the only one of the four that was on at the
time — which makes it a daily ~05:11 EDT HA action, not a device event.

**The lesson worth keeping is a method one:** an HA activity stream carries
provenance, and it was available for the asking. The whole erase chain was
built on the unexamined premise that a state change in HA meant the device had
published it. Check the context columns before reasoning from a state change.

*Nothing in the retraction touches the two real findings the trace turned up,
which stand on their own reading of the source and are kept below.*

### Standing findings from the same trace

**F1 — consecutive panics are invisible.** `last_reset` is a state, so HA
collapses PANIC -> PANIC into one row and only a PANIC -> DEEPSLEEP -> PANIC
sequence is countable. The cadence above is therefore a lower bound. A
monotonic persisted panic counter would make the real rate visible; it is also
the cheapest thing on this list.

**F2 — a transient NVS read permanently rewrites the table.**
`config_apply.c:206` sets `have_prev = (nvs_config_get_timer_defs(&prev) ==
ESP_OK)`, so *any* failure — transient included — makes `existed` false for
every slot, drops `break`/`reload` to the compile-time answer, and line 355
then writes that back to flash. A read error and a genuinely-absent blob are
different events and must not share a branch; `ESP_ERR_NVS_NOT_FOUND` is the
only one that means "never configured". This is BUG-5's unconditional
constraint applied to the writer rather than the logger. Not implicated in
anything observed so far — found by reading, not by failing.

**F3 — the erase at `main.c:410` is silent.** Whether or not it has ever
fired, the most destructive act the firmware can perform logs nothing,
publishes nothing and leaves no counter, and the reseed then restores WiFi and
MQTT from compiled-in defaults so the device reconnects looking healthy. It
should not be possible for this to happen without saying so. `nvs_get_stats()`
would also settle the headroom question the S1 tail raised and never measured.

### What is being built

HA-visible diagnostics, approved 2026-08-19. The device has to say what it was
doing when it died, because inference from an activity stream has already
produced one wrong answer. See the implementor brief for scope.

**Landed.** `main/panic_diag.c` + `include/panic_diag.h`, host-tested in
`test/test_panic_diag`. Eleven new HA entities (discovery schema v19), +3,696 B
of image — 1,496,784 → 1,500,480 B, 81.8 % of the app slot.

* **How often.** `panic_cnt`, a monotonic `u32` in NVS bumped once per
  `ESP_RST_PANIC` boot and published as **Panic count**. `last_reset` is a
  *state*, so HA collapses PANIC → PANIC into one row and the rate was
  unknowable; a counter is differenceable between any two publishes.
* **Doing what.** A 24-byte `RTC_NOINIT_ATTR` breadcrumb carrying two phase
  slots (main task / network side), uptime, free heap and both task stack
  floors, re-sealed with a magic + FNV-1a checksum on every phase change. It
  is latched on the boot after a panic and copied to NVS, because RTC memory
  reaches the next boot only and this firmware does not open a network window
  on every wake. `RTC_DATA_ATTR` was not a candidate: it is zeroed by a panic
  reset. The guard is what makes `RTC_NOINIT_ATTR` safe here and is the whole
  difference from the timer-state case the project note rejected.
* **Phases:** BOOT / AWAKE / RENDER / SLEEP on the main slot, NET / OTA_CHECK /
  MQTT / OTA_DL on the network slot, published joined ("RENDER+OTA_CHECK").
  Two slots rather than one byte because a single byte would be written by
  whichever task moved last and would routinely report a network panic as a
  render.
* **F3's headroom question is now measured**, though F3 itself is untouched as
  briefed: `nvs_get_stats()` free entries is published every window as **NVS
  free entries**. Free only — total is a constant of a frozen partition table
  and used is total − free. The erase at `main.c:410` is still silent.

**Reviewed, and four defects in it fixed before it shipped.** Image is now
1,500,848 B, 81.8 %, 58,908 B under the guard; 32 host cases in
`test_panic_diag`, up from 24.

* **The breadcrumb could lie on the one path it was built for.**
  `panic_diag_exit()` restored the saved outer phase *unconditionally*, and
  enter/exit is a read-modify-write spanning the whole phase body — the
  spinlock makes each end atomic but cannot stop a third task moving the slot
  in between. The awake failsafe does exactly that: it fires on the esp_timer
  task and marks SLEEP while a `render()` started on `ota_dl` is still inside
  a 2–4 s e-ink refresh, and that refresh's exit then put AWAKE back over it.
  A panic in the sleep funnel would have published `AWAKE+OTA_DL`. Now a
  compare-and-restore (`panic_diag_rec_exit`): the slot is only rewound if it
  still holds the phase being left, and a refused exit touches nothing at all.
* **`stack_main` could carry another task's floor.** `render()` runs from
  `ota_dl` (16 KB stack) as well as the main task (7 KB), so a panic during a
  download published a high-water mark the main task cannot physically produce
  under the label "stack free (main)" — a number that sends the reader after a
  stack bug that is not there. A sample now declares whether the marking task
  owns the slot (`panic_sample_t::stack_foreign`); the phase still moves, only
  the stack figure is withheld.
* **A `_Static_assert` claimed a guarantee it does not provide.** It compares
  two hardcoded literals, not the phase table, so adding a longer phase name
  would have silently truncated the label to something that still reads like a
  valid phase in HA (`panic_diag_fill_stat` discards the `snprintf` return).
  The comment in `panic_diag.c` and the matching claim in `stats_json.h:79`
  both said otherwise. Both corrected, and the property is now pinned by
  `test_every_phase_pair_fits_the_published_field`, which walks the whole
  `PANIC_PHASE__COUNT²` cross-product through the real table.
* **Two publish paths dropped an oversized payload without a word.** The
  entity-table loop at `mqtt_ha.c:241` is where all eleven diagnostic entities
  land, and the stat publish has 86 B of headroom left at the measured 938 B
  worst case. Both now log the skip, as the action-discovery path beside them
  already did.
* **The stored phase numbers are now pinned by a test.** They reach a newer
  image off flash, so renumbering the enum decodes old records as the wrong
  phase — silently, and only on the devices that actually panicked. Every
  other test in the file survives a renumbering, which is the point.

**Follow-on, 2026-08-20: BOOT was too coarse to be a finding.** Every panic
observed so far reports `BOOT` with the net slot idle, and that phase spans
NVS init, the OTA rollback detector, `buttons_init`, `battery_init`, the LVGL
framebuffer, a heap check and `lock_gate_check_charge()` — which itself can run
a full e-ink refresh AND an entire network window without leaving BOOT. The
main slot now carries five appended sub-phases (`BOOT_NVS`, `BOOT_OTA`,
`BOOT_DISP`, `BOOT_BATT`, `BOOT_LOCK`, values 9-13) so the breadcrumb names the
init call rather than the window. Appended after `OTA_DL` rather than inserted
next to `BOOT`, because the values are stored in RTC memory and in an NVS blob
and reach a newer image off flash. A hand-flipped reset-loop soak harness
(`include/panic_soak.h`, `MAGTAG_PANIC_SOAK`, ships at 0) restarts at the end
of the BOOT window so the span can be exercised in minutes with a console
attached instead of once a night; it deliberately adds no Kconfig symbol,
because a reconfigure would rewrite this project's hand-maintained `sdkconfig`.
Image **1,501,408 B, 81.8 %** of the `ota_0` slot, 58,348 B under the 85 %
guard — the subdivision plus the review fixes below cost 928 B over the
1,500,480 B this entry measured for the feature itself. `test_panic_diag` is
36 host cases.

**Reviewed again, and three things fixed in it.** The review could not break
mark ordering, enter/exit pairing, the label width budget, cross-version record
decode or the slot map, and those stand. What it did find:

* **A genuine uninitialised read, introduced by the stack-ownership fix
  itself (`dc74ac6`), not by the subdivision.** `panic_diag_enter()` and `panic_diag_exit()` declare
  `panic_sample_t s;` as a bare local; `sample_now()` filled three of its four
  fields and `note_stack_ownership()` returns early — writing nothing — for
  every net-slot phase. `panic_diag_rec_mark()` reads `stack_foreign` on both
  sides, so whether `stack_net` was recorded at all was decided by whatever
  was on the stack. Fixed at the root: `panic_diag_sample_reset()` (pure,
  host-tested) zeroes the sample and `sample_now()` calls it first, so a field
  added later is covered by the same line. The host suite never saw it because
  every case passes an explicit sample; it now has one that walks the
  sampler's own build sequence over 0xFF-filled memory.
* **The 2 s panic-quiet window was described as "small".** It is
  allocation-light and 2,000 ms of blocking delay, on precisely the post-panic
  boots this feature measures, and it sits before the first new mark. It stays
  unmarked — its whole body is a `vTaskDelay`, so no panic can originate in it
  from the task a mark would describe — but `panic_diag.h` now says so, and
  says that a post-panic `panic_uptime_s` of 2 is that delay rather than time
  spent in NVS.
* **Three load-bearing comments were factually wrong.** `ota_flow_init()` does
  not read partition state and does not write NVS on an ordinary boot; the
  LVGL draw buffer is a 4,744 B `.bss` static rather than "the largest single
  allocation on the device" (the heap work in `display_init()` is `lv_init()`'s
  pool and `lv_display_create()`); and `panic_soak.h` claimed no render runs
  before AWAKE while `lock_gate_check_charge()` paints Charge Me! with a full
  refresh inside the same window. That last one carries an operational
  consequence the header now spells out: below `BATT_LOCK_PCT` the locked path
  never returns, so the `#if MAGTAG_PANIC_SOAK` block at the end of `app_main`
  is never reached and the soak silently becomes a 10-minute deep-sleep cycle.
  Run it on USB power with a charged battery.

One review claim was **rejected**: that the harness's own `esp_restart()`
re-arms the Charge Me! repaint guard every iteration because `s_charge_locked`
is `RTC_DATA_ATTR`. It does zero the flag, but every path that reaches the
restart left it `false` anyway — `lock_gate_check_charge()` deep-sleeps
without returning whenever the lock is engaged — so the paint is a one-off and
the stall is the whole of the hazard.

**Still unexplained by the breadcrumb: the sample is taken at the MARK, not at
the fault.** `panic_diag.c` fills uptime/heap/stack when a phase is entered and
nothing re-samples inside a phase, so the published figures describe the start
of the phase the device died in. Combined with `panic_uptime_s` being published
in whole seconds (`last->uptime_ms / 1000u`), a BOOT panic still publishes an
uptime of 0. The subdivision narrows WHERE; it does not improve WHEN. Left as
found — it is a real limitation of the design, not a defect in the
implementation, and the phase reading is the evidence being collected first.

**Deferred, deliberately: NVS write amplification in a panic loop.** Before
this feature a panic boot-loop cost zero NVS writes; each iteration now costs
a counter write plus a blob write, both committed. Minimum loop period is
~4.5–5 s, so a sustained loop is ~68k entries/day into six pages shared with
the WiFi stack. Raw endurance survives it (~3 years of *continuous* looping),
but the churn raises the odds of `ESP_ERR_NVS_NO_FREE_PAGES` — whose handler
is the silent erase of F3. The fix is cheap (skip the blob write when nothing
was latched; only rewrite when the record differs) and is **not** being made
yet, because the number that decides whether it matters does not exist: read
**NVS free entries** on the first window after this image lands, then decide.

## BUG-11 — bed time is evaluated against a clock nothing has validated

**Status:** OPEN — registered, not fixed ·
**Found:** 2026-08-20, by reading, while subdividing the BOOT panic phase ·
**Rewritten 2026-08-21**, after review refuted the mechanism the first version
claimed and with it the severity, the trigger set and the priority
**Severity:** one bed-time evaluation taken against an unvalidated clock, on a
**power-on reset only** — battery removal, full depletion, first power-up

### The defect

`lock_gate_check_bedtime()` (`main/lock_gate.c:107`) opens with

```c
if (!bedtime_active(time_util_minutes_of_day(now), config_cache_bedtime_minutes())) {
```

and `now` is `hal_time_now()`, handed in by the caller. Nothing anywhere on
that path asks whether the clock has ever been set. It is called from
`wake_flow_handle_timer_tick()` at `main/wake_flow.c:1051` — whose own comment
says it runs *before* the sync block — and again from
`wake_flow_handle_button_wake()` at `main/wake_flow.c:1205`.

**The OTA path has exactly the guard this one is missing.**
`ota_policy_check_gate()` refuses to act on an unvalidated clock:

```c
if (!in->time_valid)
    return OTA_REASON_NO_TIME;
```

`main/ota_policy.c:165`, against `ota_gate_in_t::time_valid`, documented at
`include/ota_policy.h:163` as *"NTP has set the clock this session"*. So the
firmware already holds the position that a decision taken against an unset
clock is not a decision. `lock_gate` has no equivalent, and it is making a
comparison — minutes-of-day against a configured window — that is *more*
sensitive to a wrong clock than the OTA gate is, not less.

### CORRECTION — the mechanism this entry was first built on is false

The first version claimed the IDF wall-clock offset "is cleared by any
non-deep-sleep reset", and from that derived a severity of *"one bed-time wake
lost after every panic — and after every OTA reboot, brownout and serial
reset"*. **It is not cleared by any of those.** Verified against this
checkout's toolchain, 2026-08-21:

* `sdkconfig` carries `CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y`.
* Under that symbol the epoch offset is written to RTC retention
  **registers**, not to `.rtc.bss`: `esp_time_impl_set_boot_time()` does
  `REG_WRITE(RTC_BOOT_TIME_LOW_REG, …)` / `REG_WRITE(RTC_BOOT_TIME_HIGH_REG, …)`
  — `components/esp_libc/src/port/esp_time_impl.c:72-73`.
* The startup zeroing is exactly one `memset`, over `_rtc_bss_start` ..
  `_rtc_bss_end`, and only when the reset reason is not a deep-sleep wake —
  `components/esp_system/port/cpu_start.c:468-469`. Retention registers are
  not in that range.

So the wall clock **survives** a panic reset, `esp_restart()`, a brownout and
a serial DTR/RTS reset. Only a genuine power-on reset — battery removed, cell
fully depleted, first power-up — loses it.

The same paragraph named `g_rtc_state.next_ntp_sync` as one of "two
independent pieces of state" that make `hal_time_now()` bogus. It is not one
of them at all. `hal_time_now()` is `return time(NULL);`
(`main/hal_time.c:8-10`) and never reads `g_rtc_state`. `next_ntp_sync` feeds
only `timer_needs_ntp_sync()` and `timer_last_ntp_sync()`
(`main/timer.c:723-739`), which are consumed at `wake_flow.c:1062` — *after*
the bed-time call at `:1051`. Losing it changes WHEN a sync is scheduled,
never WHAT the clock reads.

### What survives the correction, and it is still a defect

On a power-on reset the offset genuinely is gone, and the boot that follows
reads near-epoch until NTP lands. The bed-time check at `wake_flow.c:1051`
runs **before** the sync block on that same wake — its own call-site comment
says so — so exactly one bed-time evaluation is made against a clock nothing
has validated. `bedtime_active()` answers no, the lock does not engage, and
the wake takes an ordinary short sleep instead of `BEDTIME_SLEEP_SEC` (7200,
`include/sleep_plan.h:96`). It is a lost wake, not a permanent failure: the
next tick, with a synced clock, engages the lock normally.

Rare, and real. A device whose battery is pulled or run flat during the
evening comes back inside the bed-time window and skips it once. The missing
guard is the defect whether or not the trigger is common — the OTA gate holds
the opposite position ten files away.

### Priority, re-derived — said out loud rather than re-ranked silently

The old `0.6` slot and its **blocked by BUG-10** rationale were derived
entirely from the refuted linkage: *"a panic is precisely what invalidates the
clock, so the two are the same night's story."* With the linkage gone, that
rationale goes with it. A panic does not invalidate the clock, so fixing this
cannot confound the panic measurement through the mechanism the freeze was
protecting.

It is therefore **unblocked**, and it moves down the list on cost/benefit
rather than on dependency: a power-on reset landing inside the bed-time window
is far rarer than a panic, and the fix needs a design decision (below) rather
than a patch. The order-of-work table now carries it at **4**, with no
blocker.

One reason for care remains, and it is not the old one: the open question
below is unexplained, it lives on this same code path, and changing what
`lock_gate_check_bedtime()` does would destroy the evidence needed to settle
it. Settle the question first.

### OPEN QUESTION — bed time did not engage on the panic boots, and nothing here explains why

**Unresolved. Recorded as a question, not as evidence.** The first version of
this entry attributed two nights of 1-7 minute wake gaps to this defect. That
attribution does not follow and has been deleted: the clock survives a panic,
so those post-panic boots were not evaluating bed time against a bogus clock.

What is left is an observation with no explanation:

* The wall clock survives a panic (above), and TZ is applied **inside** the
  BOOT window — `setenv("TZ", …)` / `tzset()` at `main/main.c:487-488`, before
  the AWAKE mark — so a post-panic boot knows the local time.
* `s_bedtime_locked` is `RTC_DATA_ATTR` (`main/lock_gate.c:38`), so a panic
  zeroes it. `lock_gate.c:33-37` states the design intent for exactly this
  case: the gate recomputes from wall-clock time on every boot, so *"a hard
  reset cannot unlock the night — it merely replays the engage once."*
* At 23:21 local with bed time configured at 22:00, `bedtime_active(1401,
  1320)` is true. A post-panic boot there should have engaged: paint, alert,
  and `BEDTIME_SLEEP_SEC` (7200 s).
* On 2026-08-21 the device published at 03:21:48Z, panicked, and published
  again **82 seconds later**. That is impossible if it had engaged.

"The panicking boots never reach the check" does **not** close it: the boots
that published reached AWAKE, and a boot that reaches AWAKE has already run
`wake_flow_handle_timer_tick()` and therefore already evaluated bed time.

The leading remaining candidate is that **the stored bed-time value is not
what the operator believes it is.** `config_cache_bedtime_minutes()`
(`main/config_cache.c:44`) folds the raw HHMM and answers -1 for "disabled"
only when the stored value is a literal `0`; any *other* value that fails
validation falls back to `NVS_DEFAULT_BEDTIME` rather than disabling. Either
branch — a stored 0, or a bad edit folding to a default nobody expects —
produces a device that never locks at the hour the operator has in mind.

**The discriminator is cheap and needs no firmware change.** Read the bed-time
entity back out of Home Assistant and compare it against what is expected, and
watch whether the Bed Time screen appears at all on any night. A screen that
never appears while the read-back looks correct points somewhere else entirely
and this question stays open; a screen at the wrong hour, or a read-back that
disagrees with the operator, closes it as configuration.

### Fix shape, for whoever picks this up later

Not prescriptive, and recorded because the reasoning is cheap to lose:

* the input is the same one the OTA gate already computes; the question is
  whether `lock_gate` should take a `time_valid` argument or whether the two
  callers in `wake_flow.c` should hold the guard,
* *"has NTP ever set this clock"* and *"has NTP set it this session"* are
  different questions, and the OTA gate deliberately asks the second one. On a
  power-on reset the two coincide, which is a reason to be explicit about
  which is being asked rather than a reason it does not matter,
* and there is a real decision underneath about what bed time should DO when
  it cannot tell the time: skipping the lock (today's behaviour, by accident)
  and holding the previous lock state (which RTC memory cannot supply on the
  one reset that triggers this) are both defensible, which is a reason for the
  fix to be designed rather than patched.

**No pinning test yet, and that is a known gap.** The register's own rule is
that an open defect is pinned by a test asserting the current, wrong
behaviour. Write the pin with the fix, and flip it deliberately.

## BUG-12 — a network window that ends before MQTT leaves the net phase slot stale

**Status:** OPEN — found by review 2026-08-21, **not fixed in that pass** ·
**Severity:** a later main-task panic publishes a network phase that was over
before it happened — the breadcrumb misleading in exactly the direction the
two-slot design exists to prevent

### The defect

`net_window_task()` (`main/net_window.c`) marks the net slot three times and
clears it once:

* `panic_diag_enter(PANIC_PHASE_NET)` at `:52`, unconditionally,
* `panic_diag_enter(PANIC_PHASE_OTA_CHECK)` at `:124` and
  `panic_diag_enter(PANIC_PHASE_MQTT)` at `:126`, both inside
  `if (wifi_up) { … if (xQueueReceive(…)) { … } }`,
* and one exit at `:137` — `panic_diag_exit(PANIC_PHASE_MQTT, PANIC_PHASE_NONE)`.

`panic_diag_rec_exit()` is a **compare-and-restore**, deliberately and for
good reason: it is the fix that stopped the awake failsafe's SLEEP mark being
overwritten by a late render exit. It rewinds a slot only if that slot still
holds the phase being left. So the single exit clears the net slot only on the
path that actually reached MQTT. **When the window ends before MQTT the slot
is never cleared** and reads `NET` for the rest of the boot.

The reachable case is a failed association: `wifi_session_begin()` returns
non-`ESP_OK`, `wifi_up` is false, neither inner mark runs, and the exit at
`:137` compares `MQTT` against a slot holding `NET` and refuses. The comment
directly above that line names this exact case and claims the opposite —

> *"Placed after `wifi_session_end()` so the radio teardown is still covered,
> and outside the `wifi_up` branch so the no-wifi path clears it too."*

— and the no-wifi path is the one path it does not clear.

### Failure mode

The main task carries on and eventually marks `SLEEP`. A panic in the sleep
funnel then publishes **`SLEEP+NET`**: a network phase that ended, unentered,
minutes earlier, sitting beside a main-slot phase that is genuinely current.
`panic_diag.h`'s whole argument for two slots is that a reader must be able to
ask *"was it in the network path?"* from the label alone; a stale net slot
answers yes when the answer is no. On a device whose open question is
precisely *"is the OTA/TLS path implicated?"*, that is a false positive on the
hypothesis under test.

`OTA_CHECK` is **not** reachable as a stale value through ordinary control
flow — `panic_diag_enter(PANIC_PHASE_MQTT)` is the next statement after
`ota_flow_check()` returns. `NET` is the value that actually sticks.

### Why it was not fixed where it was found

Found while reviewing the BOOT subdivision (`9dfb2f9`), which is a diagnostics
change to a different file. The fix touches window sequencing on the path the
panic measurement is currently being read from, and it is not a one-liner: the
shape is either an exit paired with each enter, or one unconditional
"the window is over" clear that does not compare, and choosing between them is
a decision about what the net slot should mean when no window task is alive.
Registered rather than patched in a review-fix pass.

**No pinning test yet.** The pure layer is host-tested and behaves correctly;
what is missing is a test of `net_window_task`'s sequence, which has no host
harness today.

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
  lesson is **R3**. **No hardware row on purpose** — the archive entry argues
  why, and records that this image must reach the device by OTA rather than a
  USB reflash, which would reseed NVS and destroy S1's in-flight observation.
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
