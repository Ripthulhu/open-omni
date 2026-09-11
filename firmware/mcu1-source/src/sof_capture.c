#include "sof_capture.h"
#include <stddef.h>
#include <string.h>
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Capture publication needs lock-free32-bit atomics");

/* UM11126 rev2.8 Tables38,46,55,56,67,373,538,752. See the NXP SOF contract
 * note for primary SDK provenance and our 49.152MHz vs sample 24.576MHz clock. */
#define AHB0 0x40000200u
#define AHB1 0x40000204u
#define SET0 0x40000220u
#define SET1 0x40000224u
#define CLR0 0x40000240u
#define CLR1 0x40000244u
#define RESET0 0x40000100u
#define RESET1 0x40000104u
#define RESET_SET1 0x40000124u
#define RESET_CLR1 0x40000144u
#define SELECTOR 0x4000026cu
#define CAP_ROUTE 0x40006020u
#define TIMER 0x40008000u
#define USB_INFO 0x40084004u
#define PLL_MD 0x40000590u
#define FRO_CTRL 0x40013010u
#define NVIC_ISER0 0xe000e100u
#define TIMER_BIT (1u << 26)
#define MUX_BIT (1u << 11)
#define TIMER_IRQ (1u << 10)

static bool rd(omni_sof_capture_t *s, uint32_t a, uint32_t *v)
{ return s->ops.read(s->ops.context,a,v); }
static bool wr(omni_sof_capture_t *s, uint32_t a, uint32_t v)
{ return s->ops.write(s->ops.context,a,v); }

static void finish(omni_sof_capture_t *s, omni_sof_capture_state_t state,
                   omni_sof_capture_error_t error)
{
    bool ok=true;
    if(s->timer_enabled) {
        if(!wr(s,TIMER+4u,0u)) ok=false;
        if(!wr(s,SELECTOR,s->saved_selector)) ok=false;
        if(!wr(s,s->saved_reset?RESET_SET1:RESET_CLR1,TIMER_BIT)) ok=false;
        if(!wr(s,CLR1,TIMER_BIT)) ok=false;
        s->timer_enabled=false;
    }
    if(s->route_saved) {
        if(!wr(s,CAP_ROUTE,s->saved_route)) ok=false;
        s->route_saved=false;
    }
    if(s->mux_enabled_here) {
        if(!wr(s,CLR0,MUX_BIT)) ok=false;
        s->mux_enabled_here=false;
    }
    s->error=ok?error:OMNI_SOF_IO;
    s->state=ok?state:OMNI_SOF_FAILED;
}

void omni_sof_capture_init(omni_sof_capture_t *s,const omni_sof_capture_ops_t *ops)
{
    memset(s,0,sizeof(*s));
    atomic_init(&s->count,0u);
    if(ops) s->ops=*ops;
}

bool omni_sof_capture_start(omni_sof_capture_t *s,uint32_t now_ms)
{
    return omni_sof_capture_start_mode(s,now_ms,OMNI_SOF_CLOCK_PLL0);
}

bool omni_sof_capture_start_mode(omni_sof_capture_t *s,uint32_t now_ms,
                                omni_sof_capture_clock_t clock_selector)
{
    uint32_t ahb0,ahb1,reset0,reset1,nvic,check;
    if(s->state==OMNI_SOF_RUNNING) return false;
    if(clock_selector!=OMNI_SOF_CLOCK_PLL0 && clock_selector!=OMNI_SOF_CLOCK_FRO96) {
        s->state=OMNI_SOF_FAILED; s->error=OMNI_SOF_CONFIG; return false;
    }
    s->clock_selector=clock_selector;
    if(!s->ops.read || !s->ops.write) {
        s->state=OMNI_SOF_FAILED; s->error=OMNI_SOF_IO; return false;
    }
    atomic_store_explicit(&s->count,0u,memory_order_release);
    s->polls=s->crossed=s->duplicates=s->elapsed_ms=0u;
    s->have_capture=false; s->start_ms=now_ms; s->error=OMNI_SOF_OK;
    if(!rd(s,AHB0,&ahb0) || !rd(s,AHB1,&ahb1) || !rd(s,RESET0,&reset0) ||
       !rd(s,RESET1,&reset1) || !rd(s,NVIC_ISER0,&nvic) ||
       !rd(s,SELECTOR,&s->saved_selector)) goto io;
    if((ahb1&TIMER_BIT) || (nvic&TIMER_IRQ) || (reset0&MUX_BIT)) {
        finish(s,OMNI_SOF_FAILED,OMNI_SOF_OWNERSHIP); return false;
    }
    s->saved_selector&=7u; /* Reserved bits have undefined read values. */
    s->saved_reset=reset1&TIMER_BIT;
    if(!(ahb0&MUX_BIT)) {
        if(!wr(s,SET0,MUX_BIT)) goto io;
        s->mux_enabled_here=true;
    }
    if(!rd(s,CAP_ROUTE,&s->saved_route)) goto io;
    s->saved_route&=31u;
    s->route_saved=true;
    if(!wr(s,SET1,TIMER_BIT)) goto io;
    s->timer_enabled=true;
    if(!wr(s,RESET_SET1,TIMER_BIT) || !wr(s,RESET_CLR1,TIMER_BIT) ||
       !wr(s,SELECTOR,(uint32_t)clock_selector) || !wr(s,CAP_ROUTE,0x14u) ||
       !wr(s,TIMER+4u,0u) || !wr(s,TIMER+0x0cu,0u) ||
       !wr(s,TIMER+0x70u,0u) || !wr(s,TIMER+0x14u,0u) ||
       !wr(s,TIMER+0x74u,0u) || !wr(s,TIMER+0x28u,1u) ||
       !wr(s,TIMER+0x08u,0u) || !wr(s,TIMER+0x10u,0u) ||
       !wr(s,TIMER,0xffu) || !wr(s,TIMER,0xffu)) goto io;
    /* Exact readback before starting a counter. No presumed PLL LOCK state. */
    if(!rd(s,SELECTOR,&check)) goto io;
    if((check&7u)!=(uint32_t)clock_selector) goto config;
    if(!rd(s,CAP_ROUTE,&check)) goto io;
    if((check&31u)!=0x14u) goto config;
    if(!rd(s,TIMER+0x28u,&check)) goto io;
    if(check!=1u) goto config;
    if(!wr(s,TIMER+4u,1u)) goto io;
    s->state=OMNI_SOF_RUNNING;
    return true;
config:
    finish(s,OMNI_SOF_FAILED,OMNI_SOF_CONFIG); return false;
io:
    finish(s,OMNI_SOF_FAILED,OMNI_SOF_IO); return false;
}

void omni_sof_capture_poll(omni_sof_capture_t *s,uint32_t now_ms)
{
    uint32_t record[4];
    if(s->state!=OMNI_SOF_RUNNING) return;
    s->elapsed_ms=now_ms-s->start_ms;
    if(s->elapsed_ms>=OMNI_SOF_CAPTURE_TIMEOUT_MS) {
        finish(s,OMNI_SOF_FAILED,OMNI_SOF_TIMEOUT); return;
    }
    ++s->polls;
    if(!rd(s,USB_INFO,&record[0]) || !rd(s,TIMER+0x2cu,&record[1]) ||
       !rd(s,USB_INFO,&record[2]) ||
       !rd(s,s->clock_selector==OMNI_SOF_CLOCK_FRO96?FRO_CTRL:PLL_MD,&record[3])) {
        finish(s,OMNI_SOF_FAILED,OMNI_SOF_IO); return;
    }
    record[0]&=2047u; record[2]&=2047u;
    if(record[0]!=record[2]) ++s->crossed;
    /* Initial reset CR0=0 is not evidence of an actual capture. A later wrap
     * to zero is retained; no signed or ordering assumptions about ticks. */
    if((!s->have_capture && record[1]==0u) ||
       (s->have_capture && record[1]==s->previous_capture)) {
        ++s->duplicates; return;
    }
    uint32_t count=atomic_load_explicit(&s->count,memory_order_relaxed);
    memcpy(s->samples[count],record,sizeof(record));
    atomic_store_explicit(&s->count,count+1u,memory_order_release);
    s->have_capture=true; s->previous_capture=record[1];
    if(count+1u==OMNI_SOF_CAPTURE_SAMPLES) finish(s,OMNI_SOF_DONE,OMNI_SOF_OK);
}

void omni_sof_capture_stop(omni_sof_capture_t *s)
{
    if(s->state==OMNI_SOF_RUNNING) finish(s,OMNI_SOF_STOPPED,OMNI_SOF_OK);
}
void omni_sof_capture_status(const omni_sof_capture_t *s,uint32_t out[15])
{
    uint32_t count=atomic_load_explicit(&s->count,memory_order_acquire);
    uint32_t v[15]={1u,s->state,s->error,count,s->polls,s->crossed,
        s->duplicates,s->elapsed_ms,OMNI_SOF_CAPTURE_SAMPLES,
        OMNI_SOF_CAPTURE_TIMEOUT_MS,s->saved_selector,s->saved_route,0,0,0};
    if(s->clock_selector==OMNI_SOF_CLOCK_FRO96) {
        v[0]=2u; v[12]=(uint32_t)s->clock_selector; v[13]=FRO_CTRL;
    }
    memcpy(out,v,sizeof(v));
}
void omni_sof_capture_trace(const omni_sof_capture_t *s,uint32_t index,uint32_t out[15])
{
    memset(out,0,60u);
    uint32_t count=atomic_load_explicit(&s->count,memory_order_acquire);
    out[0]=1u; out[1]=s->state; out[2]=s->error; out[3]=count; out[4]=index;
    if(s->clock_selector==OMNI_SOF_CLOCK_FRO96) {
        out[0]=2u; out[13]=(uint32_t)s->clock_selector;
    }
    if(index<count) memcpy(out+5,s->samples[index],16u);
    if(index<count && index+1u<count) memcpy(out+9,s->samples[index+1u],16u);
}

static bool mmio_read(void *context,uint32_t address,uint32_t *value)
{
    (void)context;
    *value=*(volatile const uint32_t *)(uintptr_t)address;
    return true;
}
static bool mmio_write(void *context,uint32_t address,uint32_t value)
{
    (void)context;
    *(volatile uint32_t *)(uintptr_t)address=value;
    return true;
}
omni_sof_capture_ops_t omni_sof_capture_mmio_ops(void)
{
    omni_sof_capture_ops_t ops={NULL,mmio_read,mmio_write}; return ops;
}
