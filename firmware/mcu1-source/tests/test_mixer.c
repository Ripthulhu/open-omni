#include "mixer.h"
#include <assert.h>
int main(void)
{
    omni_mixer m;omni_mixer_init(&m);
    assert(omni_mixer_effective(&m,OMNI_MIX_USB1,40,false)==40);
    assert(omni_mixer_effective(&m,OMNI_MIX_LINE,40,true)==100);
    assert(omni_mixer_set(&m,OMNI_MIX_LINE,60,true,false));
    assert(omni_mixer_effective(&m,OMNI_MIX_LINE,50,false)==30);
    assert(omni_mixer_effective(&m,OMNI_MIX_LINE,50,true)==0);
    assert(omni_mixer_set(&m,OMNI_MIX_LINE,60,false,false));
    assert(omni_mixer_effective(&m,OMNI_MIX_LINE,0,true)==60);
    assert(omni_mixer_set(&m,OMNI_MIX_LINE,60,false,true));
    assert(omni_mixer_effective(&m,OMNI_MIX_LINE,100,false)==0);
    assert(m.input[OMNI_MIX_LINE].level==60);
    assert(!omni_mixer_set(&m,OMNI_MIX_USB2,50,true,false));
    assert(!omni_mixer_set(&m,OMNI_MIX_USB3,50,true,false));
    assert(!omni_mixer_set(&m,OMNI_MIX_LINE,101,true,false));
    return 0;
}
