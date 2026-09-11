#include "display.h"
#include <assert.h>
#include <string.h>
typedef struct { unsigned pins, starts, cancels; int busy, reject; bool data;
    size_t length; uint8_t bytes[1024]; } mock;
static void pins(void *v,bool a,bool b) { mock *m=v;
    assert((m->pins==0 && !a && !b)||(m->pins==1 && !a && b)||(m->pins==2 && a && b)); ++m->pins; }
static int start(void *v,bool data,const uint8_t *p,size_t n) { mock *m=v;
    assert(n<=1024); m->data=data; m->length=n; memcpy(m->bytes,p,n); ++m->starts; return m->reject; }
static int busy(void *v) { return ((mock *)v)->busy; }
static void cancel(void *v) { ++((mock *)v)->cancels; }
static void setup(omni_display *d,mock *m,uint32_t t) {
    memset(m,0,sizeof(*m)); omni_display_io io={m,pins,start,busy,cancel};
    assert(omni_display_init(d,io,t));
    omni_display_poll(d,t); omni_display_poll(d,t+999U); assert(m->pins==1);
    omni_display_poll(d,t+1000U); omni_display_poll(d,t+1009U); assert(m->pins==2);
    omni_display_poll(d,t+1010U); omni_display_poll(d,t+1109U); assert(m->starts==0);
    omni_display_poll(d,t+1110U); assert(d->state==OMNI_DISPLAY_INIT);
}
int main(void) {
    omni_display d; mock m; uint8_t frame[1024]; memset(frame,0xa5,sizeof(frame));
    setup(&d,&m,UINT32_MAX-500U); /* All delay comparisons survive timer wrap. */
    uint32_t t=609;
    assert(!omni_display_present(&d,frame));
    omni_display_poll(&d,t); assert(m.starts==1 && !m.data && m.length==28 && m.bytes[0]==0xfd);
    m.busy=1; omni_display_poll(&d,++t); assert(d.state==OMNI_DISPLAY_INIT && m.starts==1);
    m.busy=0; omni_display_poll(&d,++t); assert(d.state==OMNI_DISPLAY_CLEAR);
    omni_display_poll(&d,++t); assert(m.data && m.length==1024);
    for(unsigned i=0;i<1024;i++) assert(m.bytes[i]==0);
    omni_display_poll(&d,++t); omni_display_poll(&d,++t);
    assert(!m.data && m.length==1 && m.bytes[0]==0xaf);
    omni_display_poll(&d,++t); assert(d.state==OMNI_DISPLAY_SETTLE);
    omni_display_poll(&d,t+99); assert(d.state==OMNI_DISPLAY_SETTLE);
    t+=100; omni_display_poll(&d,t); assert(d.state==OMNI_DISPLAY_READY);
    assert(omni_display_present(&d,frame)); memset(frame,0,sizeof(frame));
    assert(!omni_display_present(&d,frame));
    omni_display_poll(&d,++t); assert(!m.data && m.length==6);
    omni_display_poll(&d,++t); omni_display_poll(&d,++t);
    assert(m.data && m.length==1024 && m.bytes[0]==0xa5 && m.bytes[1023]==0xa5);
    omni_display_poll(&d,++t); assert(d.state==OMNI_DISPLAY_READY && !m.cancels);
    assert(!omni_display_brightness(&d,0) && !omni_display_brightness(&d,11));
    assert(omni_display_brightness(&d,7));
    assert(!omni_display_present(&d,frame) && !omni_display_brightness(&d,2));
    omni_display_poll(&d,++t);assert(!m.data && m.length==2 && m.bytes[0]==0x81 && m.bytes[1]==161);
    m.busy=1;omni_display_poll(&d,++t);assert(d.contrast==0 && d.state==OMNI_DISPLAY_CONTRAST);
    m.busy=0;omni_display_poll(&d,++t);assert(d.contrast==161 && d.state==OMNI_DISPLAY_READY);
    for(int failure=0;failure<3;failure++) {
        setup(&d,&m,0); t=1110;
        if(failure==0) m.reject=-1;
        omni_display_poll(&d,t);
        if(failure==1) { m.busy=-1; omni_display_poll(&d,t+1); }
        if(failure==2) { m.busy=1; omni_display_poll(&d,t+99); assert(!m.cancels); omni_display_poll(&d,t+100); }
        assert(d.state==OMNI_DISPLAY_FAILED && m.cancels==1);
        assert(!omni_display_present(&d,frame)); omni_display_poll(&d,t+1000);
        assert(m.starts==1 && m.cancels==1);
    }
    return 0;
}
