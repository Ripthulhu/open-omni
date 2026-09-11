#include "dsp_meter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct { uint8_t bytes[256];unsigned used,budget;int drain,tx_error; } bus;
static uint8_t report[76];
static int tx(void *ctx,uint8_t b)
{
    (void)ctx;if(bus.tx_error) return bus.tx_error;
    if(!bus.budget) return 0;
    --bus.budget;assert(bus.used<sizeof(bus.bytes));bus.bytes[bus.used++]=b;return 1;
}
static int drain(void *ctx) { (void)ctx;return bus.drain; }
static void poll(uint32_t now,bool start,unsigned budget)
{ bus.budget=budget;omni_dsp_meter_poll(now,start,(omni_dsp_meter_io){0,tx,drain}); }
static void feed(const uint8_t *p,unsigned n,uint32_t now)
{ for(unsigned i=0;i<n;++i) omni_dsp_meter_observe(p[i],now); }
static uint32_t word(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint32_t status(unsigned index,uint32_t now)
{ uint8_t out[60];assert(omni_dsp_meter_read(0,now,out));return word(out+4u*index); }
static void setup(uint32_t now)
{
    omni_dsp_meter_init();omni_dsp_meter_acquire(now);
    memset(&bus,0,sizeof(bus));bus.drain=1;
    memcpy(report,(uint8_t[]){0xdb,76,0x50,3},4);
    for(unsigned i=4;i<76;++i) report[i]=(uint8_t)(i*7u);
    /* Real signed32 BE floor and marker-containing values remain raw. */
    memcpy(report+4,(uint8_t[]){0xff,0xff,0xda,0x80,0,0,0,1,0xdb,0xdd,5,0x15},12);
}
static void cache_check(uint32_t now,uint32_t generation)
{
    uint8_t a[60],b[60];
    assert(omni_dsp_meter_read(1,now,a));assert(omni_dsp_meter_read(2,now,b));
    assert(word(a)==1 && word(a+4)==1 && word(a+8)==generation);
    assert(word(b)==1 && word(b+4)==2 && word(b+8)==generation);
    assert(!memcmp(a+12,report,48));assert(!memcmp(b+12,report+48,28));
    for(unsigned i=40;i<60;++i) assert(!b[i]);
    assert(status(10,now)==generation);
}
static void framing_and_publication(void)
{
    uint8_t a[60],before[60];
    memset(a,0xa5,sizeof(a));memcpy(before,a,sizeof(a));
    assert(!omni_dsp_meter_read(3,0,a) && !memcmp(a,before,sizeof(a)));
    assert(!omni_dsp_meter_read(0,0,0));
    setup(0);assert(status(10,0)==0 && status(9,0)==UINT32_MAX);
    feed(report,76,0);assert(!status(10,0));
    poll(1,false,4);assert(!bus.used);
    bus.drain=0;poll(2,true,4);
    assert(!memcmp(bus.bytes,(uint8_t[]){0xbd,4,0x50,2},4));
    feed(report,75,3);poll(3,false,0);assert(!status(10,3));
    feed(report+75,1,4);assert(!status(10,4));
    poll(4,false,0);assert(omni_dsp_meter_busy() && !status(10,4));
    bus.drain=1;poll(5,false,0);assert(!omni_dsp_meter_busy());cache_check(5,1);
    assert(status(8,5)==4 && status(9,5)==1 && (status(2,5)&3u)==3u);
    assert((status(2,503)&3u)==3u && (status(2,504)&3u)==1u);
    omni_dsp_meter_release(505);cache_check(505,1);assert(!(status(2,505)&16u));
    omni_dsp_meter_acquire(506);cache_check(506,1);
    poll(506,true,4);report[75]^=1u;feed(report,76,507);poll(507,false,0);cache_check(507,2);
}
static void eligibility_and_errors(void)
{
    setup(0);poll(0,true,2);feed(report,3,1);
    poll(2,false,4);feed(report+3,73,3);poll(3,false,0);
    assert(status(10,3)==0 && omni_dsp_meter_busy()); /* pre-TX prefix */
    feed((uint8_t[]){0xdd,3,0x50,0},4,4);poll(4,false,0);
    assert(omni_dsp_meter_busy()); /* Positive ACK is insufficient. */
    uint8_t bad[76];memcpy(bad,report,sizeof(bad));bad[3]=2;
    feed(bad,76,5);bad[1]=75;feed(bad,75,6);
    assert(status(11,6)==3); /* pre-TX plus two malformed matching responses */
    feed((uint8_t[]){0xdb,5,0xe4,3,3},5,7);assert(status(11,7)==3);
    feed(report,76,8);poll(8,false,0);cache_check(8,1);
    poll(100,true,4);feed((uint8_t[]){0xdd,3,0x50,1},4,101);poll(101,false,0);
    assert(!omni_dsp_meter_busy() && !omni_dsp_meter_transport_fault());cache_check(101,1);
    poll(200,true,4);feed(report,10,201);poll(221,false,0);
    assert(status(12,221)==1);feed(report+10,66,222);assert(status(10,222)==1);
    feed(report,76,223);poll(223,false,0);cache_check(223,2);
}
static void cadence_timeout_wrap(void)
{
    setup(UINT32_MAX-50u);poll(UINT32_MAX-50u,true,4);
    feed(report,76,UINT32_MAX-49u);poll(UINT32_MAX-49u,false,0);
    assert((status(2,0)&3u)==3u && status(9,0)==50u);
    poll(48,true,4);assert(bus.used==4);
    poll(49,true,4);assert(bus.used==8);
    poll(299,false,0);assert(status(5,299)==1 && !omni_dsp_meter_transport_fault());
    assert(!omni_dsp_meter_busy());
    poll(300,true,4);assert(bus.used==8); /* No immediate timeout retry. */
    poll(399,true,4);assert(bus.used==12);
    feed(report,76,400);poll(400,false,0);
    poll(10000,true,4);assert(bus.used==16);
    feed(report,76,10001);poll(10001,false,0);poll(10001,true,4);
    assert(bus.used==16); /* Missed slots coalesce; no catch-up burst. */
}
static void cooperative_yield(void)
{
    for(unsigned submitted=0;submitted<=4;++submitted) {
        setup(0);bus.drain=0;poll(0,true,submitted);
        omni_dsp_meter_yield(1);poll(1,false,4);
        if(submitted==0) assert(!omni_dsp_meter_busy() && !bus.used);
        else {
            assert(omni_dsp_meter_busy() && bus.used==4);
            bus.drain=1;poll(2,false,4);assert(!omni_dsp_meter_busy());
            assert(!memcmp(bus.bytes,(uint8_t[]){0xbd,4,0x50,2},4));
        }
        assert(!omni_dsp_meter_transport_fault());
        unsigned used=bus.used;poll(1000,true,4);assert(bus.used==used);
        omni_dsp_meter_release(1001);omni_dsp_meter_acquire(1002);
        bus.drain=1;poll(1002,true,4);assert(bus.used==used+4u);
    }
    setup(0);poll(0,true,4);omni_dsp_meter_yield(1);
    assert(!omni_dsp_meter_busy() && status(14,1)==1);
    feed(report,76,2);assert(!status(10,2));
}
static void transport_failures(void)
{
    for(unsigned kind=0;kind<5;++kind) {
        setup(0);
        if(kind==0) bus.tx_error=-1;
        if(kind==1) bus.drain=-1;
        if(kind==2) bus.drain=0;
        poll(0,true,kind==3?1u:4u);
        if(kind==2 || kind==3) poll(250,false,0);
        if(kind==4) omni_dsp_meter_poll(1,false,(omni_dsp_meter_io){0,0,drain});
        assert(omni_dsp_meter_transport_fault() && !omni_dsp_meter_busy());
        unsigned used=bus.used;poll(1000,true,4);assert(bus.used==used);
        omni_dsp_meter_acquire(1001);assert(omni_dsp_meter_transport_fault()); /* owned acquire cannot clear */
        omni_dsp_meter_release(1002);omni_dsp_meter_acquire(1003);
        assert(!omni_dsp_meter_transport_fault());
    }
    setup(0);poll(0,true,2);omni_dsp_meter_release(1);
    assert(omni_dsp_meter_transport_fault() && status(14,1)==1);
    setup(0);poll(0,true,4);feed(report,76,1);poll(1,false,0);
    omni_dsp_meter_io_error(2);cache_check(2,1);
    assert((status(2,2)&3u)==1u); /* Cache retained, freshness inhibited by physical fault. */
}
int main(void)
{
    framing_and_publication();eligibility_and_errors();cadence_timeout_wrap();
    cooperative_yield();transport_failures();
    puts("DSP meters: exact GET/raw BE reply, generation cache, eligibility, cadence/wrap, yield and faults passed");
    return 0;
}
