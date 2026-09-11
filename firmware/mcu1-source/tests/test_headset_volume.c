#include "headset_volume.h"
#include "native_gain.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    const int dbs[]={-55,-49,-30,-6,0};
    const uint8_t steps[]={1,7,26,50,56};
    for(unsigned i=0;i<5u;++i) {
        uint8_t step=255,muted=255;int16_t db=0;
        assert(omni_headset_volume_step((int16_t)(dbs[i]*256),false,&step));
        assert(step==steps[i]);
        assert(omni_headset_volume_db(step,-30*256,&db,&muted));
        assert(db==dbs[i]*256 && !muted);
    }
    uint8_t step=255,muted=255;int16_t db=0;
    assert(omni_headset_volume_step(-17*256,true,&step) && !step);
    assert(omni_headset_volume_db(0,-17*256,&db,&muted) && db==-55*256 && muted);
    assert(omni_headset_volume_db(1,db,&db,&muted) && db==-55*256 && !muted);
    assert(!omni_headset_volume_step(-56*256,false,&step));
    assert(!omni_headset_volume_step(-1,false,&step));
    assert(!omni_headset_volume_db(57,0,&db,&muted));
    for(int v=-55;v<=-49;++v) {
        assert(omni_native_gain_wire((int16_t)(v*256),false,&step) && step==1u);
        assert(omni_native_gain_wire((int16_t)(v*256),true,&step) && !step);
    }
    puts("Headset E085/E011 mapping, physical minimum and separate analog floor passed");
    return 0;
}
