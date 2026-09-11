#ifndef OMNI_CHARGER_POLICY_H
#define OMNI_CHARGER_POLICY_H
#include <stdint.h>
/* Recognized live profile only. Never programs current/voltage/OTP registers. */
int omni_charger_profile(const uint8_t r[10]);
int omni_charger_action(const uint8_t r[10],int present,int allow,uint32_t elapsed,
                        uint8_t *address,uint8_t *value);
#endif
