#pragma once
#ifndef NATIVE
#include "esp_system.h" /* esp_reset_reason_t */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Boot forensics: the USB CDC console drops output around sleep/reset
   transitions, so a crash's evidence must ride channels that survive —
   the stat payload (HA "Last reset" sensor) and a late-wake log line.
   Anything but DEEPSLEEP on a wake means the previous wake died.

   Takes the reason as a parameter rather than calling esp_reset_reason()
   itself, which is what makes the string table host-testable. Never
   returns NULL: the result goes straight into a log format and a JSON
   payload. */
const char *wake_flow_reset_reason_str(esp_reset_reason_t reason);

#ifdef __cplusplus
}
#endif
