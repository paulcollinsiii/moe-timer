#pragma once
#include <stddef.h>
#include <stdint.h>

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
} cmd_result_t;

typedef struct {
    int slot;    /* grant target (0 = Screen) */
    int32_t sec; /* grant seconds */
} cmd_action_t;

/* Writes an ack JSON ({"id":...,"ok":bool[,"err":...]}) into ack. Records
   the applied id in NVS for dedup on GRANT/LOCATE. */
cmd_result_t cmd_apply(const char *json, cmd_action_t *out, char *ack, size_t ack_len);

#ifdef __cplusplus
}
#endif
