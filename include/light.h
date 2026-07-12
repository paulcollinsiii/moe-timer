#pragma once

/* Ambient light sensor (ALS-PT19 on GPIO 3 / ADC1). Raw millivolts —
   uncalibrated brightness for HA trend graphs, not lux. */

#ifdef __cplusplus
extern "C" {
#endif

/* Optional — light_read_mv self-initializes on first use per wake. */
void light_init(void);
int light_read_mv(void); /* <=0 on failure */

#ifdef __cplusplus
}
#endif
