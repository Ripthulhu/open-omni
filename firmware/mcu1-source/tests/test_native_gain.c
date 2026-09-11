/* Reuse the fragmented-I/O, 200ms-delayed DSP fixture, not a second fake with
 * different timing assumptions. Its existing contract tests remain separate. */
#define main gain_trial_fixture_contract
#include "test_dsp_gain_trial.c"
#undef main
#include "native_gain.h"
static unsigned opens,closes;
static bool open_ok=true;
static bool native_open(void *ctx,omni_dsp_volume_io *io)
{
    ++opens; if(!open_ok) return false;
    fake *f=ctx; f->read=f->available=f->used=0;
    *io=(omni_dsp_volume_io){ctx,tx,rx,drain}; return true;
}
static void native_close(void *ctx) { (void)ctx; ++closes; }
static void poll_native(omni_native_gain *v,fake *f,uint32_t now,bool online,int16_t db,bool mute)
{
    f->now=now;
    if(f->scheduled && (uint32_t)(now-f->due)<0x80000000u) {
        memcpy(f->current,f->pending,4); f->scheduled=false;
    }
    f->tx_budget=f->rx_budget=1;
    omni_native_gain_poll(v,now,online,db,mute);
}

#ifndef OMNI_NATIVE_FIXTURE_ONLY
int main(void)
{
    omni_native_gain v; fake f={.current={100,37,42,63},.mode=2};
    assert(omni_native_gain_init(&v,(omni_native_transport){&f,native_open,native_close}));
    uint8_t wire;
    assert(!omni_native_gain_wire(-49*256-1,false,&wire));
    assert(!omni_native_gain_wire(-1,false,&wire));
    uint8_t previous=0;
    for(int db=-49;db<=0;++db) {
        assert(omni_native_gain_wire((int16_t)(db*256),false,&wire));
        assert(wire>previous && wire<=100); previous=wire;
    }
    assert(previous==100);
    for(uint32_t now=0;now<400;++now) {
        poll_native(&v,&f,now,true,-30*256,true);
        if(now<250) assert(!v.ready);
    }
    assert(v.ready && !v.fault && f.current[0]==0 && f.sets==1 && opens==closes);
    unsigned before=f.sets;
    for(uint32_t now=400;now<500;++now)
        poll_native(&v,&f,now,true,(int16_t)(-((int)(now%40u))*256),true);
    assert(f.sets==before && v.verified_revision==v.desired_revision);
    for(uint32_t now=500;now<1400;++now) {
        int16_t db=now<510u?-10*256:-5*256;
        poll_native(&v,&f,now,true,db,false);
    }
    assert(omni_native_gain_wire(-5*256,false,&wire));
    assert(v.ready && !v.fault && v.verified_wire==wire && f.current[0]==wire);
    assert(f.sets==3 && f.current[1]==37 && f.current[2]==42 && f.current[3]==63);
    assert(v.verified_revision==v.desired_revision && opens==closes);
    /* Peer mode changes are detected by periodic reads and stop writes. */
    f.mode=1;
    for(uint32_t now=1400;now<2600;++now) poll_native(&v,&f,now,true,-5*256,false);
    assert(v.fault && !v.ready && f.sets==3 && opens==closes);
    poll_native(&v,&f,2600,false,-20*256,true);
    assert(!v.fault && !v.ready && !v.have_verified);
    f.mode=2;
    for(uint32_t now=2601;now<3000;++now) poll_native(&v,&f,now,true,-20*256,true);
    assert(v.ready && v.epoch==2 && f.current[0]==0);
    /* Loss while a partial command is pending closes once, with no retry. */
    poll_native(&v,&f,3000,true,-20*256,false);
    poll_native(&v,&f,3001,true,-20*256,false);
    poll_native(&v,&f,3002,false,-20*256,false);
    assert(!v.ready && v.cancellations==1 && opens==closes);
    open_ok=false;
    poll_native(&v,&f,3003,true,-20*256,false);
    assert(v.fault && !v.ready && !v.owned);
    puts("Native DSP gain: mapping, delayed coalescing, mute memory, epochs and fault gates passed");
    return 0;
}

#endif
