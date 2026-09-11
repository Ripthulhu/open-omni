#include "usb_iso_banks.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Hardware model follows UM11126 rev2.8, 40.8.1/40.8.4: each transaction
 * updates address/NBYTES, clears ACTIVE even for a short packet and switches
 * EPINUSE. A second queued packet is independent of the first packet length.
 * The arm adapter rejects ACTIVE and reconstructs the entire descriptor. */
typedef struct {
    omni_iso_banks_t state;
    uint32_t descriptor[2];
    uint8_t *buffer[2];
    uint8_t next_bank, endpoint, next_marker;
    unsigned arm_calls, fail_arm_call, deliveries, tokens, misses, fill_calls;
    uint8_t expected_marker[4096];
    uint16_t expected_length[4096];
    unsigned queued, read_index;
    bool token_during_consume, stop_during_consume;
} model;

static uint32_t read_bank(void *context, uint8_t endpoint, uint8_t bank)
{
    model *m=context;
    assert(endpoint==m->endpoint && bank<2u);
    return m->descriptor[bank];
}

static bool arm_bank(void *context, uint8_t endpoint, uint8_t bank,
                     uint8_t *buffer, uint16_t length)
{
    model *m=context;
    assert(endpoint==m->endpoint && bank<2u);
    assert(((uintptr_t)buffer&63u)==0u);
    ++m->arm_calls;
    if(m->arm_calls==m->fail_arm_call) return false;
    assert(!(m->descriptor[bank]&OMNI_ISO_ACTIVE));
    for(unsigned other=0u;other<2u;++other)
        if(m->descriptor[other]&OMNI_ISO_ACTIVE) assert(m->buffer[other]!=buffer);
    m->buffer[bank]=buffer;
    m->descriptor[bank]=OMNI_ISO_ACTIVE | (1u<<26) |
        ((uint32_t)length<<16) | (((uint32_t)(uintptr_t)buffer>>6)&0xffffu);
    return true;
}

static bool token(model *m, uint16_t bytes)
{
    unsigned bank=m->next_bank;
    if(!(m->descriptor[bank]&OMNI_ISO_ACTIVE)) { ++m->misses; return false; }
    uint16_t capacity=(uint16_t)((m->descriptor[bank]>>16)&0x3ffu);
    assert(bytes<=capacity);
    uint8_t marker=++m->next_marker;
    if(m->endpoint==OMNI_ISO_RX_ENDPOINT) {
        memset(m->buffer[bank],marker,bytes);
        assert(m->queued<4096u);
        m->expected_marker[m->queued]=marker;
        m->expected_length[m->queued++]=bytes;
    }
    m->descriptor[bank]=(1u<<26) | ((uint32_t)(capacity-bytes)<<16) |
        (((m->descriptor[bank]&0xffffu)+(bytes/64u))&0xffffu);
    m->next_bank^=1u;
    ++m->tokens;
    return true;
}

static void consume(void *context, const uint8_t *buffer, uint16_t length)
{
    model *m=context;
    assert(m->endpoint==OMNI_ISO_RX_ENDPOINT && m->read_index<m->queued);
    for(unsigned bank=0u;bank<2u;++bank) assert(buffer!=m->buffer[bank]);
    assert(length==m->expected_length[m->read_index]);
    uint8_t marker=m->expected_marker[m->read_index++];
    if(m->token_during_consume) {
        m->token_during_consume=false;
        /* The host may complete both now-armed banks while the CPU still
         * reads the old payload. The free third slot prevents any overwrite. */
        assert(token(m,(uint16_t)(m->state.packet_capacity-m->state.frame_bytes)));
        assert(token(m,(uint16_t)(m->state.packet_capacity-2u*m->state.frame_bytes)));
    }
    for(unsigned i=0u;i<length;++i) assert(buffer[i]==marker);
    if(m->stop_during_consume) {
        m->stop_during_consume=false;
        omni_iso_banks_stop(&m->state);
        assert(!omni_iso_banks_quiesced(&m->state));
    }
    ++m->deliveries;
}

static void fill_feedback(void *context, uint8_t *buffer)
{
    model *m=context;
    for(unsigned bank=0u;bank<2u;++bank)
        if(m->descriptor[bank]&OMNI_ISO_ACTIVE) assert(m->buffer[bank]!=buffer);
    ++m->fill_calls;
    buffer[0]=(uint8_t)m->fill_calls; buffer[1]=0u; buffer[2]=48u; buffer[3]=0u;
}

static void init_model(model *m, uint8_t endpoint, uint8_t first)
{
    memset(m,0,sizeof(*m));
    m->endpoint=endpoint; m->next_bank=first;
    omni_iso_banks_ops_t ops={m,read_bank,arm_bank,consume,fill_feedback};
    omni_iso_banks_init(&m->state,&ops);
    assert(((uintptr_t)&m->state.payload&63u)==0u);
}

static void cancel_hardware(model *m)
{
    /* Represents confirmed EPSKIP completion (not merely writing EPSKIP). */
    m->descriptor[0]&=~OMNI_ISO_ACTIVE;
    m->descriptor[1]&=~OMNI_ISO_ACTIVE;
}

static void rx_short_packets_and_coalescing(void)
{
    model m; init_model(&m,OMNI_ISO_RX_ENDPOINT,0u);
    assert(omni_iso_banks_open(&m.state,m.endpoint,0u));
    assert(token(&m,192u)); /* short of196 must not close the other bank */
    assert(token(&m,188u));
    assert(omni_iso_banks_service(&m.state)==2u);
    assert(m.deliveries==2u && m.state.bytes==380u && m.state.running);
    assert(m.state.coalesced_services==1u);
    const uint16_t sizes[]={0u,196u,192u,188u};
    for(unsigned i=0u;i<2000u;++i) {
        assert(token(&m,sizes[i%4u]));
        assert(omni_iso_banks_service(&m.state)==1u);
        assert(omni_iso_banks_service(&m.state)==0u);
    }
    assert(m.state.packets==2002u && m.misses==0u);
    assert(m.state.arm_failures==0u && m.state.malformed==0u);
    assert(m.state.ownership_errors==0u && m.deliveries==m.tokens);
}

static void hardware_can_run_during_copy(void)
{
    model m; init_model(&m,OMNI_ISO_RX_ENDPOINT,1u);
    assert(omni_iso_banks_open(&m.state,m.endpoint,1u));
    assert(token(&m,196u));
    m.token_during_consume=true;
    assert(omni_iso_banks_service(&m.state)==2u); /* bounded despite new work */
    assert(m.tokens==3u && m.deliveries==2u);
    assert(omni_iso_banks_service(&m.state)==1u);
    assert(m.deliveries==3u && m.state.bytes==576u && m.misses==0u);
}

static void depth_is_two_not_a_lossless_promise(void)
{
    model m; init_model(&m,OMNI_ISO_RX_ENDPOINT,0u);
    assert(omni_iso_banks_open(&m.state,m.endpoint,0u));
    assert(token(&m,192u)); assert(token(&m,192u));
    assert(!token(&m,192u)); /* third token before service exceeds capacity */
    assert(omni_iso_banks_service(&m.state)==2u);
    assert(m.misses==1u && m.state.packets==2u);
    /* Compare a one-active-bank service gap under the same hardware model. */
    init_model(&m,OMNI_ISO_RX_ENDPOINT,0u);
    assert(arm_bank(&m,m.endpoint,0u,m.state.payload.rx[0],196u));
    assert(token(&m,192u)); assert(!token(&m,192u));
    assert(m.tokens==1u && m.misses==1u);
}

static void stop_cancel_reset_reopen(void)
{
    for(unsigned completed=0u;completed<=2u;++completed) {
        model m; init_model(&m,OMNI_ISO_RX_ENDPOINT,0u);
        assert(omni_iso_banks_open(&m.state,m.endpoint,0u));
        for(unsigned n=0u;n<completed;++n) assert(token(&m,192u));
        uint32_t generation=m.state.generation;
        omni_iso_banks_stop(&m.state);
        assert(omni_iso_banks_service(&m.state)==0u);
        if(completed<2u) assert(!omni_iso_banks_quiesced(&m.state));
        assert(!omni_iso_banks_open(&m.state,m.endpoint,0u));
        cancel_hardware(&m);
        assert(omni_iso_banks_quiesced(&m.state));
        assert(!m.state.claimed && m.deliveries==0u);
        assert(m.state.generation!=generation);
        m.next_bank=1u; m.read_index=m.queued; /* canceled packets never delivered */
        assert(omni_iso_banks_open(&m.state,m.endpoint,1u));
        assert(omni_iso_banks_service(&m.state)==0u); /* stale old IRQ */
        assert(token(&m,188u));
        assert(omni_iso_banks_service(&m.state)==1u && m.deliveries==1u);
    }
    model m; init_model(&m,OMNI_ISO_RX_ENDPOINT,0u);
    assert(omni_iso_banks_open(&m.state,m.endpoint,0u));
    assert(token(&m,192u)); m.stop_during_consume=true;
    assert(omni_iso_banks_service(&m.state)==1u && !m.state.running);
    assert(!omni_iso_banks_quiesced(&m.state));
    cancel_hardware(&m); assert(omni_iso_banks_quiesced(&m.state));
}

static void fail_closed(void)
{
    for(unsigned fail=1u;fail<=3u;++fail) {
        model m; init_model(&m,OMNI_ISO_RX_ENDPOINT,0u);
        m.fail_arm_call=fail;
        bool opened=omni_iso_banks_open(&m.state,m.endpoint,0u);
        if(fail<=2u) assert(!opened);
        else {
            assert(opened && token(&m,192u));
            assert(omni_iso_banks_service(&m.state)==0u);
        }
        assert(m.state.claimed && !m.state.running && m.state.arm_failures==1u);
        assert(m.deliveries==0u);
        if(fail>1u) assert(!omni_iso_banks_quiesced(&m.state));
        cancel_hardware(&m); assert(omni_iso_banks_quiesced(&m.state));
    }
    for(unsigned remaining=195u;remaining<=197u;remaining+=2u) {
        model m; init_model(&m,OMNI_ISO_RX_ENDPOINT,0u);
        assert(omni_iso_banks_open(&m.state,m.endpoint,0u));
        m.descriptor[0]=(remaining<<16)|(1u<<26);
        assert(omni_iso_banks_service(&m.state)==0u);
        assert(m.state.malformed==1u && !m.state.running && m.deliveries==0u);
    }
    model m; init_model(&m,OMNI_ISO_RX_ENDPOINT,0u);
    assert(!omni_iso_banks_open(&m.state,0x83u,0u));
    assert(!omni_iso_banks_open(&m.state,0x03u,2u));
    m.descriptor[0]=OMNI_ISO_ACTIVE;
    assert(!omni_iso_banks_open(&m.state,0x03u,0u));
    assert(!m.state.claimed && m.arm_calls==0u);
}

static void feedback_banks_are_independent(void)
{
    model m; init_model(&m,OMNI_ISO_FEEDBACK_ENDPOINT,0u);
    assert(omni_iso_banks_open(&m.state,m.endpoint,0u));
    assert(m.buffer[0]!=m.buffer[1]);
    assert(m.buffer[0][0]==1u && m.buffer[1][0]==2u);
    assert(token(&m,4u));
    assert(omni_iso_banks_service(&m.state)==1u);
    assert(m.buffer[0][0]==3u && m.buffer[1][0]==2u);
    assert(token(&m,4u)); assert(token(&m,4u));
    assert(omni_iso_banks_service(&m.state)==2u);
    assert(m.state.packets==3u && m.state.bytes==12u && m.fill_calls==5u);
    assert(m.deliveries==0u);
    omni_iso_banks_stop(&m.state);
    assert(!omni_iso_banks_quiesced(&m.state));
    cancel_hardware(&m); assert(omni_iso_banks_quiesced(&m.state));
}

static void all_playback_formats(void)
{
    const uint16_t capacities[]={196u,294u,388u,582u};
    const uint8_t strides[]={4u,6u,4u,6u};
    for(unsigned fmt=0;fmt<4u;++fmt) for(unsigned first=0;first<2u;++first) {
        model m;init_model(&m,OMNI_ISO_RX_ENDPOINT,(uint8_t)first);
        uint16_t cap=capacities[fmt];uint8_t stride=strides[fmt];
        assert(omni_iso_banks_open_format(&m.state,m.endpoint,(uint8_t)first,cap,stride));
        assert(m.state.packet_capacity==cap && m.state.frame_bytes==stride);
        assert(((uintptr_t)m.state.payload.rx[1]&63u)==0u && sizeof(m.state.payload.rx[0])>=cap);
        assert(!omni_iso_banks_open_format(&m.state,m.endpoint,(uint8_t)first,196u,4u));
        assert(m.state.packet_capacity==cap && m.state.frame_bytes==stride);
        const uint16_t sizes[]={0u,(uint16_t)(cap-2u*stride),(uint16_t)(cap-stride),cap};
        for(unsigned i=0;i<1000u;++i) {
            assert(token(&m,sizes[i%4u]));
            if(i%2u) assert(omni_iso_banks_service(&m.state)==2u);
        }
        assert(m.deliveries==1000u && !m.misses && !m.state.malformed);
        assert(token(&m,cap));m.token_during_consume=true;
        assert(omni_iso_banks_service(&m.state)==2u);
        assert(omni_iso_banks_service(&m.state)==1u);
        assert(m.deliveries==1003u && m.tokens==1003u && !m.state.ownership_errors);
        omni_iso_banks_stop(&m.state);cancel_hardware(&m);
        assert(omni_iso_banks_quiesced(&m.state));
        m.next_bank=0u;
        assert(omni_iso_banks_open_format(&m.state,m.endpoint,0u,cap,stride));
        assert(token(&m,(uint16_t)(stride-1u)));
        assert(!omni_iso_banks_service(&m.state) && m.state.malformed==1u && !m.state.running);
    }
    model m;init_model(&m,OMNI_ISO_RX_ENDPOINT,0u);
    const uint16_t invalid[][2]={{0,0},{582,4},{196,6},{583,6},{1024,4},{294,3},{4,4}};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        assert(!omni_iso_banks_open_format(&m.state,m.endpoint,0u,invalid[i][0],(uint8_t)invalid[i][1]));
        assert(!m.state.claimed && !m.arm_calls);
    }
    assert(!omni_iso_banks_open_format(NULL,3u,0u,196u,4u));
}

int main(void)
{
    rx_short_packets_and_coalescing(); hardware_can_run_during_copy();
    depth_is_two_not_a_lossless_promise(); stop_cancel_reset_reopen();
    fail_closed(); feedback_banks_are_independent();all_playback_formats();
    puts("ISO banks: ordered short packets, ownership, feedback and lifecycle passed");
    return 0;
}
