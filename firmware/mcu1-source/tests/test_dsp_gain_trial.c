#include "dsp_gain_trial.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t current[4], frame[8], rx[256], sent[1024];
    unsigned used, read, available, sent_n, sets, commands, tx_budget, rx_budget;
    int tx_error, rx_error, drain_error;
    bool drop_ack, bad_ack_length, bad_status, bad_type, wrong_opcode, stale, malformed;
    uint8_t mode;
    uint8_t pending[4];
    uint32_t now,due;
    bool scheduled;
} fake;
static void push(fake *f,const uint8_t *p,unsigned n)
{
    if(f->read==f->available) f->read=f->available=0;
    assert(f->available+n<=sizeof(f->rx)); memcpy(f->rx+f->available,p,n); f->available+=n;
}
static int tx(void *ctx,uint8_t b)
{
    fake *f=ctx;
    if(f->tx_error) return -1;
    if(!f->tx_budget) return 0;
    --f->tx_budget; assert(f->sent_n<sizeof(f->sent)); f->sent[f->sent_n++]=b;
    assert(f->used<8); f->frame[f->used++]=b;
    if(f->used<2 || f->used<f->frame[1]) return 1;
    ++f->commands;
    uint8_t op=f->frame[2], reply[8]={0xdb,0,op,3};
    if(f->frame[3]==1) {
        assert(op==0x47 && f->used==8); ++f->sets;
        if(!f->stale) {
            memcpy(f->pending,f->frame+4,4); f->due=f->now+200u; f->scheduled=true;
        }
    } else {
        assert(f->frame[3]==2 && f->used==4);
        reply[1]=op==0x43?5:8;
        if(op==0x43) reply[4]=f->mode;
        else memcpy(reply+4,f->current,4);
        if(f->bad_type) reply[3]=2;
        if(f->wrong_opcode) reply[2]=0xe1;
        push(f,reply,reply[1]);
    }
    if(f->malformed) push(f,(uint8_t[]){0xdb,2},2);
    if(!f->drop_ack) {
        uint8_t ack[]={0xdd,f->bad_ack_length?4:3,op,f->bad_status?1:0};
        push(f,ack,4);
    }
    f->used=0; return 1;
}
static int rx(void *ctx,uint8_t *b)
{
    fake *f=ctx; if(f->rx_error) return -1;
    if(!f->rx_budget || f->read==f->available) return 0;
    --f->rx_budget; *b=f->rx[f->read++]; return 1;
}
static int drain(void *ctx) { fake *f=ctx; return f->drain_error?-1:1; }
static void setup(omni_gain_trial *v,fake *f,uint32_t now)
{
    memset(v,0,sizeof(*v)); *f=(fake){.current={100,37,42,63},.mode=2};
    assert(omni_gain_trial_begin(v,(omni_dsp_volume_io){f,tx,rx,drain},now));
    assert(!omni_gain_trial_begin(v,(omni_dsp_volume_io){f,tx,rx,drain},now));
}
static void tick(omni_gain_trial *v,fake *f,uint32_t now)
{
    f->now=now;
    if(f->scheduled && (uint32_t)(now-f->due)<0x80000000u) {
        memcpy(f->current,f->pending,4); f->scheduled=false;
    }
    f->tx_budget=1; f->rx_budget=1; omni_gain_trial_poll(v,now);
}
static uint32_t run_hold(omni_gain_trial *v,fake *f,uint32_t start)
{
    for(uint32_t n=0;n<750;++n) {
        tick(v,f,start+n);
        if(v->phase==GAIN_HOLD || !omni_gain_trial_busy(v)) return start+n;
    }
    assert(0); return 0;
}
static void good(uint32_t start)
{
    omni_gain_trial v; fake f; setup(&v,&f,start);
    uint32_t hold=run_hold(&v,&f,start);
    assert(v.phase==GAIN_HOLD && v.attenuated_verified && !v.restored_verified);
    assert(f.sets==1 && !memcmp(f.current,(uint8_t[]){75,37,42,63},4));
    unsigned sent=f.sent_n;
    tick(&v,&f,hold+9999u); assert(f.sent_n==sent && v.phase==GAIN_HOLD);
    for(uint32_t n=10000;n<10750 && omni_gain_trial_busy(&v);++n) tick(&v,&f,hold+n);
    assert(v.phase==GAIN_DONE && v.restored_verified && f.sets==2);
    assert(!memcmp(f.current,(uint8_t[]){100,37,42,63},4));
    assert(f.commands==8 && f.sent_n==40 && v.set_may_have_applied==0x44);
    assert(!memcmp(v.tx_frame[2],(uint8_t[]){0xbd,8,0x47,1,75,37,42,63},8));
    assert(!memcmp(v.tx_frame[6],(uint8_t[]){0xbd,8,0x47,1,100,37,42,63},8));
    tick(&v,&f,hold+20000u); assert(f.sent_n==40);
}
static void faults(void)
{
    for(unsigned scenario=0;scenario<12;++scenario) {
        omni_gain_trial v; fake f; setup(&v,&f,0);
        switch(scenario) {
        case 0:f.mode=1;break; case 1:f.current[0]=25;break;
        case 2:f.drop_ack=true;break; case 3:f.bad_ack_length=true;break;
        case 4:f.bad_status=true;break; case 5:f.bad_type=true;break;
        case 6:f.wrong_opcode=true;break; case 7:f.tx_error=1;break;
        case 8:f.rx_error=1;break; case 9:f.drain_error=1;break;
        case 10:f.current[2]=101;break; case 11:f.malformed=true;break;
        }
        (void)run_hold(&v,&f,0); assert(v.phase==GAIN_FAILED && !f.sets);
        unsigned sent=f.sent_n; tick(&v,&f,1000); assert(f.sent_n==sent);
    }
    for(unsigned change=0;change<5;++change) {
        omni_gain_trial v; fake f; setup(&v,&f,0);
        uint32_t hold=run_hold(&v,&f,0); assert(v.phase==GAIN_HOLD);
        if(change<4) --f.current[change]; else f.mode=1;
        for(uint32_t n=10000;n<10750 && omni_gain_trial_busy(&v);++n) tick(&v,&f,hold+n);
        assert(v.phase==GAIN_FAILED && f.sets==1 && !v.restored_verified);
        assert(v.error==(change<4?GAIN_CHANGED:GAIN_MODE));
    }
    omni_gain_trial v; fake f; setup(&v,&f,0); f.stale=true;
    (void)run_hold(&v,&f,0);
    assert(v.phase==GAIN_FAILED && v.error==GAIN_READBACK && f.sets==1 && v.attempts==3);
}
static void partial_set(void)
{
    for(unsigned offset=0;offset<8;++offset) {
        omni_gain_trial v; fake f; setup(&v,&f,0); uint32_t now=0;
        while(v.step<2) { tick(&v,&f,now++); assert(now<200); }
        for(unsigned n=0;n<offset;++n) tick(&v,&f,now++);
        assert(v.offset==offset);
        f.tx_budget=0; f.rx_budget=32; omni_gain_trial_poll(&v,now++);
        assert(v.offset==offset);
        omni_gain_trial_cancel(&v); unsigned sent=f.sent_n; tick(&v,&f,now);
        assert(v.phase==GAIN_CANCELED && f.sent_n==sent && !v.restored_verified);
    }
    omni_gain_trial v; fake f; setup(&v,&f,0);
    /* Expired partial RX fails, even if the remainder could later parse. */
    push(&f,(uint8_t[]){0xdb},1); tick(&v,&f,0); tick(&v,&f,20);
    assert(v.phase==GAIN_FAILED && v.error==GAIN_FRAME && !f.sets);
}
static void restore(void)
{
    omni_gain_trial v; fake f; setup(&v,&f,0); memset(&v,0,sizeof(v));
    memcpy(f.current,(uint8_t[]){75,37,42,63},4);
    uint8_t original[]={100,37,42,63};
    assert(!omni_gain_restore_begin(&v,(omni_dsp_volume_io){&f,tx,rx,drain},0,
        (uint8_t[]){75,99,42,63},original));
    assert(omni_gain_restore_begin(&v,(omni_dsp_volume_io){&f,tx,rx,drain},0,f.current,original));
    for(uint32_t now=0;now<750 && omni_gain_trial_busy(&v);++now) tick(&v,&f,now);
    assert(v.phase==GAIN_DONE && v.restored_verified && !v.attenuated_verified);
    assert(f.sets==1 && f.commands==4 && !memcmp(f.current,original,4));
}
int main(void)
{
    good(0); good(UINT32_MAX-100u); faults(); partial_set(); restore();
    puts("DSP gain trial: fragmented I/O, restore, preservation, faults and cancellation passed");
    return 0;
}
