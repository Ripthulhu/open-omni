#include "board_clock.h"
#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_device_dci.h"
#include "usb_device_lpcip3511.h"
#include "fsl_device_registers.h"
#include "usb_iso_lpc5528.h"
#include "usb_iso_banks.h"
#include "usb_audio_ring.h"
#include "audio_probe.h"
#include "ui.h"
#include <string.h>

typedef struct {
    omni_iso_banks_t banks;
    usb_device_lpc3511ip_state_struct_t *controller;
    omni_audio_format format;
    uint32_t cancel_failures;
    uint8_t physical;
} iso_endpoint;
static iso_endpoint endpoints[2];
/* Keep publication evidence outside the bank/payload object. These are the
 * exact last CPU commands, never inferred from hardware-mutated descriptors. */
typedef struct {
    uint32_t armed[2], first[15], failures;
    uint8_t armed_valid;
} iso_evidence;
static iso_evidence evidence[2];

bool omni_usb_iso_managed(uint8_t ep) { return ep==3u || ep==0x84u; }
static iso_endpoint *get(uint8_t ep)
{ return ep==3u?&endpoints[0]:(ep==0x84u?&endpoints[1]:NULL); }
static volatile uint32_t *descriptor(iso_endpoint *ep,unsigned bank)
{ return ep->controller->epCommandStatusList+ep->physical*2u+bank; }
static iso_evidence *history(iso_endpoint *ep)
{ return &evidence[ep==&endpoints[1]?1u:0u]; }
static void fail_descriptor(iso_endpoint *ep,unsigned bank,uint32_t raw,uint32_t reason)
{
    iso_evidence *ev=history(ep);
    ++ev->failures;
    if(!ev->first[2]) {
        uint32_t v[15]={1u,ep->banks.endpoint,reason,
            bank|((uint32_t)ep->banks.expected_bank<<8)|((uint32_t)ev->armed_valid<<16),
            raw,ev->armed[bank],*descriptor(ep,bank^1u),ev->armed[bank^1u],
            USB0->INFO,USB0->INTSTAT,USB0->EPINUSE,USB0->EPBUFCFG,
            USB0->EPSKIP,omni_ui_milliseconds(),ev->failures};
        memcpy(ev->first,v,sizeof(v));
    }
    omni_iso_banks_stop(&ep->banks);
}
static uint32_t read_bank(void *context,uint8_t endpoint,uint8_t bank)
{
    (void)endpoint;
    iso_endpoint *ep=context;
    uint32_t value=*descriptor(ep,bank);
    if(!(value&OMNI_ISO_ACTIVE)) {
        __DMB();
        if(ep->banks.claimed && ep->banks.running) {
            iso_evidence *ev=history(ep);
            uint32_t armed=ev->armed[bank],reason=0u;
            uint32_t remaining=(value>>16)&0x3ffu,capacity=(armed>>16)&0x3ffu;
            if(!(ev->armed_valid&(1u<<bank))) reason|=OMNI_ISO_FAULT_NO_ARM;
            /* Each arm is ONE packet. UM11126 table763 specifies floor/64,
             * but live USB0 OUT294 -> remaining6 advanced5 units for288 bytes
             * (516992 latch85260311 ->04060316). Accept only floor OR ceil
             * of the exact transferred length, never an arbitrary address in
             * the slot. Zero bytes advances0. Payload ownership still uses the
             * recorded original slot, not this hardware-mutated cursor.
             * Four-byte IN has the separately observed +1 form (f729). */
            if((value&0xfc000000u)!=0x04000000u) reason|=OMNI_ISO_FAULT_CONTROL;
            if(remaining>capacity) reason|=OMNI_ISO_FAULT_LENGTH;
            else {
                uint32_t transferred=capacity-remaining;
                uint32_t expected=((armed&0xffffu)+transferred/64u)&0xffffu;
                bool address_ok=(value&0xffffu)==expected;
                if(endpoint==3u && (value&0xffffu)==
                   (((armed&0xffffu)+(transferred+63u)/64u)&0xffffu))
                    address_ok=true;
                if(endpoint==0x84u && capacity==4u && remaining==0u &&
                   (value&0xffffu)==(((armed&0xffffu)+1u)&0xffffu))
                    address_ok=true;
                if(!address_ok) reason|=OMNI_ISO_FAULT_ADDRESS;
                if(endpoint==3u && transferred%ep->banks.frame_bytes) reason|=OMNI_ISO_FAULT_PCM_LENGTH;
            }
            if(endpoint==0x84u && remaining) reason|=OMNI_ISO_FAULT_FEEDBACK_LENGTH;
            if(reason) {
                fail_descriptor(ep,bank,value,reason);
                /* The bank callback has no error result. Return an ACTIVE
                 * sentinel solely to stop service before consume/refill;
                 * preserve the REAL descriptor and ownership unchanged. */
                return value|OMNI_ISO_ACTIVE;
            }
        }
    }
    return value;
}
static bool arm_bank(void *context,uint8_t endpoint,uint8_t bank,uint8_t *buffer,uint16_t length)
{
    iso_endpoint *ep=context;
    uintptr_t address=(uintptr_t)buffer;
    if(!omni_usb_iso_managed(endpoint) || bank>1u || (address&63u) ||
       (address&~(uintptr_t)0x3fffffu)!=USB0->DATABUFSTART ||
       (address&0x3fffffu)+length>0x400000u ||
       (endpoint==3u?length!=ep->format.max_packet:length!=4u)) return false;
    volatile uint32_t *word=descriptor(ep,bank);
    if(*word&OMNI_ISO_ACTIVE) return false;
    /* UM11126 table763: hardware mutates both address and NBytes. Rebuild
     * every field; never modify an ACTIVE descriptor. No EPINUSE writes. */
    uint32_t command=OMNI_ISO_ACTIVE|0x04000000u|((uint32_t)length<<16)|
        ((uint32_t)(address>>6)&0xffffu);
    iso_evidence *ev=history(ep);
    ev->armed[bank]=command; ev->armed_valid|=(uint8_t)(1u<<bank);
    __DMB();
    *word=command;
    __DMB();
    return true;
}
static void consume(void *context,const uint8_t *buffer,uint16_t length)
{
    iso_endpoint *ep=context;
    audio_probe_iso_receive_format(buffer,length,ep->format.epoch);
}
static void fill_feedback(void *context,uint8_t *buffer)
{
    iso_endpoint *ep=context;
    uint32_t value=usb_audio_ring_feedback_format(&ep->format)<<2;
    for(unsigned i=0;i<4u;++i) buffer[i]=(uint8_t)(value>>(i*8u));
}
bool omni_usb_iso_open(void *device,uint8_t endpoint)
{
    omni_audio_format format;
    (void)omni_audio_format_make(&format,48000u,16u,0u);
    return omni_usb_iso_open_format(device,endpoint,&format);
}
bool omni_usb_iso_open_format(void *device,uint8_t endpoint,const omni_audio_format *format)
{
    iso_endpoint *ep=get(endpoint);
    if(!ep || !device || !omni_audio_format_valid(format) || ep->banks.claimed) return false;
    usb_device_struct_t *dev=device;
    usb_device_lpc3511ip_state_struct_t *controller=dev->controllerHandle;
    uint8_t physical=(uint8_t)(((endpoint&15u)<<1)|(endpoint>>7));
    if(!controller || controller->registerBase!=USB0 ||
       (uintptr_t)controller->epCommandStatusList!=USB0->EPLISTSTART ||
       USB0->DATABUFSTART!=0x20000000u ||
       controller->endpointState[physical].stateUnion.stateBitField.endpointType!=USB_ENDPOINT_ISOCHRONOUS ||
       controller->endpointState[physical].stateUnion.stateBitField.maxPacketSize!=(endpoint==3u?format->max_packet:4u))
        return false;
    ep->controller=controller; ep->physical=physical;ep->format=*format;
    if(!ep->banks.ops.read) {
        omni_iso_banks_ops_t ops={ep,read_bank,arm_bank,consume,fill_feedback};
        omni_iso_banks_init(&ep->banks,&ops);
    }
    /* Configuration is software-owned. Retain the live selector, which can
     * change in hardware independently for other active endpoints. */
    USB0->EPBUFCFG|=1u<<physical;
    USB0->INTSTAT=1u<<physical;
    history(ep)->armed_valid=0u;
    return omni_iso_banks_open_format(&ep->banks,endpoint,(uint8_t)((USB0->EPINUSE>>physical)&1u),
        endpoint==3u?format->max_packet:4u,endpoint==3u?format->frame_bytes:4u);
}
bool omni_usb_iso_interrupt(void *controller,uint8_t physical)
{
    uint8_t endpoint=(uint8_t)((physical>>1)|((physical&1u)<<7));
    iso_endpoint *ep=get(endpoint);
    if(!ep) return false;
    /* Swallow stale/unclaimed IRQs too: the SDK logical-transfer engine
     * never owns these endpoints, even immediately after a reset/close. */
    if(ep->controller==controller && ep->banks.claimed)
        (void)omni_iso_banks_service(&ep->banks);
    return true;
}
bool omni_usb_iso_preinit(void *controller,uint8_t endpoint)
{
    (void)controller;
    if(!omni_usb_iso_managed(endpoint)) return true;
    return !endpoints[0].banks.claimed && !endpoints[1].banks.claimed;
}
bool omni_usb_iso_cancel(void *controller,uint8_t endpoint)
{
    if(!omni_usb_iso_managed(endpoint)) return true;
    uint32_t mask=0;
    for(unsigned i=0;i<2u;++i) {
        iso_endpoint *ep=&endpoints[i];
        if(!ep->banks.claimed) continue;
        if(ep->controller!=controller) return false;
        omni_iso_banks_stop(&ep->banks);
        mask|=1u<<ep->physical;
    }
    if(!mask) return true;
    /* Other SDK endpoints are fixed at bank0 with double buffering disabled.
     * Only these two selectors can change in hardware. EPSKIP does NOT switch
     * bank: first park BOTH currently selected banks, then select their peers
     * while neither endpoint can advance. Never release just one early.
     * One combined EPSKIP store per phase avoids clearing a pending peer skip. */
    for(unsigned spins=0;spins<OMNI_CPU_GUARD_ITERATIONS(4096u);++spins) {
        if(USB0->EPSKIP) continue;
        uint32_t before=USB0->EPINUSE,skip=0,select=before;
        for(unsigned i=0;i<2u;++i) {
            iso_endpoint *ep=&endpoints[i];
            if(!(mask&(1u<<ep->physical))) continue;
            unsigned bank=(before>>ep->physical)&1u;
            uint32_t selected=*descriptor(ep,bank),peer=*descriptor(ep,bank^1u);
            if(selected&OMNI_ISO_ACTIVE) skip|=1u<<ep->physical;
            if(peer&OMNI_ISO_ACTIVE) select^=1u<<ep->physical;
        }
        __DMB();
        if(before!=USB0->EPINUSE || USB0->EPSKIP) continue;
        if(skip) { USB0->EPSKIP=skip; __DMB(); continue; }
        /* Every selected bank is inactive and selectors were stable across
         * the observations. No CPU can rearm here; no hardware can toggle.
         * Single-buffer endpoints never change their selectors. */
        if(select!=before) { USB0->EPINUSE=select; __DMB(); continue; }
        USB0->INTSTAT=mask;
        for(unsigned i=0;i<2u;++i) {
            iso_endpoint *ep=&endpoints[i];
            if(!(mask&(1u<<ep->physical))) continue;
            *descriptor(ep,0)=0x40000000u; *descriptor(ep,1)=0x40000000u;
            __DMB();
            if(!omni_iso_banks_quiesced(&ep->banks)) return false;
        }
        return true;
    }
    ++get(endpoint)->cancel_failures;
    return false; /* SDK must not overwrite descriptors or release buffers. */
}

void omni_usb_iso_bus_reset(void *controller)
{
    /* Called only from the DCI's DRES_C handler, before notifying classes.
     * Do not assume reset cleared ACTIVE in descriptor SRAM. Cancel/observe
     * both banks before releasing storage, as on an ordinary close. */
    for(unsigned i=0;i<2u;++i) {
        iso_endpoint *ep=&endpoints[i];
        if(ep->controller!=controller || !ep->banks.claimed) continue;
        (void)omni_usb_iso_cancel(controller,ep->banks.endpoint);
    }
}
bool omni_usb_iso_failed(void)
{
    for(unsigned i=0;i<2u;++i)
        if(endpoints[i].banks.claimed && !endpoints[i].banks.running) return true;
    return false;
}
bool omni_usb_iso_status(uint8_t endpoint,uint32_t out[15])
{
    iso_endpoint *ep=get(endpoint);
    if(!ep) return false;
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    const omni_iso_banks_t *s=&ep->banks;
    uint32_t v[15]={1u,endpoint,(uint32_t)s->claimed|((uint32_t)s->running<<1),s->generation,
        s->packets,s->bytes,s->arm_failures,s->malformed,s->ownership_errors,
        s->coalesced_services,s->discarded,s->expected_bank,ep->cancel_failures,
        s->bank_slot[0]|((uint32_t)s->bank_slot[1]<<8),
        s->slot_owner[0]|((uint32_t)s->slot_owner[1]<<8)|((uint32_t)s->slot_owner[2]<<16)};
    memcpy(out,v,sizeof(v)); __set_PRIMASK(mask); return true;
}
bool omni_usb_iso_failure_status(uint8_t endpoint,uint32_t out[15])
{
    iso_endpoint *ep=get(endpoint);
    if(!ep || !out) return false;
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    iso_evidence *ev=history(ep);
    memcpy(out,ev->first,sizeof(ev->first));
    out[0]=1u; out[1]=endpoint; out[14]=ev->failures;
    __set_PRIMASK(mask); return true;
}
