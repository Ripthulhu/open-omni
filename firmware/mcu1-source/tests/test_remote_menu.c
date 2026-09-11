#include "remote_menu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static omni_remote_menu menu;
static struct {uint8_t bytes[256];unsigned used,budget;int drained,error;} bus;
static int tx(void *context,uint8_t value)
{
    (void)context;if(bus.error)return bus.error;
    if(!bus.budget)return 0;
    --bus.budget;assert(bus.used<sizeof(bus.bytes));bus.bytes[bus.used++]=value;return 1;
}
static int drained(void *context) {(void)context;return bus.drained;}
static void reset(void)
{assert(omni_remote_menu_init(&menu));memset(&bus,0,sizeof(bus));bus.drained=1;}
static void poll(uint32_t now,bool ready,unsigned budget)
{
    unsigned before=bus.used;bus.budget=budget;
    omni_remote_menu_poll(&menu,now,ready,(omni_remote_menu_io){0,tx,drained});
    assert(bus.used-before<=4u);
}
static void feed(const uint8_t *p,size_t n,uint32_t now)
{for(size_t i=0;i<n;++i)omni_remote_menu_byte(&menu,p[i],now);}
static void ack(uint8_t opcode,uint8_t status,uint32_t now)
{feed((uint8_t[]){0xdd,3,opcode,status},4,now);}
static void event(uint8_t value,uint32_t now)
{feed((uint8_t[]){0xdb,4,0x91,value},4,now);}
static void link(uint8_t value,uint32_t now)
{feed((uint8_t[]){0xdb,5,0xe4,3,value},5,now);}
static void wire(unsigned offset,const uint8_t *value,unsigned n)
{assert(bus.used==offset+n);assert(!memcmp(bus.bytes+offset,value,n));}
static void enter(uint32_t now)
{
    event(10,now);poll(now,true,4);ack(0x91,0,now+1);poll(now+1,false,4);
    poll(now+20,true,4);ack(0x92,0,now+21);poll(now+21,false,4);
    assert(menu.phase==REMOTE_MENU_ACCEPTED&&menu.mode_known&&menu.mode_open);
}
int main(void)
{
    uint32_t status[15];
    reset();poll(0,true,4);assert(!bus.used&&!omni_remote_menu_busy(&menu));
    assert(!omni_remote_menu_request(&menu,0,true,2,0));
    assert(!omni_remote_menu_request(&menu,0x80000000u,true,2,0));
    assert(!omni_remote_menu_request(&menu,1,true,3,0));
    assert(omni_remote_menu_request(&menu,1,true,2,100));
    assert(omni_remote_menu_request(&menu,1,true,2,101));
    assert(!omni_remote_menu_request(&menu,1,false,2,101));
    poll(101,false,4);assert(!bus.used);
    poll(102,true,2);assert(menu.phase==REMOTE_MENU_SEND&&bus.used==2);
    omni_remote_menu_byte(&menu,0xdd,102); /* old ACK began during partial TX */
    poll(103,false,2);wire(0,(uint8_t[]){0xbd,4,0x91,10},4);
    feed((uint8_t[]){3,0x91,0},3,103);assert(!menu.ack&&menu.unrelated==1);
    ack(0x92,0,104);assert(!menu.ack);ack(0x91,0,104);poll(104,false,4);
    assert(menu.phase==REMOTE_MENU_GAP&&omni_remote_menu_active(&menu));
    event(10,105);event(10,106);assert(menu.duplicates==2&&!menu.pending);
    poll(122,true,4);assert(bus.used==4);poll(123,false,4);assert(bus.used==4);
    poll(123,true,4);wire(4,(uint8_t[]){0xbd,4,0x92,2},4);
    ack(0x92,0,124);poll(124,false,4);assert(menu.phase==REMOTE_MENU_ACCEPTED);
    assert(menu.mode_known&&menu.mode_open&&menu.completed==1);
    assert(omni_remote_menu_request(&menu,1,true,2,125));poll(127,true,4);assert(bus.used==8);
    assert(omni_remote_menu_request(&menu,2,true,2,130));poll(130,true,2);
    assert(omni_remote_menu_request(&menu,3,false,1,131));
    assert(omni_remote_menu_request(&menu,4,false,1,132));assert(menu.coalesced==1);
    poll(132,false,4);wire(8,(uint8_t[]){0xbd,4,0x92,2},4);
    ack(0x92,0,133);poll(133,false,4);assert(menu.pending&&menu.last_token==2);
    poll(154,true,4);wire(12,(uint8_t[]){0xbd,4,0x91,9},4);
    assert(menu.token==4);ack(0x91,0,155);poll(155,false,4);poll(174,true,4);
    wire(16,(uint8_t[]){0xbd,4,0x92,1},4);ack(0x92,0,175);poll(175,false,4);
    assert(menu.mode_known&&!menu.mode_open&&!menu.pending);
    /* No92 for a pure context reconciliation; already accepted state needs no TX. */
    assert(omni_remote_menu_request(&menu,5,false,0,176));poll(176,true,4);
    assert(bus.used==20&&menu.phase==REMOTE_MENU_ACCEPTED);
    /* Local and remote close/open intents must not toggle on duplicate input. */
    assert(omni_remote_menu_request(&menu,5,false,0,177));poll(177,true,4);assert(bus.used==20);
    event(10,180);event(10,181);poll(181,true,4);
    wire(20,(uint8_t[]){0xbd,4,0x91,10},4);ack(0x91,0,182);poll(182,false,4);
    poll(201,true,4);ack(0x92,0,202);poll(202,false,4);
    event(8,203);poll(203,true,4);wire(28,(uint8_t[]){0xbd,4,0x91,9},4);
    ack(0x91,0,204);poll(204,false,4);poll(223,true,4);
    wire(32,(uint8_t[]){0xbd,4,0x92,1},4);ack(0x92,0,224);poll(224,false,4);

    /* A fresh peer request after completion gets its echo again. In-flight
     * duplicates coalesce, and neither repeated request toggles the UI state. */
    reset();enter(0);event(10,30);event(10,31);assert(menu.pending&&menu.duplicates==1);
    poll(31,true,4);wire(8,(uint8_t[]){0xbd,4,0x91,10},4);
    assert(menu.desired_open&&menu.planned_open);
    /* A DD ACK before wire drain may latch but never releases the owner. */
    reset();event(10,0);bus.drained=0;poll(0,true,4);ack(0x91,0,1);poll(1,false,4);
    assert(menu.phase==REMOTE_MENU_DRAIN&&!menu.mode_known);
    bus.drained=1;poll(2,false,4);assert(menu.phase==REMOTE_MENU_GAP&&menu.mode_known);
    omni_remote_menu_yield(&menu,3);assert(!omni_remote_menu_active(&menu)&&menu.mode_known);
    /* Negative/malformed responses stop before the second frame. */
    reset();event(10,0);poll(0,true,4);ack(0x91,7,1);poll(1,false,4);
    assert(menu.phase==REMOTE_MENU_NACK&&menu.nacks==1&&!menu.mode_known&&bus.used==4);
    reset();event(10,0);poll(0,true,4);feed((uint8_t[]){0xdd,4,0x91,0},4,1);poll(1,false,4);
    assert(menu.phase==REMOTE_MENU_INVALID_REPLY&&bus.used==4);
    /* Unrelated frames and command-valued payload bytes cannot acknowledge a transfer. */
    reset();event(10,0);poll(0,true,4);
    feed((uint8_t[]){0xdb,8,0x20,1,0xdd,3,0x91,0},8,1);poll(1,false,4);assert(!menu.ack);
    poll(500,false,4);assert(menu.phase==REMOTE_MENU_TIMEOUT&&!menu.fault&&menu.timeouts==1);
    /* A partial frame is completed/drained during yield, never interleaved. */
    reset();event(10,0);poll(0,true,2);omni_remote_menu_yield(&menu,1);
    assert(omni_remote_menu_active(&menu));poll(1,false,4);assert(menu.phase==REMOTE_MENU_CANCELLED);
    wire(0,(uint8_t[]){0xbd,4,0x91,10},4);
    reset();event(10,0);poll(0,true,2);poll(500,false,0);
    assert(menu.phase==REMOTE_MENU_TIMEOUT&&menu.fault);
    assert(!omni_remote_menu_request(&menu,1,true,2,501));
    omni_remote_menu_release(&menu,501);omni_remote_menu_acquire(&menu);
    assert(omni_remote_menu_request(&menu,1,true,2,502));
    reset();event(10,0);bus.error=-1;poll(0,true,4);assert(menu.phase==REMOTE_MENU_IO_ERROR&&menu.fault);
    /* Queue deadline is bounded even without transport ownership, across wrap. */
    reset();assert(omni_remote_menu_request(&menu,9,true,2,UINT32_MAX-999u));
    poll(999,false,4);assert(menu.pending);poll(1000,false,4);
    assert(!menu.pending&&menu.phase==REMOTE_MENU_TIMEOUT&&menu.last_token==9);
    /* Reconnect preserves local desired menu and sends910A only; repeated E4
     * observations do not repeatedly enter or fabricate92 context. */
    reset();enter(100);link(3,122);assert(!menu.pending&&bus.used==8);
    link(1,130);assert(!menu.mode_known&&!omni_remote_menu_busy(&menu));
    unsigned before=bus.used;link(3,140);poll(140,true,4);
    wire(before,(uint8_t[]){0xbd,4,0x91,10},4);ack(0x91,0,141);poll(141,false,4);
    assert(!omni_remote_menu_busy(&menu));link(3,142);poll(142,true,4);assert(bus.used==before+4);
    /* Disconnect/reconnect while a partial sequence drains must not lose the
     * restoration intent just because the old active intent also said open. */
    reset();link(3,0);event(10,1);poll(1,true,2);link(1,2);link(3,3);
    assert(menu.pending&&menu.cancel);poll(3,false,4);assert(menu.phase==REMOTE_MENU_CANCELLED);
    poll(24,true,4);wire(4,(uint8_t[]){0xbd,4,0x91,10},4);
    ack(0x91,0,25);poll(25,false,4);assert(menu.phase==REMOTE_MENU_ACCEPTED&&bus.used==8);
    assert(!omni_remote_menu_observe(&menu,(uint8_t[]){0xdb,4,0x91,4},4,26));
    assert(omni_remote_menu_status(&menu,0,status)&&status[0]==1&&status[7]==1);
    assert(omni_remote_menu_status(&menu,1,status)&&status[2]==8);
    assert(!omni_remote_menu_status(&menu,2,status));
    puts("Remote menu: exact91/92 order, ACK ownership, opaque contexts, coalescing, duplicates, deadlines, reconnect and cancellation passed");
    return 0;
}
