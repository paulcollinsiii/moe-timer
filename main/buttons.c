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

/* Latch presses while awake. No false latch for the wake button: it is
   already low at boot, so no falling edge fires. */
static void buttons_watch_begin(void) {
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) { /* INVALID_STATE = already installed */
        ESP_LOGE(TAG, "isr service install failed: %s", esp_err_to_name(ret));
        return;
    }
    portENTER_CRITICAL(&s_latch_mux);
    button_latch_reset();
    portEXIT_CRITICAL(&s_latch_mux);
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

uint8_t buttons_take_pressed(void) {
    portENTER_CRITICAL(&s_latch_mux);
    uint8_t mask = button_latch_take();
    portEXIT_CRITICAL(&s_latch_mux);
    return mask;
}

uint8_t buttons_take_pressed_mask(uint8_t mask) {
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
   which is a bigger move than the policy carve. */
void buttons_configure_wakeup_if(bool enable) {
    /* A locked sleep arms nothing: leave the RTC domain exactly as the
       last sleep left it, on a battery that cannot spare the work. */
    if (!enable)
        return;
    /* THE ONE NVS READ ON THIS PATH, and it is worth naming because of
       where it lands. button_a_toggle_allowed() asks the chore names blob
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
    buttons_policy_in_t pol = {
        .enable = enable,
        .swap_allowed = timer_swap_allowed(),
        .mode_toggle_allowed = button_a_toggle_allowed(),
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
    /* GPIO wakeup (esp_sleep_enable_gpio_wakeup) is light-sleep-only on
       ESP32-S2 — deep sleep requires EXT1 on RTC-capable pins
       (11/12/14/15 all are). */
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
