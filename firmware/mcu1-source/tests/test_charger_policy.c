#include "charger_policy.h"
#include <assert.h>
#include <string.h>
int main(void)
{
    uint8_t r[10]={0x9f,0xac,0x0f,0x91,0xa3,0x7a,0xc0,0x37,2,0},a=0,v=0;
    assert(omni_charger_profile(r));
    assert(omni_charger_action(r,-1,1,30000,&a,&v)==0);
    assert(omni_charger_action(r,0,1,30000,&a,&v)==0);
    assert(omni_charger_action(r,1,1,0,&a,&v)==1 && a==1 && v==0xa4);
    r[1]=0xa4;
    assert(omni_charger_action(r,1,1,19999,&a,&v)==0);
    assert(omni_charger_action(r,1,1,20000,&a,&v)==1 && a==2 && v==0x4f);
    assert(omni_charger_action(r,0,1,0,&a,&v)==1 && a==1 && v==0xac);
    assert(omni_charger_action(r,-1,1,0,&a,&v)==1 && v==0xac);
    for(unsigned bit=0;bit<6;bit++) {
        r[9]=(uint8_t)(1u<<bit);
        assert(omni_charger_action(r,1,1,0,&a,&v)==1 && v==0xac);
    }
    r[9]=0;r[8]=0;
    assert(omni_charger_action(r,1,1,0,&a,&v)==1 && v==0xac);
    r[8]=2;r[4]=0xa7;
    assert(omni_charger_action(r,1,1,0,&a,&v)==-1);
    r[4]=0xa3;r[8]=0x42;
    assert(!omni_charger_profile(r)); /* Do not generalize to a different revision. */

    /* Exact replacement profile observed on the board and emitted by stock.
     * An enabled battery must never have current02=2B overwritten by old4F. */
    const uint8_t stock[10]={0x0f,0xac,0x2b,0x9f,0xa7,0x18,0xc0,0xb7,0x42,0};
    memcpy(r,stock,sizeof(r));assert(omni_charger_profile(r));
    assert(omni_charger_action(r,1,1,0,&a,&v)==1 && a==1 && v==0xa4);
    r[1]=v;
    const uint32_t ages[]={0,19999,20000,40000,1000000,UINT32_MAX};
    for(unsigned i=0;i<sizeof(ages)/sizeof(ages[0]);i++) {
        a=0xee;v=0xee;
        assert(omni_charger_action(r,1,1,ages[i],&a,&v)==0);
        assert(a==0xee && v==0xee && r[2]==0x2b && r[4]==0xa7);
    }
    for(unsigned charge=0;charge<4;charge++) {
        r[8]=(uint8_t)(0x42u|(charge<<3));
        assert(omni_charger_profile(r));
        assert(omni_charger_action(r,1,1,UINT32_MAX,&a,&v)==0);
    }
    r[8]=0x42;
    assert(omni_charger_action(r,0,1,0,&a,&v)==1 && a==1 && v==0xac);
    assert(omni_charger_action(r,-1,1,0,&a,&v)==1 && a==1 && v==0xac);
    assert(omni_charger_action(r,1,0,0,&a,&v)==1 && a==1 && v==0xac);
    for(unsigned bit=0;bit<6;bit++) {
        r[9]=(uint8_t)(1u<<bit);
        assert(omni_charger_action(r,1,1,0,&a,&v)==1 && a==1 && v==0xac);
    }
    r[9]=0;r[8]=0xc2;
    assert(omni_charger_action(r,1,1,0,&a,&v)==1 && a==1 && v==0xac);
    r[8]=0x40;
    assert(omni_charger_action(r,1,1,0,&a,&v)==1 && a==1 && v==0xac);
    /* Every altered stable config byte or revision is refused, even when it
     * borrows a value allowed on the other board. Unknown profiles are read-only. */
    for(unsigned reg=0;reg<9;reg++) {
        memcpy(r,stock,sizeof(r));r[reg]^=reg==8?0x20u:1u;
        a=0xee;v=0xee;
        assert(omni_charger_action(r,1,1,0,&a,&v)==-1 && a==0xee && v==0xee);
    }
    memcpy(r,stock,sizeof(r));r[2]|=0x80;
    assert(!omni_charger_profile(r));
    r[2]=0x6b;assert(omni_charger_profile(r)); /* Transient strobe only. */
    r[1]=0xa4;assert(omni_charger_action(r,1,1,UINT32_MAX,&a,&v)==0);
    r[2]=0x0f;assert(!omni_charger_profile(r));
    assert(!omni_charger_profile(0));
    return 0;
}
