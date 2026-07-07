#include "timer.h"

#include <string.h>
#include <time.h>

#include "hal_time.h"

/* ---- RTC state ---- */
#ifndef NATIVE
rtc_state_t RTC_DATA_ATTR g_rtc_state;
#else
rtc_state_t g_rtc_state;
#endif

/* Slot definitions live in rodata, not RTC memory — re-injected every boot
   (timer_defs.c on firmware, the test table on host). */
static const timer_def_t *s_defs;
static int s_defs_count;

void timer_set_defs(const timer_def_t *defs, int count) {
    s_defs = defs;
    s_defs_count = (defs == NULL) ? 0 : count;
    if (s_defs_count > TIMER_SLOT_COUNT)
        s_defs_count = TIMER_SLOT_COUNT;
}

static bool slot_enabled(int slot) {
    if (slot == 0)
        return true; /* Screen always exists */
    if (slot < 0 || slot >= s_defs_count)
        return false;
    return s_defs[slot].name != NULL && s_defs[slot].name[0] != '\0' && s_defs[slot].duration_sec > 0;
}

static timer_slot_state_t *active(void) {
    return &g_rtc_state.slots[g_rtc_state.active_slot];
}

int timer_active_slot(void) {
    return g_rtc_state.active_slot;
}

const timer_def_t *timer_active_def(void) {
    int slot = g_rtc_state.active_slot;
    if (slot == 0 || slot >= s_defs_count)
        return NULL;
    return &s_defs[slot];
}

int timer_extra_count(void) {
    int n = 0;
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        if (slot_enabled(i))
            n++;
    }
    return n;
}

bool timer_any_reloadable(void) {
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        if (slot_enabled(i) && s_defs[i].reloadable)
            return true;
    }
    return false;
}

bool timer_select_next(void) {
    timer_state_t st = active()->state;
    if (st == TIMER_RUNNING || st == TIMER_BREAK)
        return false; /* pause first; BREAK is enforced */
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        int cand = (g_rtc_state.active_slot + i) % TIMER_SLOT_COUNT;
        if (slot_enabled(cand)) {
            g_rtc_state.active_slot = (uint8_t)cand;
            return true;
        }
    }
    return false; /* no other enabled slot */
}

bool timer_reload(void) {
    timer_slot_state_t *sl = active();
    if (sl->state == TIMER_RUNNING)
        return false; /* pause first */
    uint16_t completions = sl->completions;
    memset(sl, 0, sizeof(*sl));
    sl->state = TIMER_IDLE;
    sl->completions = completions; /* reload never counts as a run */
    return true;
}

timer_state_t timer_get_state(void) {
    return active()->state;
}

int64_t timer_expiry_wall(void) {
    return active()->expiry_wall_time;
}

uint16_t timer_completions(void) {
    return active()->completions;
}

void timer_reset(void) {
    memset(&g_rtc_state, 0, sizeof(g_rtc_state));
    /* all slots IDLE (=0), active_slot 0 (Screen), counters cleared */
}

void timer_start(time_t now, int32_t allocation_sec) {
    timer_slot_state_t *sl = active();
    sl->state = TIMER_RUNNING;
    sl->allocation_sec = allocation_sec;
    sl->expiry_wall_time = (int64_t)now + allocation_sec;
    sl->run_accum_sec = 0;
    sl->run_started_wall = (int64_t)now;
}

int32_t timer_tick(time_t now) {
    timer_slot_state_t *sl = active();
    if (sl->state == TIMER_BREAK) {
        if ((int64_t)now >= sl->break_expiry_wall) {
            sl->state = TIMER_PAUSED; /* break over — wait for manual resume */
            sl->break_expiry_wall = 0;
        }
        return sl->remaining_at_pause; /* screen-time stays frozen */
    }
    if (sl->state == TIMER_PAUSED) {
        return sl->remaining_at_pause;
    }
    if (sl->state != TIMER_RUNNING) {
        return 0; /* IDLE or EXPIRED */
    }
    int64_t remaining = sl->expiry_wall_time - (int64_t)now;
    if (remaining <= 0) {
        sl->state = TIMER_EXPIRED;
        sl->completions++; /* the run reached 00:00 */
        return (int32_t)remaining;
    }
    /* expiry_wall_time is NOT modified here */
    return (int32_t)remaining;
}

/* Fold the current run segment into the accrual counter. */
static void fold_run_segment(time_t now) {
    timer_slot_state_t *sl = active();
    if (sl->run_started_wall != 0) {
        int64_t seg = (int64_t)now - sl->run_started_wall;
        if (seg > 0)
            sl->run_accum_sec += (int32_t)seg;
        sl->run_started_wall = 0;
    }
}

void timer_pause(time_t now) {
    timer_slot_state_t *sl = active();
    if (sl->state != TIMER_RUNNING)
        return; /* no-op; caller checks state */
    int64_t remaining = sl->expiry_wall_time - (int64_t)now;
    sl->remaining_at_pause = (remaining > 0) ? (int32_t)remaining : 0;
    sl->expiry_wall_time = 0;
    fold_run_segment(now);
    sl->state = TIMER_PAUSED;
}

void timer_resume(time_t now) {
    timer_slot_state_t *sl = active();
    if (sl->state != TIMER_PAUSED)
        return; /* no-op; caller checks state */
    sl->expiry_wall_time = (int64_t)now + sl->remaining_at_pause;
    sl->run_started_wall = (int64_t)now;
    sl->state = TIMER_RUNNING;
}

int32_t timer_run_accum(time_t now) {
    timer_slot_state_t *sl = active();
    int32_t accum = sl->run_accum_sec;
    if (sl->state == TIMER_RUNNING && sl->run_started_wall != 0) {
        int64_t seg = (int64_t)now - sl->run_started_wall;
        if (seg > 0)
            accum += (int32_t)seg;
    }
    return accum;
}

bool timer_break_due(time_t now, int32_t interval_sec) {
    if (g_rtc_state.active_slot != 0)
        return false; /* eye-rest breaks are Screen-only */
    if (active()->state != TIMER_RUNNING || interval_sec <= 0)
        return false;
    return timer_run_accum(now) >= interval_sec;
}

void timer_start_break(time_t now, int32_t duration_sec) {
    timer_slot_state_t *sl = active();
    if (sl->state != TIMER_RUNNING)
        return;
    int64_t remaining = sl->expiry_wall_time - (int64_t)now;
    sl->remaining_at_pause = (remaining > 0) ? (int32_t)remaining : 0;
    sl->expiry_wall_time = 0;
    sl->run_accum_sec = 0; /* fresh 30-min window after the break */
    sl->run_started_wall = 0;
    sl->break_expiry_wall = (int64_t)now + duration_sec;
    sl->state = TIMER_BREAK;
}

int32_t timer_break_remaining(time_t now) {
    timer_slot_state_t *sl = active();
    if (sl->state != TIMER_BREAK)
        return 0;
    int64_t remaining = sl->break_expiry_wall - (int64_t)now;
    return (remaining > 0) ? (int32_t)remaining : 0;
}

void timer_shift_expiry(int64_t delta_sec) {
    /* An NTP sync may step time(NULL); every stored WALL time must step by
       the same amount so stored durations are preserved. PAUSED stores a
       duration — no shift. Only the active slot can be RUNNING/BREAK. */
    timer_slot_state_t *sl = active();
    if (sl->state == TIMER_RUNNING && sl->expiry_wall_time != 0) {
        sl->expiry_wall_time += delta_sec;
        if (sl->run_started_wall != 0)
            sl->run_started_wall += delta_sec;
    } else if (sl->state == TIMER_BREAK && sl->break_expiry_wall != 0) {
        sl->break_expiry_wall += delta_sec;
    }
}

static void fill_date(char *buf, int year, int mon, int day) {
    buf[0] = '0' + (year / 1000) % 10;
    buf[1] = '0' + (year / 100) % 10;
    buf[2] = '0' + (year / 10) % 10;
    buf[3] = '0' + year % 10;
    buf[4] = '-';
    buf[5] = '0' + mon / 10;
    buf[6] = '0' + mon % 10;
    buf[7] = '-';
    buf[8] = '0' + day / 10;
    buf[9] = '0' + day % 10;
    buf[10] = '\0';
}

bool timer_is_new_day(time_t now) {
    if (g_rtc_state.last_date[0] == '\0')
        return true;
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[11];
    fill_date(today, tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
    return (strcmp(today, g_rtc_state.last_date) != 0);
}

void timer_record_date(time_t now) {
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    fill_date(g_rtc_state.last_date, tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
}

bool timer_needs_ntp_sync(time_t now) {
    if (g_rtc_state.next_ntp_sync == 0)
        return true;
    return (int64_t)now >= g_rtc_state.next_ntp_sync;
}

void timer_record_ntp_sync(time_t now) {
    g_rtc_state.next_ntp_sync = (int64_t)now + NTP_SYNC_INTERVAL_SEC;
}

/* ---- crash-recovery snapshot ---- */

/* Widest plausible expiry horizon (also bounds allocation): corrupt data
   that slips past the checksum still cannot restore a nonsense timer. */
#define SNAPSHOT_MAX_HORIZON_SEC (7 * 86400)

uint8_t timer_snapshot_checksum(const timer_snapshot_t *snap) {
    timer_snapshot_t tmp = *snap;
    tmp.checksum = 0;
    const uint8_t *p = (const uint8_t *)&tmp;
    uint8_t x = 0;
    for (size_t i = 0; i < sizeof(tmp); i++)
        x ^= p[i];
    return x;
}

void timer_make_snapshot(timer_snapshot_t *out) {
    memset(out, 0, sizeof(*out)); /* also zeroes padding for the checksum */
    out->version = TIMER_SNAPSHOT_VERSION;
    out->active_slot = g_rtc_state.active_slot;
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        const timer_slot_state_t *sl = &g_rtc_state.slots[i];
        timer_snapshot_slot_t *os = &out->slots[i];
        os->state = (uint8_t)sl->state;
        os->remaining_at_pause = sl->remaining_at_pause;
        os->allocation_sec = sl->allocation_sec;
        os->expiry_wall_time = sl->expiry_wall_time;
        os->run_accum_sec = sl->run_accum_sec;
        os->run_started_wall = sl->run_started_wall;
        os->break_expiry_wall = sl->break_expiry_wall;
        os->completions = sl->completions;
    }
    memcpy(out->date, g_rtc_state.last_date, sizeof(out->date));
    out->checksum = timer_snapshot_checksum(out);
}

static bool snapshot_valid(const timer_snapshot_t *snap, time_t now) {
    if (snap->version != TIMER_SNAPSHOT_VERSION)
        return false;
    /* All-zeros XORs to 0 — indistinguishable from blank storage */
    if (snap->slots[0].state == 0 && snap->slots[0].expiry_wall_time == 0 && snap->date[0] == '\0')
        return false;
    if (timer_snapshot_checksum(snap) != snap->checksum)
        return false;
    if (snap->active_slot >= TIMER_SLOT_COUNT)
        return false;
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        const timer_snapshot_slot_t *sl = &snap->slots[i];
        if (sl->state > TIMER_BREAK)
            return false;
        if (sl->allocation_sec < 0 || sl->allocation_sec > SNAPSHOT_MAX_HORIZON_SEC)
            return false;
        if (sl->remaining_at_pause < 0 || sl->remaining_at_pause > sl->allocation_sec)
            return false;
        if (sl->state == TIMER_RUNNING) {
            int64_t delta = sl->expiry_wall_time - (int64_t)now;
            if (delta > SNAPSHOT_MAX_HORIZON_SEC || delta < -SNAPSHOT_MAX_HORIZON_SEC)
                return false;
        }
        if (sl->state == TIMER_BREAK) {
            int64_t delta = sl->break_expiry_wall - (int64_t)now;
            if (delta > SNAPSHOT_MAX_HORIZON_SEC || delta < -SNAPSHOT_MAX_HORIZON_SEC)
                return false;
        }
    }
    return true;
}

bool timer_restore_snapshot(const timer_snapshot_t *snap, time_t now) {
    if (!snapshot_valid(snap, now))
        return false;
    /* Stale day: never restore yesterday's timer (rollover will reset) */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[11];
    fill_date(today, tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
    if (strcmp(today, snap->date) != 0)
        return false;

    g_rtc_state.active_slot = snap->active_slot;
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        const timer_snapshot_slot_t *ss = &snap->slots[i];
        timer_slot_state_t *sl = &g_rtc_state.slots[i];
        sl->state = (timer_state_t)ss->state;
        sl->remaining_at_pause = ss->remaining_at_pause;
        sl->allocation_sec = ss->allocation_sec;
        sl->expiry_wall_time = ss->expiry_wall_time;
        sl->run_accum_sec = ss->run_accum_sec;
        sl->run_started_wall = ss->run_started_wall;
        sl->break_expiry_wall = ss->break_expiry_wall;
        sl->completions = ss->completions;
        /* Expiry passed while powered off (snapshot saved before the EXPIRED
           transition landed): restore directly as EXPIRED so the next tick
           does not re-transition and re-fire the already-heard alert. The
           run still reached 00:00 — count it. */
        if (sl->state == TIMER_RUNNING && sl->expiry_wall_time <= (int64_t)now) {
            sl->state = TIMER_EXPIRED;
            sl->completions++;
        }
        /* Break finished while powered off: restore as PAUSED (manual resume) */
        if (sl->state == TIMER_BREAK && sl->break_expiry_wall <= (int64_t)now) {
            sl->state = TIMER_PAUSED;
            sl->break_expiry_wall = 0;
        }
    }
    memcpy(g_rtc_state.last_date, snap->date, sizeof(g_rtc_state.last_date));
    return true;
}
