#ifndef OMNI_CHARGER_H
#define OMNI_CHARGER_H
#include <stdint.h>
void omni_charger_poll(uint32_t now);
void omni_charger_status(uint32_t out[15]);
unsigned omni_charger_indicator(uint32_t now);
#endif
