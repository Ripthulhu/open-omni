#include "mcu2_controls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    omni_mcu2_controls s;uint8_t payload[5]={0};omni_mcu2_controls_init(&s);
    assert(!omni_mcu2_controls_snapshot(&s,payload));
    /* User's exact extend/mute/unmute/retract sequence, observed DSP D3 raw. */
    const uint8_t states[]={0,1,0,1};
    for(unsigned i=0;i<sizeof(states);++i) {
        uint8_t frame[]={0xdb,6,0xd3,3,1,states[i]};
        assert(omni_mcu2_controls_observe(&s,frame,sizeof(frame),i*5000U));
        assert(s.mic_state==states[i] && s.mic_generation==i+1U);
        assert(!omni_mcu2_controls_snapshot(&s,payload));
    }
    assert(s.last_mic_ms==15000U && s.known==OMNI_PEER_HAVE_MIC_STATE);
    /* High/low-gain D3 subtype3 is not mic-state or an extra unmute. */
    assert(!omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,6,0xd3,3,3,1},6,20));
    assert(s.mic_generation==4U);
    assert(!omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,6,0xd3,3,1,2},6,20));
    assert(s.mic_state==1U && s.rejected==1U);
    assert(omni_mcu2_controls_native(&s,28,50));
    assert(!omni_mcu2_controls_native(&s,57,50));
    assert(omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,6,0xd3,3,2,8},6,25));
    /* Boom sidetone cannot accidentally initialize the separate legacy slot. */
    assert(!omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,7,0xd4,1,3,1,5},7,26));
    assert(!omni_mcu2_controls_snapshot(&s,payload));
    assert(omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,5,0xd4,3,5},5,27));
    assert(omni_mcu2_controls_snapshot(&s,payload));
    assert(!memcmp(payload,(uint8_t[]){50,50,1,80,0},5));
    uint32_t generation=s.generation;
    assert(omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,5,0xd4,3,5},5,28));
    assert(s.generation==generation);
    uint8_t bulk[46]={0xdb,46,0x20,1,56};bulk[14]=0;bulk[15]=10;
    assert(omni_mcu2_controls_observe(&s,bulk,sizeof(bulk),30));
    assert(omni_mcu2_controls_snapshot(&s,payload));
    assert(!memcmp(payload,(uint8_t[]){100,50,0,100,0},5));
    bulk[15]=0;generation=s.generation;
    assert(!omni_mcu2_controls_observe(&s,bulk,sizeof(bulk),31));
    assert(s.generation==generation); /* Malformed bulk changes no partial state. */
    bulk[1]=45;assert(!omni_mcu2_controls_observe(&s,bulk,sizeof(bulk),32));
    assert(omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,5,0xe4,3,1},5,33));
    assert(s.known==(OMNI_PEER_HAVE_VOLUME|OMNI_PEER_HAVE_BALANCE));
    assert(!omni_mcu2_controls_snapshot(&s,payload));
    assert(omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,5,0xe4,3,3},5,34));
    assert(!omni_mcu2_controls_snapshot(&s,payload)); /* Reconnect needs fresh settings. */
    const uint8_t steps[]={38,39,40,41,42,41,40,39,38,37};
    for(unsigned i=0;i<sizeof(steps);++i) {
        uint8_t frame[]={0xdb,6,0xd2,3,0,steps[i]};
        assert(omni_mcu2_controls_observe(&s,frame,sizeof(frame),40U+i));
        generation=s.generation;
        assert(s.loudness_step==steps[i] && s.volume_percent==(unsigned)steps[i]*100U/56U);
        frame[4]=1;frame[5]=10; /* Other source's distinct level cannot overwrite tag0. */
        assert(omni_mcu2_controls_observe(&s,frame,sizeof(frame),40U+i));
        frame[4]=2;frame[5]=20;
        assert(omni_mcu2_controls_observe(&s,frame,sizeof(frame),40U+i));
        assert(s.generation==generation && s.loudness_step==steps[i]);
    }
    generation=s.generation;
    assert(!omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,6,0xd2,3,0,57},6,60));
    assert(!omni_mcu2_controls_observe(&s,(uint8_t[]){0xdb,6,0xd2,9,3,0},6,60));
    assert(s.generation==generation);
    puts("MCU2 passive microphone/control snapshot contracts passed");return 0;
}
