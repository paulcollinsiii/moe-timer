#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Pure SoC curve (battery_soc.c) — host-tested. 0-100, clamped. */
int battery_percent_from_mv(int mv);

/* ADC side (battery.c, hardware only). MagTag: VBAT through a 100k/100k
   divider to GPIO4 (ADC1_CH3). */
void battery_init(void);
int battery_read_mv(void); /* calibrated battery mV (x2 divider); <=0 on failure */
/* ADC1 is single-owner: battery.c holds the unit handle; light.c borrows
   it (adc_oneshot_unit_handle_t, opaque here to keep the header ESP-free). */
void *battery_adc_unit(void);

#ifdef __cplusplus
}
#endif
