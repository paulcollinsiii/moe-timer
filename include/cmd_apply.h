#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct stats_snapshot; /* stats_snapshot_t, stats_json.h: only a pointer passes here */

/* Parse a retained Home Assistant command document (magtag/<id>/cmd).
   Pure over cJSON + nvs_config (dedup store) + timer (name→slot); the
   caller executes the returned action and publishes the ack. Apply-once:
   a command whose "id" matches the last applied one returns CMD_DUP. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CMD_NONE = 0, /* empty/cleared payload — nothing to do */
    CMD_GRANT,    /* out->slot / out->sec valid */
    CMD_LOCATE,   /* run the locate alarm */
    CMD_DUP,      /* id already applied — skip */
    CMD_INVALID,  /* unparseable, no id, or a bad field */
    CMD_HELD,     /* a valid grant, held back: see cmd_apply_hold */
} cmd_result_t;

typedef struct {
    int slot;    /* grant target (0 = Screen) */
    int32_t sec; /* grant seconds */
} cmd_action_t;

/* Writes an ack JSON ({"id":...,"ok":bool[,"err":...]}) into ack. Records
   the applied id in NVS for dedup on GRANT/LOCATE. */
cmd_result_t cmd_apply(const char *json, cmd_action_t *out, char *ack, size_t ack_len);

/* cmd_apply, except that with hold_grants a valid grant returns CMD_HELD
   and its id is NOT recorded, so the caller neither applies, acks nor
   clears it and the retained command comes back next window. BUG-14:
   mqtt_ha holds grants while the snapshot says no_clock, because there is
   no settled day to put the time on (owner decision Q1, 2026-09-25). A
   locate is not day-scoped and runs regardless. */
cmd_result_t cmd_apply_hold(const char *json, cmd_action_t *out, char *ack, size_t ack_len, bool hold_grants);

/* What mqtt_ha calls: cmd_apply_hold holding grants exactly when the
   window's snapshot says no_clock. One line, and a function rather than
   an inline argument so that pass-through is pinned in test_cmd_apply
   (mqtt_ha.c is in no host suite). */
cmd_result_t cmd_apply_for_snapshot(const char *json, cmd_action_t *out, char *ack, size_t ack_len,
                                    const struct stats_snapshot *snap);

#ifdef __cplusplus
}
#endif
