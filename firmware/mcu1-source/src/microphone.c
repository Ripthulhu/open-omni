#include "microphone.h"
#include "fsl_device_registers.h"
#include "board_clock.h"
#include <string.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define BIT (1u<<4)
#define SIZE 1024u
static uint16_t blocks[2][96] __attribute__((aligned(4)));
static uint32_t templates[2][4] __attribute__((aligned(16)));
static int16_t samples[SIZE];
static volatile uint32_t active, fault, written, read_index, received, nonzero;
static volatile uint32_t overruns, underruns, interrupts, peak, fifo_errors;
static uint32_t frames, expected_b, primed, configured;

bool omni_microphone_stop(void)
{
    if(!configured) return true;
    active=0;
    REG(0x40082050u)=BIT;
    REG(0x40082028u)=BIT;
    REG(0x40086e00u)&=~(1u<<13);
    INPUTMUX->DMA0_REQ_ENA_CLR=BIT;
    uint32_t spins=0;
    while((REG(0x40082038u)&BIT) && ++spins<OMNI_CPU_GUARD_ITERATIONS(4096u)) {}
    if(REG(0x40082038u)&BIT) { fault|=8u;return false; }
    REG(0x40082078u)=BIT;
    REG(0x40082058u)=BIT;REG(0x40082060u)=BIT;REG(0x40082040u)=BIT;
    return true;
}

bool omni_microphone_start(uint32_t head[4], uint32_t rate)
{
    if(!head || (rate!=48000u && rate!=96000u) || !omni_microphone_stop()) return false;
    configured=1;frames=rate/1000u;
    written=read_index=received=nonzero=overruns=underruns=interrupts=peak=fifo_errors=0;
    fault=expected_b=primed=0;
    memset(blocks,0,sizeof(blocks));
    for(unsigned i=0;i<2u;++i) {
        templates[i][0]=0x4103u|((frames-1u)<<16)|(i?0x20u:0x10u);
        templates[i][1]=0x40086e30u;
        templates[i][2]=(uint32_t)(uintptr_t)&blocks[i][frames-1u];
        templates[i][3]=(uint32_t)(uintptr_t)templates[i^1u];
    }
    memcpy(head,templates[0],16);
    REG(0x40001094u)=0x4101u; /* PIO1_5, stock FC0 RX data function. */
    REG(0x40086e00u)=0x22002u; /* RX enable, flush and DMA request. */
    REG(0x40086e08u)=0x2u; /* RX request at one FIFO entry. */
    REG(0x40086e04u)=2u;
    INPUTMUX->DMA0_REQ_ENA_SET=BIT;
    REG(0x40082440u)=0x10001u;
    REG(0x40082020u)=BIT;
    REG(0x40082448u)=head[0];
    __DMB();active=1;
    REG(0x40082048u)=BIT;
    NVIC_SetPriority(DMA0_IRQn,2);NVIC_EnableIRQ(DMA0_IRQn);
    REG(0x40082070u)=BIT;
    return true;
}

void omni_microphone_irq(void)
{
    uint32_t a=REG(0x40082058u)&BIT,b=REG(0x40082060u)&BIT,e=REG(0x40082040u)&BIT;
    if(!a && !b && !e) return;
    if(a) REG(0x40082058u)=BIT;
    if(b) REG(0x40082060u)=BIT;
    if(e) REG(0x40082040u)=BIT;
    if(!active) return;
    uint32_t transfer=REG(0x40082448u),remaining=(transfer>>16)&1023u;
    if(e || (a && b) || (expected_b?!b:!a) ||
       (transfer&0x30u)!=(a?0x20u:0x10u) || remaining<frames/2u || remaining>=frames) {
        fault|=1u;active=0;REG(0x40082028u)=BIT;REG(0x40082050u)=BIT;return;
    }
    const uint16_t *data=blocks[a?0u:1u];
    uint32_t w=written,r=read_index,step=frames/48u;
    for(unsigned i=0;i<frames;i+=step) {
        int32_t sample=(int16_t)data[i];
        if(step==2u) sample=(sample+(int16_t)data[i+1u])/2;
        uint32_t magnitude=(uint32_t)(sample<0?-sample:sample);
        if(magnitude>peak) peak=magnitude;
        if(sample) ++nonzero;
        if(w-r<SIZE) samples[w++%SIZE]=(int16_t)sample;
        else ++overruns;
        ++received;
    }
    __DMB();written=w;expected_b^=1u;++interrupts;
    if(REG(0x40086e04u)&2u) {++fifo_errors;REG(0x40086e04u)=2u;}
}

unsigned omni_microphone_packet(uint8_t out[98])
{
    uint32_t mask=__get_PRIMASK();__disable_irq();
    uint32_t r=read_index,w=written,available=w-r;
    if(!primed && available>=96u) primed=1u;
    unsigned count=48u;
    if(active && primed) {
        if(available>144u) count=49u;
        else if(available<96u) count=47u;
    }
    for(unsigned i=0;i<count;++i) {
        int16_t sample=0;
        if(active && primed && r!=w) sample=samples[r++%SIZE];
        else if(active && primed) ++underruns;
        out[2u*i]=(uint8_t)sample;out[2u*i+1u]=(uint8_t)((uint16_t)sample>>8);
    }
    read_index=r;__set_PRIMASK(mask);
    return count*2u;
}

void omni_microphone_status(uint32_t out[15])
{
    uint32_t mask=__get_PRIMASK();__disable_irq();
    uint32_t v[15]={1u,active,fault,frames*1000u,received,nonzero,peak,
        written-read_index,overruns,underruns,interrupts,fifo_errors,
        configured?REG(0x40086e04u):0u,configured?REG(0x40082448u):0u,
        configured?REG(0x40001094u):0u};
    memcpy(out,v,sizeof(v));__set_PRIMASK(mask);
}
