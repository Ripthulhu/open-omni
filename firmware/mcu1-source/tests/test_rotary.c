#include "rotary.h"
#include <assert.h>
int main(void) {
    omni_rotary r; const uint8_t phases[]={0,1,3,2};
    assert(!omni_rotary_init(&r,4,2)); assert(!omni_rotary_init(&r,0,3));
    for(unsigned start=0;start<4;start++) for(unsigned edges=2;edges<=4;edges+=2) {
        assert(omni_rotary_init(&r,phases[start],(uint8_t)edges)); int sum=0;
        for(unsigned i=1;i<=8;i++) sum+=omni_rotary_sample(&r,phases[(start+i)%4]);
        assert(sum==(int)(8/edges) && r.invalid==0);
        sum=0;
        for(unsigned i=1;i<=8;i++) sum+=omni_rotary_sample(&r,phases[(start+8-i)%4]);
        assert(sum==-(int)(8/edges) && r.invalid==0);
    }
    assert(omni_rotary_init(&r,0,2));
    for(unsigned i=0;i<20;i++) { assert(!omni_rotary_sample(&r,1)); assert(!omni_rotary_sample(&r,0)); }
    assert(!omni_rotary_sample(&r,1)); assert(!omni_rotary_sample(&r,2));
    assert(r.invalid==1 && r.accumulated==0);
    assert(!omni_rotary_sample(&r,2)); assert(!omni_rotary_sample(&r,0));
    assert(omni_rotary_sample(&r,1)==1);
    return 0;
}
