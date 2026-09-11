#include "home_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct guarded {uint8_t pre[16], frame[1024], post[16];};
static void guard(const struct guarded *g)
{
    for(unsigned i=0;i<16;i++) {assert(g->pre[i]==0xa5);assert(g->post[i]==0xa5);}
}
static void save(const char *directory,const char *name,const uint8_t f[1024])
{
    if(!directory) return;
    char path[512];int n=snprintf(path,sizeof(path),"%s/%s.pbm",directory,name);
    assert(n>0 && (size_t)n<sizeof(path));
    FILE *fp=fopen(path,"wb");assert(fp);fputs("P4\n128 64\n",fp);
    for(unsigned y=0;y<64;y++) for(unsigned x=0;x<128;x+=8) {
        unsigned byte=0;
        for(unsigned bit=0;bit<8;bit++)
            if(f[(y/8)*128+x+bit]&(1u<<(y%8))) byte|=128u>>bit;
        assert(fputc((int)byte,fp)!=EOF);
    }
    assert(fclose(fp)==0);
}
static bool pixel(const uint8_t *f,unsigned x,unsigned y)
{return (f[(y/8)*128+x]&(1u<<(y%8)))!=0;}
int main(int argc,char **argv)
{
    const char *out=argc>1?argv[1]:NULL;
    struct guarded g;memset(&g,0xa5,sizeof(g));
    omni_home_view v={.source="USB1",.link_known=true,.connected=true,
        .battery_known=true,.battery_percent=83,.spare_state=2,.spare_millivolts=4040,.percent=73,.db_x256=-9*256,
        .sample_rate=48000,.sample_bits=16,.gain_known=true,.gain_ready=true,
        .input={{"INPUT1",true,68},{"INPUT2",false,0},{"ANALOG",false,0},{"OUTPUT",true,23}},
        .stereo_known=true,.left=68,.right=62};
    omni_home_ui_render(g.frame,&v);guard(&g);save(out,"home-inputs",g.frame);
    assert(pixel(g.frame,127,9) && pixel(g.frame,127,54)); /* Full-width separators. */
    uint8_t baseline[1024];memcpy(baseline,g.frame,sizeof(baseline));
    uint8_t bt_frames[6][128];
    for(unsigned i=0;i<6u;++i) {
        v.bluetooth_known=i!=0u;v.bluetooth_state=(uint8_t)(0x2fu+i);
        omni_home_ui_render(g.frame,&v);guard(&g);
        memcpy(bt_frames[i],g.frame,128);
        if(i<3u) assert(!memcmp(bt_frames[i],bt_frames[0],128));
        if(i==3u || i==4u) assert(memcmp(bt_frames[i],bt_frames[i-1u],128));
        if(i==5u) assert(!memcmp(bt_frames[i],bt_frames[4],128));
        assert(!memcmp(baseline+128,g.frame+128,896));
    }
    v.bluetooth_known=false;omni_home_ui_render(g.frame,&v);
    v.input[1].level=100;v.input[2].level=255;
    omni_home_ui_render(g.frame,&v);assert(memcmp(baseline,g.frame,1024)==0);
    /* Unknown data cannot create apparent meter activity. */
    v.stereo_view=true;omni_home_ui_render(g.frame,&v);guard(&g);save(out,"home-stereo",g.frame);
    v.spare_state=3;omni_home_ui_render(g.frame,&v);save(out,"home-spare-full",g.frame);
    v.spare_state=4;omni_home_ui_render(g.frame,&v);save(out,"home-spare-fault",g.frame);
    v.spare_state=2;
    v.spare_millivolts=4033;omni_home_ui_render(g.frame,&v);memcpy(baseline,g.frame,1024);
    v.spare_millivolts=4040;omni_home_ui_render(g.frame,&v);
    assert(!memcmp(baseline,g.frame,1024)); /* Live charge voltage does not change CHG. */
    v.spare_millivolts=4042;omni_home_ui_render(g.frame,&v);
    assert(!memcmp(baseline,g.frame,128)); /* No percentage inferred from terminal voltage. */
    assert(!memcmp(baseline+128,g.frame+128,896)); /* Header only. */
    v.spare_millivolts=4150;omni_home_ui_render(g.frame,&v);memcpy(baseline,g.frame,1024);
    v.spare_millivolts=4200;omni_home_ui_render(g.frame,&v);
    assert(!memcmp(baseline,g.frame,1024)); /* Charging status stays independent of voltage. */
    v.spare_state=3;omni_home_ui_render(g.frame,&v);
    assert(memcmp(baseline,g.frame,128)); /* Full comes only from charger termination. */
    v.spare_state=2;v.spare_millivolts=4040;
    v.bias_mode=true;v.bias_known=true;v.bias=18;v.secondary_source=2;
    v.bias_q14[0]=4617;v.bias_q14[1]=16384;
    omni_home_ui_render(g.frame,&v);save(out,"home-bias",g.frame);
    memcpy(baseline,g.frame,1024);
    v.bias=19;omni_home_ui_render(g.frame,&v);
    assert(!memcmp(baseline,g.frame,1024)); /* Detents are not displayed as a gain percentage. */
    v.bias=12;v.bias_q14[0]=16384;
    omni_home_ui_render(g.frame,&v);save(out,"home-bias-center",g.frame);
    v.secondary_source=3;v.bias=0;v.bias_q14[1]=0;
    omni_home_ui_render(g.frame,&v);save(out,"home-bias-usb3",g.frame);
    v.secondary_source=0;v.bias_known=false;
    omni_home_ui_render(g.frame,&v);save(out,"home-bias-unknown",g.frame);
    memcpy(baseline,g.frame,1024);v.bias_q14[0]=255;v.bias_q14[1]=65535;
    omni_home_ui_render(g.frame,&v);assert(!memcmp(baseline,g.frame,1024));
    v.muted=true;v.percent=0;v.db_x256=-55*256;
    omni_home_ui_render(g.frame,&v);save(out,"home-muted",g.frame);
    assert(pixel(g.frame,127,56));
    v.gain_fault=true;omni_home_ui_render(g.frame,&v);save(out,"home-gain-fault",g.frame);
    assert(pixel(g.frame,92,56)); /* Fault is distinct from pending SYNC/user MUTE. */
    memset(&v,0,sizeof(v));memcpy(v.source,"--",3);v.stereo_view=true;
    omni_home_ui_render(g.frame,&v);save(out,"home-unknown",g.frame);
    memcpy(baseline,g.frame,1024);v.left=99;v.right=255;v.battery_percent=255;
    omni_home_ui_render(g.frame,&v);assert(memcmp(baseline,g.frame,1024)==0);
    v.stereo_known=true;v.battery_known=true;v.percent=255;v.battery_percent=255;
    v.left=255;v.right=255;v.sample_rate=192000;v.sample_bits=24;
    omni_home_ui_render(g.frame,&v);memcpy(baseline,g.frame,1024);
    v.percent=100;v.battery_percent=100;v.left=100;v.right=100;
    omni_home_ui_render(g.frame,&v);assert(memcmp(baseline,g.frame,1024)==0);guard(&g);
    omni_menu_view m={.input={{100,true,true,false},{0,false,false,false},
        {0,false,false,false},{58,true,false,false}},.selected=3};
    omni_menu_ui_render(g.frame,&m);guard(&g);save(out,"menu-inputs",g.frame);
    assert(pixel(g.frame,127,39)); /* Second available selected row spans full panel. */
    memcpy(baseline,g.frame,1024);m.input[1].level=255;m.input[2].muted=true;
    omni_menu_ui_render(g.frame,&m);assert(!memcmp(baseline,g.frame,1024)); /* Hidden routes add no rows. */
    m.detail=true;m.field=0;m.editing=true;
    omni_menu_ui_render(g.frame,&m);save(out,"menu-level",g.frame);
    m.field=1;m.editing=false;
    omni_menu_ui_render(g.frame,&m);save(out,"menu-link",g.frame);
    m.field=2;m.input[3].muted=true;
    omni_menu_ui_render(g.frame,&m);save(out,"menu-mute",g.frame);
    m.selected=0;omni_menu_ui_render(g.frame,&m);memcpy(baseline,g.frame,1024);
    m.selected=1;omni_menu_ui_render(g.frame,&m);
    assert(memcmp(baseline,g.frame,1024)==0); /* Invalid/hidden selection falls back to a working route. */
    omni_menu_view expanded=m;expanded.detail=false;expanded.selected=3;
    for(unsigned i=0;i<4u;++i) expanded.input[i].available=true;
    omni_menu_ui_render(g.frame,&expanded);guard(&g);
    assert(pixel(g.frame,127,48) && !pixel(g.frame,127,63)); /* Future four-route list remains bounded. */
    for(unsigned i=0;i<256;i++) {
        m.selected=(uint8_t)i;m.field=(uint8_t)i;m.detail=(i&1u)!=0;
        omni_menu_ui_render(g.frame,&m);guard(&g);
        v=(omni_home_view){.source={'A','A','A','A','A','A'},.percent=(uint8_t)i,
            .db_x256=(int16_t)(i*256u),.battery_percent=(uint8_t)i,.battery_known=true};
        for(unsigned k=0;k<4;k++) memcpy(v.input[k].label,"ABCDE",5);
        omni_home_ui_render(g.frame,&v);guard(&g);
    }
    omni_home_ui_render(NULL,NULL);omni_menu_ui_render(NULL,NULL);
    omni_home_ui_render(g.frame,NULL);for(unsigned i=0;i<1024;i++) assert(g.frame[i]==0);
    puts("home/menu render tests passed");return 0;
}
