#include "eq_menu.h"
#include "dsp_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static omni_settings_menu_io io;
static uint32_t now=1;
static uint8_t wire[140];static unsigned used;
void omni_settings_menu_bind(omni_settings_menu_io callbacks) {io=callbacks;}
uint32_t omni_ui_milliseconds(void) {return now;}
void omni_ui_settings_status(uint32_t out[15]) {memset(out,0,60);}
bool omni_ui_settings_set(unsigned a,unsigned b,unsigned c,unsigned d) {(void)a;(void)b;(void)c;(void)d;return false;}
bool omni_mcu2_runtime_read(unsigned p,uint32_t out[15]) {(void)p;(void)out;return false;}
bool omni_mcu2_select_input(uint8_t p) {(void)p;return false;}
bool omni_headset_query_busy(void) {return false;}
bool omni_headset_query_request(uint32_t t,unsigned p,uint32_t n) {(void)t;(void)p;(void)n;return false;}
static int tx(void *ctx,uint8_t b) {(void)ctx;assert(used<sizeof(wire));wire[used++]=b;return 1;}
static int drain(void *ctx) {(void)ctx;return 1;}
static void complete(bool accept)
{
    used=0;uint32_t w[15];
    for(unsigned i=0;i<12;++i) omni_dsp_settings_poll(++now,true,(omni_dsp_settings_io){0,tx,drain});
    omni_dsp_settings_status(0,w);assert(w[4]==DSP_SETTINGS_WAIT);
    const uint8_t ack[]={0xdd,3,wire[2],accept?0u:1u};
    for(unsigned i=0;i<4;++i) omni_dsp_settings_observe(ack[i],now);
    omni_dsp_settings_poll(++now,false,(omni_dsp_settings_io){0,tx,drain});
    assert(!omni_dsp_settings_busy());
}
int main(void)
{
    omni_settings_menu_bind_native();unsigned value;
    for(unsigned channel=0;channel<3;++channel) {
        unsigned id=12u+channel,base=128u+64u*channel;
        uint8_t saved[128],readback[128];
        assert(!io.write(base+OMNI_EQ_BEGIN,0)); /* Unknown curve is not replaced. */
        assert(!io.write(id,channel==1u?9u:4u));
        assert(io.write(id,1));complete(true);
        assert(io.write(base+OMNI_EQ_BEGIN,0)); /* Clone active preset into draft. */
        assert(!omni_dsp_settings_busy());
        for(unsigned band=0;band<10;++band) {
            assert(io.write(base+band,band?240u:0u));
            assert(io.read(base+band,&value) && value==(band?240u:0u));
            assert(io.write(base+band,247));assert(!io.write(base+band,248));assert(io.write(base+band,band?240u:0u));
        }
        if(!channel) {
            assert(io.write(base+10,20001));assert(io.read(base+10,&value) && value==20001);
            assert(!io.write(base+10,20002));assert(!io.write(base+10,19));
            assert(io.write(base+20,2500));assert(!io.write(base+20,199));
            assert(io.write(base+30,5));assert(!io.write(base+30,7));
        } else assert(!io.write(base+10,1000));
        assert(io.write(base+OMNI_EQ_APPLY,0));
        assert(!io.write(base+OMNI_EQ_APPLY,0));
        complete(true);
        size_t n=omni_dsp_settings_custom(id,saved);assert(n==(channel?78u:128u));
        assert(wire[2]==0x1bu+2u*channel && used==n+4u);
        assert(!memcmp(wire+4,saved,n));
        assert(saved[0]==(channel==1u?8u:4u));
        assert(saved[channel?68u:71u]==136u); /* -12 dB signed int8. */
        assert(saved[channel?69u:77u]==120u);
        assert(io.write(id,0));complete(true); /* Select flat without losing custom. */
        assert(io.write(id,channel==1u?9u:4u));complete(true);
        assert(!memcmp(wire+4,saved,n));
        assert(io.write(base,120));assert(io.write(base+OMNI_EQ_APPLY,0));complete(false);
        assert(omni_dsp_settings_custom(id,readback)==n && !memcmp(saved,readback,n));
        assert(io.write(base+OMNI_EQ_FLAT,0));assert(io.read(base,&value) && value==120);
    }
    {   /* Live apply auto-submits a draft; discard restores the pre-edit profile. */
        unsigned id=12,base=128;
        assert(io.write(base+OMNI_EQ_DISCARD,0));       /* drop any leftover draft */
        omni_settings_menu_live_poll(now);
        if(omni_dsp_settings_busy()) complete(true);
        assert(io.write(id,1));complete(true);          /* active = BASS preset */
        assert(io.write(base+OMNI_EQ_BEGIN,0));         /* fresh draft; capture baseline */
        assert(io.write(base,200));                     /* raise band0 -> pending */
        omni_settings_menu_live_poll(now);              /* live submit */
        assert(omni_dsp_settings_busy());complete(true);
        assert(wire[2]==0x1bu && wire[4+71]==80u);      /* band0 +8.0 dB signed int8 */
        assert(io.write(base+OMNI_EQ_DISCARD,0));       /* revert to baseline */
        omni_settings_menu_live_poll(now);
        assert(omni_dsp_settings_busy());complete(true);
        uint8_t bass[128];size_t bn=omni_dsp_settings_eq_preset(12,1,bass);
        assert(used==bn+4u && !memcmp(wire+4,bass,bn)); /* baseline resent */
    }
    assert(!io.read(320,&value));assert(!io.write(320,0));
    puts("EQ drafts, signed gains, bounds, full frames, NACK and custom retention pass");return 0;
}
