#include "mcu2_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t tx[OMNI_MCU2_FRAME_BYTES*32U],rx[OMNI_MCU2_FRAME_BYTES*12U];
    size_t sent,used,pos,tx_limit;
    unsigned tx_calls,rx_calls;
    bool block_tx,tx_error,rx_error,block_idle;
} wire;
static int tx(void *context,uint8_t value)
{
    wire *w=context;++w->tx_calls;
    if(w->tx_error) return -1;
    if(w->block_tx || (w->tx_limit && w->sent>=w->tx_limit)) return 0;
    assert(w->sent<sizeof(w->tx));w->tx[w->sent++]=value;return 1;
}
static int rx(void *context,uint8_t *value)
{
    wire *w=context;++w->rx_calls;
    if(w->rx_error) return -1;
    if(w->pos==w->used) return 0;
    *value=w->rx[w->pos++];return 1;
}
static bool tx_idle(void *context) { return !((wire *)context)->block_idle; }
static void init(omni_mcu2_runtime *s,wire *w)
{
    memset(w,0,sizeof(*w));assert(omni_mcu2_runtime_init(s,(mcu2_link_io){w,tx,rx},0));
    s->tx_idle=tx_idle;
}
static void poll(omni_mcu2_runtime *s,wire *w,uint32_t now)
{
    w->rx_calls=w->tx_calls=0;omni_mcu2_runtime_poll(s,now);
    assert(w->rx_calls<=OMNI_MCU2_RUNTIME_RX_BUDGET);
    assert(w->tx_calls<=OMNI_MCU2_RUNTIME_TX_BUDGET);
    assert(s->rx_used<OMNI_MCU2_FRAME_BYTES);
}
static void run(omni_mcu2_runtime *s,wire *w,uint32_t now)
{ for(unsigned i=0;i<200U;++i) poll(s,w,now); }
static void incoming(wire *w,const uint8_t *prefix,size_t n)
{
    assert(w->used+OMNI_MCU2_FRAME_BYTES<=sizeof(w->rx));
    memset(w->rx+w->used,0,OMNI_MCU2_FRAME_BYTES);memcpy(w->rx+w->used,prefix,n);
    w->used+=OMNI_MCU2_FRAME_BYTES;
}
static void assert_frame(const wire *w,unsigned index,const uint8_t *prefix,size_t n)
{
    size_t pos=(size_t)index*OMNI_MCU2_FRAME_BYTES;
    assert(pos+OMNI_MCU2_FRAME_BYTES<=w->sent);
    assert(!memcmp(w->tx+pos,prefix,n));
    for(size_t i=n;i<OMNI_MCU2_FRAME_BYTES;++i) assert(w->tx[pos+i]==0U);
}
static void contracts(void)
{
    omni_mcu2_runtime s;wire w;
    init(&s,&w);run(&s,&w,1);
    assert(s.tx_frames==4 && w.sent==4U*OMNI_MCU2_FRAME_BYTES);
    assert_frame(&w,0,(uint8_t[]){0xbc,4,0xe1,2},4);
    assert_frame(&w,1,(uint8_t[]){0xbc,3,0x87},3);
    assert_frame(&w,2,(uint8_t[]){0xaa,3,12},3);
    assert_frame(&w,3,(uint8_t[]){0x12,0x63,0},3);
    incoming(&w,(uint8_t[]){0xcb,4,0x87,0xff},4);
    incoming(&w,(uint8_t[]){0xcb,7,0xe1,3,1,0x32,0},7);
    incoming(&w,(uint8_t[]){0xcb,4,0x87,3},4);
    incoming(&w,(uint8_t[]){0xcb,6,0x88,2,2,1},6);
    incoming(&w,(uint8_t[]){0xcb,4,0x87,0xa0},4);
    incoming(&w,(uint8_t[]){0xcc,6,0xd2,1,0x26,0x32},6);
    run(&s,&w,10);
    assert(s.rx_frames==6 && !s.rejected_frames && s.ready && s.readiness==0xa0);
    assert(s.have_version && !memcmp(s.version,(uint8_t[]){1,0x32,0},3));
    assert(s.query_replied[0]==1 && s.query_replied[1]==1);
    assert(s.detected_ports==2 && s.queried_ports==3 && s.active_mode==2 && s.backend_setting==1);
    assert(s.secondary_volume==38 && s.secondary_aux==50 && s.controls_generation==1);
    assert(s.lifecycle==0); /* USB readiness never fabricates wireless state. */
    assert(!omni_mcu2_runtime_lifecycle(&s,0));
    assert(!omni_mcu2_runtime_controls(&s,101,50,1,80,0));
    assert(!omni_mcu2_runtime_controls(&s,50,50,1,80,99));
    assert(omni_mcu2_runtime_lifecycle(&s,3));
    assert(omni_mcu2_runtime_controls(&s,40,50,1,80,0));
    /* A hundred updates use one pending slot, retaining the latest value. */
    for(unsigned value=0;value<=100U;++value)
        assert(omni_mcu2_runtime_controls(&s,(uint8_t)value,50,1,80,0));
    run(&s,&w,20);
    assert(s.tx_frames==6);
    assert_frame(&w,4,(uint8_t[]){0xbc,6,0xe4,3,3,0},6);
    assert_frame(&w,5,(uint8_t[]){0xbc,8,0xef,100,50,1,80,0},8);
    assert(omni_mcu2_runtime_controls(&s,100,50,1,80,0));run(&s,&w,21);
    assert(s.tx_frames==6);
    assert(!omni_mcu2_runtime_select(&s,0)); /* Absent port. */
    assert(omni_mcu2_runtime_select(&s,1));assert(s.selection==OMNI_MCU2_SELECT_OBSERVED);
    assert(s.tx_frames==6); /* Already selected, do not provoke restart. */
    incoming(&w,(uint8_t[]){0xcb,6,0x88,3,2,1},6);run(&s,&w,30);
    assert(omni_mcu2_runtime_select(&s,0));assert(!omni_mcu2_runtime_select(&s,1));
    run(&s,&w,40);assert(s.selection==OMNI_MCU2_SELECT_WAITING);
    assert_frame(&w,6,(uint8_t[]){0x12,0x33,0},3);
    incoming(&w,(uint8_t[]){0xcb,6,0x88,3,0,1},6);run(&s,&w,50);
    assert(s.selection==OMNI_MCU2_SELECT_WAITING);poll(&s,&w,540);
    assert(s.selection==OMNI_MCU2_SELECT_OBSERVED);
    assert(omni_mcu2_runtime_select(&s,1));run(&s,&w,550);
    poll(&s,&w,5550);assert(s.selection==OMNI_MCU2_SELECT_TIMEOUT);
    uint32_t status[15];omni_mcu2_runtime_status(&s,0,status);assert(status[10]==0x3201);
}
static void framing(void)
{
    omni_mcu2_runtime s;wire w;init(&s,&w);
    /* Start in a previous frame's zero padding, as after warm MCU1 boot. */
    w.used=300;
    incoming(&w,(uint8_t[]){0xcb,4,0x87,0xff},4);run(&s,&w,10);
    assert(s.rx_frames==1 && s.discarded==300 && s.have_readiness && !s.ready);
    init(&s,&w);
    incoming(&w,(uint8_t[]){0xcb,4,0x87,0xa0},4);w.rx[800]=0x44;
    incoming(&w,(uint8_t[]){0xcb,7,0xe1,3,1,0x32,0},7);run(&s,&w,10);
    assert(s.rejected_frames && s.rx_frames==1 && s.have_version && !s.have_readiness);
    init(&s,&w); /* A missing byte cannot poison all following envelopes. */
    incoming(&w,(uint8_t[]){0xcb,4,0x87,0xa0},4);--w.used;
    incoming(&w,(uint8_t[]){0xcb,7,0xe1,3,1,0x32,0},7);run(&s,&w,10);
    assert(s.rejected_frames && s.have_version && !s.have_readiness);
    init(&s,&w);
    memcpy(w.rx,(uint8_t[]){0xcb,4,0x87,0xa0},4);w.used=4;run(&s,&w,10);
    assert(!s.have_readiness);incoming(&w,(uint8_t[]){0xcb,4,0x87,0xff},4);
    run(&s,&w,110);assert(s.gaps==1 && s.have_readiness && !s.ready);
    init(&s,&w);incoming(&w,(uint8_t[]){0xcb,7,0xe1,2,1,0x32,0},7);
    run(&s,&w,10);assert(!s.have_version && s.rejected_frames==1);
}
static void coalescing(void)
{
    omni_mcu2_runtime s;wire w;init(&s,&w);run(&s,&w,1);
    assert(omni_mcu2_runtime_controls(&s,40,50,1,80,0));
    w.tx_limit=w.sent+4U;poll(&s,&w,10);assert(s.tx_used==4);
    /* Freeze after the header/volume, mutate every remaining payload field:
     * in-flight prefix stays immutable; next full envelope gets new values. */
    assert(omni_mcu2_runtime_controls(&s,80,60,0,70,25));
    w.tx_limit=0;run(&s,&w,11);
    assert_frame(&w,4,(uint8_t[]){0xbc,8,0xef,40,50,1,80,0},8);
    assert_frame(&w,5,(uint8_t[]){0xbc,8,0xef,80,60,0,70,25},8);
    assert(s.tx_frames==6);
    assert(omni_mcu2_runtime_dsp86(&s,1));assert(!omni_mcu2_runtime_dsp86(&s,4));
    run(&s,&w,12);assert_frame(&w,6,(uint8_t[]){0xbc,6,0x86,3,1,0},6);
}
static void failure(void)
{
    omni_mcu2_runtime s;wire w;init(&s,&w);w.block_tx=true;
    run(&s,&w,1);assert(!w.sent);poll(&s,&w,3001);assert(s.fault && s.io_errors==1);
    w.block_tx=false;run(&s,&w,4000);assert(!w.sent);
    init(&s,&w);poll(&s,&w,1);assert(w.sent==64);w.block_tx=true;
    poll(&s,&w,3001);assert(s.fault);assert(!omni_mcu2_runtime_query(&s,OMNI_MCU2_QUERY_VERSION));
    init(&s,&w);w.rx_error=true;poll(&s,&w,1);assert(s.fault && !w.sent);
    init(&s,&w);run(&s,&w,1);run(&s,&w,3001);run(&s,&w,6001);run(&s,&w,9001);
    assert(s.version_attempts==3 && s.query_sent[0]==3); /* Bounded full-frame retries. */
    init(&s,&w);poll(&s,&w,1);omni_mcu2_runtime_stop(&s);assert(s.fault && !s.started);
    size_t sent=w.sent;run(&s,&w,4);assert(w.sent==sent);
    init(&s,&w);
    assert(omni_mcu2_runtime_init(&s,(mcu2_link_io){&w,tx,rx},0xfffffff0U));
    poll(&s,&w,0xfffffff5U);w.block_tx=true;
    poll(&s,&w,0xfffffff5U+2999U);assert(!s.fault);
    poll(&s,&w,0xfffffff5U+3000U);assert(s.fault);
}
static void first_selection(void)
{
    omni_mcu2_runtime s;wire w;init(&s,&w);run(&s,&w,1);
    assert(!omni_mcu2_runtime_select(&s,0));
    incoming(&w,(uint8_t[]){0xcb,4,0x87,1},4);run(&s,&w,10);
    assert(s.have_detect && !s.have_state && !s.have_readiness && !s.ready);
    assert(!omni_mcu2_runtime_select(&s,1));
    assert(omni_mcu2_runtime_select(&s,0));run(&s,&w,11);
    assert(s.selection==OMNI_MCU2_SELECT_WAITING && !s.have_state && !s.ready);
    assert_frame(&w,4,(uint8_t[]){0x12,0x33,0},3);
    /* If the side was already selected, stock can ignore the command. No
     * response is not success and must never provoke a second selection. */
    run(&s,&w,5011);assert(s.selection==OMNI_MCU2_SELECT_TIMEOUT);
    unsigned selections=0;
    for(size_t offset=0;offset+OMNI_MCU2_FRAME_BYTES<=w.sent;offset+=OMNI_MCU2_FRAME_BYTES)
        if(w.tx[offset]==0x12U && w.tx[offset+1U]==0x33U) ++selections;
    assert(selections==1U && !s.have_state && !s.ready);
    poll(&s,&w,6010);assert(!omni_mcu2_runtime_select(&s,0)); /* Exact freshness boundary. */
    incoming(&w,(uint8_t[]){0xcb,4,0x87,0},4);run(&s,&w,6011);
    assert(!omni_mcu2_runtime_select(&s,0));
    /* A new direct absence observation takes precedence over old CB88. */
    incoming(&w,(uint8_t[]){0xcb,6,0x88,3,2,1},6);run(&s,&w,6020);
    incoming(&w,(uint8_t[]){0xcb,4,0x87,0},4);run(&s,&w,6021);
    assert(!omni_mcu2_runtime_select(&s,1));
    /* Unsigned age works across timer wrap. */
    init(&s,&w);
    assert(omni_mcu2_runtime_init(&s,(mcu2_link_io){&w,tx,rx},0xfffffff0U));
    incoming(&w,(uint8_t[]){0xcb,4,0x87,2},4);run(&s,&w,0xfffffff5U);
    poll(&s,&w,5);assert(omni_mcu2_runtime_select(&s,1));
    omni_mcu2_runtime_stop(&s);assert(s.selection==OMNI_MCU2_SELECT_FAILED);
}
static void source_gain(void)
{
    omni_mcu2_runtime s={0};wire w;
    assert(!omni_mcu2_runtime_gain(NULL,1));assert(!omni_mcu2_runtime_gain(&s,1));
    init(&s,&w);assert(!omni_mcu2_runtime_gain(&s,13));
    assert(s.gain_generation==1U && s.gain_desired==12U && !s.have_gain_sent);
    run(&s,&w,1);assert(s.have_gain_sent && s.gain_sent==12U && s.gain_sent_generation==1U);
    assert(omni_mcu2_runtime_gain(&s,12));run(&s,&w,2);assert(s.tx_frames==4U);
    for(unsigned i=0;i<100U;++i) assert(omni_mcu2_runtime_gain(&s,(uint8_t)(i%13U)));
    uint32_t latest=s.gain_generation;assert(s.gain_desired==8U);
    w.block_idle=true;run(&s,&w,10);
    assert(s.active==OMNI_MCU2_TX_GAIN && s.tx_used==OMNI_MCU2_FRAME_BYTES);
    assert(s.gain_sent==12U && s.gain_sent_generation==1U && s.tx_frames==4U);
    size_t sent=w.sent;run(&s,&w,11);assert(w.sent==sent); /* No repeated final byte. */
    assert(omni_mcu2_runtime_gain(&s,3));assert(omni_mcu2_runtime_gain(&s,4));
    uint32_t newer=s.gain_generation;
    uint32_t status[15];omni_mcu2_runtime_status(&s,2,status);
    assert((status[1]&112U)==112U && status[2]==4U && status[3]==12U && status[7]==8U);
    w.block_idle=false;poll(&s,&w,12);
    assert(s.gain_sent==8U && s.gain_sent_generation==latest && s.gain_sent_ms==12U);
    run(&s,&w,13);assert(s.gain_sent==4U && s.gain_sent_generation==newer && s.tx_frames==6U);
    assert_frame(&w,4,(uint8_t[]){0xaa,3,8},3);assert_frame(&w,5,(uint8_t[]){0xaa,3,4},3);
    assert(omni_mcu2_runtime_gain(&s,7));w.tx_limit=w.sent+2U;poll(&s,&w,20);
    assert(s.tx_used==2U);assert(omni_mcu2_runtime_gain(&s,2));w.tx_limit=0;run(&s,&w,21);
    assert_frame(&w,6,(uint8_t[]){0xaa,3,7},3);assert_frame(&w,7,(uint8_t[]){0xaa,3,2},3);
    /* A later fault/stop never changes the last completely transmitted gain. */
    assert(omni_mcu2_runtime_gain(&s,0));w.block_idle=true;run(&s,&w,30);
    poll(&s,&w,3029);assert(!s.fault);poll(&s,&w,3030);assert(s.fault && s.gain_sent==2U);
    assert(!omni_mcu2_runtime_gain(&s,12));
    init(&s,&w);s.tx_idle=NULL;run(&s,&w,1);assert(!s.have_gain_sent);
    poll(&s,&w,3001);assert(s.fault && !s.have_gain_sent);
    init(&s,&w);run(&s,&w,1);assert(omni_mcu2_runtime_gain(&s,0));
    w.block_idle=true;run(&s,&w,0xfffffff0U);
    poll(&s,&w,0xfffffff0U+2999U);assert(!s.fault);
    poll(&s,&w,0xfffffff0U+3000U);assert(s.fault && s.gain_sent==12U);
    init(&s,&w);run(&s,&w,1);assert(omni_mcu2_runtime_gain(&s,0));omni_mcu2_runtime_stop(&s);
    assert(!omni_mcu2_runtime_gain(&s,1));assert(s.gain_sent==12U && !s.pending);
}
static void replay(const char *path)
{
    FILE *file=fopen(path,"rb");assert(file);
    omni_mcu2_runtime s;wire w;init(&s,&w);uint32_t count=0;
    while((w.used=fread(w.rx,1,OMNI_MCU2_FRAME_BYTES,file))!=0U) {
        assert(w.used==OMNI_MCU2_FRAME_BYTES);w.pos=0;run(&s,&w,10);
        ++count;
    }
    assert(!ferror(file));fclose(file);
    assert(s.rx_frames==count && !s.rejected_frames && !s.discarded && !s.fault);
    printf("replayed %u stock MCU2 envelopes: %s\n",count,path);
}
int main(int argc,char **argv)
{
    contracts();framing();coalescing();failure();first_selection();source_gain();
    for(int i=1;i<argc;++i) replay(argv[i]);
    puts("MCU2 persistent runtime contracts passed");return 0;
}
