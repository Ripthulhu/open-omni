#include "board_clock.h"
#include "usb_audio_ring.h"
#include "audio_queue.h"
#include "source_mix.h"
#include "audio_rate.h"
#include "audio_delivery.h"
#include "fsl_device_registers.h"
#include "ui.h"
#include <string.h>
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define DMA_BIT 0x800u
#define TRACE_SIZE 128u
#define TARGET_FRAMES 512u
uint32_t omni_usb_audio[OMNI_USB_AUDIO_WORDS] __attribute__((aligned(4)));
static uint32_t first_descriptor[4] __attribute__((aligned(16)));
static uint32_t second_descriptor[4] __attribute__((aligned(16)));
static omni_audio_queue_t queue;
static omni_audio_rate_t rate;
static omni_audio_delivery_t delivery;
static omni_audio_format stream_format={.sample_rate=48000u,.epoch=0u,
    .max_packet=196u,.sample_bits=16u,.frame_bytes=4u,.frames_per_ms=48u,.alternate=1u};
static volatile uint32_t stale_packets;
static volatile uint32_t active,prepared,dma_fault,completed,frames_in,ignored;
static volatile uint32_t gain_ready;
static volatile uint32_t source_gain=OMNI_SOURCE_MIX_UNITY;
static omni_source_mix_ramp source_ramp;
void usb_audio_ring_source_gain(unsigned q14)
{ if(q14<=OMNI_SOURCE_MIX_UNITY) source_gain=q14; }
void usb_audio_ring_gain_ready(int ready) { gain_ready=ready?1u:0u; }
static volatile uint32_t expected_b,max_isr_cycles,fifo_errors;
static uint32_t feedback_force,last_servo,mean_fill_q8;
static volatile uint32_t fill_sum,fill_count,fill_min,fill_max;
static uint32_t trace[TRACE_SIZE][8],trace_count,trace_frozen;
/* DMA preempts USB; serialize short trace publication, never sample copying.
 * The first fault freezes preceding USB/DMA event history (~64ms). */
static void record(uint32_t kind,uint32_t frames,uint32_t detail)
{
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    if(!trace_frozen) {
        uint32_t *p=trace[trace_count%TRACE_SIZE];
        p[0]=++trace_count; p[1]=omni_ui_milliseconds(); p[2]=kind|(frames<<8);
        p[3]=atomic_load_explicit(&queue.written,memory_order_relaxed);
        p[4]=atomic_load_explicit(&queue.read,memory_order_relaxed);
        p[5]=rate.last_md; p[6]=detail; p[7]=REG(0x40088e04u);
        if(kind>=3u) trace_frozen=1u;
    }
    __set_PRIMASK(mask);
}
void usb_audio_ring_start(void)
{
    omni_audio_format format;
    (void)omni_audio_format_make(&format,48000u,16u,0u);
    (void)usb_audio_ring_start_format(&format);
}
uint32_t usb_audio_ring_dma_words(void) { return (uint32_t)stream_format.frames_per_ms*2u; }
bool usb_audio_ring_start_format(const omni_audio_format *format)
{
    if(!omni_audio_format_valid(format) || active ||
       (prepared && ((REG(0x40082020u)|REG(0x40082030u)|REG(0x40082038u))&DMA_BIT))) return false;
    /* DMA must be quiesced. Reject USB throughout clearing; activation occurs
     * immediately before I2S MAINENABLE/SETTRIG, not before the DSP handshake. */
    active=0; prepared=0; gain_ready=0;
    /* Feedback runs in USB IRQ even while the PCM consumer is stopped. Publish
     * the small format snapshot atomically, never mask IRQ across queue clear. */
    uint32_t mask=__get_PRIMASK();__disable_irq();
    stream_format=*format;
    __set_PRIMASK(mask);
    uint32_t target_frames=TARGET_FRAMES*(uint32_t)format->frames_per_ms/48u;
    (void)omni_source_mix_ramp_init(&source_ramp,source_gain);
    feedback_force=0; /* A diagnostic override belongs to one stream only. */
    if(!omni_audio_queue_init_format(&queue,target_frames,format)) return false;
    (void)omni_audio_delivery_reset_format(&delivery,format->frame_bytes,format->frames_per_ms);
    memset(omni_usb_audio,0,sizeof(omni_usb_audio));
    (void)omni_audio_rate_init(&rate,TARGET_FRAMES);
    dma_fault=completed=frames_in=ignored=expected_b=max_isr_cycles=fifo_errors=stale_packets=0;
    fill_sum=fill_count=0; fill_min=OMNI_AUDIO_QUEUE_FRAMES; fill_max=0;
    mean_fill_q8=target_frames<<8; trace_count=trace_frozen=0;
    last_servo=omni_ui_milliseconds();
    REG(0x40000590u)=OMNI_AUDIO_RATE_MD_NOMINAL;
    prepared=1;return true;
}
void usb_audio_ring_dma_setup(uint32_t channel_descriptor[4])
{
    uint32_t words=usb_audio_ring_dma_words();
    if(!prepared || active || !channel_descriptor ||
       channel_descriptor[0]!=(0x1213u|((words-1u)<<16)) ||
       channel_descriptor[1]!=(uint32_t)(uintptr_t)&omni_usb_audio[words-1u] ||
       channel_descriptor[2]!=0x40088e20u) { dma_fault|=64u;return; }
    /* UM11126 section 21.5.4, table 420: the mutable channel head initially
     * describes A, then reloads from immutable B and A templates. The head
     * cannot also serve as template A: loading B replaces its address/link
     * fields. A exhausts with INTA, B with INTB. Templates change only stopped. */
    first_descriptor[0]=channel_descriptor[0];
    first_descriptor[1]=channel_descriptor[1];
    first_descriptor[2]=channel_descriptor[2];
    first_descriptor[3]=(uint32_t)(uintptr_t)second_descriptor;
    second_descriptor[0]=0x1223u|((words-1u)<<16);
    second_descriptor[1]=(uint32_t)(uintptr_t)&omni_usb_audio[2u*words-1u];
    second_descriptor[2]=0x40088e20u;
    second_descriptor[3]=(uint32_t)(uintptr_t)first_descriptor;
    channel_descriptor[3]=(uint32_t)(uintptr_t)second_descriptor;
    __DMB();
    REG(0x40082058u)=DMA_BIT; REG(0x40082060u)=DMA_BIT; REG(0x40082040u)=DMA_BIT;
    NVIC_SetPriority(DMA0_IRQn,2); NVIC_ClearPendingIRQ(DMA0_IRQn);
    REG(0x40082048u)=DMA_BIT; NVIC_EnableIRQ(DMA0_IRQn);
}
void usb_audio_ring_activate(void)
{
    if(prepared && !dma_fault) { last_servo=omni_ui_milliseconds(); __DMB(); active=1; }
}
void usb_audio_ring_stop(void)
{
    active=0; gain_ready=0;
    if(prepared) {
        NVIC_DisableIRQ(DMA0_IRQn); REG(0x40082050u)=DMA_BIT;
        REG(0x40082028u)=DMA_BIT;
        uint32_t spins=0;
        while((REG(0x40082038u)&DMA_BIT) && ++spins<OMNI_CPU_GUARD_ITERATIONS(4096u)) {}
        if(!(REG(0x40082038u)&DMA_BIT)) REG(0x40082078u)=DMA_BIT;
        else dma_fault|=8u;
        REG(0x40082058u)=DMA_BIT; REG(0x40082060u)=DMA_BIT;
        NVIC_ClearPendingIRQ(DMA0_IRQn);
        REG(0x40000590u)=OMNI_AUDIO_RATE_MD_NOMINAL;
        omni_audio_rate_reset(&rate);
    }
    /* Retain fault/trace/counters until next explicit start. */
}
bool usb_audio_ring_reset(void)
{
    usb_audio_ring_stop();
    if(prepared && (REG(0x40082038u)&DMA_BIT)) return false;
    dma_fault=0; prepared=0; return true;
}
void usb_audio_ring_write(const uint8_t *pcm,uint32_t bytes)
{
    usb_audio_ring_write_format(pcm,bytes,stream_format.epoch);
}
void usb_audio_ring_write_format(const uint8_t *pcm,uint32_t bytes,uint32_t epoch)
{
    if(epoch!=stream_format.epoch) { ++stale_packets;return; }
    uint32_t frames=bytes/stream_format.frame_bytes;
    if(!active) { ignored+=frames; return; }
    frames_in+=frames;
    uint32_t accepted=omni_audio_queue_push(&queue,pcm,bytes);
    record(accepted==frames && bytes%stream_format.frame_bytes==0u?1u:4u,frames,USB0->INFO);
}
void usb_audio_ring_observe_packet(uint32_t bytes,uint32_t usb_frame,uint32_t now_ms)
{
    usb_audio_ring_observe_packet_format(bytes,usb_frame,now_ms,stream_format.epoch);
}
void usb_audio_ring_observe_packet_format(uint32_t bytes,uint32_t usb_frame,uint32_t now_ms,uint32_t epoch)
{
    if(active && epoch==stream_format.epoch) omni_audio_delivery_observe(&delivery,bytes,usb_frame,now_ms);
}
void usb_audio_ring_delivery_status(uint32_t out[15])
{
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    memcpy(out,delivery.words,60u);
    __set_PRIMASK(mask);
}
static void dma_failure(uint32_t why,uint32_t flags)
{
    dma_fault|=why; record(5,why,flags); active=0;
    /* Main finishes quiescence. Never guess ownership after coalesced flags. */
    REG(0x40082028u)=DMA_BIT; REG(0x40082050u)=DMA_BIT;
    REG(0x40088c00u)=0x1f0000u; REG(0x40086c00u)=0xf0430u;
    NVIC_DisableIRQ(DMA0_IRQn);
}
void DMA0_IRQHandler(void)
{
    uint32_t start=SysTick->VAL;
    uint32_t a=REG(0x40082058u)&DMA_BIT,b=REG(0x40082060u)&DMA_BIT;
    uint32_t err=REG(0x40082040u)&DMA_BIT;
    if(!active) { REG(0x40082058u)=a; REG(0x40082060u)=b; return; }
    if(err || (a && b) || (!a && !b) || (expected_b ? !b : !a)) {
        dma_failure(1u,a|(b<<1)|(err<<2)); return;
    }
    /* Refill only with substantial time left in the OTHER descriptor. A live
     * countdown is useful for this ownership bound, never queue occupancy.
     * >=65/129 words gives about 677/672us at 48/96k, before FIFO/clock-pull
     * allowance. The admissible window stays at two thirds of one ms. */
    uint32_t transfer=REG(0x400824b8u);
    uint32_t remaining=(transfer>>16)&1023u;
    uint32_t frames=stream_format.frames_per_ms,words=frames*2u;
    if((transfer&0x30u)!=(a?0x20u:0x10u) || remaining<words*2u/3u || remaining>=words) {
        dma_failure(16u,transfer); return;
    }
    if(a) REG(0x40082058u)=a; else REG(0x40082060u)=b;
    uint32_t fill=omni_audio_queue_fill(&queue);
    if(fill>OMNI_AUDIO_QUEUE_FRAMES) { dma_failure(2u,fill); return; }
    if(fill<fill_min) fill_min=fill;
    if(fill>fill_max) fill_max=fill;
    if(fill_count<1000u) { fill_sum+=fill; ++fill_count; }
    uint32_t take=omni_audio_queue_render(&queue,
        &omni_usb_audio[a?0u:words],frames);
    /* Only the just-released block is writable. Snapshot one aligned target
     * for both channels; unity remains bit-exact. No change to clock/queue
     * ownership or the other, currently playing descriptor. */
    (void)omni_source_mix_apply_format(&source_ramp,source_gain,
        &omni_usb_audio[a?0u:words],frames,stream_format.sample_bits,stream_format.frames_per_ms);
    /* Keep DMA/USB clocks and queue consumption running during startup gain
     * discovery or a control fault. Never expose inherited maximum gain.
     * This is not a PCM volume/mute implementation. */
    if(!gain_ready) memset(&omni_usb_audio[a?0u:words],0,words*sizeof(uint32_t));
    completed+=frames; expected_b^=1u; __DMB();
    uint32_t fs=REG(0x40088e04u);
    if(fs&1u) { ++fifo_errors; REG(0x40088e04u)=1u; }
    record(take<frames?3u:(fs&1u?6u:2u),take,REG(0x400824b8u));
    /* Modulo1ms service-time observation, NOT WCET or interrupt latency.
     * A coalesced/completed descriptor is separately treated as a fault. */
    uint32_t elapsed=(start+SysTick->LOAD+1u-SysTick->VAL)%(SysTick->LOAD+1u);
    if(elapsed>max_isr_cycles) max_isr_cycles=elapsed;
    /* Another completion during refill violates our ownership deadline. */
    if((REG(0x40082058u)|REG(0x40082060u)|REG(0x40082040u))&DMA_BIT)
        dma_failure(4u,elapsed);
}
void usb_audio_ring_clock_servo(void)
{
    if(!active) return;
    uint32_t now=omni_ui_milliseconds(),dt=now-last_servo;
    if(dt<32u) return;
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    uint32_t sum=fill_sum,count=fill_count; fill_sum=fill_count=0;
    last_servo=now;
    if(count) mean_fill_q8=(sum*256u)/count;
    uint32_t md=omni_audio_rate_update_format(&rate,mean_fill_q8,stream_format.sample_rate,dt,count!=0u);
    REG(0x40000590u)=md;
    __set_PRIMASK(mask);
    if(!count) dma_failure(32u,dt); /* active clock/DMA made no progress in 32ms */
}
uint32_t usb_audio_ring_fault(void) { return dma_fault; }
uint32_t usb_audio_ring_feedback(void) { return feedback_force?feedback_force:((uint32_t)stream_format.frames_per_ms<<14); }
uint32_t usb_audio_ring_feedback_format(const omni_audio_format *format)
{
    if(!omni_audio_format_valid(format)) return 0u;
    if(format->epoch==stream_format.epoch && format->sample_rate==stream_format.sample_rate &&
       format->sample_bits==stream_format.sample_bits) return usb_audio_ring_feedback();
    return (uint32_t)format->frames_per_ms<<14;
}
bool usb_audio_ring_set_force(uint32_t v)
{
    /* Keep research nominal+/-1-frame tests within the active endpoint MPS.
     * Reject before the later 10.14->16.16 shift can wrap arbitrary input. */
    uint32_t nominal=stream_format.frames_per_ms;
    if(v && (v<((nominal-1u)<<14) || v>((nominal+1u)<<14))) return false;
    feedback_force=v;return true;
}
void usb_audio_ring_set_md_force(uint32_t v) { omni_audio_rate_set_force(&rate,v); }
void usb_audio_ring_set_gains(int32_t kp,int32_t ki,int reset_i)
{
    /* ABI v2: Ki is MD/(frame*s), identified by diagnostic 44 version 2. */
    (void)omni_audio_rate_set_gains(&rate,kp?kp:rate.kp,ki?ki:rate.ki);
    if(reset_i) omni_audio_rate_reset(&rate);
}
int32_t usb_audio_ring_kp(void) { return rate.kp; }
int32_t usb_audio_ring_ki_q8(void) { return rate.ki; }
int32_t usb_audio_ring_servo_i(void) { return omni_audio_rate_integral(&rate); }
uint32_t usb_audio_ring_write_index(void) { return (atomic_load(&queue.written)&2047u)*2u; }
uint32_t usb_audio_ring_frames_written(void) { return frames_in; }
uint32_t usb_audio_ring_frames_out(void) { return queue.accepted; }
uint32_t usb_audio_ring_corrections(void) { return queue.overflows+queue.underruns; }
uint32_t usb_audio_ring_dups(void) { return queue.silence; }
uint32_t usb_audio_ring_drops(void) { return queue.rejected; }
uint32_t usb_audio_ring_step(void) { return 65536u; }
uint32_t usb_audio_ring_gap_min(void) { return fill_min*2u; }
uint32_t usb_audio_ring_gap_max(void) { return fill_max*2u; }
uint32_t usb_audio_ring_gap_now(void) { return (mean_fill_q8*2u)>>8; }
void usb_audio_ring_status(uint32_t out[15])
{
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    uint32_t v[15]={2u,active,dma_fault,completed,omni_audio_queue_fill(&queue),
        queue.underruns,queue.silence,queue.overflows,queue.rejected,fifo_errors,
        max_isr_cycles,trace_count,trace_frozen,ignored,rate.last_md};
    memcpy(out,v,sizeof(v)); __set_PRIMASK(mask);
}
void usb_audio_ring_format_status(uint32_t out[15])
{
    if(!out) return;
    uint32_t mask=__get_PRIMASK();__disable_irq();
    uint32_t v[15]={1u,stream_format.epoch,stream_format.sample_rate,stream_format.sample_bits,
        stream_format.frame_bytes,stream_format.frames_per_ms,stream_format.max_packet,
        usb_audio_ring_dma_words(),stale_packets,prepared,active,
        TARGET_FRAMES*(uint32_t)stream_format.frames_per_ms/48u,OMNI_AUDIO_QUEUE_FRAMES};
    memcpy(out,v,sizeof(v));__set_PRIMASK(mask);
}
void usb_audio_ring_trace(uint8_t index,uint32_t out[15])
{
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    memset(out,0,60); out[0]=2; out[1]=trace_count; out[2]=trace_frozen; out[3]=TRACE_SIZE;
    if(index<TRACE_SIZE) memcpy(out+4,trace[index],sizeof(trace[index]));
    __set_PRIMASK(mask);
}
