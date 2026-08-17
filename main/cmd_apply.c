/* Parse an HA command document. Pure over cJSON/nvs_config/timer. */
#include "cmd_apply.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "nvs_config.h"
#include "timer.h"

#define GRANT_MIN_MINUTES 1
#define GRANT_MAX_MINUTES 240

cmd_result_t cmd_apply(const char *json, cmd_action_t *out, char *ack, size_t ack_len) {
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        snprintf(ack, ack_len, "{\"ok\":false,\"err\":\"parse\"}");
        return CMD_INVALID;
    }

    const cJSON *grant = cJSON_GetObjectItemCaseSensitive(root, "grant");
    const cJSON *locate = cJSON_GetObjectItemCaseSensitive(root, "locate");

    /* Empty/cleared retained payload: no command fields — clean no-op */
    if (grant == NULL && locate == NULL) {
        cJSON_Delete(root);
        return CMD_NONE;
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || id->valuestring[0] == '\0') {
        snprintf(ack, ack_len, "{\"ok\":false,\"err\":\"no_id\"}");
        cJSON_Delete(root);
        return CMD_INVALID;
    }

    /* Apply-once: the retained command is re-delivered every window.
       Zero-initialised: a stored id longer than this buffer makes the read
       fail and write nothing, and the return is not checked — uninitialised
       stack into strcmp would make the dedup compare garbage, so a retained
       `grant` could re-apply every window. nvs_config_set_cmd_id now bounds
       the write, so a too-long id cannot be stored in the first place. */
    char last[40] = {0};
    nvs_config_get_cmd_id(last, sizeof(last));
    if (strcmp(last, id->valuestring) == 0) {
        cJSON_Delete(root);
        return CMD_DUP;
    }

    cmd_result_t result;
    if (grant != NULL) {
        const cJSON *timer = cJSON_GetObjectItemCaseSensitive(grant, "timer");
        const cJSON *min = cJSON_GetObjectItemCaseSensitive(grant, "min");
        int slot = timer_slot_by_name(cJSON_IsString(timer) ? timer->valuestring : NULL);
        if (slot < 0) {
            snprintf(ack, ack_len, "{\"id\":\"%s\",\"ok\":false,\"err\":\"timer\"}", id->valuestring);
            cJSON_Delete(root);
            return CMD_INVALID;
        }
        if (!cJSON_IsNumber(min) || min->valuedouble < GRANT_MIN_MINUTES || min->valuedouble > GRANT_MAX_MINUTES) {
            snprintf(ack, ack_len, "{\"id\":\"%s\",\"ok\":false,\"err\":\"min\"}", id->valuestring);
            cJSON_Delete(root);
            return CMD_INVALID;
        }
        out->slot = slot;
        out->sec = min->valueint * 60;
        snprintf(ack, ack_len, "{\"id\":\"%s\",\"ok\":true,\"grant\":%d}", id->valuestring, min->valueint);
        result = CMD_GRANT;
    } else if (cJSON_IsTrue(locate)) {
        snprintf(ack, ack_len, "{\"id\":\"%s\",\"ok\":true,\"locate\":true}", id->valuestring);
        result = CMD_LOCATE;
    } else {
        snprintf(ack, ack_len, "{\"id\":\"%s\",\"ok\":false,\"err\":\"unknown\"}", id->valuestring);
        cJSON_Delete(root);
        return CMD_INVALID;
    }

    nvs_config_set_cmd_id(id->valuestring); /* dedup only genuinely applied commands */
    cJSON_Delete(root);
    return result;
}
