#ifndef OMNI_CHARGER_ADC_H
#define OMNI_CHARGER_ADC_H
#include <stdint.h>
void omni_charger_adc_poll(uint32_t now);
int omni_charger_adc_present(uint32_t now);
/* Stock voltage conversion, fresh present samples only; -1 means unavailable. */
int omni_charger_adc_millivolts(uint32_t now);
void omni_charger_adc_status(uint32_t out[15]);
#endif
