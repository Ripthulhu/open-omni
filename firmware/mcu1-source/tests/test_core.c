#include "omni_core.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    omni_volume v;
    uint8_t out[16], payload[2];
    /* Synthetic USB prototype range, not yet a measured DSP gain table. */
    assert(omni_volume_init(&v,-96*256,0,256,-60*256));
    assert(v.muted);
    assert(!omni_volume_set(&v,1,true));
    assert(!omni_volume_set(&v,-1,true));
    assert(omni_volume_set(&v,-30*256,true));
    uint32_t rev=v.revision;
    assert(omni_volume_notification(&v,out)==6);
    assert(!memcmp(out,(uint8_t[]){0,1,0,2,0,5},6));
    assert(omni_volume_set(&v,-20*256,true));
    omni_volume_notification_complete(&v,2,rev);
    assert(v.pending==1);
    omni_volume_notification_complete(&v,2,v.revision);
    assert(v.pending==0);
    for (int db=-96*256;db<=0;db+=256) {
        payload[0]=(uint8_t)db; payload[1]=(uint8_t)((uint16_t)db>>8);
        assert(omni_uac2_control(&v,(omni_setup){0x21,1,0x200,0x500,2},payload,2,out,16)==0);
        assert(omni_uac2_control(&v,(omni_setup){0xa1,1,0x200,0x500,2},0,0,out,16)==2);
        assert(!memcmp(payload,out,2));
        assert(v.pending==0);
    }
    omni_mute_set(&v,false,true); assert(v.current==0);
    assert(omni_volume_notification(&v,out)==6 && out[3]==1);
    assert(omni_uac2_control(&v,(omni_setup){0xa1,2,0x200,0x500,8},0,0,out,16)==8);
    assert(!memcmp(out,(uint8_t[]){1,0,0,0xa0,0,0,0,1},8));
    assert(omni_uac2_control(&v,(omni_setup){0xa1,2,0x100,0xa00,14},0,0,out,16)==14);
    assert(!memcmp(out,(uint8_t[]){1,0,0x80,0xbb,0,0,0x80,0xbb,0,0,0,0,0,0},14));
    assert(omni_uac2_control(&v,(omni_setup){0xa1,2,0x100,0xa00,2},0,0,out,16)==2);
    assert(omni_uac2_control(&v,(omni_setup){0xa1,2,0x100,0xa00,14},0,0,out,1)==-1);
    assert(omni_uac2_control(&v,(omni_setup){0xa1,1,0x201,0x500,2},0,0,out,16)==-1);
    assert(omni_uac2_control(&v,(omni_setup){0x21,1,0x200,0x500,2},payload,1,out,16)==-1);
    payload[0]=2;
    assert(omni_uac2_control(&v,(omni_setup){0x21,1,0x100,0x500,1},payload,1,out,16)==-1);
    assert(!omni_volume_init(&v,-100,0,30,0));

    /* Staged 0..56 attenuation index: 0=loud (max dB), 56=silent (min dB),
     * monotonic non-decreasing as level drops; cache mirrors the pure fn. */
    omni_volume a;
    assert(omni_volume_init(&a,-96*256,0,256,-60*256));
    assert(a.attenuation==omni_volume_attenuation(&a));
    assert(omni_volume_attenuation(&a)==35u);              /* 60/96*56 == 35 */
    assert(omni_volume_set(&a,0,true) && a.attenuation==0u);         /* loudest */
    assert(omni_volume_set(&a,-96*256,true) && a.attenuation==56u);  /* silent  */
    uint8_t prev=0u;
    for (int db=0; db>=-96*256; db-=256) {
        assert(omni_volume_set(&a,(int16_t)db,true));
        assert(a.attenuation==omni_volume_attenuation(&a));
        assert(a.attenuation>=prev);
        prev=a.attenuation;
    }
    assert(prev==56u);
    assert(omni_volume_attenuation((const omni_volume*)0)==0u);      /* NULL -> 0 */
    omni_volume d;                          /* degenerate range: no div-by-zero */
    assert(omni_volume_init(&d,0,0,1,0));
    assert(omni_volume_attenuation(&d)==0u && d.attenuation==0u);
    /* Regression: dial minimum must notify Windows mute, and increasing from
     * a host-muted zero must notify both the new gain and unmute. */
    omni_volume dial;
    assert(omni_volume_init(&dial,-49*256,0,256,-48*256));
    omni_mute_set(&dial,false,false);
    omni_volume_dial(&dial,-1);
    assert(dial.current==-49*256 && dial.muted && dial.pending==3);
    assert(omni_volume_notification(&dial,out)==6 && out[3]==2);
    omni_volume_notification_complete(&dial,2,dial.revision);
    assert(omni_volume_notification(&dial,out)==6 && out[3]==1);
    omni_volume_notification_complete(&dial,1,dial.revision);
    assert(!dial.pending);
    omni_volume_dial(&dial,1);
    assert(dial.current==-48*256 && !dial.muted && dial.pending==3);
    omni_mute_set(&dial,true,false);
    omni_volume_dial(&dial,0); assert(dial.muted);
    omni_volume_dial(&dial,-1); assert(dial.muted);
    omni_volume_dial(&dial,1000); assert(dial.current==0 && !dial.muted);
    omni_mute_set(&dial,true,false);
    omni_volume_dial(&dial,1); assert(dial.current==0 && !dial.muted);
    /* Independent audio functions: programmable playback, fixed48k mic.
     * Reject malformed writes without mutating the selected playback clock. */
    omni_uac2_clocks clocks={48000u,true};
    uint8_t reply[26], rate96[]={0,0x77,1,0}, rate48[]={0x80,0xbb,0,0};
    omni_setup get_range={0xa1,2,0x100,0xa00,26};
    assert(omni_uac2_control_format(&v,&clocks,get_range,0,0,reply,sizeof(reply))==26);
    assert(!memcmp(reply,(uint8_t[]){2,0,0x80,0xbb,0,0,0x80,0xbb,0,0,0,0,0,0,
                                  0,0x77,1,0,0,0x77,1,0,0,0,0,0},26));
    omni_setup set_rate={0x21,1,0x100,0xa00,4};
    assert(omni_uac2_control_format(&v,&clocks,set_rate,rate96,4,0,0)==0);
    assert(clocks.playback_rate==96000u);
    assert(omni_uac2_control_format(&v,&clocks,(omni_setup){0xa1,1,0x100,0xa00,4},0,0,reply,26)==4);
    assert(!memcmp(reply,rate96,4));
    assert(omni_uac2_control_format(&v,&clocks,(omni_setup){0xa1,2,0x100,0xb02,26},0,0,reply,26)==14);
    assert(!memcmp(reply,(uint8_t[]){1,0,0x80,0xbb,0,0,0x80,0xbb,0,0,0,0,0,0},14));
    assert(omni_uac2_control_format(&v,&clocks,(omni_setup){0xa1,1,0x100,0xb02,4},0,0,reply,26)==4);
    assert(!memcmp(reply,rate48,4));
    const omni_setup bad_writes[]={
        {0x21,1,0x100,0xb02,4}, {0x21,1,0x100,0xa02,4},
        {0x21,1,0x101,0xa00,4}, {0x21,1,0x200,0xa00,4},
        {0x21,2,0x100,0xa00,4}, {0x21,1,0x100,0xa00,3},
        {0x21,1,0x200,0x502,2}};
    for(unsigned i=0;i<sizeof(bad_writes)/sizeof(bad_writes[0]);++i) {
        assert(omni_uac2_control_format(&v,&clocks,bad_writes[i],rate48,4,0,0)==-1);
        assert(clocks.playback_rate==96000u);
    }
    assert(omni_uac2_control_format(&v,&clocks,set_rate,(uint8_t[]){0x44,0xac,0,0},4,0,0)==-1);
    assert(omni_uac2_control_format(&v,&clocks,set_rate,rate48,3,0,0)==-1);
    assert(omni_uac2_control_format(&v,&clocks,set_rate,0,4,0,0)==-1);
    assert(clocks.playback_rate==96000u);
    assert(omni_uac2_control_format(&v,&clocks,set_rate,rate48,4,0,0)==0);
    assert(clocks.playback_rate==48000u);
    clocks.playback_valid=false;
    assert(omni_uac2_control_format(&v,&clocks,(omni_setup){0xa1,1,0x200,0xa00,1},0,0,reply,26)==1 && reply[0]==0);
    assert(omni_uac2_control_format(&v,&clocks,(omni_setup){0xa1,1,0x200,0xb02,1},0,0,reply,26)==1 && reply[0]==1);
    assert(omni_uac2_control_format(&v,&clocks,get_range,0,0,reply,25)==-1);
    get_range.length=2;
    assert(omni_uac2_control_format(&v,&clocks,get_range,0,0,reply,2)==2 && reply[0]==2);
    return 0;
}
