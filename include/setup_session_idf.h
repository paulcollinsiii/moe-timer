#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "setup_session.h" /* setup_session_event_t, setup_session_poll_out_t — these ARE setup_session_ops_t */

/* The setup session's device glue (main/setup_session_idf.c, not host-
   tested). These eight functions exist to be assigned straight into a
   setup_session_ops_t, the same way include/ota.h's declare the functions
   that become ota_flow_ops_t — the composition is main.c's, which
   also supplies the one field this file does not implement:
   `extend_awake`, already injected elsewhere (main.c's
   extend_awake_failsafe) and reused here rather than duplicated.

   Everything else the ops table needs — the three render functions —
   belongs to the setup screens, declared beside the screens they paint,
   not here. */

#ifdef __cplusplus
extern "C" {
#endif

uint8_t setup_session_idf_rand_byte(void);
uint32_t setup_session_idf_now_ms(void);
bool setup_session_idf_start(const char *ap_ssid, const char *ap_password);
void setup_session_idf_stop(void);
setup_session_event_t setup_session_idf_poll(uint32_t timeout_ms, setup_session_poll_out_t *out);
bool setup_session_idf_set_wifi_creds(const char *ssid, const char *password);
void setup_session_idf_clear_wifi_driver_store(void);
bool setup_session_idf_set_mqtt_creds(const char *uri, const char *user, const char *pass, bool keep_pass);

#ifdef __cplusplus
}
#endif
