#define OMNI_NATIVE_FIXTURE_ONLY
#include "test_native_gain.c"
int main(void)
{
    omni_native_gain v;fake f={.current={100,37,100,63},.mode=2};
    assert(omni_native_gain_init(&v,(omni_native_transport){&f,native_open,native_close}));
    uint8_t levels[4]={80,0,60,0};
    assert(omni_native_gain_inputs(&v,5,levels));
    for(uint32_t t=0;t<500;t++) poll_native(&v,&f,t,true,0,false);
    assert(v.ready && f.current[0]==80 && f.current[2]==60 && f.current[1]==37 && f.current[3]==63);
    for(uint32_t t=500;t<1500;t++) {
        levels[2]=(uint8_t)(t<550?40:10);
        assert(omni_native_gain_inputs(&v,5,levels));
        poll_native(&v,&f,t,true,0,false);
    }
    assert(v.ready && !v.fault && f.current[0]==80 && f.current[2]==10);
    assert(f.current[1]==37 && f.current[3]==63 && opens==closes);
    assert(v.verified_revision==v.desired_revision);
    levels[2]=25;
    assert(omni_native_gain_inputs(&v,5,levels));
    poll_native(&v,&f,1500,true,0,false);
    assert(v.owned);
    v.yielding=true;
    for(uint32_t t=1501;t<2000;t++) poll_native(&v,&f,t,true,0,false);
    assert(!v.owned && f.current[2]==25 && !v.fault);
    unsigned stopped=f.sets;
    levels[2]=75;assert(omni_native_gain_inputs(&v,5,levels));
    for(uint32_t t=2000;t<2500;t++) poll_native(&v,&f,t,true,0,false);
    assert(!v.owned && f.sets==stopped); /* UART handoff cannot open a new batch. */
    assert(!omni_native_gain_inputs(&v,15,levels));
    puts("Mixer tuple: delayed/coalesced B changes, unchanged A and preserved A1/C passed");
    return 0;
}
