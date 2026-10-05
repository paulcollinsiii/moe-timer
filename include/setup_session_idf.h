#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "setup_session.h" /* setup_session_event_t, setup_session_poll_out_t — these ARE setup_session_ops_t */

/* The setup session's device glue (main/setup_session_idf.c, not host-
   tested). These nine functions are the session's device effects, and
   setup_mode_ops() below assembles them, with the setup screens' two render
   functions, into the setup_session_ops_t the wake flow runs the session
   with. The one field it cannot supply is `extend_awake`: its real
   implementation owns the awake failsafe's timer handle in main.c, so the
   caller passes it in. */

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
bool setup_session_idf_join_wifi(const char *ssid, const char *password);

/* The complete ops table, by value. `extend_awake` is the awake failsafe's
   extender (same signature as setup_session_ops_t.extend_awake); everything
   else is this module's and the setup screens'. */
setup_session_ops_t setup_mode_ops(bool (*extend_awake)(int seconds));

#ifdef __cplusplus
}
#endif
