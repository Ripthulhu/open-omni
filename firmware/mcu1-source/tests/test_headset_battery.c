#include "headset_battery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct { uint8_t bytes[128];unsigned used,budget;int drained; } io;
static int tx(void *context,uint8_t b)
{
    (void)context;if(!io.budget) return 0;--io.budget;
    assert(io.used<sizeof(io.bytes));io.bytes[io.used++]=b;return 1;
}
static int drain(void *context) { (void)context;return io.drained; }
static void poll(uint32_t now,bool start,unsigned budget)
{ io.budget=budget;omni_headset_battery_poll(now,start,(omni_headset_battery_io){0,tx,drain}); }
static void feed(const uint8_t *p,size_t n,uint32_t now)
{ for(size_t i=0;i<n;++i) omni_headset_battery_observe(p[i],now); }
static void percent(uint8_t value,uint32_t now)
{ feed((uint8_t[]){0xdb,8,0xe2,3,1,0x0f,0x0a,value},8,now); }
static void charge(uint8_t value,uint32_t now)
{ feed((uint8_t[]){0xdb,6,0xe2,3,2,value},6,now); }
static void connection(uint8_t value,uint32_t now)
{ feed((uint8_t[]){0xdb,5,0xe4,3,value},5,now); }
static void setup(void)
{ omni_headset_battery_init();memset(&io,0,sizeof(io));io.drained=1; }
static void telemetry(void)
{
    uint32_t w[15],copy[15];
    /* Reads before initialization must be unknown and must not create state. */
    assert(omni_headset_battery_display(0)==255);
    assert(omni_headset_battery_status(0,0,w) && !w[2] && w[3]==255);
    setup();charge(1,99);assert(omni_headset_battery_display(99)==(255u|(2u<<8)));
    percent(88,100);charge(1,101);
    assert(omni_headset_battery_display(102)==(88u|(2u<<8)));
    assert(omni_headset_battery_status(1,102,w));
    assert(w[9]==8 && w[10]==6);
    assert(!memcmp(w+11,(uint8_t[]){0xdb,8,0xe2,3,1,15,10,88},8));
    assert(!memcmp(w+13,(uint8_t[]){0xdb,6,0xe2,3,2,1,0,0},8));
    memcpy(copy,w,sizeof(w));assert(omni_headset_battery_status(1,102,w));
    assert(!memcmp(copy,w,sizeof(w)));
    assert(omni_headset_battery_display(90100)==(255u|(2u<<8)));
    assert(omni_headset_battery_display(90101)==255u);
    percent(100,UINT32_MAX-10u);
    assert((omni_headset_battery_display(10)&255u)==100);
    percent(101,20);assert((omni_headset_battery_display(20)&255u)==255);
    charge(3,20);assert(omni_headset_battery_display(20)==255);
    connection(3,21);percent(40,22);charge(0,22);
    assert(omni_headset_battery_display(22)==(40u|256u));
    connection(2,23);assert(omni_headset_battery_display(23)==255);
    percent(50,24);assert(omni_headset_battery_display(24)==255);
    connection(3,25);assert(omni_headset_battery_display(25)==255);
    percent(51,26);assert((omni_headset_battery_display(26)&255u)==51);
    connection(255,27);percent(52,28);assert(omni_headset_battery_display(28)==255);
    assert(omni_headset_battery_status(0,28,w) && (w[2]&4u) && !(w[2]&8u));
    poll(29,true,5);assert(io.used==4 && io.bytes[2]==0xe4); /* Link queries continue while disconnected. */
    connection(3,30);poll(30,true,5);assert(io.used==4 && !omni_headset_battery_busy());
    poll(50,true,5);assert(io.used==9 && io.bytes[6]==0xe2);
    omni_headset_battery_release();
    percent(53,31);feed((uint8_t[]){0xdb,7,0xe2,3,1,15,10},7,32);
    assert((omni_headset_battery_display(32)&255u)==255);
    percent(53,33);feed((uint8_t[]){0xdb,9,0xe2,3,1,15,10,53,0},9,34);
    assert((omni_headset_battery_display(34)&255u)==255);
    charge(1,35);feed((uint8_t[]){0xdb,5,0xe2,3,2},5,36);
    assert(omni_headset_battery_display(36)==255);
    charge(1,37);feed((uint8_t[]){0xdb,7,0xe2,3,2,1,0},7,38);
    assert(omni_headset_battery_display(38)==255);
    assert(!omni_headset_battery_status(2,0,w));assert(!omni_headset_battery_status(0,0,0));
}
static void queries(void)
{
    uint32_t w[15];setup();poll(0,false,5);assert(!io.used);
    poll(1,true,2);assert(io.used==2 && omni_headset_battery_busy());
    io.drained=0;poll(2,false,5);assert(io.used==4);
    assert(!memcmp(io.bytes,(uint8_t[]){0xbd,4,0xe4,2},4));
    connection(3,3);poll(3,false,5);assert(omni_headset_battery_busy());
    io.drained=1;poll(4,false,5);assert(!omni_headset_battery_busy());
    poll(23,true,5);assert(io.used==4);
    poll(24,true,5);assert(io.used==9 && io.bytes[8]==1);
    assert(!memcmp(io.bytes+4,(uint8_t[]){0xbd,5,0xe2,2,1},5));
    percent(77,25);poll(25,false,5);assert(!omni_headset_battery_busy());
    poll(44,true,5);assert(io.used==9);
    poll(45,true,5);assert(io.used==14 && io.bytes[13]==2);
    feed((uint8_t[]){0xdb,5,0x91,3,1,0xdd,3,0xe2,0},9,46);
    poll(46,false,5);assert(omni_headset_battery_busy());
    charge(2,47);poll(47,false,5);assert(!omni_headset_battery_busy());
    assert(omni_headset_battery_display(47)==(77u|(3u<<8)));
    poll(1046,true,5);assert(io.used==14);
    poll(1047,true,5);assert(io.used==18 && io.bytes[16]==0xe4);
    connection(3,1048);poll(1048,false,5);assert(!omni_headset_battery_busy());
    poll(1068,true,5);assert(io.used==18); /* Link success does not repoll battery everysecond. */
    poll(30024,true,5);assert(io.used==22 && io.bytes[20]==0xe4);
    connection(3,30025);poll(30025,false,5);poll(30045,true,5);
    assert(io.used==27 && io.bytes[26]==1);
    percent(255,30046);poll(30046,false,5);
    poll(30066,true,5);assert(io.used==27); /* invalid percentage never advances to charge */
    assert(omni_headset_battery_status(0,30066,w) && w[13]==1);
    setup();poll(0,true,5);
    feed((uint8_t[]){0xdd,4,0xe4,1},4,1);poll(1,false,0);
    assert(omni_headset_battery_busy());
    feed((uint8_t[]){0xdd,3,0xe4,1},4,2);poll(2,false,0);
    assert(!omni_headset_battery_busy());
    assert(omni_headset_battery_status(1,2,w) && w[6]==1);
    /* Unknown/disconnected boot still probes; a later connectedepoch refreshes immediately. */
    setup();connection(1,0);poll(0,true,5);assert(io.used==4);
    connection(1,1);poll(1,false,5);poll(1000,true,5);assert(io.used==4);
    poll(1001,true,5);assert(io.used==8);connection(3,1002);poll(1002,false,5);
    poll(1022,true,5);assert(io.used==13 && io.bytes[12]==1);
    /* A pre-submission E4 frame cannot finish the active query. */
    setup();poll(0,true,2);feed((uint8_t[]){0xdb,5,0xe4},3,1);
    poll(2,false,5);feed((uint8_t[]){3,3},2,3);poll(3,false,5);
    assert(omni_headset_battery_busy());connection(3,4);poll(4,false,5);
    assert(!omni_headset_battery_busy());
}
static void failures(void)
{
    uint32_t w[15];
    for(unsigned phase=0;phase<3;++phase) {
        setup();percent(70,0);charge(1,0);
        io.drained=phase==1?0:1;poll(0,true,phase==0?2:5);
        poll(250,false,0);assert(!omni_headset_battery_busy());
        assert(omni_headset_battery_transport_fault()==(phase<2));
        assert(omni_headset_battery_status(0,250,w) && w[11]==1);
        if(phase<2) assert(omni_headset_battery_display(250)==255);
        else assert((omni_headset_battery_display(250)&255u)==70);
        unsigned sent=io.used;poll(251,true,5);assert(io.used==sent);
        omni_headset_battery_release();assert(omni_headset_battery_display(251)==255);
        omni_headset_battery_acquire();assert(!omni_headset_battery_transport_fault());
        poll(252,true,5);assert(io.used==sent+4);
    }
    setup();poll(0,true,1);
    omni_headset_battery_poll(1,false,(omni_headset_battery_io){0,0,drain});
    assert(omni_headset_battery_transport_fault());
    assert(omni_headset_battery_status(1,1,w) && w[5]==1);
    setup();feed((uint8_t[]){0xdb,8,0xe2},3,0);
    assert(omni_headset_battery_frame_pending());omni_headset_battery_expire(20);
    assert(!omni_headset_battery_frame_pending());
    assert(omni_headset_battery_status(1,20,w) && w[8]==1);
    poll(21,true,1);omni_headset_battery_release();
    assert(!omni_headset_battery_busy() && omni_headset_battery_display(21)==255);
    assert(omni_headset_battery_status(0,21,w) && w[14]==1);
}
int main(void)
{
    telemetry();queries();failures();
    puts("Headset battery: raw telemetry, connection epochs, cadence, drain, malformed replies and faults passed");
    return 0;
}
