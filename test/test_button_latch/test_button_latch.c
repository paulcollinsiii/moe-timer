#include <stdint.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/button_latch.c"

/* Arbitrary microsecond base; the latch only compares differences. */
#define T0_US ((int64_t)1000000000)

void setUp(void) {
    button_latch_reset();
}

void tearDown(void) {}

void test_take_returns_zero_when_nothing_latched(void) {
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_record_then_take_returns_bit_and_clears(void) {
    button_latch_record(0, T0_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take());
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take()); /* consumed */
}

void test_multiple_buttons_combine_into_one_mask(void) {
    button_latch_record(0, T0_US);
    button_latch_record(3, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8((1u << 0) | (1u << 3), button_latch_take());
}

void test_edges_within_debounce_window_are_ignored(void) {
    /* Contact chatter on the PRESS: only the first edge of a burst latches,
       and after a take chatter still inside the window must not re-latch.
       Chatter is what this window is for and all it is for — bounce on the
       RELEASE is a hold duration away and is the release gate's job. */
    button_latch_record(1, T0_US);
    button_latch_record(1, T0_US + BUTTON_LATCH_DEBOUNCE_US / 2);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
    button_latch_record(1, T0_US + BUTTON_LATCH_DEBOUNCE_US - 1);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* A SECOND PRESS NEEDS BOTH: past the debounce window AND a release seen
   in between. The debounce half is what this case was before M2-T12, and on
   its own it accepted the release bounce of the first press as a second one
   — the bounce lands a whole hold duration after the press edge, so it
   always clears the window. `+ 2 *` rather than `+`: the sample sits AT
   T0 + one window, and the release observation is its own anchor
   (BUTTON_LATCH_RELEASE_SETTLE_US), so an edge at the same instant would be
   rejected by that even though it clears this one. */
void test_edge_beyond_debounce_window_and_past_a_release_is_accepted(void) {
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(1, T0_US + 2 * BUTTON_LATCH_REFRACTORY_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

/* THE SAME-BUTTON REFRACTORY WINDOW. Level samples can land anywhere in the
   wake button's release bounce, so this window is what says one physical tap
   is one press. It is measured from the last ACCEPTED press of
   that button, so it holds even when the release gate is already open — which
   is exactly the case a sample landing in a high phase of the bounce train
   creates. Here the gate IS open (released at +20 ms, settle long over) and
   the edge is still refused. */
void test_a_same_button_edge_inside_the_refractory_window_is_ignored(void) {
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 20000);
    button_latch_record(1, T0_US + BUTTON_LATCH_REFRACTORY_US - 1);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_a_same_button_edge_past_the_refractory_window_is_accepted(void) {
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 20000);
    button_latch_record(1, T0_US + BUTTON_LATCH_REFRACTORY_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

/* The lag being removed: a different button right after the first is
   accepted at once, with no window of its own in the way. */
void test_a_different_button_is_accepted_immediately_after_a_press(void) {
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 5000);
    button_latch_record(2, T0_US + 20000);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

/* The window is a per-button fact: press C, then B, and B's window does not
   start until B is accepted, nor does C's end early because B arrived. */
void test_each_buttons_refractory_window_is_its_own(void) {
    button_latch_record(2, T0_US);
    button_latch_record(1, T0_US + 100000);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 150000);
    button_latch_record(2, T0_US + 250000); /* inside C's window */
    button_latch_record(1, T0_US + 350000); /* inside B's window */
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
    button_latch_record(2, T0_US + BUTTON_LATCH_REFRACTORY_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

/* THE WAKE PRESS IS SEEDED. It is consumed by wake handling and never passes
   through button_latch_record, so without a seed its own release bounce
   meets an empty window. The seed starts the window and arms nothing: no
   bit is latched, since the wake press is acted on elsewhere. */
void test_a_seeded_wake_press_opens_a_refractory_window_without_latching(void) {
    button_latch_seed_press(1, T0_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
    button_latch_note_levels(0, T0_US + 1000); /* the tap was already over at boot */
    button_latch_record(1, T0_US + 100000);    /* its release bounce */
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
    button_latch_record(1, T0_US + BUTTON_LATCH_REFRACTORY_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

void test_a_seeded_wake_press_does_not_delay_other_buttons(void) {
    button_latch_seed_press(1, T0_US);
    button_latch_note_levels(0, T0_US + 1000);
    button_latch_record(2, T0_US + 20000);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

void test_seeding_no_button_or_a_bad_one_is_a_no_op(void) {
    button_latch_seed_press(-1, T0_US);
    button_latch_seed_press(4, T0_US);
    button_latch_note_levels(0, T0_US + 1000);
    button_latch_record(0, T0_US + 20000);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take());
}

void test_reset_forgets_a_seeded_window(void) {
    button_latch_seed_press(3, T0_US);
    button_latch_reset();
    button_latch_record(3, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 3, button_latch_take());
}

/* button_latch_begin IS THE WAKE'S ARMING SEQUENCE: reset, seed the wake
   press, take the first level sample, in that order. buttons.c calls only it,
   so these cases are the host coverage for the order and for the seed being
   there at all. */
void test_begin_seeds_the_wake_buttons_refractory_window(void) {
    /* Dropping the seed leaves only the settle anchor from the sample, which
       a bounce edge 100 ms later clears. */
    button_latch_begin(1, 0, T0_US);
    button_latch_record(1, T0_US + 100000);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
    button_latch_record(1, T0_US + BUTTON_LATCH_REFRACTORY_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

void test_begin_samples_after_seeding_not_before(void) {
    /* Seeding AFTER the sample would close the gate on a pad the sample saw
       up, and nothing later would reopen it: the first real press past the
       window would be lost. */
    button_latch_begin(1, 0, T0_US);
    button_latch_record(1, T0_US + 2 * BUTTON_LATCH_REFRACTORY_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

void test_begin_with_the_wake_button_still_held_keeps_its_gate_closed(void) {
    button_latch_begin(1, 1u << 1, T0_US);
    button_latch_record(1, T0_US + 2 * BUTTON_LATCH_REFRACTORY_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take()); /* still down: bounce */
    button_latch_note_levels(0, T0_US + 3 * BUTTON_LATCH_REFRACTORY_US);
    button_latch_record(1, T0_US + 4 * BUTTON_LATCH_REFRACTORY_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

void test_begin_resets_whatever_an_earlier_wake_left(void) {
    button_latch_record(2, T0_US);
    button_latch_begin(1, 0, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
    button_latch_record(2, T0_US + 20000); /* the old window is gone */
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

void test_begin_with_no_wake_button_seeds_nothing(void) {
    button_latch_begin(-1, 0, T0_US);
    button_latch_record(1, T0_US + 20000);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

/* THE PER-BUTTON REFRACTORY WINDOW (Button C's counted clicks). A button's
   window defaults to BUTTON_LATCH_REFRACTORY_US, and button_latch_set_refractory
   changes ONE button's. The figure in force is stamped when a press is
   ACCEPTED, so a change never reaches back into a window already running. */
#define SHORT_WINDOW_US BUTTON_LATCH_SWAP_REFRACTORY_US

void test_every_button_defaults_to_the_long_refractory_window(void) {
    for (int b = 0; b < 4; b++) {
        button_latch_reset();
        button_latch_record(b, T0_US);
        button_latch_take();
        button_latch_note_levels(0, T0_US + 20000);
        button_latch_record(b, T0_US + BUTTON_LATCH_REFRACTORY_US - 1);
        TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
        button_latch_record(b, T0_US + BUTTON_LATCH_REFRACTORY_US);
        TEST_ASSERT_EQUAL_UINT8(1u << b, button_latch_take());
    }
}

void test_a_shorter_window_lets_a_faster_same_button_press_through(void) {
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    button_latch_record(2, T0_US); /* accepted under the short window */
    button_latch_take();
    button_latch_note_levels(0, T0_US + 60000); /* released, and seen */
    button_latch_record(2, T0_US + SHORT_WINDOW_US - 1);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take()); /* still inside it */
    button_latch_record(2, T0_US + SHORT_WINDOW_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take()); /* a 300 ms window would have refused this */
}

void test_a_window_change_touches_only_the_named_button(void) {
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    for (int b = 0; b < 4; b++) {
        if (b == 2)
            continue;
        button_latch_reset();
        button_latch_set_refractory(2, SHORT_WINDOW_US);
        button_latch_record(b, T0_US);
        button_latch_take();
        button_latch_note_levels(0, T0_US + 20000);
        button_latch_record(b, T0_US + SHORT_WINDOW_US + 1000);
        TEST_ASSERT_EQUAL_UINT8(0, button_latch_take()); /* still the 300 ms window */
        button_latch_record(b, T0_US + BUTTON_LATCH_REFRACTORY_US);
        TEST_ASSERT_EQUAL_UINT8(1u << b, button_latch_take());
    }
}

void test_a_window_change_does_not_reach_into_a_window_already_running(void) {
    button_latch_record(2, T0_US); /* accepted under the long window */
    button_latch_take();
    button_latch_note_levels(0, T0_US + 20000);
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    button_latch_record(2, T0_US + SHORT_WINDOW_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_restoring_the_default_puts_the_long_window_back_for_the_next_press(void) {
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    button_latch_set_refractory(2, BUTTON_LATCH_REFRACTORY_US);
    button_latch_record(2, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 20000);
    button_latch_record(2, T0_US + SHORT_WINDOW_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_a_nonsense_window_or_button_is_ignored(void) {
    button_latch_set_refractory(-1, SHORT_WINDOW_US);
    button_latch_set_refractory(4, SHORT_WINDOW_US);
    button_latch_set_refractory(2, 0);  /* zero would switch the guard off */
    button_latch_set_refractory(2, -5); /* so would a negative */
    button_latch_record(2, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 20000);
    button_latch_record(2, T0_US + 50000);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take()); /* the default is still there */
}

void test_reset_restores_every_default_window(void) {
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    button_latch_begin(-1, 0, T0_US); /* a new wake */
    button_latch_record(2, T0_US + 20000);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take()); /* accepted: the test is live */
    button_latch_note_levels(0, T0_US + 40000);
    button_latch_record(2, T0_US + 20000 + SHORT_WINDOW_US + 5000);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take()); /* the long window, not the short one */
}

/* THE FAILURE THE SHORT WINDOW MUST NOT BUY: ONE TAP OF C COUNTED AS TWO,
   which in the gesture is skipping two timers instead of one.

   The wake press is SEEDED, and a seed always opens the DEFAULT window,
   whatever the setter says — so the case below sets the short window FIRST
   (the worst order) and checks the release bounce is still refused. The
   bounce comes after a level sample reopened the gate (the pad read up in a
   high phase of the ringing), past the short window and past the 10 ms
   settle anchor of that sample: only the seeded long window rejects it. */
void test_a_seeded_wake_press_keeps_the_long_window_when_the_short_one_is_set(void) {
    /* The setter FIRST, then the seed: the order the wake does NOT use
       (arming runs before the gesture installs anything), and the only one in
       which a seed that took the configured window would differ. */
    button_latch_reset();
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    button_latch_seed_press(2, T0_US);
    button_latch_note_levels(0, T0_US + SHORT_WINDOW_US + 10000); /* up, mid-ring: the gate reopens */
    button_latch_record(2, T0_US + SHORT_WINDOW_US + 25000);      /* 15 ms on: past the settle AND the short window */
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());              /* one tap is one press */
    button_latch_record(2, T0_US + BUTTON_LATCH_REFRACTORY_US);   /* a real second tap, past the long window */
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

/* ...and in the order the wake does use: armed first, the short window
   installed afterwards (a gesture opening). The seed is already stamped. */
void test_a_seeded_wake_press_keeps_the_long_window_when_the_short_one_is_set_after_arming(void) {
    button_latch_begin(2, 0, T0_US);
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    button_latch_note_levels(0, T0_US + SHORT_WINDOW_US + 10000); /* up, mid-ring: the gate reopens */
    button_latch_record(2, T0_US + SHORT_WINDOW_US + 25000);      /* 15 ms on: past the settle AND the short window */
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());              /* one tap is one press */
    button_latch_record(2, T0_US + BUTTON_LATCH_REFRACTORY_US);   /* a real second tap, past the long window */
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

/* The same shape for a press the latch ACCEPTED (not the seed), as far as a
   10 ms settle goes: a sample in a high phase of the ringing is followed by
   an edge inside the settle window, which is refused at either figure. What
   the short window cannot cover is ringing that outlasts the settle AND the
   window, and neither can the long one for a longer hold; that residual is
   the bench check's, and is recorded in the notes. */
void test_an_accepted_press_under_the_short_window_still_has_the_settle_backstop(void) {
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    button_latch_record(2, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 200000);   /* a 200 ms hold: sample mid-ring */
    button_latch_record(2, T0_US + 200000 + 5000); /* its bounce, 5 ms later */
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* The release gate and the settle anchor run BEFORE the window in effect
   matters: a press the gate refuses is refused at any window. */
void test_the_release_gate_still_refuses_under_the_short_window(void) {
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    button_latch_record(2, T0_US);
    button_latch_take();
    button_latch_record(2, T0_US + 10 * SHORT_WINDOW_US); /* never seen released */
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_a_rejected_edge_does_not_extend_the_short_window(void) {
    button_latch_set_refractory(2, SHORT_WINDOW_US);
    button_latch_record(2, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 30000);
    button_latch_record(2, T0_US + 100000); /* refused, inside the window */
    button_latch_record(2, T0_US + SHORT_WINDOW_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

/* THE RELEASE GATE, and the field defect it closes (M2-T12 finding 3). One
   physical press is a falling edge going down and, on a bouncing contact,
   another coming back up — and the second is separated from the first by
   the HOLD, which is as long as the person holding it. So no window
   measured from the press edge can reject it, and while the coalescing
   drain read every latched bit as a press it toggled a chore row back:
   "toggling 3 flips 2 back to green".
   Ten times the debounce window here to make the point that widening the
   window is not the fix. */
void test_an_edge_with_no_release_observed_is_not_a_second_press(void) {
    button_latch_record(1, T0_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
    button_latch_record(1, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* The gate is armed by the OBSERVATION and not by the take: consuming a
   press says nothing about whether the finger came off. */
void test_taking_a_press_does_not_arm_the_release_gate(void) {
    button_latch_record(0, T0_US);
    button_latch_take();
    button_latch_take(); /* however many times */
    button_latch_record(0, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* A sample taken while the button is STILL DOWN observes no release, which
   is the case a level poll hits when the press outlasts the poll interval —
   the common one, since a poll is milliseconds and a press is not. */
void test_a_sample_with_the_button_still_held_does_not_arm_the_gate(void) {
    button_latch_record(2, T0_US);
    button_latch_take();
    button_latch_note_levels(1u << 2, T0_US + BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(2, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* THE SECOND ANCHOR. A level sample can land in a HIGH phase of a bounce
   train and report "up" while the contact is still chattering; the falling
   edge a millisecond later would then be a press with the gate armed. The
   release observation is therefore a debounce anchor in its own right —
   which the press edge cannot be, being a hold duration in the past.
   ONE MICROSECOND, so this is the case that fails if the settle window is
   ever narrowed to nothing; the case below it is the one that fails if it is
   ever widened back to the chatter window. Between them they bracket
   BUTTON_LATCH_RELEASE_SETTLE_US from both sides, which is the only way a
   figure whose two failure modes are opposite can be pinned at all. */
void test_an_edge_right_after_the_release_sample_is_still_bounce(void) {
    button_latch_record(3, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(3, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US + 1);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* THE OTHER SIDE OF THAT BRACKET: a GENUINE press landing past the settle
   window but within a poll interval (BUTTON_LATCH_DEBOUNCE_US) of a release
   observation.
   The gate reopens at a POLL, so the poll that admits the press is also the
   anchor it is measured against. Were that anchor a whole poll interval wide,
   a press within 50 ms of the poll would be rejected as bounce — and then
   LOST outright, because the next poll reads the pad as held and closes the
   gate again for the rest of the press. With R the release, P the first poll
   at or after it and g the RELEASE-to-press gap, every g below
   (P - R) + window would go that way. The settle window is 10 ms so that
   only the narrow band it covers is at risk. This applies to a re-press at or
   beyond the refractory window (BUTTON_LATCH_REFRACTORY_US) press-to-press,
   the only kind the refractory window admits; the probe sits 500 ms after
   the first press for that reason. */
void test_a_press_past_the_settle_window_is_accepted_inside_the_chatter_window(void) {
    /* 30 ms, WRITTEN AS A DURATION rather than derived from the two windows,
       so that widening the settle anchor to a whole poll interval fails the
       BEHAVIOUR below and not merely an arithmetic guard. A guard tripping
       says "these constants no longer differ"; the assertion says "the press
       was eaten", and it is the second sentence a future reader needs. */
    const int64_t probe_gap_us = 30000;
    const int64_t release = T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US;
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_note_levels(0, release);
    button_latch_record(1, release + probe_gap_us);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u << 1, button_latch_take(),
                                    "a real press soon after the poll that reopened the gate was rejected as "
                                    "bounce, which is the lost correction press of M2-T12");
    /* AFTER the assertion, because these only say whether the assertion
       above was worth making. The press has to sit strictly between the two
       windows or it proves nothing: too early and the gate is right to
       reject it, too late and it clears an anchor of any width. */
    TEST_ASSERT_TRUE_MESSAGE(probe_gap_us > BUTTON_LATCH_RELEASE_SETTLE_US,
                             "the probe gap no longer clears the settle anchor");
    TEST_ASSERT_TRUE_MESSAGE(probe_gap_us < BUTTON_LATCH_DEBOUNCE_US,
                             "the probe gap no longer lands inside one poll interval, so this case passes "
                             "without distinguishing the settle anchor from a poll-wide one");
}

/* THE SEEDING SAMPLE ANCHORS TOO, for a button it finds already UP.
   button_latch_reset() seeds the gate open with NO anchor, because it cannot
   know what the pads read; until M2-T12's fix pass nothing anchored such a
   button at all, so its very next falling edge cleared both windows. That
   pad is not always an idle one: a wake press tapped and released during
   boot can be sampled in a HIGH phase of its own release bounce train, and
   the falling edge a millisecond later was then a phantom press of the
   button that caused the wake — the same phantom the gate exists to stop,
   arrived at through the seed instead of through the sample. */
void test_the_seeding_sample_anchors_a_button_it_finds_already_up(void) {
    button_latch_note_levels(0, T0_US); /* buttons_watch_begin()'s sample: every pad up */
    button_latch_record(2, T0_US + 1);  /* the bounce train's next falling edge */
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
    /* And a real press once the contact has settled is still a press: the
       anchor is a window, not a lock. */
    button_latch_record(2, T0_US + BUTTON_LATCH_RELEASE_SETTLE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

/* The gate is per button, like the debounce window: a sample that sees B up
   must not arm C, which is still down. */
void test_the_release_gate_is_per_button(void) {
    button_latch_record(1, T0_US);
    button_latch_record(2, T0_US);
    button_latch_take();
    button_latch_note_levels(1u << 2, T0_US + BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(1, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    button_latch_record(2, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

/* THE WAKE BUTTON, which is why buttons.c samples the pads before it arms
   the ISRs. It is already down at boot so it fires no falling edge going
   in; seeded as HELD, its release bounce is bounce. Seeded by a reset
   alone — every button "released" — that same bounce is a phantom press of
   the very button that caused the wake, which on a chore ack toggled the
   row straight back and looked like the press had done nothing. */
void test_a_button_seeded_as_held_rejects_its_release_bounce(void) {
    button_latch_note_levels(1u << 1, T0_US); /* buttons_watch_begin()'s sample */
    button_latch_record(1, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

/* And the other half of that seed: a button that was NOT held at boot is
   free to be pressed without anybody having sampled it first. Without this
   the seed could be "reject everything until a sample arrives" and the case
   above would still pass. */
void test_a_button_seeded_as_up_accepts_its_first_press(void) {
    button_latch_note_levels(1u << 1, T0_US); /* only B was held */
    button_latch_record(2, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

/* A repeated sample must not keep pushing the anchor forward, or a press
   made just after the latest poll is rejected as bounce off it — which
   would be every press on a device that polls, i.e. the whole coalescing
   loop. Only the transition down-to-up anchors. */
void test_repeated_release_samples_do_not_push_the_anchor_forward(void) {
    button_latch_record(0, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + BUTTON_LATCH_DEBOUNCE_US);
    /* Nine more polls, the last of them right before the press. */
    for (int i = 2; i <= 10; i++) {
        button_latch_note_levels(0, T0_US + (int64_t)i * BUTTON_LATCH_DEBOUNCE_US);
    }
    button_latch_record(0, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US + 1);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take());
}

/* Out-of-range bits in the held mask are the caller's business, not a
   crash: the mask is four bits wide and a wider one must be ignored above
   the fourth, which ASan would catch as a write past s_released_seen. */
void test_note_levels_ignores_bits_above_the_four_buttons(void) {
    button_latch_note_levels(0xF0, T0_US);
    button_latch_record(0, T0_US + 10 * BUTTON_LATCH_DEBOUNCE_US);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take());
}

void test_debounce_windows_are_per_button(void) {
    button_latch_record(0, T0_US);
    /* A different button inside button 0's window still latches */
    button_latch_record(2, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8((1u << 0) | (1u << 2), button_latch_take());
}

void test_debounce_window_anchors_on_accepted_edge_only(void) {
    /* A rejected edge must not extend the window: T0 accepted, T0+0.8 W
       rejected, T0+1.2 W is beyond T0+W and must be accepted even though it
       is within W of the REJECTED edge (W = BUTTON_LATCH_REFRACTORY_US).
       The release sample sits at T0+5ms so the gate is armed for all of
       this and the only thing under test is the press-edge anchor — early
       enough that its own anchor (BUTTON_LATCH_RELEASE_SETTLE_US, 10 ms, so
       expired by T0+15ms) cannot be what rejects or admits either edge. */
    button_latch_record(1, T0_US);
    button_latch_take();
    button_latch_note_levels(0, T0_US + BUTTON_LATCH_DEBOUNCE_US / 10);
    button_latch_record(1, T0_US + (BUTTON_LATCH_REFRACTORY_US * 4) / 5);
    button_latch_record(1, T0_US + (BUTTON_LATCH_REFRACTORY_US * 6) / 5);
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

void test_out_of_range_button_ignored(void) {
    button_latch_record(-1, T0_US);
    button_latch_record(4, T0_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_reset_clears_mask_and_debounce_history(void) {
    button_latch_record(0, T0_US);
    button_latch_reset();
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
    /* History cleared: an edge inside the old window latches again */
    button_latch_record(0, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take());
}

void test_take_masked_returns_only_requested_bits(void) {
    button_latch_record(0, T0_US);
    button_latch_record(2, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8(1u << 0, button_latch_take_masked(1u << 0));
}

void test_take_masked_leaves_other_bits_latched(void) {
    /* The C1 field case: A-poll during the grid wait must not eat a
       latched C press meant for the tick-wake drain. */
    button_latch_record(0, T0_US);
    button_latch_record(2, T0_US + 1000);
    button_latch_take_masked(1u << 0);
    TEST_ASSERT_EQUAL_UINT8(1u << 2, button_latch_take());
}

void test_take_masked_consumed_bit_stays_consumed(void) {
    button_latch_record(0, T0_US);
    button_latch_take_masked(1u << 0);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take_masked(1u << 0));
}

void test_take_masked_empty_mask_is_noop(void) {
    button_latch_record(1, T0_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take_masked(0));
    TEST_ASSERT_EQUAL_UINT8(1u << 1, button_latch_take());
}

void test_take_masked_requested_but_unlatched_returns_zero(void) {
    button_latch_record(3, T0_US);
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take_masked(1u << 0));
    TEST_ASSERT_EQUAL_UINT8(1u << 3, button_latch_take());
}

void test_take_masked_full_mask_equals_take(void) {
    button_latch_record(0, T0_US);
    button_latch_record(3, T0_US + 1000);
    TEST_ASSERT_EQUAL_UINT8((1u << 0) | (1u << 3), button_latch_take_masked(0x0F));
    TEST_ASSERT_EQUAL_UINT8(0, button_latch_take());
}

void test_pick_empty_mask_returns_none(void) {
    TEST_ASSERT_EQUAL_INT(-1, button_latch_pick(0, 0x0F));
}

void test_pick_single_button_returns_it(void) {
    TEST_ASSERT_EQUAL_INT(1, button_latch_pick(1u << 1, 0x0F));
}

/* B (1) is the start/pause/resume button and therefore the time-sensitive
   one, so it outranks everything — including A, whose binding (the
   Timers/Chores mode toggle) only chooses which screen is painted and so
   loses nothing by waiting for the next press. */
void test_pick_priority_b_over_all(void) {
    TEST_ASSERT_EQUAL_INT(1, button_latch_pick(0x0F, 0x0F));
}

/* THE swallowing case, and the reason the order had to move with the
   layout. A is index 0, so an order that still led with it would let a
   press of the mode toggle beat a genuine start/pause press: pick returns
   A, the dispatch changes which screen is painted, and the B press is
   gone — taken out of the latch by the same unmasked take and discarded
   at sleep. The user presses start, nothing happens, and there is no
   feedback to tell them why. This mattered more once A gained a binding,
   not less: both drains admit A to their allowed masks now (M2-T3), so
   this priority is the only thing left standing between the two presses. */
void test_pick_priority_b_over_a(void) {
    TEST_ASSERT_EQUAL_INT(1, button_latch_pick((1u << 0) | (1u << 1), 0x0F));
}

void test_pick_priority_c_over_d_and_a(void) {
    TEST_ASSERT_EQUAL_INT(2, button_latch_pick((1u << 0) | (1u << 2) | (1u << 3), 0x0F));
}

void test_pick_priority_d_over_a(void) {
    TEST_ASSERT_EQUAL_INT(3, button_latch_pick((1u << 0) | (1u << 3), 0x0F));
}

/* A is last, not absent: it is still a valid pick when it is the only
   thing latched. The callers that must never act on it drop it from the
   ALLOWED mask instead (wake_flow's two drains), which is the case
   below. */
void test_pick_returns_a_when_it_is_the_only_button_latched(void) {
    TEST_ASSERT_EQUAL_INT(0, button_latch_pick(1u << 0, 0x0F));
}

void test_pick_respects_allowed_mask(void) {
    /* A latched but not allowed: fall through to the best allowed */
    TEST_ASSERT_EQUAL_INT(2, button_latch_pick((1u << 0) | (1u << 2), 1u << 2));
    /* Only disallowed buttons latched: none */
    TEST_ASSERT_EQUAL_INT(-1, button_latch_pick(1u << 3, (1u << 0) | (1u << 1) | (1u << 2)));
    /* The wake_flow drains' allowed set, with A dropped: a latched A
       alone picks nothing at all, so the drain never runs. */
    TEST_ASSERT_EQUAL_INT(-1, button_latch_pick(1u << 0, (1u << 1) | (1u << 2)));
}

/* BOOT's rank: LAST. button_latch_boot_wins() takes button_latch_pick's OWN
   return value for the same wake, so every one of B, C, D and A — in that
   priority order, exactly as the picks above resolve them — already
   outranks BOOT the moment any of them is the pick. */
void test_boot_loses_to_any_resolved_pick(void) {
    TEST_ASSERT_FALSE(button_latch_boot_wins(button_latch_pick(0x0F, 0x0F)));                              /* B */
    TEST_ASSERT_FALSE(button_latch_boot_wins(button_latch_pick((1u << 2) | (1u << 3) | (1u << 0), 0x0F))); /* C */
    TEST_ASSERT_FALSE(button_latch_boot_wins(button_latch_pick((1u << 3) | (1u << 0), 0x0F)));             /* D */
    TEST_ASSERT_FALSE(button_latch_boot_wins(button_latch_pick(1u << 0, 0x0F)));                           /* A */
}

void test_boot_wins_only_when_nothing_else_is_picked(void) {
    TEST_ASSERT_TRUE(button_latch_boot_wins(button_latch_pick(0, 0x0F)));       /* nothing latched */
    TEST_ASSERT_TRUE(button_latch_boot_wins(button_latch_pick(1u << 3, 0x00))); /* latched but disallowed */
    TEST_ASSERT_TRUE(button_latch_boot_wins(-1));                               /* the raw sentinel too */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_take_returns_zero_when_nothing_latched);
    RUN_TEST(test_record_then_take_returns_bit_and_clears);
    RUN_TEST(test_multiple_buttons_combine_into_one_mask);
    RUN_TEST(test_edges_within_debounce_window_are_ignored);
    RUN_TEST(test_edge_beyond_debounce_window_and_past_a_release_is_accepted);
    RUN_TEST(test_an_edge_with_no_release_observed_is_not_a_second_press);
    RUN_TEST(test_taking_a_press_does_not_arm_the_release_gate);
    RUN_TEST(test_a_sample_with_the_button_still_held_does_not_arm_the_gate);
    RUN_TEST(test_an_edge_right_after_the_release_sample_is_still_bounce);
    RUN_TEST(test_a_press_past_the_settle_window_is_accepted_inside_the_chatter_window);
    RUN_TEST(test_the_seeding_sample_anchors_a_button_it_finds_already_up);
    RUN_TEST(test_a_same_button_edge_inside_the_refractory_window_is_ignored);
    RUN_TEST(test_a_same_button_edge_past_the_refractory_window_is_accepted);
    RUN_TEST(test_a_different_button_is_accepted_immediately_after_a_press);
    RUN_TEST(test_each_buttons_refractory_window_is_its_own);
    RUN_TEST(test_a_seeded_wake_press_opens_a_refractory_window_without_latching);
    RUN_TEST(test_a_seeded_wake_press_does_not_delay_other_buttons);
    RUN_TEST(test_seeding_no_button_or_a_bad_one_is_a_no_op);
    RUN_TEST(test_reset_forgets_a_seeded_window);
    RUN_TEST(test_begin_seeds_the_wake_buttons_refractory_window);
    RUN_TEST(test_begin_samples_after_seeding_not_before);
    RUN_TEST(test_begin_with_the_wake_button_still_held_keeps_its_gate_closed);
    RUN_TEST(test_begin_resets_whatever_an_earlier_wake_left);
    RUN_TEST(test_begin_with_no_wake_button_seeds_nothing);
    RUN_TEST(test_every_button_defaults_to_the_long_refractory_window);
    RUN_TEST(test_a_shorter_window_lets_a_faster_same_button_press_through);
    RUN_TEST(test_a_window_change_touches_only_the_named_button);
    RUN_TEST(test_a_window_change_does_not_reach_into_a_window_already_running);
    RUN_TEST(test_restoring_the_default_puts_the_long_window_back_for_the_next_press);
    RUN_TEST(test_a_nonsense_window_or_button_is_ignored);
    RUN_TEST(test_reset_restores_every_default_window);
    RUN_TEST(test_a_seeded_wake_press_keeps_the_long_window_when_the_short_one_is_set);
    RUN_TEST(test_a_seeded_wake_press_keeps_the_long_window_when_the_short_one_is_set_after_arming);
    RUN_TEST(test_an_accepted_press_under_the_short_window_still_has_the_settle_backstop);
    RUN_TEST(test_the_release_gate_still_refuses_under_the_short_window);
    RUN_TEST(test_a_rejected_edge_does_not_extend_the_short_window);
    RUN_TEST(test_the_release_gate_is_per_button);
    RUN_TEST(test_a_button_seeded_as_held_rejects_its_release_bounce);
    RUN_TEST(test_a_button_seeded_as_up_accepts_its_first_press);
    RUN_TEST(test_repeated_release_samples_do_not_push_the_anchor_forward);
    RUN_TEST(test_note_levels_ignores_bits_above_the_four_buttons);
    RUN_TEST(test_debounce_windows_are_per_button);
    RUN_TEST(test_debounce_window_anchors_on_accepted_edge_only);
    RUN_TEST(test_out_of_range_button_ignored);
    RUN_TEST(test_reset_clears_mask_and_debounce_history);
    RUN_TEST(test_take_masked_returns_only_requested_bits);
    RUN_TEST(test_take_masked_leaves_other_bits_latched);
    RUN_TEST(test_take_masked_consumed_bit_stays_consumed);
    RUN_TEST(test_take_masked_empty_mask_is_noop);
    RUN_TEST(test_take_masked_requested_but_unlatched_returns_zero);
    RUN_TEST(test_take_masked_full_mask_equals_take);
    RUN_TEST(test_pick_empty_mask_returns_none);
    RUN_TEST(test_pick_single_button_returns_it);
    RUN_TEST(test_pick_priority_b_over_all);
    RUN_TEST(test_pick_priority_b_over_a);
    RUN_TEST(test_pick_priority_c_over_d_and_a);
    RUN_TEST(test_pick_priority_d_over_a);
    RUN_TEST(test_pick_returns_a_when_it_is_the_only_button_latched);
    RUN_TEST(test_pick_respects_allowed_mask);
    RUN_TEST(test_boot_loses_to_any_resolved_pick);
    RUN_TEST(test_boot_wins_only_when_nothing_else_is_picked);
    return UNITY_END();
}
