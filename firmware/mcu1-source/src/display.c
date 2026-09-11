#include "display.h"
#include <string.h>

static const uint8_t init_commands[]={0xfd,0x12,0xae,0xd5,0xa0,0xa8,0x3f,
    0xd3,0,0x40,0x20,0,0xa1,0xc8,0xda,0x12,0xd9,0x22,0xdb,0x34,
    0xa4,0xa6,0x22,0,7,0x21,0,0x7f};
static const uint8_t on[]={0xaf}, range[]={0x22,0,7,0x21,0,0x7f};

static void fail(omni_display *d)
{
    d->io.cancel(d->io.context);
    d->active=false;
    d->state=OMNI_DISPLAY_FAILED;
}
bool omni_display_init(omni_display *d, omni_display_io io, uint32_t now)
{
    if (!d || !io.pins || !io.start || !io.busy || !io.cancel) return false;
    memset(d,0,sizeof(*d)); d->io=io; d->since=now;
    d->state=OMNI_DISPLAY_START;
    return true;
}
/* Returns true only after completion, not merely after queue acceptance. */
static bool transfer(omni_display *d, uint32_t now, bool data,
                     const uint8_t *bytes, size_t length)
{
    if (!d->active) {
        if (d->io.start(d->io.context,data,bytes,length)!=0) { fail(d); return false; }
        d->since=now; d->active=true;
        return false;
    }
    int busy=d->io.busy(d->io.context);
    if (busy<0 || (busy && (uint32_t)(now-d->since)>=100U)) { fail(d); return false; }
    if (busy) return false;
    d->active=false;
    return true;
}
void omni_display_poll(omni_display *d, uint32_t now)
{
    switch(d->state) {
    case OMNI_DISPLAY_START:
        d->io.pins(d->io.context,false,false); d->since=now;
        d->state=OMNI_DISPLAY_WAIT_1; break;
    case OMNI_DISPLAY_WAIT_1:
        if ((uint32_t)(now-d->since)<1000U) break;
        d->io.pins(d->io.context,false,true); d->since=now;
        d->state=OMNI_DISPLAY_WAIT_2; break;
    case OMNI_DISPLAY_WAIT_2:
        if ((uint32_t)(now-d->since)<10U) break;
        d->io.pins(d->io.context,true,true); d->since=now;
        d->state=OMNI_DISPLAY_WAIT_3; break;
    case OMNI_DISPLAY_WAIT_3:
        if ((uint32_t)(now-d->since)>=100U) d->state=OMNI_DISPLAY_INIT;
        break;
    case OMNI_DISPLAY_INIT:
        if (transfer(d,now,false,init_commands,sizeof(init_commands))) d->state=OMNI_DISPLAY_CLEAR;
        break;
    case OMNI_DISPLAY_CLEAR:
        if (transfer(d,now,true,d->frame,sizeof(d->frame))) d->state=OMNI_DISPLAY_ON;
        break;
    case OMNI_DISPLAY_ON:
        if (transfer(d,now,false,on,sizeof(on))) { d->state=OMNI_DISPLAY_SETTLE; d->since=now; }
        break;
    case OMNI_DISPLAY_SETTLE:
        if ((uint32_t)(now-d->since)>=100U) d->state=OMNI_DISPLAY_READY;
        break;
    case OMNI_DISPLAY_RANGE:
        if (transfer(d,now,false,range,sizeof(range))) d->state=OMNI_DISPLAY_PIXELS;
        break;
    case OMNI_DISPLAY_PIXELS:
        if (transfer(d,now,true,d->frame,sizeof(d->frame))) d->state=OMNI_DISPLAY_READY;
        break;
    case OMNI_DISPLAY_CONTRAST:
        if (transfer(d,now,false,d->command,sizeof(d->command))) {
            d->contrast=d->command[1];d->state=OMNI_DISPLAY_READY;
        }
        break;
    case OMNI_DISPLAY_READY: case OMNI_DISPLAY_FAILED: default: break;
    }
}
bool omni_display_present(omni_display *d,const uint8_t frame[1024])
{
    if (!frame || d->state!=OMNI_DISPLAY_READY) return false;
    memcpy(d->frame,frame,sizeof(d->frame)); d->state=OMNI_DISPLAY_RANGE;
    return true;
}
bool omni_display_brightness(omni_display *d,unsigned level)
{
    if(!d || d->state!=OMNI_DISPLAY_READY || level<1u || level>10u) return false;
    d->command[0]=0x81u;d->command[1]=(uint8_t)(level*23u);
    d->state=OMNI_DISPLAY_CONTRAST;return true;
}
