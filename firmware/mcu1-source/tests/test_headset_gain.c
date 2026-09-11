#include "headset_gain.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    uint8_t frame[5],frames[128][5],rx[4096],value;
    unsigned used,count,read,available,budget;
    bool drain,drop_bulk,drop_ack,drop_get,nack,bad_get,tx_error,drain_error;
    bool force_get;uint8_t get_value;
    uint32_t now,revision,when[128];
} fake;
static void push(fake *f,const uint8_t *p,unsigned n)
{
    if(f->read==f->available) f->read=f->available=0;
    assert(f->available+n<=sizeof(f->rx));memcpy(f->rx+f->available,p,n);f->available+=n;
}
static int tx(void *context,uint8_t b)
{
    fake *f=context;if(f->tx_error)return -1;if(!f->budget)return 0;--f->budget;
    assert(f->used<5u);f->frame[f->used++]=b;
    if(f->used<2u || f->used<f->frame[1]) return 1;
    assert(f->count<128u);memcpy(f->frames[f->count],f->frame,f->used);f->when[f->count++]=f->now;
    if(f->frame[2]==0x20u) {
        assert(f->used==4u && !memcmp(f->frame,(uint8_t[]){0xbd,4,0x20,1},4));
        if(!f->drop_bulk) {uint8_t r[46]={0xdb,46,0x20,1};r[4]=f->value;r[45]=2;push(f,r,46);}
    } else {
        assert(f->used==5u && f->frame[2]==0xd2u);
        if(f->frame[3]==1u) {
            assert(f->frame[4]<=56u);if(!f->nack)f->value=f->frame[4];
            if(!f->drop_ack)push(f,(uint8_t[]){0xdd,3,0xd2,f->nack?3u:0u},4);
        } else {
            assert(f->frame[3]==2u && f->frame[4]==0u);
            if(f->force_get) f->value=f->get_value;
            if(!f->drop_get)push(f,(uint8_t[]){0xdb,6,0xd2,3,0,f->bad_get?57u:f->value},6);
        }
    }
    f->used=0;return 1;
}
static int drained(void *context) {fake *f=context;return f->drain_error?-1:(int)f->drain;}
static void feed(omni_headset_gain *v,fake *f,uint32_t now)
{
    for(unsigned n=0;n<4u && f->read<f->available;++n)
        omni_headset_gain_byte(v,f->rx[f->read++],now,f->revision);
}
static void tick(omni_headset_gain *v,fake *f,uint32_t now,bool can_start,bool can_apply)
{
    f->now=now;f->budget=2;feed(v,f,now);
    omni_headset_gain_poll(v,now,can_start,can_apply,(omni_headset_gain_io){f,tx,drained});
}
static void setup(omni_headset_gain *v,fake *f,uint8_t ll,uint32_t now)
{
    memset(f,0,sizeof(*f));f->drain=true;f->value=56;f->revision=10;
    assert(omni_headset_gain_init(v));assert(omni_headset_gain_desire(v,ll,10,now));
    omni_headset_gain_acquire(v,now);
}
static uint32_t ready(omni_headset_gain *v,fake *f,uint32_t now)
{
    uint32_t started=now;
    do {tick(v,f,now++,true,true);assert((uint32_t)(now-started)<1000u);} while(!omni_headset_gain_settled(v));
    return now;
}
static void raw(omni_headset_gain *v,const uint8_t *p,unsigned n,uint32_t now,uint32_t revision)
{ for(unsigned i=0;i<n;++i)omni_headset_gain_byte(v,p[i],now,revision); }
static uint32_t get_wait(omni_headset_gain *v,fake *f,uint32_t now)
{
    f->drop_get=true;uint32_t began=now;
    while(v->operation!=2u || v->phase!=HEADSET_GAIN_WAIT) {
        tick(v,f,now++,true,true);assert((uint32_t)(now-began)<1000u);
    }
    return now;
}
int main(void)
{
    omni_headset_gain v;fake f;uint8_t ll;uint32_t rev,ms,w[15];
    setup(&v,&f,0,0);assert(!omni_headset_gain_desire(&v,57,11,0));
    raw(&v,(uint8_t[]){0xdb,6,0xd2,3,0,56},6,0,10);
    assert(!omni_headset_gain_take_remote(&v,&ll,&rev,&ms));
    for(uint32_t n=0;n<100u;++n)tick(&v,&f,n,true,false);
    assert(omni_headset_gain_primed(&v) && !omni_headset_gain_ready(&v) && f.count==1u);
    uint32_t now=ready(&v,&f,100);
    assert(f.count==3u && f.value==0 && v.acks==1u && v.completed==1u);
    assert(!memcmp(f.frames[1],(uint8_t[]){0xbd,5,0xd2,1,0},5));
    assert(!memcmp(f.frames[2],(uint8_t[]){0xbd,5,0xd2,2,0},5));
    assert(f.when[2]-f.when[1]>=20u); /* GET needs no DD ACK. */

    assert(omni_headset_gain_desire(&v,10,11,now));
    while(v.phase!=HEADSET_GAIN_GAP)tick(&v,&f,now++,true,true);
    assert(omni_headset_gain_desire(&v,20,12,now));
    assert(omni_headset_gain_desire(&v,30,13,now));
    assert(omni_headset_gain_ready(&v)); /* No PCM chopping during ordinary changes. */
    now=ready(&v,&f,now);
    assert(v.verified==30 && v.verified_revision==13 && v.coalesced==2u);
    assert(f.count==7u && f.frames[4][3]==2u && f.frames[5][4]==30u);
    raw(&v,(uint8_t[]){0xdb,6,0xd2,3,0,10},6,now,13); /* delayed own echo */
    assert(!omni_headset_gain_take_remote(&v,&ll,&rev,&ms));
    raw(&v,(uint8_t[]){0xdb,6,0xd2,3,1,9,0xdb,6,0xd2,3,2,9},12,now,13);
    assert(!omni_headset_gain_take_remote(&v,&ll,&rev,&ms));
    omni_headset_gain_byte(&v,0xdb,now,77);
    raw(&v,(uint8_t[]){6,0xd2,3,0,9},5,now+1u,78);
    assert(omni_headset_gain_take_remote(&v,&ll,&rev,&ms) && ll==9u && rev==77u && ms==now+1u);
    assert(!omni_headset_gain_take_remote(&v,&ll,&rev,&ms));
    assert(omni_headset_gain_status(&v,0,w) && w[7]==30u);

    /* Reconnect preserves desired state and requires a new bulk20; startup
     * max reports are never adopted. It does not need a physical UART reset. */
    raw(&v,(uint8_t[]){0xdb,5,0xe4,3,1},5,now,13);
    assert(!omni_headset_gain_ready(&v) && !omni_headset_gain_primed(&v));
    raw(&v,(uint8_t[]){0xdb,6,0xd2,3,0,56},6,now,13);
    assert(!omni_headset_gain_take_remote(&v,&ll,&rev,&ms));
    raw(&v,(uint8_t[]){0xdb,5,0xe4,3,3},5,now,13);
    unsigned count=f.count;now=ready(&v,&f,now+1u);
    assert(f.count==count+3u && f.value==30u);

    /* Partial TX must drain before cancellation; no GET is replayed. */
    assert(omni_headset_gain_desire(&v,31,14,now));
    tick(&v,&f,now++,true,true);assert(f.used==2u);f.drain=false;
    omni_headset_gain_yield(&v,now);
    for(unsigned i=0;i<10u;++i)tick(&v,&f,now++,true,true);
    assert(v.phase==HEADSET_GAIN_DRAIN && omni_headset_gain_active(&v));
    count=f.count;f.drain=true;tick(&v,&f,now++,true,true);
    assert(v.phase==HEADSET_GAIN_CANCELLED && f.count==count);
    omni_headset_gain_release(&v,now);assert(!v.have_link && !v.connected);
    omni_headset_gain_acquire(&v,now);now=ready(&v,&f,now);
    assert(v.verified==31u);

    /* Missing responses, NACK and malformed matching data latch without SET retries. */
    setup(&v,&f,5,0);f.drop_bulk=true;
    for(uint32_t n=0;n<800u;++n)tick(&v,&f,n,true,true);
    assert(v.phase==HEADSET_GAIN_TIMEOUT && v.blocked && !v.fault && f.count==1u);
    assert(omni_headset_gain_desire(&v,6,11,800));tick(&v,&f,801,true,true);assert(f.count==1u);
    setup(&v,&f,5,0);f.drop_ack=true;
    for(uint32_t n=0;n<900u;++n)tick(&v,&f,n,true,true);
    assert(v.phase==HEADSET_GAIN_TIMEOUT && f.count==2u && !v.have_verified);
    setup(&v,&f,5,0);f.drop_get=true;
    for(uint32_t n=0;n<900u;++n)tick(&v,&f,n,true,true);
    assert(v.phase==HEADSET_GAIN_TIMEOUT && f.count==3u && !v.have_verified);
    setup(&v,&f,5,0);f.nack=true;
    raw(&v,(uint8_t[]){0xdb,5,0xe4,3,3},5,0,10);
    for(uint32_t n=0;n<100u;++n)tick(&v,&f,n,true,true);
    assert(v.phase==HEADSET_GAIN_NACK && f.count==2u && v.peer_status==3u);
    raw(&v,(uint8_t[]){0xdb,5,0xe4,3,3},5,100,10);
    tick(&v,&f,101,true,true);assert(v.blocked && f.count==2u);
    setup(&v,&f,5,0);f.bad_get=true;
    for(uint32_t n=0;n<100u;++n)tick(&v,&f,n,true,true);
    assert(v.phase==HEADSET_GAIN_INVALID_REPLY && f.count==3u);
    setup(&v,&f,5,0);f.tx_error=true;tick(&v,&f,0,true,true);
    assert(v.phase==HEADSET_GAIN_IO_ERROR && v.fault && !omni_headset_gain_ready(&v));
    setup(&v,&f,5,0);f.drain_error=true;
    for(uint32_t n=0;n<10u;++n)tick(&v,&f,n,true,true);
    assert(v.phase==HEADSET_GAIN_IO_ERROR && v.fault);
    setup(&v,&f,5,0);
    for(uint32_t n=0;n<=2000u;++n)tick(&v,&f,n,false,true);
    assert(v.phase==HEADSET_GAIN_DEFERRED && f.count==0u && !v.blocked && !v.timeouts);
    assert(!omni_headset_gain_ready(&v) && v.queue_deferrals==1u);
    now=ready(&v,&f,2001u);assert(v.verified==5u);
    setup(&v,&f,56,UINT32_MAX-10u);now=ready(&v,&f,UINT32_MAX-10u);
    assert(v.verified==56u && now<100u);

    /* Physical turns during an owned SET/GAP remain candidates after startup. */
    setup(&v,&f,10,0);now=ready(&v,&f,0);
    assert(omni_headset_gain_desire(&v,20,11,now));f.revision=11;
    while(v.phase!=HEADSET_GAIN_GAP)tick(&v,&f,now++,true,true);
    f.value=21u;raw(&v,(uint8_t[]){0xdb,6,0xd2,3,0,21},6,now,11);
    assert(omni_headset_gain_take_remote(&v,&ll,&rev,&ms) && ll==21u && rev==11u);
    assert(omni_headset_gain_desire(&v,21,12,now));f.revision=12;
    now=ready(&v,&f,now);assert(v.verified==21u && v.verified_revision==12u && !v.invalid);
    assert(f.count==5u); /* Readback already equals the new master: no extra SET. */

    /* A valid differing GET is both an observed shadow and a CAS candidate. */
    setup(&v,&f,10,0);now=ready(&v,&f,0);
    assert(omni_headset_gain_desire(&v,20,11,now));f.revision=11;now=get_wait(&v,&f,now);
    raw(&v,(uint8_t[]){0xdb,6,0xd2,3,0,22},6,now,11);
    assert(omni_headset_gain_take_remote(&v,&ll,&rev,&ms) && ll==22u && rev==11u);
    assert(omni_headset_gain_desire(&v,ll,12,now));tick(&v,&f,now++,true,true);
    assert(omni_headset_gain_settled(&v) && v.verified==22u && !v.invalid && v.conflicts==1u);
    assert(omni_headset_gain_status(&v,0,w) && (w[3]&2048u) && (w[14]>>8)==1u);

    /* An old owned GET starting after newer Windows input is NOT a new peer
     * event, even though its first-byte revision sees that newer input. */
    setup(&v,&f,10,0);now=ready(&v,&f,0);
    assert(omni_headset_gain_desire(&v,20,11,now));now=get_wait(&v,&f,now);
    assert(omni_headset_gain_desire(&v,30,12,now));
    raw(&v,(uint8_t[]){0xdb,6,0xd2,3,0,22},6,now,12);
    assert(!omni_headset_gain_take_remote(&v,&ll,&rev,&ms));
    tick(&v,&f,now++,true,true);assert(omni_headset_gain_ready(&v) && !v.reconciliations);
    f.drop_get=false;f.revision=12;now=ready(&v,&f,now);
    assert(v.verified==30u && v.verified_revision==12u && !v.invalid);

    /* A fragmented conflict carries the OLD first-byte revision through the
     * caller's CAS; a newer master value is reconciled without being replaced. */
    assert(omni_headset_gain_desire(&v,31,13,now));now=get_wait(&v,&f,now);
    omni_headset_gain_byte(&v,0xdb,now,13);
    assert(omni_headset_gain_desire(&v,32,14,now));
    raw(&v,(uint8_t[]){6,0xd2,3,0,29},5,now,14);
    assert(omni_headset_gain_take_remote(&v,&ll,&rev,&ms) && ll==29u && rev==13u);
    /* CAS rejects13 vs current14: do not apply candidate. */
    f.drop_get=false;f.revision=14;now=ready(&v,&f,now);
    assert(v.verified==32u && v.verified_revision==14u && !v.invalid);

    /* Untrusted startup max is never adopted. Repeated legal mismatches get
     * exactly one reconciliation per unchanged master revision, not a loop. */
    setup(&v,&f,20,0);f.force_get=true;f.get_value=56u;
    for(uint32_t n=0;n<300u;++n) tick(&v,&f,n,true,true);
    assert(v.phase==HEADSET_GAIN_CONFLICT && v.blocked && !v.fault && !v.have_verified);
    assert(v.conflicts==2u && v.reconciliations==1u && !v.invalid && f.count==5u);
    assert(!omni_headset_gain_take_remote(&v,&ll,&rev,&ms));
    assert(omni_headset_gain_desire(&v,20,10,300));tick(&v,&f,301,true,true);assert(f.count==5u);
    f.force_get=false;assert(omni_headset_gain_desire(&v,21,11,302));now=ready(&v,&f,302);
    assert(v.verified==21u && !v.blocked);

    /* Runtime conflict keeps prior readiness through its one bounded retry. */
    assert(omni_headset_gain_desire(&v,22,12,now));f.force_get=true;f.get_value=21u;
    while(v.reconciliations<2u) {tick(&v,&f,now++,true,true);assert(omni_headset_gain_ready(&v));}
    while(v.phase!=HEADSET_GAIN_CONFLICT) {tick(&v,&f,now++,true,true);assert(now<1000u);}
    assert(v.blocked && !v.invalid);

    /* A wholly unsent runtime request cannot mute a previously verified
     * path. Expiry reports a deferred admission, and only actual admission
     * resumes it; repeatedly publishing the SAME tuple does not spin/requeue. */
    setup(&v,&f,10,0);now=ready(&v,&f,0);count=f.count;
    assert(omni_headset_gain_desire(&v,11,11,now));uint32_t queued=now;
    for(;now<queued+2500u;++now) {
        assert(omni_headset_gain_desire(&v,11,11,now));tick(&v,&f,now,false,true);
        assert(omni_headset_gain_ready(&v) && !v.blocked && !v.fault);
    }
    assert(v.phase==HEADSET_GAIN_DEFERRED && v.queue_deferrals==1u && !v.timeouts && f.count==count);
    assert(omni_headset_gain_status(&v,0,w) && (w[3]&4096u) && (w[3]>>16)==1u);
    now=ready(&v,&f,now);assert(v.verified==11u && f.count==count+2u);

    /* Coalescing a genuinely changed queued tuple refreshes admission time.
     * The wire remains untouched throughout six seconds of denied admission. */
    count=f.count;uint32_t began=now,last_intent=now;
    for(unsigned elapsed=0;elapsed<6000u;++elapsed) {
        if(elapsed%150u==0u) {
            uint8_t target=(uint8_t)(20u+(elapsed/150u)%11u);
            assert(omni_headset_gain_desire(&v,target,100u+elapsed/150u,now));last_intent=now;
        }
        tick(&v,&f,now++,false,true);
        assert(v.phase==HEADSET_GAIN_QUEUED && v.queued_ms==last_intent && omni_headset_gain_ready(&v));
    }
    assert(now-began==6000u && f.count==count && v.queue_deferrals==1u && !v.timeouts);
    now=ready(&v,&f,now);assert(v.verified==v.desired && v.verified_revision==v.revision);
    puts("Headset D2 gain: exact SET/GET, concurrent physical/CAS conflicts, bounded reconciliation, reconnect and faults passed");
    return 0;
}
