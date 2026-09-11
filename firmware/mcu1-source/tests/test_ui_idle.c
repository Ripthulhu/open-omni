#include "ui_idle.h"
#include <assert.h>

int main(void)
{
    omni_ui_idle s;
    omni_ui_idle_init(&s,100);
    /* Continuous background redraw/status polling cannot postpone blanking. */
    for(uint32_t t=100;t<60100;t++) assert(omni_ui_idle_update(&s,t,-256,0,false));
    assert(!omni_ui_idle_update(&s,60100,-256,0,false));
    assert(!omni_ui_idle_update(&s,80100,-256,0,false));
    assert(omni_ui_idle_update(&s,80101,-256,0,true));
    assert(!omni_ui_idle_update(&s,140101,-256,0,false));
    assert(omni_ui_idle_update(&s,140102,-512,0,false));
    assert(!omni_ui_idle_update(&s,200102,-512,0,false));
    assert(omni_ui_idle_update(&s,200103,-512,1,false));
    /* Unsigned elapsed time remains valid across the millisecond wrap. */
    omni_ui_idle_init(&s,UINT32_MAX-100u);
    assert(omni_ui_idle_update(&s,UINT32_MAX-100u,0,0,false));
    assert(omni_ui_idle_update(&s,59898u,0,0,false));
    assert(!omni_ui_idle_update(&s,59899u,0,0,false));
    assert(!omni_ui_idle_timeout(&s,7) && s.timeout_ms==60000u);
    for(unsigned index=0;index<=6;index++) {
        static const unsigned minutes[]={0,1,5,10,15,30,60};
        omni_ui_idle_init(&s,100);assert(omni_ui_idle_timeout(&s,index));
        assert(omni_ui_idle_update(&s,100,0,0,false));
        assert(s.timeout_ms==minutes[index]*60000u);
        if(index) {
            assert(omni_ui_idle_update(&s,99u+s.timeout_ms,0,0,false));
            assert(!omni_ui_idle_update(&s,100u+s.timeout_ms,0,0,false));
        } else assert(omni_ui_idle_update(&s,2000000000u,0,0,false));
    }
    return 0;
}
