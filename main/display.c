#include "display.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display_screens.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "panic_diag.h"
#include "sdkconfig.h"
#include "ssd1680.h"

static const char *TAG = "display";

/* MagTag EPD pinout (Adafruit schematic) */
static const ssd1680_pins_t PINS = {
    .pin_sclk = 36, .pin_mosi = 35, .pin_cs = 8, .pin_dc = 7, .pin_rst = 6, .pin_busy = 5};

/* LVGL I1 draw buffer: 8-byte palette header + 1 bit per pixel */
static uint8_t s_lvbuf[8 + DISP_HOR * DISP_VER / 8];
static uint8_t s_panel_fb[SSD1680_FB_SIZE];
static uint8_t s_panel_clean[SSD1680_FB_SIZE]; /* inverse pass of the double partial */
/* Previous displayed frame, kept across deep sleep so the cleaning pass can
   be limited to the characters that actually changed. Zeroed (invalid) on
   power-on reset, like the panel's own previous-frame RAM. */
static RTC_DATA_ATTR uint8_t s_prev_fb[SSD1680_FB_SIZE];
static RTC_DATA_ATTR bool s_prev_fb_valid;
static lv_display_t *s_disp;
static bool s_initialized;
static bool s_panel_slept; /* panel in deep sleep — must re-init before next flush */
static ssd1680_refresh_mode_t s_pending_mode = SSD1680_REFRESH_FULL;
/* Whether the pending partial gets the ghost-cleaning double pass.
   display_refresh_plan() answers it; render() is the only writer, so no
   paint path can leave a stale value behind for the next one. */
static bool s_pending_clean;
/* Partial/full cadence counter (policy, distinct from the driver's
   protection guard). Display-owned RTC state — the cadence survives deep
   sleep without living in the timer module's rtc_state_t. The arithmetic
   over it is display_refresh_plan()'s, in display_layout.c, because this
   file is not compiled by any host suite. */
static RTC_DATA_ATTR uint8_t s_partial_count;

/* Which of the three display_screen_t kinds the frame now on the glass
   drew, and whether that is known at all — the input display_refresh_plan()
   needs to tell an ack-sized chore partial from a whole-screen change INTO
   the checklist. Only the first is exempt from the ghost-cleaning pass.

   SAME RTC SEMANTICS AS s_prev_fb_valid ALONGSIDE, and for the same
   reason: it describes what is physically on the panel, so it must
   survive deep sleep and must NOT survive esp_restart(). It does both.
   The bootloader reloads .rtc.data on every reset except a deep-sleep
   wake (see s_takeover_on_panel's declaration for the citation), so after
   a reboot the valid flag comes back false and the next partial cleans —
   which is correct, because ssd1680.c's own previous-frame flag was wiped
   by the same reboot and the driver is promoting that paint to a full
   refresh anyway.

   A SEPARATE FLAG RATHER THAN A FOURTH ENUMERATOR. "No known previous
   screen" is not a screen, and adding DISPLAY_SCREEN_NONE would silently
   weaken every -Wswitch-exhaustive switch over this type (build_for_state()
   below is one) into one that compiles with a case nobody thought about.
   The flag costs a byte of RTC memory and costs the switches nothing. */
static RTC_DATA_ATTR display_screen_t s_prev_screen;
static RTC_DATA_ATTR bool s_prev_screen_valid;

/* "The panel is showing a full-screen takeover." Set by display_ota(),
   consumed by the next display_update().

   Zeroing s_partial_count is NOT enough on its own: the next
   display_update() increments it to 1, and 1 < DISPLAY_FULL_REFRESH_EVERY_N,
   so the paint that lands on top of a 28 pt full-panel headline is a
   PARTIAL — the worst case for ghosting. lock_gate_promote_render()
   solves exactly this for the lock screens, one layer up; this is the
   same idea expressed where the takeover is painted, so no caller has to
   remember.

   WHAT THIS FLAG ACTUALLY COVERS: the repaint that follows a takeover
   WITHIN THE SAME WAKE. That is ota_flow's failure-path repaint (the
   update screen goes up, the download fails, wake_flow repaints the
   normal layout) and the equivalent after a lock screen. RTC_DATA_ATTR
   so it also survives DEEP SLEEP, which is a real case: the awake
   failsafe can sleep the device with the update screen still on the
   glass, and the next wake's first paint has to be full.

   WHAT IT DOES NOT COVER, corrected from an earlier claim here that it
   did: esp_restart(). RTC_DATA_ATTR does not survive a software reset on
   the ESP32-S2. The bootloader loads the .rtc.data segment on every reset
   EXCEPT a deep-sleep wake (esp_image_format.c: `load_rtc_memory =
   esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP`), so after
   the OTA reboot this flag comes back as whatever the new image's
   initialiser says — false.

   The post-OTA first paint is full anyway, and it is worth knowing by
   WHICH mechanism, because it is not this one. ssd1680.c's
   s_prev_frame_valid is RTC_DATA_ATTR too and is wiped by the same
   reboot, so ssd1680_resolve_refresh_mode() promotes the requested
   PARTIAL to FULL and logs "partial promoted to full: no valid previous
   frame this power cycle". The driver guard is doing the work. Nobody
   should "simplify it away" on the strength of the flag above, and no
   later task needs to add code for the post-reboot case — it is already
   correct, for this reason rather than for the one previously written
   down here. */
static RTC_DATA_ATTR bool s_takeover_on_panel;

/* Bring-up knobs: if the image is rotated 180 deg or mirrored on hardware,
   flip these (see docs/hardware_smoke_test.md step 2). */
#define ROT_FLIP_X 0
#define ROT_FLIP_Y 1

static uint32_t tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Landscape row bands holding per-wake content. Partial refreshes drive
   them inverse->true — a localized flash — so the content does not
   accumulate ghosting between the every-5th-wake full refreshes. Extents
   are the exact widget geometry from display_screens.c (align y + font
   line_height / bar height); byte alignment then widens each edge outward
   by up to 7 rows — that slop is the packed-framebuffer format, not
   margin. Bands must not share a framebuffer byte (y/8): a shared byte
   would be inverted twice and cancel out.

   NOTE what this table does NOT constrain. After byte alignment the four
   bands are 0..2, 3..6, 7..10 and 11..15 of a 16-byte row — contiguous,
   and jointly every byte of every row. So no widget can land outside a
   band, and "does this new label need a band?" is never the question for
   a widget on the main screen; the answer is always no. The rule the
   comment above states is a constraint on the BAND EXTENTS themselves
   (keep them from sharing a byte with each other), which is why adding a
   fifth band or moving a row boundary is the change that needs care. A
   screen that only ever full-refreshes needs no entry either, for the
   different reason that this table is read only on the partial path.

   AND THE CHORE SCREEN NEEDS NO ENTRY FOR THE FIRST REASON, not for a
   special one. §2.5's exemption (display_refresh_plan()) keeps most chore
   partials away from this table, but NOT all of them: it covers only
   CHORES -> CHORES, so the partial that changes the screen INTO the
   checklist cleans and does read these bands. That is fine, and by the
   joint-coverage property above rather than by luck — the four bands are
   every byte of every row, so the checklist's rows land inside them just
   as the break screen's do, and display_fb_invert_dirty_rows() limits the
   flash to bytes that actually changed. The extents are the MAIN screen's
   widget geometry and always were; no screen needs its own table. */
static const struct {
    int y0, y1; /* inclusive landscape rows */
} CLEAN_BANDS[] = {
    {3, 17},   /* header: date/time + last sync (12 pt at y=3) */
    {26, 49},  /* progress bar (y=26, h=24) + Charge Me!!! badge */
    {58, 87},  /* remaining time (28 pt at y=58) + battery % (12 pt at y=66) */
    {95, 125}, /* mode/state row (12 pt, bottom -18) + button row (bottom -2);
                  one band — their byte ranges would otherwise overlap */
};

/* Invert band bytes only where fb differs from the previous frame; returns
   the number of dirty portrait rows (0 = nothing in the bands changed). */
static int invert_clean_bands(uint8_t *fb) {
    int dirty = 0;
    for (size_t i = 0; i < sizeof(CLEAN_BANDS) / sizeof(CLEAN_BANDS[0]); i++) {
        /* Landscape row y maps to panel x bit px (see flush_cb transpose) */
        int p0 = ROT_FLIP_X ? (DISP_VER - 1 - CLEAN_BANDS[i].y1) : CLEAN_BANDS[i].y0;
        int p1 = ROT_FLIP_X ? (DISP_VER - 1 - CLEAN_BANDS[i].y0) : CLEAN_BANDS[i].y1;
        dirty += display_fb_invert_dirty_rows(fb, s_prev_fb, SSD1680_HEIGHT, SSD1680_WIDTH / 8, p0 / 8, p1 / 8);
    }
    return dirty;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)area;                      /* RENDER_MODE_FULL: always the whole frame */
    const uint8_t *src = px_map + 8; /* skip I1 palette header */

    /* Every flush ends in panel deep sleep (mode 1, registers lost). A
       second render in the same awake period — final-minute TIME'S UP,
       post-alert main screen — must wake and re-init the panel first, or
       it silently writes to a sleeping controller. */
    if (s_panel_slept) {
        esp_err_t ret = ssd1680_init(&PINS);
        if (ret != ESP_OK) {
            /* BUSY can straggle coming out of panel deep sleep — retry
               once rather than silently dropping the frame (a dropped
               frame leaves e.g. the big TIME'S UP screen stuck). */
            vTaskDelay(pdMS_TO_TICKS(100));
            ret = ssd1680_init(&PINS);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "panel re-init failed - dropping frame");
            lv_display_flush_ready(disp);
            return;
        }
        s_panel_slept = false;
    }

    /* Two renders in one wake (countdown step → pause/break/alert) can
       land inside the driver's minimum refresh interval, and the guard
       silently DROPS the frame — the panel would keep the stale screen
       (field-observed with the mid-watch break). Absorb the remainder
       here so every accepted flush actually reaches glass. */
    int32_t guard_wait = ssd1680_refresh_wait();
    if (guard_wait > 0) {
        vTaskDelay(pdMS_TO_TICKS(guard_wait * 1000 + 100)); /* +margin: guard is second-granular */
    }

    /* Transpose landscape 296x128 -> panel portrait 128x296. */
    memset(s_panel_fb, 0, sizeof(s_panel_fb));
    for (int y = 0; y < DISP_VER; y++) {
        for (int x = 0; x < DISP_HOR; x++) {
            int bit = (src[y * (DISP_HOR / 8) + x / 8] >> (7 - (x & 7))) & 1;
            /* LVGL I1: 1 = white. Panel fb convention: 1 = black. */
            if (!bit) {
                int px = ROT_FLIP_X ? (DISP_VER - 1 - y) : y;
                int py = ROT_FLIP_Y ? (DISP_HOR - 1 - x) : x;
                s_panel_fb[py * (SSD1680_WIDTH / 8) + px / 8] |= (uint8_t)(0x80 >> (px & 7));
            }
        }
    }

    /* Ghost-cleaning double partial: pass 1 inverts the changed characters
       inside the text bands, pass 2 restores the true frame, so those
       pixels are driven both ways. Skipped when nothing in the bands
       changed or when previous-frame state is invalid (driver would
       promote to full anyway), and skipped by policy on a chore partial
       that follows another chore paint (s_pending_clean — see
       display_refresh_plan(), which exempts CHORES -> CHORES and nothing
       else, so a partial that changes screen still cleans). The intermediate
       pass doesn't re-arm the refresh-rate guard (both passes are one
       render), so pass 2 starts the moment BUSY releases — no fixed
       inter-pass delay.

       The driver's minimum-interval guard is NOT part of this decision and
       is absorbed above unconditionally: a paint that skips the double
       pass still waits out the 1 s floor like any other. */
    if (s_pending_clean && s_pending_mode == SSD1680_REFRESH_PARTIAL && ssd1680_partial_diff_ready() &&
        s_prev_fb_valid) {
        memcpy(s_panel_clean, s_panel_fb, sizeof(s_panel_clean));
        if (invert_clean_bands(s_panel_clean) > 0 && ssd1680_write_framebuffer(s_panel_clean) == ESP_OK) {
            ssd1680_refresh_intermediate(SSD1680_REFRESH_PARTIAL);
        }
    }

    if (ssd1680_write_framebuffer(s_panel_fb) == ESP_OK) {
        if (ssd1680_refresh(s_pending_mode) == ESP_OK) { /* errors logged inside */
            memcpy(s_prev_fb, s_panel_fb, sizeof(s_prev_fb));
            s_prev_fb_valid = true;
        }
    }
    ssd1680_sleep();
    s_panel_slept = true;
    lv_display_flush_ready(disp);
}

void display_init(void) {
    if (ssd1680_init(&PINS) != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed - continuing headless");
    }
    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(DISP_HOR, DISP_VER);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(s_disp, s_lvbuf, NULL, sizeof(s_lvbuf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    s_initialized = true;
}

/* The single choke point every paint in this file goes through, which is
   why the RENDER breadcrumb is here rather than at eight call sites.
   Covers the LVGL render pass AND the synchronous flush underneath it
   (the SPI writes and the panel's BUSY wait), which between them are the
   longest uninterruptible stretch of a quiet wake.

   ENTER/EXIT rather than a plain mark, because RENDER nests: it sits
   inside AWAKE, or inside BOOT, or inside SLEEP, and a paint that left
   the phase reading RENDER afterwards would misreport every subsequent
   panic in that wake. The previous value is restored rather than
   hard-coded back to AWAKE for the same reason.

   This function is also reached from the OTA task
   (wake_flow_repaint_current_state and display_ota), so the MAIN slot is
   written by two tasks — never concurrently, because ota_task_run_apply
   blocks the main task on a semaphore for the whole attempt. That
   produces the honest reading "RENDER+OTA_DL" while the download paints
   its progress screen. panic_diag.h carries the argument in full.

   `ghost_clean` is a parameter rather than a static every caller has to
   remember to set: flush_cb reads it from inside the lv_refr_now() below,
   so a paint path that forgot to write it would silently inherit the
   PREVIOUS paint's answer — and on this panel that is a wrong refresh
   nobody can see from here. As a parameter the compiler asks instead.
   Only a PARTIAL can act on it, so the full-refresh entry points below
   pass false; that is not a new restriction on them, it is what flush_cb's
   own mode test already gave them. */
static void render(ssd1680_refresh_mode_t mode, bool ghost_clean) {
    const panic_phase_t prev = panic_diag_enter(PANIC_PHASE_RENDER);
    s_pending_mode = mode;
    s_pending_clean = ghost_clean;
    lv_refr_now(s_disp); /* renders + calls flush_cb synchronously */
    panic_diag_exit(PANIC_PHASE_RENDER, prev);
}

/* A plain switch over display_screen_for() and nothing else: the
   precedence between the three layouts — in particular that chore mode
   outranks the break screen, so §2.6's "A -> Chores" prompt leads
   somewhere — lives in that pure function, where test_display can reach
   it. Every enumerator listed and no default, so -Wswitch (an error under
   IDF's -Wall -Werror) catches a fourth screen kind that nobody wired up
   here rather than letting it paint the timer screen.

   Returns the screen it built so display_update() can hand it to
   display_refresh_plan() without asking display_screen_for() a second
   time — two calls could not disagree today, but the screen that was
   PAINTED is the one the refresh policy is about, and taking it from here
   is what keeps that true.

   It also RECORDS that screen, for the next paint to compare against.
   Here rather than at the two call sites because this is the one place a
   display_screen_t is turned into a frame: both callers (display_update
   and display_full_refresh) go on to render unconditionally, including
   display_update's takeover early-return below — that path has already
   built this screen and paints it full, so the record is right for it
   too. A future caller that built a screen and then did NOT paint it
   would break that, and there is no such caller; the alternative, a
   record updated at each render() site, is the one that has been got
   wrong before.

   `prev_out` HANDS BACK WHAT THE RECORD SAID ON ENTRY, and exists so that
   a caller needing the previous screen cannot read it too late. The
   overwrite happens here, so a caller that read the statics itself would
   have to do so on the line BEFORE this call — an ordering requirement
   that holds today and that nothing would enforce tomorrow. Returning the
   old value from inside the swap makes it unorderable instead. NULL for
   callers that do not need it. Pass it to display_refresh_plan() and
   nowhere else: it describes the glass, not the state.

   CALLERS THAT PAINT SOMETHING ELSE must call forget_painted_screen()
   instead — see it. */
typedef struct {
    display_screen_t screen;
    bool valid;
} painted_screen_t;

static display_screen_t build_for_state(const display_state_t *st, painted_screen_t *prev_out) {
    const display_screen_t screen = display_screen_for(st->timer_state, st->app_mode, st->chore_count);
    if (prev_out != NULL) {
        prev_out->screen = s_prev_screen;
        prev_out->valid = s_prev_screen_valid;
    }
    s_prev_screen = screen;
    s_prev_screen_valid = true;
    switch (screen) {
        case DISPLAY_SCREEN_CHORES:
            display_screens_build_chores(st);
            break;
        case DISPLAY_SCREEN_BREAK:
            display_screens_build_break(st);
            break;
        case DISPLAY_SCREEN_MAIN:
            display_screens_build_main(st);
            break;
    }
    return screen;
}

/* The takeover painters below (TIME'S UP, Charge Me, sync failed,
   bedtime, the OTA banner) do not go through build_for_state(): what they
   put on the glass is none of the three display_screen_t kinds, so the
   record cannot name it and must say so.

   NOT MERELY TIDINESS. Only display_ota() also sets s_takeover_on_panel;
   the other four do not, so the paint after one of them takes the ordinary
   cadence path. Leave the record alone and a checklist painted after a
   TIME'S UP screen would read back the CHORES it left there before the
   alert, match, and skip the cleaning pass over a 28 pt full-panel
   headline — the worst frame on this device to skip it on. */
static void forget_painted_screen(void) {
    s_prev_screen_valid = false;
}

void display_update(const display_state_t *st) {
    if (!s_initialized)
        display_init();
    /* `prev` is what the record said before this build overwrote it —
       the screen on the glass right now. The plan below is a question
       about that frame, not about this one. */
    painted_screen_t prev;
    const display_screen_t screen = build_for_state(st, &prev);
    /* A takeover screen is still on the glass — from earlier in this
       wake, or from before a deep sleep. NOT from before a reboot: the
       flag does not survive esp_restart (see its declaration), and the
       driver's own previous-frame guard is what covers that case. Promote
       this paint to a full refresh and clear the flag. It returns before
       display_refresh_plan() is reached, so the chore exemption cannot
       reach a repaint over a takeover even when the checklist is what
       paints next — a full refresh over a 28 pt headline is not something
       the cadence gets a say in.

       That covers display_ota() only, because it is the only painter that
       sets the flag. The other four takeovers (TIME'S UP, Charge Me, sync
       failed, bedtime) fall through to the cadence below, and what keeps
       the exemption off THOSE is forget_painted_screen() — see it. */
    if (s_takeover_on_panel) {
        s_takeover_on_panel = false;
        s_partial_count = 0;
        render(SSD1680_REFRESH_FULL, false);
        return;
    }
    /* Policy: full refresh every Nth partial (anti-ghosting), and which
       partials get the ghost-cleaning double pass. Both answers come from
       display_layout.c so they can be host-tested; the counter lives in
       RTC memory so the cadence survives deep sleep. */
    const display_refresh_plan_t plan = display_refresh_plan(screen, prev.screen, prev.valid, &s_partial_count);
    render(plan.full ? SSD1680_REFRESH_FULL : SSD1680_REFRESH_PARTIAL, plan.ghost_clean);
}

void display_full_refresh(const display_state_t *st) {
    if (!s_initialized)
        display_init();
    build_for_state(st, NULL);   /* a full refresh asks the plan nothing */
    s_takeover_on_panel = false; /* this paint is already the full one */
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL, false);
}

void display_timesup(void) {
    if (!s_initialized)
        display_init();
    display_screens_build_timesup();
    forget_painted_screen();
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL, false);
}

void display_charge_me(void) {
    if (!s_initialized)
        display_init();
    display_screens_build_charge_me();
    forget_painted_screen();
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL, false);
}

void display_sync_failed(void) {
    if (!s_initialized)
        display_init();
    display_screens_build_sync_failed();
    forget_painted_screen();
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL, false);
}

void display_bedtime(void) {
    if (!s_initialized)
        display_init();
    display_screens_build_bedtime();
    forget_painted_screen();
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL, false);
}

/* Painted between the OTA check window and the download window, with the
   radio down: net_window.c documents that a panel refresh coinciding with
   a WiFi TX burst browns out the rail. Full refresh only, so it needs no
   CLEAN_BANDS entry (that table is read only on the partial path). */
void display_ota(const char *from_version, const char *to_version) {
    if (!s_initialized)
        display_init();
    display_screens_build_ota(from_version, to_version);
    /* Whatever paints next — the failure-path repaint, or the first paint
       of the new firmware after the reboot — must be full, not a partial
       over a full-panel 28 pt headline. See s_takeover_on_panel. */
    s_takeover_on_panel = true;
    forget_painted_screen(); /* belt and braces: the flag above already forces full */
    s_partial_count = 0;
    render(SSD1680_REFRESH_FULL, false);
}
