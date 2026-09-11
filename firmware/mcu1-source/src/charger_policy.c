#include "charger_policy.h"
int omni_charger_profile(const uint8_t r[10])
{
    if(!r || (r[1]&0xf7u)!=0xa4 || (r[2]&0x80u) || r[6]!=0xc0) return 0;
    /* Separate observed profiles, never a permissive mixture of their limits.
     * REG02 bit6 is the self-clearing watchdog strobe; bit7 requests reset. */
    int legacy=r[0]==0x9f && (r[2]&0x3fu)==0x0f && r[3]==0x91 &&
        r[4]==0xa3 && r[5]==0x7a && r[7]==0x37 && (r[8]&0x60u)==0;
    int replacement=r[0]==0x0f && (r[2]&0x3fu)==0x2b && r[3]==0x9f &&
        r[4]==0xa7 && r[5]==0x18 && r[7]==0xb7 && (r[8]&0x60u)==0x40;
    return legacy || replacement;
}
int omni_charger_action(const uint8_t r[10],int present,int allow,uint32_t elapsed,
                        uint8_t *address,uint8_t *value)
{
    if(!address || !value || !omni_charger_profile(r)) return -1;
    int enable=allow && present==1 && !(r[8]&0x80u) && !(r[9]&0x3fu) && (r[8]&2u);
    uint8_t desired=enable?0xa4:0xac;
    if(r[1]!=desired) {*address=1;*value=desired;return 1;}
    /* Replacement stock REG05=18 disables the watchdog. Preserve charge-current
     * bits if refreshing an enabled watchdog; never transplant legacy02=4F. */
    if(enable && (r[5]&0x60u) && elapsed>=20000u) {
        *address=2;*value=(uint8_t)((r[2]&0x3fu)|0x40u);return 1;
    }
    return 0;
}
