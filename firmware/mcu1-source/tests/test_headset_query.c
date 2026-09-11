#include "headset_query.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct { uint8_t bytes[32];unsigned used,budget;int drained; } bus;
static const uint8_t requests[11][5]={
    {0xbd,4,0xe1,2},{0xbd,4,0xe4,2},{0xbd,4,0x20,1},{0xbd,4,0x80,2},
    {0xbd,4,0xd5,2},{0xbd,5,0xd5,2,0x10},{0xbd,5,0xd5,2,0x11},
    {0xbd,5,0xd3,2,1},{0xbd,5,0xd3,2,2},{0xbd,5,0xd4,1,2},{0xbd,5,0xd4,2,2}
};
static const uint8_t prefixes[11][5]={
    {0xdb,13,0xe1,3},{0xdb,5,0xe4,3},{0xdb,46,0x20,1},{0xdb,14,0x80,3},
    {0xdb,5,0xd5,3},{0xdb,6,0xd5,3,0x10},{0xdb,6,0xd5,3,0x11},
    {0xdb,6,0xd3,3,1},{0xdb,6,0xd3,3,2},{0xdb,7,0xd4,1,3},{0xdb,7,0xd4,2,3}
};
static uint32_t token=100;
static int tx(void *ctx,uint8_t b)
{
    (void)ctx;if(!bus.budget) return 0;
    --bus.budget;assert(bus.used<sizeof(bus.bytes));bus.bytes[bus.used++]=b;return 1;
}
static int drain(void *ctx) { (void)ctx;return bus.drained; }
static void poll(uint32_t now,bool allowed,unsigned budget)
{ bus.budget=budget;omni_headset_query_poll(now,allowed,(omni_headset_query_io){0,tx,drain}); }
static void feed(const uint8_t *p,unsigned n,uint32_t now)
{ for(unsigned i=0;i<n;++i) omni_headset_query_observe(p[i],now); }
static void reply(unsigned selected,uint32_t now)
{
    uint8_t p[46];memset(p,0x55,sizeof(p));memcpy(p,prefixes[selected-1],5);
    feed(p,p[1],now);
}
static void begin(unsigned selected,uint32_t now)
{
    omni_headset_query_release(now);omni_headset_query_acquire();++token;
    assert(omni_headset_query_request(token,selected,now));
    memset(&bus,0,sizeof(bus));bus.drained=1;
}
static uint32_t phase(void)
{ uint32_t w[15];assert(omni_headset_query_status(0,0,w));return w[4]; }
static void whitelist(void)
{
    uint8_t raw[60];uint32_t w[15],snapshot[15];
    assert(!omni_headset_query_request(0,1,0));assert(!omni_headset_query_request(1,0,0));
    assert(!omni_headset_query_request(1,12,0));
    assert(!omni_headset_query_reply(1,raw));assert(!omni_headset_query_status(2,0,w));
    for(unsigned selected=1;selected<=11;++selected) {
        begin(selected,0);
        assert(omni_headset_query_request(token,selected,1));
        assert(!omni_headset_query_request(token,selected==1?2:1,1));
        assert(!omni_headset_query_request(token+1,selected,1));
        reply(selected,0);poll(1,false,5);assert(!bus.used); /* pre-TX observation never accepted */
        for(uint32_t now=2;now<=6;++now) poll(now,now==2,1);
        assert(bus.used==requests[selected-1][1]);
        assert(!memcmp(bus.bytes,requests[selected-1],bus.used));
        assert(phase()==HEADSET_QUERY_WAIT);
        feed((uint8_t[]){0xdd,3,requests[selected-1][2],0},4,7);
        poll(7,false,0);assert(phase()==HEADSET_QUERY_WAIT); /* ACK alone is insufficient */
        reply(selected,8);assert(!omni_headset_query_reply(token,raw));
        poll(8,false,0);assert(phase()==HEADSET_QUERY_DONE);
        assert(omni_headset_query_reply(token,raw) && raw[8]==prefixes[selected-1][1]);
        assert(!memcmp(raw+12,prefixes[selected-1],5));
        for(unsigned i=12u+raw[8];i<60;++i) assert(!raw[i]);
        assert(!omni_headset_query_reply(token+1,raw));
        assert(omni_headset_query_status(0,8,w));memcpy(snapshot,w,sizeof(w));
        reply(selected,9);poll(9,true,5);
        assert(omni_headset_query_request(token,selected,9));
        assert(omni_headset_query_status(0,9,w) && !memcmp(w,snapshot,sizeof(w)));
        omni_headset_query_io_error(10);omni_headset_query_acquire();
        assert(omni_headset_query_status(0,10,w) && !memcmp(w,snapshot,sizeof(w)));
        assert(omni_headset_query_status(1,10,w) && w[5]==8 && w[10]==1);
        assert(!memcmp(w+13,requests[selected-1],requests[selected-1][1]));
    }
}
static void interleaving_and_errors(void)
{
    uint32_t w[15];uint8_t raw[60];
    begin(1,0);poll(0,true,2);feed((uint8_t[]){0xdb,13,0xe1},3,1);
    poll(1,false,5);feed((uint8_t[]){3,0,0,0,0,0,0,0,0,0},10,2);
    poll(2,false,0);assert(phase()==HEADSET_QUERY_WAIT); /* frame began before complete TX */
    reply(1,3);poll(3,false,0);assert(phase()==HEADSET_QUERY_DONE);
    begin(6,0);poll(0,true,5);
    feed((uint8_t[]){0xdb,5,0xd5,3,1,0xdb,6,0xd5,3,0x11,2},11,1);
    poll(1,false,0);assert(phase()==HEADSET_QUERY_WAIT);
    reply(6,2);poll(2,false,0);assert(phase()==HEADSET_QUERY_DONE);
    assert(omni_headset_query_status(0,2,w) && w[12]==2 && !w[13]);
    begin(10,0);poll(0,true,5);reply(11,1);poll(1,false,0);
    assert(phase()==HEADSET_QUERY_WAIT);reply(10,2);poll(2,false,0);
    assert(phase()==HEADSET_QUERY_DONE);
    begin(1,0);poll(0,true,4);feed((uint8_t[]){0xdd,3,0xe1,1},4,1);poll(1,false,0);
    assert(phase()==HEADSET_QUERY_NACK && !omni_headset_query_transport_fault());
    assert(!omni_headset_query_reply(token,raw));
    begin(1,0);poll(0,true,4);feed((uint8_t[]){0xdd,4,0xe1,0},4,1);poll(1,false,0);
    assert(phase()==HEADSET_QUERY_INVALID_REPLY && !omni_headset_query_transport_fault());
    begin(3,0);poll(0,true,4);
    feed((uint8_t[]){0xdb,5,0x20,3,0},5,1);poll(1,false,0);
    assert(phase()==HEADSET_QUERY_INVALID_REPLY && !omni_headset_query_transport_fault());
    begin(11,0);poll(0,true,5);
    feed((uint8_t[]){0xdb,7,0xd4,2,2,1,2},7,1);poll(1,false,0);
    assert(phase()==HEADSET_QUERY_INVALID_REPLY);
}
static void deadlines_and_handoffs(void)
{
    uint32_t w[15];begin(1,UINT32_MAX-100u);
    poll(1898,false,5);assert(phase()==HEADSET_QUERY_QUEUED);
    poll(1899,true,5);assert(phase()==HEADSET_QUERY_TIMEOUT && !bus.used);
    assert(omni_headset_query_status(1,1899,w) && w[11]==1);
    for(unsigned mode=0;mode<3;++mode) {
        begin(1,0);if(mode==1) bus.drained=0;
        poll(0,true,mode==0?2:4);poll(250,false,0);
        assert(phase()==HEADSET_QUERY_TIMEOUT);
        assert(omni_headset_query_transport_fault()==(mode<2));
        assert(omni_headset_query_status(1,250,w) && w[11]==2);
        unsigned sent=bus.used;poll(251,true,5);assert(bus.used==sent);
    }
    begin(1,0);omni_headset_query_yield(1);poll(1,true,5);
    assert(phase()==HEADSET_QUERY_CANCELLED && !bus.used);
    begin(1,0);poll(0,true,0);omni_headset_query_yield(1);
    assert(phase()==HEADSET_QUERY_CANCELLED && !bus.used);
    begin(1,0);poll(0,true,2);bus.drained=0;omni_headset_query_yield(1);
    poll(1,false,5);assert(phase()==HEADSET_QUERY_DRAIN && bus.used==4);
    reply(1,2);poll(2,false,0);assert(phase()==HEADSET_QUERY_DRAIN);
    bus.drained=1;poll(3,false,0);assert(phase()==HEADSET_QUERY_CANCELLED);
    assert(omni_headset_query_status(0,3,w) && w[11]==13 && (w[5]&16u));
    begin(1,0);poll(0,true,1);
    omni_headset_query_poll(1,false,(omni_headset_query_io){0,0,drain});
    assert(phase()==HEADSET_QUERY_IO_ERROR && omni_headset_query_transport_fault());
    begin(1,0);omni_headset_query_release(1);assert(phase()==HEADSET_QUERY_CANCELLED);
}
int main(void)
{
    whitelist();interleaving_and_errors();deadlines_and_handoffs();
    puts("Headset queries:11 exact read profiles, frozen tokens/raw evidence, shared opcodes, deadlines and handoffs passed");
    return 0;
}
