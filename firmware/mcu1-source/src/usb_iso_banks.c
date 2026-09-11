#include "usb_iso_banks.h"
#include <stddef.h>
#include <string.h>

static uint8_t *payload(omni_iso_banks_t *s, uint8_t slot)
{
    return s->endpoint==OMNI_ISO_RX_ENDPOINT ? s->payload.rx[slot] :
                                               s->payload.feedback[slot];
}

static uint16_t capacity(const omni_iso_banks_t *s)
{
    return s->packet_capacity;
}

static bool arm_slot(omni_iso_banks_t *s, uint8_t bank, uint8_t slot)
{
    s->slot_owner[slot]=OMNI_ISO_SLOT_CPU;
    if(s->endpoint==OMNI_ISO_FEEDBACK_ENDPOINT)
        s->ops.fill_feedback(s->ops.context,payload(s,slot));
    if(!s->ops.arm(s->ops.context,s->endpoint,bank,payload(s,slot),capacity(s))) {
        s->slot_owner[slot]=OMNI_ISO_SLOT_FREE;
        ++s->arm_failures; s->running=false;
        return false;
    }
    s->bank_slot[bank]=slot;
    s->slot_owner[slot]=OMNI_ISO_SLOT_HARDWARE;
    return true;
}

void omni_iso_banks_init(omni_iso_banks_t *s, const omni_iso_banks_ops_t *ops)
{
    memset(s,0,sizeof(*s));
    if(ops) s->ops=*ops;
    s->bank_slot[0]=s->bank_slot[1]=OMNI_ISO_NO_SLOT;
}

bool omni_iso_banks_open(omni_iso_banks_t *s, uint8_t endpoint, uint8_t first_bank)
{
    return omni_iso_banks_open_format(s,endpoint,first_bank,
        endpoint==OMNI_ISO_RX_ENDPOINT?OMNI_ISO_RX_CAPACITY:OMNI_ISO_FEEDBACK_BYTES,4u);
}

bool omni_iso_banks_open_format(omni_iso_banks_t *s,uint8_t endpoint,uint8_t first_bank,
                                uint16_t packet_capacity,uint8_t frame_bytes)
{
    bool geometry=endpoint==OMNI_ISO_RX_ENDPOINT?
        ((frame_bytes==4u && (packet_capacity==196u || packet_capacity==388u)) ||
         (frame_bytes==6u && (packet_capacity==294u || packet_capacity==582u))):
        (packet_capacity==OMNI_ISO_FEEDBACK_BYTES && frame_bytes==4u);
    if(!s || !geometry) return false;
    if(s->claimed || s->servicing || first_bank>1u || !s->ops.read || !s->ops.arm ||
       (endpoint!=OMNI_ISO_RX_ENDPOINT && endpoint!=OMNI_ISO_FEEDBACK_ENDPOINT) ||
       (endpoint==OMNI_ISO_RX_ENDPOINT && !s->ops.consume) ||
       (endpoint==OMNI_ISO_FEEDBACK_ENDPOINT && !s->ops.fill_feedback)) return false;
    if((s->ops.read(s->ops.context,endpoint,0u) |
        s->ops.read(s->ops.context,endpoint,1u)) & OMNI_ISO_ACTIVE) return false;
    s->endpoint=endpoint; s->expected_bank=first_bank;
    s->packet_capacity=packet_capacity;s->frame_bytes=frame_bytes;
    s->bank_slot[0]=s->bank_slot[1]=OMNI_ISO_NO_SLOT;
    memset(s->slot_owner,0,sizeof(s->slot_owner));
    s->packets=s->bytes=s->arm_failures=s->malformed=0u;
    s->ownership_errors=s->coalesced_services=s->discarded=0u;
    ++s->generation;
    s->claimed=true; s->running=true;
    /* Slot identities need not match bank identities. On RX the third slot
     * will replace the first completed bank before that old slot is read. */
    if(!arm_slot(s,first_bank,0u)) return false;
    if(!arm_slot(s,(uint8_t)(first_bank^1u),1u)) return false;
    return true;
}

unsigned omni_iso_banks_service(omni_iso_banks_t *s)
{
    if(!s->claimed || !s->running || s->servicing) return 0u;
    s->servicing=true;
    unsigned completed=0u;
    while(completed<2u && s->running) {
        uint8_t bank=s->expected_bank, old=s->bank_slot[bank];
        if(old>=3u || s->slot_owner[old]!=OMNI_ISO_SLOT_HARDWARE ||
           (s->endpoint==OMNI_ISO_FEEDBACK_ENDPOINT && old>=2u)) {
            ++s->ownership_errors; s->running=false; break;
        }
        uint32_t descriptor=s->ops.read(s->ops.context,s->endpoint,bank);
        if(descriptor & OMNI_ISO_ACTIVE) break;
        uint16_t remaining=(uint16_t)((descriptor>>16)&0x3ffu);
        uint16_t bytes=capacity(s);
        if(remaining>bytes ||
           (s->endpoint==OMNI_ISO_FEEDBACK_ENDPOINT && remaining!=0u) ||
           (s->endpoint==OMNI_ISO_RX_ENDPOINT && (bytes-remaining)%s->frame_bytes!=0u)) {
            ++s->malformed; ++s->discarded; s->running=false; break;
        }
        bytes=(uint16_t)(bytes-remaining);
        s->slot_owner[old]=OMNI_ISO_SLOT_CPU;
        s->bank_slot[bank]=OMNI_ISO_NO_SLOT;
        if(s->endpoint==OMNI_ISO_RX_ENDPOINT) {
            uint8_t fresh=0u;
            while(fresh<3u && s->slot_owner[fresh]!=OMNI_ISO_SLOT_FREE) ++fresh;
            if(fresh==3u) {
                ++s->ownership_errors; ++s->discarded; s->running=false; break;
            }
            if(!arm_slot(s,bank,fresh)) { ++s->discarded; break; }
            /* At this point the old payload belongs only to the CPU, even if
             * hardware completes both other banks during the synchronous copy. */
            s->ops.consume(s->ops.context,payload(s,old),bytes);
            s->slot_owner[old]=OMNI_ISO_SLOT_FREE;
        } else {
            /* An IN payload has already been transmitted. Only this now-
             * inactive bank owns it, so refill it independently of its peer. */
            if(!arm_slot(s,bank,old)) { ++s->discarded; break; }
        }
        ++s->packets; s->bytes+=bytes; ++completed;
        s->expected_bank=(uint8_t)(bank^1u);
    }
    if(completed==2u) ++s->coalesced_services;
    s->servicing=false;
    return completed;
}

void omni_iso_banks_stop(omni_iso_banks_t *s)
{
    s->running=false;
}

bool omni_iso_banks_quiesced(omni_iso_banks_t *s)
{
    if(s->servicing || s->running) return false;
    if(!s->claimed) return true;
    if((s->ops.read(s->ops.context,s->endpoint,0u) |
        s->ops.read(s->ops.context,s->endpoint,1u)) & OMNI_ISO_ACTIVE) return false;
    memset(s->slot_owner,0,sizeof(s->slot_owner));
    s->bank_slot[0]=s->bank_slot[1]=OMNI_ISO_NO_SLOT;
    s->claimed=false;
    ++s->generation;
    return true;
}
