#include "audio_probe.h"
#include "usb_names.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    const uint8_t *b=omni_audio_config;
    assert(sizeof(omni_audio_config)==355u && b[2]+256u*b[3]==355u && b[4]==5u);
    const char *names[]={"Omni","Omni USB1","omni-a-test","USB1 Audio",
                         "USB1 Playback","USB1 Microphone","USB1 Control"};
    assert(!omni_usb_string(0,"omni-a-test"));
    for(unsigned i=1;i<256u;++i) {
        const char *name=omni_usb_string((uint8_t)i,"omni-a-test");
        if(i<=7u) assert(name && !strcmp(name,names[i-1u]) && strlen(name)<=62u);
        else assert(!name);
    }
    assert(!omni_usb_string(OMNI_USB_STRING_SERIAL,NULL));
    unsigned iface=99,alt=0,expected=0,ep_count=0,seen=0,alternates=0;
    unsigned ac_bytes[2]={0},clocks=0,iad=0,hid=0,formats=0,last_ep=0;
    for(unsigned pos=9;pos<sizeof(omni_audio_config);) {
        const uint8_t *d=b+pos;unsigned n=d[0];assert(n>=2 && pos+n<=sizeof(omni_audio_config));
        if(d[1]==11u) {
            assert(n==8 && d[2]==(iad?2u:0u) && d[3]==2 && d[4]==1 && d[5]==0 && d[6]==0x20);
            assert(d[7]==(iad?OMNI_USB_STRING_MICROPHONE:OMNI_USB_STRING_PLAYBACK));++iad;
        } else if(d[1]==4u) {
            if(iface!=99u) assert(ep_count==expected);
            iface=d[2];alt=d[3];expected=d[4];ep_count=0;
            assert(iface<5 && alt<3 && !(alternates&(1u<<(iface*3u+alt))));
            alternates|=1u<<(iface*3u+alt);
            if(alt) assert(seen&(1u<<iface));else seen|=1u<<iface;
            assert(d[8]==(iface<2?OMNI_USB_STRING_PLAYBACK:iface<4?OMNI_USB_STRING_MICROPHONE:OMNI_USB_STRING_CONTROL));
            if(iface==4) assert(alt==0 && d[5]==3 && d[6]==0 && d[7]==0);
            else assert(d[5]==1 && d[6]==((iface==0 || iface==2)?1u:2u) && d[7]==0x20);
            if(iface==0 || iface==2 || iface==4) assert(!alt);
            if(iface==3) assert(alt<=1);
        } else if(d[1]==5u) {
            assert(n==7);++ep_count;last_ep=d[2];unsigned size=d[4]+256u*d[5];
            if(iface==0) assert(!alt && d[2]==0x82 && d[3]==3 && size==6 && d[6]==4);
            else if(iface==1) {
                assert(alt==1 || alt==2);
                if(d[2]==3) assert(d[3]==5 && size==(alt==1?388u:582u) && d[6]==1);
                else assert(d[2]==0x84 && d[3]==0x11 && size==4 && d[6]==1);
            } else if(iface==3) assert(alt==1 && d[2]==0x83 && d[3]==0x05 && size==98 && d[6]==1);
            else {assert(iface==4 && d[2]==0x81 && d[3]==3 && size==64 && d[6]==10);}
        } else if(d[1]==0x24 && (iface==0 || iface==2)) {
            unsigned mic=iface==2;ac_bytes[mic]+=n;
            if(d[2]==1) assert(n==9 && d[3]==0 && d[4]==2 && d[6]==(mic?46:64) && !d[7]);
            else if(d[2]==10) {
                assert(n==8 && d[3]==(mic?11:10) && d[4]==(mic?1:3) && d[5]==(mic?5:7));++clocks;
            } else if(d[2]==2) {
                assert(n==17 && d[3]==(mic?3:1) && d[7]==(mic?11:10) && d[8]==(mic?1:2));
                assert(d[16]==(mic?OMNI_USB_STRING_MICROPHONE:OMNI_USB_STRING_PLAYBACK));
            } else if(d[2]==3) {
                assert(n==12 && d[3]==(mic?4:2) && d[7]==(mic?3:5) && d[8]==(mic?11:10));
            } else {assert(!mic && d[2]==6 && n==18 && d[3]==5 && d[4]==1 && d[5]==15);}
        } else if(d[1]==0x24) {
            assert((iface==1 || iface==3) && alt);
            if(d[2]==1) assert(n==16 && d[3]==(iface==1?1:4) && d[5]==1 && d[6]==1 && d[10]==(iface==1?2:1));
            else {assert(d[2]==2 && n==6 && d[3]==1);
                assert(d[4]==(iface==1 && alt==2?3:2) && d[5]==(iface==1 && alt==2?24:16));++formats;}
        } else if(d[1]==0x25) {
            assert(n==8 && ((iface==1 && last_ep==3)||(iface==3 && last_ep==0x83)));
        } else {assert(d[1]==0x21 && n==9 && iface==4 && pos==OMNI_HID_DESCRIPTOR_OFFSET && d[7]==27);++hid;}
        pos+=n;
    }
    assert(ep_count==expected && seen==31 && ac_bytes[0]==64 && ac_bytes[1]==46);
    assert(iad==2 && clocks==2 && hid==1 && formats==3);
    assert(alternates==((1u<<0)|(7u<<3)|(1u<<6)|(3u<<9)|(1u<<12)));
    for(unsigned rate=48000;rate<=96000;rate+=48000) for(unsigned bits=16;bits<=24;bits+=8) {
        omni_audio_format f;assert(omni_audio_format_make(&f,rate,bits,123) && omni_audio_format_valid(&f));
        assert(f.frame_bytes==bits/4 && f.max_packet==(rate/1000u+1u)*bits/4u);
        f.frame_bytes++;assert(!omni_audio_format_valid(&f));
    }
    return 0;
}
