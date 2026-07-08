#pragma once
#include <stddef.h>

/* Apply a retained Home Assistant config document (JSON) to NVS. Pure
   logic over the nvs_config accessors — host-tested with the mock NVS.
   Every field is optional except "ver"; a field is applied only if it
   validates, and a rejected field is named in the ack but never blocks
   the others. The document is applied only when its "ver" differs from
   the stored cfg_ver, so a retained message is idempotent across wakes. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONFIG_APPLIED = 0, /* at least attempted; check ack "ok" for per-field errors */
    CONFIG_SKIPPED,     /* ver matched the stored cfg_ver — nothing written */
    CONFIG_INVALID,     /* unparseable or missing "ver" — nothing written */
} config_result_t;

/* Writes an ack JSON document ({"ver":...,"ok":bool[,"errors":[...]]}) into
   ack for publishing on config_ack. */
config_result_t config_apply(const char *json, char *ack, size_t ack_len);

#ifdef __cplusplus
}
#endif
