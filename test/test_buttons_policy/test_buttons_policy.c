#include <unity.h>

/* Single-TU compilation of the pure wake-source policy (no ESP-IDF headers) */
#include "../../main/buttons_policy.c"

void setUp(void) {}
void tearDown(void) {}

#define M(btn) ((uint8_t)(1u << (btn)))

/* NB: designated initializer — a field added to buttons_policy_in_t
   defaults to 0 here instead of failing to compile, which would quietly
   narrow the exhaustive sweeps below to that field's zero value. Any new
   field needs a parameter here too.

   The same trap lives in buttons.c's `maximal` literal, where a missing
   gate would narrow the fallback level scan below the set of pads the
   policy can arm. Both are designated initializers over the same struct;
   this one is caught by a failing sweep, that one by nothing. */
static uint8_t mask5(bool enable, bool swap_allowed, bool mode_toggle_allowed, bool chore_ack_allowed,
                     bool config_locked) {
    buttons_policy_in_t in = {
        .enable = enable,
        .swap_allowed = swap_allowed,
        .mode_toggle_allowed = mode_toggle_allowed,
        .chore_ack_allowed = chore_ack_allowed,
        .config_locked = config_locked,
    };
    return buttons_policy_wake_mask(&in);
}

/* The four-gate form the cases below were written against. config_locked
   is FALSE here for the same reason chore_ack_allowed is false in
   wake_mask_for() below: it is the state every device that is not broken
   is in, and it narrows the mask to one pad, so leaving it out of the
   baseline keeps those expectations about the gate each case drives. */
static uint8_t mask4(bool enable, bool swap_allowed, bool mode_toggle_allowed, bool chore_ack_allowed) {
    return mask5(enable, swap_allowed, mode_toggle_allowed, chore_ack_allowed, false);
}

/* The three-gate form the cases below this line were written against.
   Kept rather than rewritten into every call site because the chore-ack
   gate is FALSE on every device that is not on the chore screen — the
   whole fleet, most of the time — so "the ack gate is off" is the
   baseline those expectations are about, and spelling it out twenty
   times would bury the gate each case actually drives. The cases that do
   drive it call mask4() directly, and the two exhaustive sweeps were
   widened to the new dimension rather than left at this default. */
static uint8_t wake_mask_for(bool enable, bool swap_allowed, bool mode_toggle_allowed) {
    return mask4(enable, swap_allowed, mode_toggle_allowed, false);
}

/* ---- the arm/don't-arm decision --------------------------------------- */

/* A locked sleep arms nothing, whatever the timer gates say. */
void test_disabled_arms_nothing(void) {
    TEST_ASSERT_EQUAL_UINT8(0, wake_mask_for(false, true, true));
    TEST_ASSERT_EQUAL_UINT8(0, wake_mask_for(false, false, false));
}

/* The invariant the driver's early return leans on: an enabled sleep can
   never produce an empty mask, so mask == 0 means "locked", never "no
   button qualified". If this ever fails, the driver would silently stop
   arming wake sources on a normal sleep.

   Still true now that A is conditional, and the sweep is what says so:
   B and D are unconditional, so the mask cannot empty however the two
   gates are set. The guarantee is about B and D, never about A or C. */
void test_enabled_is_never_an_empty_mask(void) {
    for (int swap = 0; swap <= 1; swap++) {
        for (int mode = 0; mode <= 1; mode++) {
            for (int ack = 0; ack <= 1; ack++) {
                for (int cfg = 0; cfg <= 1; cfg++) {
                    /* The config-error lock narrows to D and D ALONE, so
                       it is swept here rather than excused: the guarantee
                       is "not empty", and a narrowing that reached zero
                       would make the driver's early return start meaning
                       something it does not mean. */
                    TEST_ASSERT_NOT_EQUAL_UINT8(0, mask5(true, swap, mode, ack, cfg));
                }
            }
        }
    }
}

/* ---- the config-error lock's mask -------------------------------------

   Design 5.3: this lock sleeps charge-lock style but keeps Button D alive
   so a parent can force the corrected config in immediately instead of
   waiting out an interval. D and NOTHING else — B is unconditional
   everywhere above this line and has to be dropped here explicitly, which
   is why the narrowing lives in the policy rather than in the three gates.

   THE SWEEP IS THE POINT, not ceremony: the driver hands all three gates
   their honest answers now (it used to short-circuit A and C to false as
   well, spelling the rule a second time where no suite could reach it),
   so these eight combinations are the whole of what the policy has to
   drop, and this case is the only thing holding it. */
void test_a_config_locked_sleep_arms_button_d_alone(void) {
    for (int swap = 0; swap <= 1; swap++) {
        for (int mode = 0; mode <= 1; mode++) {
            for (int ack = 0; ack <= 1; ack++) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(M(BTN_D), mask5(true, swap, mode, ack, true),
                                                "a config-locked sleep armed something other than D alone");
            }
        }
    }
}

/* The narrowing must not outrank the arm decision: the charge and
   bed-time locks arm nothing at all, and a device that is BOTH config-
   locked and charge-locked takes the charge lock's answer (sleep_plan.c
   settles which mode wins; this says the mask agrees). */
void test_a_config_locked_sleep_still_arms_nothing_when_disabled(void) {
    TEST_ASSERT_EQUAL_UINT8(0, mask5(false, true, true, true, true));
    TEST_ASSERT_EQUAL_UINT8(0, mask5(false, false, false, false, true));
}

/* ---- which buttons qualify -------------------------------------------- */

/* A is the mode toggle (design 4.2), and it is a wake source only when a
   press would actually do something. Both of its refusal reasons — the
   active slot is RUNNING, and no chores are configured — arrive here
   already folded into one gate by button_a_toggle_allowed(), the same
   answer the dispatch's A arm acts on. A press that could only be refused
   must not cost a wake, a panel refresh and the battery behind it. */
void test_a_follows_mode_toggle_allowed(void) {
    TEST_ASSERT_TRUE(wake_mask_for(true, true, true) & M(BTN_A));
    TEST_ASSERT_FALSE(wake_mask_for(true, true, false) & M(BTN_A));
}

/* A's gate is A's alone: C's must not move it, in either direction. The
   cheap wrong implementation folds the two conditional buttons together
   (both are "refused while RUNNING"), and swap_allowed carries an extras
   condition A explicitly does not have — a device with no extra timers
   configured would then lose the chore screen along with Button C. */
void test_a_does_not_follow_the_swap_gate(void) {
    TEST_ASSERT_TRUE(wake_mask_for(true, false, true) & M(BTN_A));
    TEST_ASSERT_FALSE(wake_mask_for(true, true, false) & M(BTN_A));
}

/* A locked sleep arms nothing even when the toggle WOULD be allowed: A
   must be absent for the "arm nothing" reason as well as the "did not
   qualify" one. */
void test_a_still_does_not_wake_on_a_locked_sleep(void) {
    TEST_ASSERT_FALSE(wake_mask_for(false, true, true) & M(BTN_A));
}

/* B (start/pause/resume/reload) and D (refresh) are unconditional wake
   sources — they are what makes the never-empty invariant above hold. */
void test_b_and_d_always_wake(void) {
    for (int swap = 0; swap <= 1; swap++) {
        for (int mode = 0; mode <= 1; mode++) {
            for (int ack = 0; ack <= 1; ack++) {
                uint8_t m = mask4(true, swap, mode, ack);
                TEST_ASSERT_TRUE(m & M(BTN_B));
                TEST_ASSERT_TRUE(m & M(BTN_D));
            }
        }
    }
}

/* ---- C's second reason to wake: the middle checkbox (design 2.4) ------- */

/* THE CASE THAT KEEPS THE MIDDLE CHECKBOX ALIVE. In chore mode B, C and D
   are the three ack buttons, and B and D are unconditional — so if C kept
   only its swap gate, a device with no extra timers configured (where
   swap_allowed is false forever) would have a working checkbox 1 and 3
   and a dead checkbox 2, reachable only by waking the device some other
   way first. That is the exact "primary control dead to the press"
   failure this module's policy exists to avoid. */
void test_c_wakes_for_a_chore_ack_with_no_swap_available(void) {
    TEST_ASSERT_TRUE(mask4(true, false, false, true) & M(BTN_C));
    TEST_ASSERT_TRUE(mask4(true, false, true, true) & M(BTN_C));
}

/* The two reasons are an OR and neither is the other's precondition: a
   swap with no ack still wakes C (the timer screen, unchanged), and an
   ack with no swap wakes it too (the case above). With neither, C sleeps. */
void test_c_sleeps_only_when_neither_a_swap_nor_an_ack_is_available(void) {
    TEST_ASSERT_FALSE(mask4(true, false, false, false) & M(BTN_C));
    TEST_ASSERT_TRUE(mask4(true, true, false, false) & M(BTN_C));
}

/* The ack gate is C's alone. A must not follow it — A's own gate already
   covers the chore screen and is refused for different reasons — and B
   and D are unconditional, so the whole-mask form is what says so. */
void test_the_ack_gate_moves_only_button_c(void) {
    TEST_ASSERT_EQUAL_UINT8(M(BTN_B) | M(BTN_C) | M(BTN_D), mask4(true, false, false, true));
    TEST_ASSERT_EQUAL_UINT8(M(BTN_A) | M(BTN_B) | M(BTN_C) | M(BTN_D), mask4(true, false, true, true));
}

/* A locked sleep arms nothing, the ack gate included: the chore screen is
   not a reason to spend a refresh the battery cannot afford. */
void test_an_ack_does_not_wake_on_a_locked_sleep(void) {
    TEST_ASSERT_EQUAL_UINT8(0, mask4(false, false, false, true));
}

/* C is the next-timer button: no wake when a swap would be refused. The
   mode gate is driven apart underneath it for the same reason A's case
   drives the swap gate apart — neither conditional button may read the
   other's answer. */
void test_c_follows_swap_allowed(void) {
    TEST_ASSERT_TRUE(wake_mask_for(true, true, false) & M(BTN_C));
    TEST_ASSERT_FALSE(wake_mask_for(true, false, true) & M(BTN_C));
}

/* The whole mask, not just the bit under test: each gate moves its own
   button and nothing else, across the full cross product. */
void test_mask_is_exactly_the_qualifying_buttons(void) {
    TEST_ASSERT_EQUAL_UINT8(M(BTN_A) | M(BTN_B) | M(BTN_C) | M(BTN_D), wake_mask_for(true, true, true));
    TEST_ASSERT_EQUAL_UINT8(M(BTN_B) | M(BTN_C) | M(BTN_D), wake_mask_for(true, true, false));
    TEST_ASSERT_EQUAL_UINT8(M(BTN_A) | M(BTN_B) | M(BTN_D), wake_mask_for(true, false, true));
    TEST_ASSERT_EQUAL_UINT8(M(BTN_B) | M(BTN_D), wake_mask_for(true, false, false));
}

/* Bit n must be button n — the driver indexes BTN_GPIOS with the same n it
   tests the bit for, so a transposition arms the wrong pads. Literal
   expectations on purpose: unlike test_mask_is_exactly_the_qualifying_buttons
   above, which composes M(BTN_x) and so follows the enum wherever it goes,
   these values are only correct for one assignment of buttons to bit
   positions. Coverage is recorded rather than assumed here, because it is
   no longer total: with TWO independent gates the rows below separate A,
   C and the unconditional pair from each other, so five of the six
   transpositions surface — A<->B as 0x09, A<->C as 0x0b where 0x0e is
   expected, A<->D as 0x03, B<->C as 0x0c, C<->D as 0x06. B<->D does NOT —
   both are unconditional, so the mask cannot encode any difference
   between them. That gap opened when B became unconditional (the old
   reload_allowed gate had caught it) and is inherent to the policy rather
   than a defect here; it leaves the driver seam named in buttons.c — that
   the pad loop indexes BTN_GPIOS with the same bit the policy set —
   resting on review alone for that one permutation. A<->C used to be in
   that gap too and is no longer: it closed when A gained a gate of its
   own, which is why the third row below exists. */
void test_bit_positions_match_the_button_ids(void) {
    TEST_ASSERT_EQUAL_UINT8(0x0a, wake_mask_for(true, false, false)); /* B|D */
    TEST_ASSERT_EQUAL_UINT8(0x0e, wake_mask_for(true, true, false));  /* B|C|D */
    TEST_ASSERT_EQUAL_UINT8(0x0b, wake_mask_for(true, false, true));  /* A|B|D */
    TEST_ASSERT_EQUAL_UINT8(0x0f, wake_mask_for(true, true, true));   /* A|B|C|D */
    TEST_ASSERT_EQUAL_UINT8(0x00, wake_mask_for(false, true, true));  /* locked: nothing */
}

/* ---- BOOT (GPIO0) wake arming -----------------------------------------

   buttons_policy_boot_wake_allowed() is a separate decision from the mask
   above: BOOT is not a button_id_t and never occupies a bit in it
   (buttons.h). It reads only two fields -- config_locked (shared with the
   mask's D-only narrowing; clock_locked narrows the mask but leaves BOOT
   armed) and boot_currently_down (BOOT's own gate, which
   stops a level-triggered re-wake loop while BOOT is held) -- and the sweep at the end of this
   section is what proves the other three gates cannot move it. */
static buttons_policy_in_t boot_in(bool enable, bool config_locked, bool boot_currently_down) {
    buttons_policy_in_t in = {
        .enable = enable,
        .config_locked = config_locked,
        .boot_currently_down = boot_currently_down,
    };
    return in;
}

void test_boot_armed_on_an_unlocked_sleep_not_currently_down(void) {
    buttons_policy_in_t in = boot_in(true, false, false);
    TEST_ASSERT_TRUE(buttons_policy_boot_wake_allowed(&in));
}

/* Finding 1: EXT1 is level-triggered, so arming BOOT while it already
   reads low would wake the device the instant it reached deep sleep --
   and keep doing so for as long as the press lasted, since nothing
   resolves a BOOT-only wake into a button the continuation guard
   recognizes (that guard, wake_flow.c, tracks only A-D). Refusing to arm
   it while held is the fix: it never gets back into the mask to repeat. */
void test_boot_not_armed_while_currently_down(void) {
    buttons_policy_in_t in = boot_in(true, false, true);
    TEST_ASSERT_FALSE(buttons_policy_boot_wake_allowed(&in));
}

/* The config-error lock's one exit stays D alone: its fix is made in HA, and
   BOOT does not get a second escape hatch stacked on top of it, whatever
   GPIO0 happens to read. */
void test_boot_not_armed_on_a_config_locked_sleep(void) {
    buttons_policy_in_t in = boot_in(true, true, false);
    TEST_ASSERT_FALSE(buttons_policy_boot_wake_allowed(&in));
    TEST_ASSERT_EQUAL_UINT8(1u << BTN_D, buttons_policy_wake_mask(&in));
}

/* The no-clock lock is different: D alone among A-D, and BOOT as well. A
   device whose stored WiFi broke and then lost power has only BOOT to get
   back into setup. */
void test_a_no_clock_sleep_arms_d_and_boot(void) {
    buttons_policy_in_t in = {.enable = true,
                              .swap_allowed = true,
                              .mode_toggle_allowed = true,
                              .chore_ack_allowed = true,
                              .clock_locked = true};
    TEST_ASSERT_EQUAL_UINT8(1u << BTN_D, buttons_policy_wake_mask(&in));
    TEST_ASSERT_TRUE(buttons_policy_boot_wake_allowed(&in));
}

/* ...still never while BOOT already reads low (the level-triggered re-wake
   loop), and never when the sleep arms nothing. */
void test_a_no_clock_sleep_keeps_boots_other_two_rules(void) {
    buttons_policy_in_t held = {.enable = true, .clock_locked = true, .boot_currently_down = true};
    TEST_ASSERT_FALSE(buttons_policy_boot_wake_allowed(&held));
    buttons_policy_in_t off = {.enable = false, .clock_locked = true};
    TEST_ASSERT_FALSE(buttons_policy_boot_wake_allowed(&off));
    TEST_ASSERT_EQUAL_UINT8(0x00, buttons_policy_wake_mask(&off));
}

/* Both locks standing: the config-error lock's rule wins for BOOT. */
void test_both_locks_standing_keep_boot_dark(void) {
    buttons_policy_in_t in = {.enable = true, .config_locked = true, .clock_locked = true};
    TEST_ASSERT_FALSE(buttons_policy_boot_wake_allowed(&in));
    TEST_ASSERT_EQUAL_UINT8(1u << BTN_D, buttons_policy_wake_mask(&in));
}

/* The charge and Bed Time locks arm nothing at all -- BOOT included,
   same as every button in buttons_policy_wake_mask() above. */
void test_boot_not_armed_when_the_sleep_is_disabled(void) {
    buttons_policy_in_t in = boot_in(false, false, false);
    TEST_ASSERT_FALSE(buttons_policy_boot_wake_allowed(&in));
    buttons_policy_in_t locked_too = boot_in(false, true, true);
    TEST_ASSERT_FALSE(buttons_policy_boot_wake_allowed(&locked_too));
}

/* BOOT's arm decision must not move with the three per-button gates: it
   has no action of its own for swap_allowed, mode_toggle_allowed or
   chore_ack_allowed to gate. */
void test_boot_arm_decision_ignores_the_per_button_gates(void) {
    for (int swap = 0; swap <= 1; swap++) {
        for (int mode = 0; mode <= 1; mode++) {
            for (int ack = 0; ack <= 1; ack++) {
                buttons_policy_in_t in = {
                    .enable = true,
                    .swap_allowed = swap,
                    .mode_toggle_allowed = mode,
                    .chore_ack_allowed = ack,
                    .config_locked = false,
                    .boot_currently_down = false,
                };
                TEST_ASSERT_TRUE_MESSAGE(buttons_policy_boot_wake_allowed(&in),
                                         "a per-button gate moved BOOT's own arm decision");
            }
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_arms_nothing);
    RUN_TEST(test_enabled_is_never_an_empty_mask);
    RUN_TEST(test_a_config_locked_sleep_arms_button_d_alone);
    RUN_TEST(test_a_config_locked_sleep_still_arms_nothing_when_disabled);
    RUN_TEST(test_a_follows_mode_toggle_allowed);
    RUN_TEST(test_a_does_not_follow_the_swap_gate);
    RUN_TEST(test_a_still_does_not_wake_on_a_locked_sleep);
    RUN_TEST(test_b_and_d_always_wake);
    RUN_TEST(test_c_follows_swap_allowed);
    RUN_TEST(test_c_wakes_for_a_chore_ack_with_no_swap_available);
    RUN_TEST(test_c_sleeps_only_when_neither_a_swap_nor_an_ack_is_available);
    RUN_TEST(test_the_ack_gate_moves_only_button_c);
    RUN_TEST(test_an_ack_does_not_wake_on_a_locked_sleep);
    RUN_TEST(test_mask_is_exactly_the_qualifying_buttons);
    RUN_TEST(test_bit_positions_match_the_button_ids);
    RUN_TEST(test_boot_armed_on_an_unlocked_sleep_not_currently_down);
    RUN_TEST(test_boot_not_armed_while_currently_down);
    RUN_TEST(test_boot_not_armed_on_a_config_locked_sleep);
    RUN_TEST(test_a_no_clock_sleep_arms_d_and_boot);
    RUN_TEST(test_a_no_clock_sleep_keeps_boots_other_two_rules);
    RUN_TEST(test_both_locks_standing_keep_boot_dark);
    RUN_TEST(test_boot_not_armed_when_the_sleep_is_disabled);
    RUN_TEST(test_boot_arm_decision_ignores_the_per_button_gates);
    return UNITY_END();
}
