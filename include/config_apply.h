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

/* Smallest ack buffer config_apply may be given. The worst-case ack (every
   field present and wrong-typed) must fit WHOLE — a truncated ack is
   unparseable JSON, and HA then loses every error in it rather than the
   one that overflowed. config_apply caps its own error list to stay inside
   this; callers must not pass less. */
#define CONFIG_ACK_MIN 256

/* Writes an ack JSON document into ack for publishing on config_ack:
   {"ver":...,"ok":bool[,"errors":[...][,"errors_truncated":true]]}.
   errors_truncated means more fields failed than the list could name. */
config_result_t config_apply(const char *json, char *ack, size_t ack_len);

#ifdef __cplusplus
}
#endif
