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

/* The receive buffer for the retained config document (mqtt_ha.c). The
   gate in mqtt_rx.c is `total_len < config_cap`, so the largest document
   actually accepted is CONFIG_BUF_MAX - 1 bytes; a longer one is refused
   with config_ack_too_long() below.

   Its sizing is PINNED BY A TEST, not by a number written here:
   test_config_apply's test_the_worst_case_document_fits_the_receive_buffer
   builds the compact worst-case document — every field config_apply()
   parses, each at its longest honoured value — asserts it fits, and
   prints the headroom. Add a field to config_apply() and that builder is
   where it has to go too. Here (rather than private to mqtt_ha.c) only so
   that test can see the real value. */
#define CONFIG_BUF_MAX 2048

/* Writes an ack JSON document into ack for publishing on config_ack:
   {"ver":...,"ok":bool[,"errors":[...][,"errors_truncated":true]]}.
   errors_truncated means more fields failed than the list could name. */
config_result_t config_apply(const char *json, char *ack, size_t ack_len);

/* Builds the ack for a document that was REFUSED BEFORE PARSING because it
   did not fit the receive buffer, so config_apply never saw it and there is
   no "ver" to echo: {"ok":false,"err":"too_long","len":N,"max":M}.

   `len` is the size the broker declared, `max` the largest document the
   receiver accepts — one less than the buffer, because the terminator needs
   a byte. It lives beside the other ack shapes (and not at the MQTT call
   site) for one reason: these exact bytes are a public interface, read by
   an operator and by any HA template built on config_ack, and mqtt_ha.c has
   no host suite to pin them with.

   Returns snprintf's value: the length the ack WOULD have taken, which is
   not the same thing as the length written when the buffer is short. The
   two coincide here only because the ack cannot outgrow the buffer a
   conforming caller supplies — the widest form an int can print is well
   under CONFIG_ACK_MIN, which test_config_apply pins. So no caller needs
   to compare the return against ack_len, and none does; a caller passing
   less than CONFIG_ACK_MIN is out of contract and would get truncated
   JSON with a full-length return, the same way config_apply would. */
int config_ack_too_long(char *ack, size_t ack_len, int len, int max);

#ifdef __cplusplus
}
#endif
