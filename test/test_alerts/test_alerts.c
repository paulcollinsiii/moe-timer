#include <unity.h>

/* Single-TU: the alert engine compiled straight in, with FreeRTOS, the
   DAC, the NeoPixels and the button latch resolved by the stubs below.

   This suite exists for one reason above all others: the awake-failsafe
   extension used to be a compile-time binding (main.c passed the function
   pointer at every call, so it could not be omitted) and is now a runtime
   install into a module static, which CAN be omitted. Nothing in the
   compiler can catch a missing install, so the behaviour it guarantees —
   the extension happens, once per locate, with the full budget, before
   the first beep — is pinned here instead. */
// clang-format off
#include "../../main/alerts.c"
// clang-format on

/* ---- the effect log -----------------------------------------------------

   Half of what this suite pins is an ORDERING ("the failsafe is pushed
   out before the alarm starts"), not an outcome, so the stubs append to
   one shared log rather than each keeping its own counter. Everything is
   alrt_-prefixed: cppcheck's shadowFunction check runs across the whole
   TU and the module under test has statics and locals of its own. */

typedef enum {
    EV_EXTEND = 1,
    EV_PULSE_BEGIN,
    EV_PULSE_END,
    EV_AUDIO_RUN,
    EV_AUDIO_STOP,
    EV_NEOPIXEL_STOP,
} alrt_event_t;

static alrt_event_t alrt_log[64];
static int alrt_log_n;

static void alrt_log_push(alrt_event_t ev) {
    if (alrt_log_n < (int)(sizeof alrt_log / sizeof alrt_log[0])) {
        alrt_log[alrt_log_n++] = ev;
    }
}

/* Position of the first occurrence, or -1. Ordering assertions compare
   two of these; -1 on either side makes the comparison fail loudly
   instead of quietly passing on an effect that never happened. */
static int alrt_log_at(alrt_event_t ev) {
    for (int i = 0; i < alrt_log_n; i++) {
        if (alrt_log[i] == ev)
            return i;
    }
    return -1;
}

static int alrt_log_count(alrt_event_t ev) {
    int n = 0;
    for (int i = 0; i < alrt_log_n; i++) {
        if (alrt_log[i] == ev)
            n++;
    }
    return n;
}

/* ---- the dismissal model ------------------------------------------------

   alert_run_locate's own exit condition is a 600 s wall-clock read, so
   every case has to end by DISMISSAL — no test here may wait for time to
   pass. The model is the real one: a child standing next to a beeping
   device holds a button down, and run_alert's level scan catches it (the
   latch drain at the top of run_alert deliberately does not). Which round
   they reach for it is the case's parameter. */

#define ALRT_MAX_ROUNDS 8 /* hard stop, see alrt_pulse_begin note below */

static int alrt_rounds;            /* run_alert entries so far this case */
static int alrt_dismiss_after;     /* press on this round (1 = the first) */
static uint8_t alrt_held;          /* buttons_scan_held() answer */
static uint8_t alrt_latched;       /* buttons_take_pressed() latch */
static int alrt_latch_drain_calls; /* how often the latch was consumed */
static uint8_t alrt_pulse_r, alrt_pulse_g, alrt_pulse_b;
/* What the FIRST drain of the case actually returned. The call count
   alone cannot tell a consumed press from an empty latch — run_alert
   drains unconditionally — so a case that means "the stale press was
   eaten here" has to assert the CONTENT, not the count. */
static uint8_t alrt_first_drain;
static bool alrt_first_drain_seen;

uint8_t buttons_take_pressed(void) {
    uint8_t v = alrt_latched;
    alrt_latched = 0;
    alrt_latch_drain_calls++;
    if (!alrt_first_drain_seen) {
        alrt_first_drain_seen = true;
        alrt_first_drain = v;
    }
    return v;
}

uint8_t buttons_scan_held(void) {
    return alrt_held;
}

/* Every alert round begins here, which makes this the one place that
   sees each iteration of the locate loop.

   The ALRT_MAX_ROUNDS stop fails the case OUT OF BAND rather than by
   setting alrt_held. That distinction is the whole point: alrt_held is
   only observable through buttons_scan_held(), which is a line inside
   the code under test, so a guard built on it is disarmed by the very
   regressions it exists to catch. Any change that stops honouring
   dismissal — dropping the level scan, pinning `dismissed` false,
   returning false from run_alert, dropping `!dismissed` from the locate
   while — also stops the guard from ever being able to end the loop, and
   the suite spins on a 600 s wall clock instead of failing. vTaskDelay is
   a no-op here, so that is a full-speed busy loop that overflows these
   int counters (UB) long before the clock runs out, and once alrt_rounds
   wraps negative the cap un-arms for good. All four were verified to hang
   with the old alrt_held-based guard and to fail cleanly with this one.

   Residual: a regression that stopped calling pulse_begin at all would
   not be counted here, but the EV_PULSE_BEGIN assertions catch that. */
void neopixel_alert_pulse_begin(uint8_t r, uint8_t g, uint8_t b) {
    alrt_rounds++;
    if (alrt_rounds >= ALRT_MAX_ROUNDS) {
        TEST_FAIL_MESSAGE("locate loop exceeded ALRT_MAX_ROUNDS - dismissal is not being honoured");
    }
    if (alrt_rounds >= alrt_dismiss_after) {
        alrt_held = 1;
    }
    alrt_pulse_r = r;
    alrt_pulse_g = g;
    alrt_pulse_b = b;
    alrt_log_push(EV_PULSE_BEGIN);
}

void neopixel_alert_pulse_end(void) {
    alrt_log_push(EV_PULSE_END);
}

void neopixel_stop(void) {
    alrt_log_push(EV_NEOPIXEL_STOP);
}

/* ---- audio ---------------------------------------------------------------

   Four patterns share the engine; recording WHICH one ran is what keeps
   "the locate pulse" in the ordering assertions from being satisfied by
   some other alert's pulse. */

typedef enum {
    AUDIO_NONE = 0,
    AUDIO_EXPIRY,
    AUDIO_BREAK,
    AUDIO_BEDTIME,
    AUDIO_LOCATE,
} alrt_audio_id_t;

static alrt_audio_id_t alrt_audio_ran;

static void alrt_note_audio(alrt_audio_id_t id) {
    alrt_audio_ran = id;
    alrt_log_push(EV_AUDIO_RUN);
}

void audio_beep_sequence(void) {
    alrt_note_audio(AUDIO_EXPIRY);
}

void audio_break_alarm(void) {
    alrt_note_audio(AUDIO_BREAK);
}

void audio_bedtime_alarm(void) {
    alrt_note_audio(AUDIO_BEDTIME);
}

void audio_locate_alarm(void) {
    alrt_note_audio(AUDIO_LOCATE);
}

void audio_stop(void) {
    alrt_log_push(EV_AUDIO_STOP);
}

/* ---- the FreeRTOS model --------------------------------------------------

   Cooperative and deliberately NOT inline. Running the audio task inside
   xTaskCreate would set s_audio_done before run_alert's poll loop starts,
   the loop would never execute a single iteration, no button could ever
   be seen, and alert_run_locate could then only end on its 600 s
   wall-clock cap — every case in this file would hang for ten minutes.

   So the task is queued, and vTaskDelay — the only point at which the
   real alert loop yields the core — runs it to completion, which is what
   a single-core scheduler does with a task that outranks the caller.
   The consequence cases rely on: s_audio_done becomes visible on the
   FIRST vTaskDelay after the create, i.e. at the end of poll iteration 0
   (or, when a button dismissed the alert before any delay, on the first
   iteration of the join loop). */

static void (*alrt_pending_task)(void *);
static int alrt_task_create_calls;
static int alrt_task_delete_calls;
static bool alrt_task_create_fails; /* drive the visual-only alert path */

int xTaskCreate(TaskFunction_t fn, const char *name, unsigned int stack, void *arg, unsigned int prio,
                TaskHandle_t *created) {
    (void)name;
    (void)stack;
    (void)arg;
    (void)prio;
    (void)created;
    alrt_task_create_calls++;
    if (alrt_task_create_fails) {
        return !pdPASS; /* nothing queued: no audio, and nothing to join */
    }
    alrt_pending_task = fn;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks) {
    (void)ticks;
    void (*fn)(void *) = alrt_pending_task;
    if (fn != NULL) {
        alrt_pending_task = NULL; /* clear before running: a task runs once */
        fn(NULL);
    }
}

void vTaskDelete(TaskHandle_t task) {
    (void)task;
    alrt_task_delete_calls++;
}

/* ---- the extenders under observation ------------------------------------ */

static int alrt_extend_a_calls;
static int alrt_extend_a_sec;
static int alrt_extend_b_calls;
static int alrt_extend_b_sec;

static void alrt_extend_a(int seconds) {
    alrt_extend_a_calls++;
    alrt_extend_a_sec = seconds;
    alrt_log_push(EV_EXTEND);
}

static void alrt_extend_b(int seconds) {
    alrt_extend_b_calls++;
    alrt_extend_b_sec = seconds;
    alrt_log_push(EV_EXTEND);
}

/* ---- harness ------------------------------------------------------------ */

void setUp(void) {
    alrt_log_n = 0;
    alrt_rounds = 0;
    alrt_dismiss_after = 1; /* dismissed on the first round unless a case says otherwise */
    alrt_held = 0;
    alrt_latched = 0;
    alrt_latch_drain_calls = 0;
    alrt_first_drain = 0;
    alrt_first_drain_seen = false;
    alrt_task_create_fails = false;
    alrt_pulse_r = alrt_pulse_g = alrt_pulse_b = 0xFF;
    alrt_audio_ran = AUDIO_NONE;
    alrt_pending_task = NULL;
    alrt_task_create_calls = 0;
    alrt_task_delete_calls = 0;
    alrt_extend_a_calls = alrt_extend_a_sec = 0;
    alrt_extend_b_calls = alrt_extend_b_sec = 0;
    /* The module's statics are plain file-scope state, so the suite zeroes
       them directly (as test_lock_gate does) rather than making alerts.c
       carry a reset entry point the firmware would never call. Clearing
       s_extend_awake here is also what makes "never installed" — the
       default a forgotten install produces — a real, reachable case. */
    s_extend_awake = NULL;
    s_alert_active = NULL;
    s_audio_done = false;
    s_audio_task_exited = false;
}

void tearDown(void) {}

/* The budget is asserted as a literal, not as LOCATE_MAX_SEC + 60: the
   point is the number of seconds the failsafe is actually pushed out by,
   and restating the expression under test would pass no matter what that
   expression became. 660 = the 600 s locate cap plus a minute of slack so
   the failsafe cannot land on the alarm's own tail. */
#define ALRT_EXPECTED_BUDGET 660

/* ---- the installed extender --------------------------------------------- */

void test_the_installed_extender_runs_once_with_the_full_budget(void) {
    alerts_set_extend_awake(alrt_extend_a);

    alert_run_locate();

    TEST_ASSERT_EQUAL_INT(1, alrt_extend_a_calls);
    TEST_ASSERT_EQUAL_INT(ALRT_EXPECTED_BUDGET, alrt_extend_a_sec);
}

/* Ordering, and the whole reason the extension is not merely "somewhere
   in this function": pushing the failsafe out AFTER the alarm has already
   been cut short by it buys nothing. The pulse is the first thing the
   child sees or hears, so it is the deadline. */
void test_the_extension_lands_before_the_first_pulse(void) {
    alerts_set_extend_awake(alrt_extend_a);

    alert_run_locate();

    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_EXTEND));
    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_PULSE_BEGIN));
    TEST_ASSERT_TRUE(alrt_log_at(EV_EXTEND) < alrt_log_at(EV_PULSE_BEGIN));
    /* -1 on the right would satisfy the comparison above by accident */
    TEST_ASSERT_TRUE(alrt_log_at(EV_PULSE_BEGIN) >= 0);
}

/* The pulse the assertion above orders against is the LOCATE one, not
   some other alert's: red at full tilt, with the classic max-volume beep
   pattern behind it. */
void test_the_locate_round_uses_the_red_locate_pattern(void) {
    alerts_set_extend_awake(alrt_extend_a);

    alert_run_locate();

    TEST_ASSERT_EQUAL_UINT8(248, alrt_pulse_r);
    TEST_ASSERT_EQUAL_UINT8(0, alrt_pulse_g);
    TEST_ASSERT_EQUAL_UINT8(0, alrt_pulse_b);
    TEST_ASSERT_EQUAL_INT(AUDIO_LOCATE, alrt_audio_ran);
}

/* The install is a standing one, not a one-shot ticket: a second locate
   in the same wake (a parent pressing the HA button twice) must extend
   the failsafe again, or the second alarm runs on whatever is left of the
   first extension. */
void test_a_second_locate_extends_again(void) {
    alerts_set_extend_awake(alrt_extend_a);

    alert_run_locate();
    alrt_rounds = 0; /* the second call is a fresh alarm */
    alrt_held = 0;
    alert_run_locate();

    TEST_ASSERT_EQUAL_INT(2, alrt_extend_a_calls);
    TEST_ASSERT_EQUAL_INT(ALRT_EXPECTED_BUDGET, alrt_extend_a_sec);
    TEST_ASSERT_EQUAL_INT(2, alrt_log_count(EV_EXTEND));
}

/* ONE extension per alert_run_locate, not one per beep round. The inner
   loop re-enters run_alert for as long as nobody answers; an extension
   moved inside it would restart the failsafe on every round and turn the
   600 s cap into an unbounded awake window on a device nobody is coming
   back to. */
void test_an_undismissed_locate_loops_but_extends_only_once(void) {
    alerts_set_extend_awake(alrt_extend_a);
    alrt_dismiss_after = 2; /* the first round beeps out unanswered */

    alert_run_locate();

    TEST_ASSERT_EQUAL_INT(2, alrt_log_count(EV_PULSE_BEGIN)); /* it did loop */
    TEST_ASSERT_EQUAL_INT(1, alrt_extend_a_calls);
    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_EXTEND));
    /* Each round spawns its own audio task and each task reaches its
       vTaskDelete. What this pins is narrower than "a stale task cannot
       leak into the next round": the cooperative model always drains
       alrt_pending_task to completion at the first vTaskDelay, so it
       cannot represent a task outliving its round at all. Both counts
       moving together is a proxy for the join loop still being there —
       deleting the join loop does kill this — and on device that loop is
       what stops a stale task setting s_audio_done on the NEXT alert
       while two tasks drive the DAC. The device-side hazard itself is
       not reproducible here. */
    TEST_ASSERT_EQUAL_INT(2, alrt_task_create_calls);
    TEST_ASSERT_EQUAL_INT(2, alrt_task_delete_calls);
}

/* ---- the missing / cleared extender ------------------------------------- */

/* The regression this whole suite guards: main.c can now forget the
   install. When it does, the alarm must still run and still end cleanly —
   degraded (the failsafe may cut it short), never crashed. */
void test_a_never_installed_extender_is_a_safe_no_op(void) {
    /* deliberately no alerts_set_extend_awake() call */
    alert_run_locate();

    TEST_ASSERT_EQUAL_INT(0, alrt_log_count(EV_EXTEND));
    /* the alarm itself is unaffected: it pulsed, beeped and shut down */
    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_PULSE_BEGIN));
    TEST_ASSERT_EQUAL_INT(AUDIO_LOCATE, alrt_audio_ran);
    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_PULSE_END));
    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_NEOPIXEL_STOP));
}

/* Installing NULL is the same state, reached deliberately. It also pins
   the setter's contract in the other direction: a setter that ignored
   NULL (or refused to overwrite a live callback) would leave A armed. */
void test_installing_null_clears_a_live_extender(void) {
    alerts_set_extend_awake(alrt_extend_a);
    alerts_set_extend_awake(NULL);

    alert_run_locate();

    TEST_ASSERT_EQUAL_INT(0, alrt_extend_a_calls);
    TEST_ASSERT_EQUAL_INT(0, alrt_log_count(EV_EXTEND));
    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_PULSE_BEGIN)); /* still alarmed */
}

void test_reinstalling_replaces_the_previous_extender(void) {
    alerts_set_extend_awake(alrt_extend_a);
    alerts_set_extend_awake(alrt_extend_b);

    alert_run_locate();

    TEST_ASSERT_EQUAL_INT(0, alrt_extend_a_calls);
    TEST_ASSERT_EQUAL_INT(1, alrt_extend_b_calls);
    TEST_ASSERT_EQUAL_INT(ALRT_EXPECTED_BUDGET, alrt_extend_b_sec);
}

/* ---- the shared engine the locate path rides on ------------------------- */

/* alert_run() must not have picked up the extension: the expiry, break
   and bed-time alarms are seconds long and self-terminating — pushing the
   awake failsafe out for eleven minutes on every expiry beep would be a
   battery regression, not a fix. */
void test_the_ordinary_alerts_do_not_extend_the_failsafe(void) {
    alerts_set_extend_awake(alrt_extend_a);

    TEST_ASSERT_TRUE(alert_run(ALERT_EXPIRY));
    TEST_ASSERT_EQUAL_INT(AUDIO_EXPIRY, alrt_audio_ran);
    TEST_ASSERT_EQUAL_INT(0, alrt_extend_a_calls);

    /* Positive control. Without it the zero above is true by accident:
       delete the install on the first line and this test stays green,
       because nothing was ever armed to be observed. Running a locate on
       the SAME installed callback proves it was live the whole time, so
       the zero means "alert_run does not extend" rather than "there was
       no extender". */
    alert_run_locate();
    TEST_ASSERT_EQUAL_INT(1, alrt_extend_a_calls);
}

/* Dispatch: each kind reaches its own pattern. Guards the pattern table
   against the host build's Kconfig substitutions scrambling it. */
void test_each_alert_kind_runs_its_own_pattern(void) {
    TEST_ASSERT_TRUE(alert_run(ALERT_BREAK));
    TEST_ASSERT_EQUAL_INT(AUDIO_BREAK, alrt_audio_ran);
    TEST_ASSERT_EQUAL_UINT8(0, alrt_pulse_r);
    TEST_ASSERT_EQUAL_UINT8(150, alrt_pulse_g);
    TEST_ASSERT_EQUAL_UINT8(220, alrt_pulse_b);

    TEST_ASSERT_TRUE(alert_run(ALERT_BEDTIME));
    TEST_ASSERT_EQUAL_INT(AUDIO_BEDTIME, alrt_audio_ran);
    TEST_ASSERT_EQUAL_UINT8(120, alrt_pulse_r);
    TEST_ASSERT_EQUAL_UINT8(0, alrt_pulse_g);
    TEST_ASSERT_EQUAL_UINT8(200, alrt_pulse_b);
}

/* The drain at the top of every round is what stops a press from BEFORE
   the alarm pre-dismissing it; the level scan is what catches a button
   already held down through that drain. Both halves are load-bearing for
   the dismissal model every case above depends on. */
void test_a_press_latched_before_the_alarm_does_not_dismiss_it(void) {
    alrt_latched = 1;       /* a stale press from earlier in the wake */
    alrt_dismiss_after = 3; /* the real answer comes on the third round */

    alert_run_locate();

    /* The stale press really existed and really was eaten by the first
       round's drain: that drain returned the bit. Asserting
       alrt_latch_drain_calls here instead would be vacuous — run_alert
       drains unconditionally, so the count is identical whether or not
       anything was ever latched, and deleting the alrt_latched line above
       would leave the test green. */
    TEST_ASSERT_EQUAL_HEX8(1, alrt_first_drain);
    /* And it was never counted as a dismissal, so the alarm kept going.
       Drop the drain and round 1 ends it: one pulse, not three. */
    TEST_ASSERT_EQUAL_INT(3, alrt_log_count(EV_PULSE_BEGIN));
}

/* The DAC write path can fail to spawn. alerts.c documents the fallback
   in detail — "the alert runs visual-only: the poll loop still caps the
   pulse and a button still dismisses it" — but nothing exercised it, so
   the s_audio_task_exited = true that stands in for "nothing to join" was
   unverified. Get it wrong and the join loop below waits its full 2 s on
   a task that never existed, on every alert, forever. */
void test_a_failed_audio_task_still_alarms_visually_and_still_dismisses(void) {
    alrt_task_create_fails = true;
    alerts_set_extend_awake(alrt_extend_a);
    alrt_dismiss_after = 1;

    alert_run_locate();

    TEST_ASSERT_EQUAL_INT(1, alrt_task_create_calls); /* tried once */
    TEST_ASSERT_EQUAL_INT(AUDIO_NONE, alrt_audio_ran);
    TEST_ASSERT_EQUAL_INT(0, alrt_task_delete_calls); /* nothing spawned to delete */
    /* Still a real alarm, still extended, still shut down cleanly. */
    TEST_ASSERT_EQUAL_INT(1, alrt_extend_a_calls);
    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_PULSE_BEGIN));
    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_PULSE_END));
    TEST_ASSERT_EQUAL_INT(1, alrt_log_count(EV_NEOPIXEL_STOP));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_installed_extender_runs_once_with_the_full_budget);
    RUN_TEST(test_the_extension_lands_before_the_first_pulse);
    RUN_TEST(test_the_locate_round_uses_the_red_locate_pattern);
    RUN_TEST(test_a_second_locate_extends_again);
    RUN_TEST(test_an_undismissed_locate_loops_but_extends_only_once);
    RUN_TEST(test_a_never_installed_extender_is_a_safe_no_op);
    RUN_TEST(test_installing_null_clears_a_live_extender);
    RUN_TEST(test_reinstalling_replaces_the_previous_extender);
    RUN_TEST(test_the_ordinary_alerts_do_not_extend_the_failsafe);
    RUN_TEST(test_each_alert_kind_runs_its_own_pattern);
    RUN_TEST(test_a_press_latched_before_the_alarm_does_not_dismiss_it);
    RUN_TEST(test_a_failed_audio_task_still_alarms_visually_and_still_dismisses);
    return UNITY_END();
}
