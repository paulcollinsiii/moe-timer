#include "buttons.h"

#include "button_actions.h"
#include "button_latch.h"
#include "buttons_policy.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lock_gate.h"
#include "sdkconfig.h"
#include "timer.h"

static const char *TAG = "buttons";

/* Adafruit MagTag pinout: A=D15, B=D14, C=D12, D=D11. Verified in hardware
   bring-up 2026-07: with B/C swapped, physical Button C fired the BTN_B
   (reset) action. */
static const gpio_num_t BTN_GPIOS[4] = {
    GPIO_NUM_15, /* BTN_A */
    GPIO_NUM_14, /* BTN_B */
    GPIO_NUM_12, /* BTN_C */
    GPIO_NUM_11, /* BTN_D */
};

#define DEBOUNCE_US 10000 /* 10 ms */

/* ---- awake press latch (ISR-fed; see button_latch.h) ---------------- */

static portMUX_TYPE s_latch_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR button_isr(void *arg) {
    portENTER_CRITICAL_ISR(&s_latch_mux);
    button_latch_record((int)(intptr_t)arg, esp_timer_get_time());
    portEXIT_CRITICAL_ISR(&s_latch_mux);
}

/* Feed the latch a level sample, which is how the release gate learns a
   button came back up (button_latch.h). Task context only — the scan reads
   the GPIO driver, and the whole point of sampling here rather than in the
   ISR is to keep that call out of an IRAM handler.

   The scan is OUTSIDE the critical section and the note INSIDE it: the
   sample is a fact about a moment, so holding the lock across the four pad
   reads would buy nothing and lengthen a section an ISR spins on. */
static void buttons_note_levels(void) {
    const uint8_t held = buttons_scan_held();
    const int64_t t_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_latch_mux);
    button_latch_note_levels(held, t_us);
    portEXIT_CRITICAL(&s_latch_mux);
}

/* Latch presses while awake. No false latch for the wake button: it is
   already low at boot, so no falling edge fires — and the seeding sample
   below is what stops the falling-edge bounce of its RELEASE from reading as
   a second press of the button that caused the wake. That sample has to
   happen before the handlers go on, or an edge could be recorded against an
   unseeded gate.

   WHICH OF THE TWO WAYS IT DOES THAT DEPENDS ON THE PRESS, and writing only
   the first was a false unconditional: a wake press long enough to outlast
   boot reads HELD, which closes the release gate outright, while a quick tap
   is over before we get here and reads UP — and then it is the settle anchor
   on that observation (button_latch.h) doing the rejecting. Both are
   covered; only one of them is the common case, and it is not knowable from
   here which. */
static void buttons_watch_begin(void) {
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) { /* INVALID_STATE = already installed */
        ESP_LOGE(TAG, "isr service install failed: %s", esp_err_to_name(ret));
        return;
    }
    portENTER_CRITICAL(&s_latch_mux);
    button_latch_reset();
    portEXIT_CRITICAL(&s_latch_mux);
    buttons_note_levels();
    for (int i = 0; i < 4; i++) {
        gpio_set_intr_type(BTN_GPIOS[i], GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(BTN_GPIOS[i], button_isr, (void *)(intptr_t)i);
    }
}

/* Detach before buttons_configure_wakeup_if() moves the pads to the RTC mux. */
static void buttons_watch_end(void) {
    for (int i = 0; i < 4; i++) {
        gpio_isr_handler_remove(BTN_GPIOS[i]);
    }
}

/* EVERY take samples the levels first, so the release gate is fed by the
   act of consuming presses and no consumer can forget to do it. The gate's
   resolution is therefore exactly how often somebody takes: the chore-ack
   coalescer polls at the debounce window for that reason. */
uint8_t buttons_take_pressed(void) {
    buttons_note_levels();
    portENTER_CRITICAL(&s_latch_mux);
    uint8_t mask = button_latch_take();
    portEXIT_CRITICAL(&s_latch_mux);
    return mask;
}

uint8_t buttons_take_pressed_mask(uint8_t mask) {
    buttons_note_levels();
    portENTER_CRITICAL(&s_latch_mux);
    uint8_t taken = button_latch_take_masked(mask);
    portEXIT_CRITICAL(&s_latch_mux);
    return taken;
}

void buttons_init(void) {
    for (int i = 0; i < 4; i++) {
        /* ext1_wakeup_prepare() enables pad HOLD on all EXT1 pins when the
           RTC peripheral domain powers down, and hold persists through the
           deep-sleep reset — without releasing it, gpio_config() below is
           latched out and digital button reads break after the first wake. */
        rtc_gpio_hold_dis(BTN_GPIOS[i]);
        /* Pins may still be latched to the RTC domain from the previous
           deep sleep; release them so digital reads work. */
        rtc_gpio_deinit(BTN_GPIOS[i]);
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << BTN_GPIOS[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t ret = gpio_config(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed for GPIO %d: %s", BTN_GPIOS[i], esp_err_to_name(ret));
        }
    }
    /* GPIO0 (BOOT): configured UNCONDITIONALLY, not gated by
       CONFIG_MAGTAG_BOOT_WAKES — that symbol decides whether GPIO0 may
       WAKE the device (buttons_configure_wakeup_if below), not whether
       the level sampler works. buttons_is_boot_pressed() has to answer
       correctly even on a board that disabled the wake source, since the
       hold gesture then falls back to "only while already awake". Same
       release-from-RTC-domain reasoning as the loop above: a previous
       sleep may have moved this pad to the RTC mux. */
    rtc_gpio_hold_dis(GPIO_NUM_0);
    rtc_gpio_deinit(GPIO_NUM_0);
    gpio_config_t boot_cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t boot_ret = gpio_config(&boot_cfg);
    if (boot_ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed for BOOT (GPIO0): %s", esp_err_to_name(boot_ret));
    }
    buttons_watch_begin();
}

/* Which buttons earn a wake is policy and lives in buttons_policy.c; what
   stays here is the RTC/EXT1 plumbing. That plumbing is not host-tested —
   nothing compiles this TU without ESP-IDF — so four seams below rest on
   review alone: that the pad loop indexes BTN_GPIOS with the same bit the
   policy set, that timer_swap_allowed() and button_a_toggle_allowed() are
   wired into the right policy fields, that the early return stays AHEAD
   of buttons_watch_end(), and that buttons_get_wakeup_button()'s fallback
   level scan blames only a pad the policy could have armed AND resolves a
   multi-pad hold through button_latch_pick rather than by index. The last
   seam is the thinnest it has been: the bound and the tie-break are both
   pure host-tested calls now (buttons_policy.c, button_latch.c), so what
   rests on review is the wiring, not the decision. Closing the rest needs
   a host suite for this file (stubbed gpio/rtc_io/esp_sleep/FreeRTOS),
   which is a bigger move than the policy carve.

   A FIFTH SEAM LIVES ABOVE, added by M2-T12 and the same shape: that every
   take feeds buttons_note_levels() and that the seeding sample in
   buttons_watch_begin() happens before gpio_isr_handler_add(). The DECISION
   those two feed — what a level sample means for the release gate — is
   host-tested in button_latch.c, so again what rests on review is only
   whether the samples are taken. test_wake_flow's take stubs mirror both
   calls, which puts the gate itself under the coalescer; what they cannot
   mirror is this file forgetting one. */
void buttons_configure_wakeup_if(bool enable) {
    /* A locked sleep arms nothing: leave the RTC domain exactly as the
       last sleep left it, on a battery that cannot spare the work. */
    if (!enable)
        return;
    /* THE NVS READS ON THIS PATH, and they are worth naming because of
       where they land. Two gates below reach the names blob and BOTH
       short-circuit before they do — button_a_toggle_allowed() on the
       timer state, button_chore_ack_allowed() on the RTC mode byte. The
       cost per sleep, stated as the three cases rather than as a bound,
       because the worst of them is not the one a reader guesses:

         Timers mode, no timer running   ONE read. A's gate goes to flash;
                                         C's short-circuits on the mode.
         Timers mode, a timer running    NONE. A's gate short-circuits on
                                         TIMER_RUNNING first.
         CHORE mode                      TWO FULL READS, every sleep. Both
                                         gates pass their cheap half, and
                                         each loads the whole names blob
                                         into its own 64-byte stack buffer
                                         (button_actions.c) to ask a
                                         different question of it.

       Two is the real chore-screen cost and there is no caching layer
       under it; it is accepted rather than unnoticed. The chore screen is
       also the state a device sits in for seconds at a time, not hours,
       so the reads are bounded by presses and not by the clock.

       "A sleep while a timer runs reaches flash not at all" is the ONE
       claim here that is not local to this function. It needs BOTH gates
       to stay away from flash, and only A's is gated on the timer state:
       C's is gated on the mode alone, so a RUNNING timer with the mode
       byte saying CHORES would read the blob. That combination cannot
       occur, and the reason is emergent rather than enforced —
       button_a_apply() is the only writer of APP_MODE_CHORES and it
       refuses while RUNNING, timer_start() has exactly one caller
       (button_b_apply, button_actions.c) and B is rebound to an ack in
       chore mode, the awake join poll refuses to start from the chore
       screen (wake_flow.c), and no MQTT command starts a timer. CHORES
       therefore implies not RUNNING. Nothing asserts it; if a later task
       gives anything else a way to start a timer, this claim is the first
       thing it breaks and the breakage is silent — a second flash read
       per sleep, on the path that runs on every sleep.

       button_a_toggle_allowed() asks the chore names blob
       whether a list is configured — the count exists nowhere else, the
       RTC block holding only the acks, the release and the mode (timer.h)
       — and main.c has already called hal_nvs_close() by the time it gets
       here. hal_nvs's open is lazy, so this REOPENS the wake-scoped
       handle that was just released; deep sleep then drops it a few lines
       later instead of that close doing so. The close's own claim ("the
       last NVS WRITE is behind us") stays true — this is a read.
       The alternative was an approximation: arm A unconditionally and let
       the press be refused on arrival. That is the wrong trade here
       because it is the COMMON case — no device in the field has a chore
       list yet, so every mispress of A would buy a wake and a full panel
       refresh for nothing.
       The gate short-circuits on timer_get_state() first, so a sleep
       entered while a timer runs never reaches flash at all.
       STACK, because one caller is not the main task: the awake failsafe
       reaches here as awake_failsafe_cb -> enter_deep_sleep ->
       buttons_configure_wakeup_if, i.e. on the esp_timer task
       (CONFIG_ESP_TIMER_TASK_STACK_SIZE=3584). The gate's names buffer
       adds ~64 B there (button_actions.c). Running NVS from esp_timer on
       this path is pre-existing, not something this gate introduced. */
    /* THE LOCK IS REPORTED HERE AND APPLIED IN EXACTLY ONE PLACE, which
       is buttons_policy.c's early return. Each of the three gates below
       used to be written `!config_locked && ...` as well, on the grounds
       that a config-locked sleep arms D alone (buttons_policy.h) so their
       answers cannot change the mask, and two of them go to flash to
       produce one.

       THAT SPELT THE NARROWING TWICE, and the second copy was the one
       nothing could see: this file is in no host suite, and a mutant
       severing its half of the rule produced a test binary bit-identical
       to pristine — it never compiled. The two copies also fail
       differently, which is what settles it. Delete the policy's early
       return with these short-circuits present and A and C go dark with
       no rule anywhere saying they should be: the "primary control dead
       to the press" failure this module's policy exists to avoid, arrived
       at by accident and with no test to notice. Delete it with the
       narrowing spelt once and the mask merely widens back to what it was
       before the lock existed — a stray press and a wasted refresh.

       WHAT THAT GIVES UP, measured rather than asserted: the two flash
       reads the header above tabulates, a few ms of NVS, on a wake that
       has already spent up to NET_JOIN_TIMEOUT_MS (90 s) of radio — the
       config gate runs a window on EVERY wake where this flag is true,
       engage and locked re-wake alike (lock_gate.c). Three to five orders
       of magnitude apart on any plausible figure for a blob read. And the
       saving was never consistent in the first place: the same reads
       already happen unconditionally on the charge- and bed-time-locked
       sleeps, where `enable` is false and the entire mask is discarded a
       line later. */
    /* The no-clock lock (BUG-14) sleeps the config lock's sleep and wants
       the same mask: D alone, and a D press is its retry. Folded into the
       one field rather than given a second, because "D is the only exit"
       is one rule and buttons_policy.c applies it in one place. The fold
       itself is lock_gate_wake_d_only(), so test_lock_gate pins it; this
       file is in no host suite. */
    const bool config_locked = lock_gate_wake_d_only();
    buttons_policy_in_t pol = {
        .enable = enable,
        .config_locked = config_locked,
        .swap_allowed = timer_swap_allowed(),
        .mode_toggle_allowed = button_a_toggle_allowed(),
        /* C's chore binding (design 2.4): the middle checkbox. A SECOND
           reader of the names blob on this path. It short-circuits on the
           RTC mode byte, so off the checklist it costs one comparison and
           on it costs a full blob read — the second of the two the header
           above tabulates.

           NOT "the only time the gate above can have said yes", which is
           what stood here and is false in the direction that matters. The
           implication runs one way only. C's gate passing DOES imply A's
           passed: it needs a second configured row, so n >= 2 > 0, and it
           needs the mode byte to say CHORES, which by the invariant in
           the header means not RUNNING — both of A's conditions. The
           converse is what is false: A's gate says yes in TIMERS mode
           too, and it must, or A could never be armed to ENTER chore
           mode, which is the whole of M2-T3. So the two reads are not
           alternatives. In chore mode they both happen, and that is what
           bounds this path — the tabulated pair above, not an
           exclusion. */
        .chore_ack_allowed = button_chore_ack_allowed(BUTTON_CHORE_IDX_C),
        /* Read unconditionally, not just under CONFIG_MAGTAG_BOOT_WAKES:
           buttons_init() always configures GPIO0 as a digital input (see
           its comment), so the read is always valid, and keeping it out
           of an #if here means this struct literal has one shape instead
           of two. buttons_policy_boot_wake_allowed() below is the only
           reader, and it is never called when the Kconfig is off. */
        .boot_currently_down = gpio_get_level(GPIO_NUM_0) == 0,
    };
    uint8_t wake = buttons_policy_wake_mask(&pol);
    buttons_watch_end();
    uint64_t mask = 0;
    for (int i = 0; i < BTN_NONE; i++) {
        if (!(wake & (1u << i)))
            continue;
        gpio_num_t pin = BTN_GPIOS[i];
        rtc_gpio_init(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(pin); /* buttons are active-low; hold high in sleep */
        rtc_gpio_pulldown_dis(pin);
        mask |= 1ULL << pin;
    }
#if CONFIG_MAGTAG_BOOT_WAKES
    /* BOOT (GPIO0) is not a button_id_t and so never appears in `wake`
       above (buttons_policy.c's mask knows nothing about it); arm it
       separately, gated by buttons_policy_boot_wake_allowed() rather than
       OR'd in unconditionally, for two reasons:
         - it obeys config_locked, same as everything but D. Arming BOOT
           there as a "way out of a bad WiFi config" would break S21
           (docs/architecture.md, lock_gate.h): the config-error sleep's
           one exit is D, and BOOT does not get a second one stacked on
           it. A no-SSID device still recovers: D wakes it, and
           setup_trigger_decide() sends a no-SSID button wake to setup.
         - it refuses to arm while GPIO0 already reads low. EXT1 is
           level-triggered: arming a pad that is already down wakes the
           device the instant it reaches deep sleep, and keeps doing so
           for as long as the press lasts, because a BOOT-only wake
           resolves to no button_id_t at all and so cannot satisfy the
           continuation guard (wake_flow.c) the way a held A-D press can.
           Sampling the level here, before the pad moves to the RTC mux,
           is what buttons_policy_in_t.boot_currently_down is for. */
    if (buttons_policy_boot_wake_allowed(&pol)) {
        rtc_gpio_init(GPIO_NUM_0);
        rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(GPIO_NUM_0); /* active-low, like the other four */
        rtc_gpio_pulldown_dis(GPIO_NUM_0);
        mask |= 1ULL << GPIO_NUM_0;
    }
#endif
    /* GPIO wakeup (esp_sleep_enable_gpio_wakeup) is light-sleep-only on
       ESP32-S2 — deep sleep requires EXT1 on RTC-capable pins
       (0/11/12/14/15 all are). */
    esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ext1 wakeup config failed: %s", esp_err_to_name(ret));
    }
}

button_id_t buttons_get_wakeup_button(void) {
    if (!(esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1))) {
        return BTN_NONE;
    }

    /* EXT1 status latches which pin(s) triggered the wake */
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    for (int i = 0; i < 4; i++) {
        if (status & (1ULL << BTN_GPIOS[i])) {
            ESP_LOGI(TAG, "Wakeup button: %d (GPIO %d)", i, BTN_GPIOS[i]);
            return (button_id_t)i;
        }
    }

    if (status != 0) {
        /* The latch is not empty, it just names no A-D pad — the only
           other pin EXT1 can ever be armed on is GPIO0 (BOOT), so this is
           a BOOT-only wake (or BOOT plus a since-released A-D pad; either
           way no A-D pad is what woke it). That is not a dead press to
           warn about below: nothing here claims to identify BOOT,
           buttons_woke_by_boot() does. Falling through to the level scan
           would be worse than silent — this wake already told us which
           pin(s) triggered it, and a merely-HELD A-D pad that was not one
           of them would get blamed for a wake it could not have caused. */
        return BTN_NONE;
    }

    /* Fallback: latch was empty — debounce then scan levels, but only
       across pads that COULD have been armed. The maximal mask (every
       gate open) is the set of buttons the policy will arm under some
       condition; a button outside it cannot have caused this EXT1 wake
       whatever the user happens to be holding. Derived from the policy
       rather than hardcoded so that buttons_policy.c stays the single
       source of truth, and so a button that becomes conditionally armed
       keeps being scanned — a conditional button is still in the maximal
       mask. Nothing is retained across the sleep: the armed mask was
       computed before it and RAM is gone by now, so recomputing the bound
       is the only option anyway. NB: designated initializer — a gate
       field added to buttons_policy_in_t defaults to false here and would
       narrow this below maximal; any new gate must be set true.
       The primary path above needs no such filter: an unarmed pad can
       never appear in the EXT1 status latch.

       WHAT THE FILTER STOPPED BUYING when A gained a binding, and why
       there is a second line below it now: the mask used to exclude A
       outright, so a wake genuinely caused by B, C or D while A was also
       held could not be misreported as BTN_A. That was never the mask's
       purpose — it fell out of A having no gate — and admitting A to the
       maximal mask spent it.

       Index order alone is NOT an acceptable tie-break here, and calling
       the returned ambiguity "genuine" would be wrong. The maximal mask
       is COUNTERFACTUAL: it asks "could any gate arm this pad?", whereas
       the mask that actually armed this sleep was the real one. A's gate
       (button_a_toggle_allowed) is false on every device with no chore
       list configured — which buttons_policy.c notes is the whole fleet
       as it ships. On such a device a held A provably could not have
       caused this wake, yet A is index 0 and would win a first-match
       scan; the toggle would then be refused by its own gate, the tail
       would repaint, and the real B press would be gone with no feedback
       at all. That is the "primary control dead to the press" failure
       this module's policy is otherwise built to avoid, and button_latch.c
       already records having fixed exactly this bug in the latch pick.

       So resolve through button_latch_pick, which IS that policy: pure,
       host-tested, B > C > D > A, and already the tie-break both latch
       drains use. Reusing it is not a second policy to keep in step — it
       is the one policy, called from a third place. Where the ambiguity
       IS genuine (a chore list configured, A really armable, two pads
       held), yielding to the time-sensitive press is the same answer the
       drains give, which is the point. The scan stays best-effort by
       construction: it runs only when the EXT1 status latch came back
       empty, and with two pads held no information survives to say which
       one fired. */
    const buttons_policy_in_t maximal = {
        .enable = true,
        .swap_allowed = true,
        .mode_toggle_allowed = true,
        /* Set for the rule above, not because it widens anything: C is
           already in the mask through swap_allowed, so the ack gate adds
           no pad. It is here so the literal stays MAXIMAL by
           construction — the next gate to arrive may be the one that
           does. */
        .chore_ack_allowed = true,
        /* config_locked is left out, and FALSE is the maximal value for
           it — it is the one field that NARROWS. Setting it true here
           would cut the fallback level scan down to button D and lose
           every other press this path exists to recover. The literal is
           maximal by VALUE, not by mentioning every field. */
    };
    const uint8_t armable = buttons_policy_wake_mask(&maximal);
    esp_rom_delay_us(DEBOUNCE_US);
    const int pick = button_latch_pick(buttons_scan_held(), armable);
    if (pick >= 0) {
        return (button_id_t)pick;
    }
    ESP_LOGW(TAG, "EXT1 wakeup but no button identified");
    return BTN_NONE;
}

bool buttons_is_pressed(button_id_t btn) {
    if (btn == BTN_NONE || (int)btn >= 4)
        return false;
    return gpio_get_level(BTN_GPIOS[(int)btn]) == 0;
}

uint8_t buttons_scan_held(void) {
    uint8_t mask = 0;
    for (int b = 0; b < 4; b++) {
        if (buttons_is_pressed((button_id_t)b))
            mask |= (uint8_t)(1u << b);
    }
    return mask;
}

bool buttons_is_boot_pressed(void) {
    return gpio_get_level(GPIO_NUM_0) == 0; /* active-low, like the other four */
}

bool buttons_woke_by_boot(button_id_t wakeup_button) {
#if CONFIG_MAGTAG_BOOT_WAKES
    /* BOOT ranks last, full stop: whatever A-D button
       buttons_get_wakeup_button() already resolved for this wake —
       by EXT1 status order on its primary path, by button_latch_pick's
       B > C > D > A on its fallback scan — outranks BOOT outright, so
       this never even looks at GPIO0. See button_latch_boot_wins() for
       why that one line is the whole rule. */
    if (!button_latch_boot_wins(wakeup_button == BTN_NONE ? -1 : (int)wakeup_button))
        return false;
    /* No level-read fallback here, unlike buttons_get_wakeup_button()'s
       empty-latch scan: that scan has a second button to
       disambiguate against when the latch comes back empty, so a
       debounced level read is still informative. This function has only
       one candidate, GPIO0, so a level read with no corroborating EXT1
       cause would read a timer tick or a cold boot that merely happens
       to catch BOOT held as "BOOT woke it" — exactly the false positive
       setup_trigger_decide()'s no-SSID rule depends on not seeing. An
       empty or absent EXT1 status settles as "not BOOT". */
    if (!(esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1)))
        return false;
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    return (status & (1ULL << GPIO_NUM_0)) != 0;
#else
    (void)wakeup_button;
    return false; /* GPIO0 is never armed, so it cannot have caused a wake */
#endif
}
