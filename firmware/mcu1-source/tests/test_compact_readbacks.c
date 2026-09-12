#include "headset_query.h"
#include "dsp_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A5 regression: the two compact direct readbacks - microphone noise
 * (BD 04 DB 02 -> DB 07 DB 03 en lvl xx) and the selected-field E3 tuple
 * (BD 05 E3 02 01 -> DB 09 E3 03 01 startup led call meta). Drives the real
 * headset_query whitelist producer and the real dsp_settings observe_db
 * consumer over src/interchip.c framing; nothing is stubbed. Fails on current
 * source (selectors 12/13 rejected; observe_db has no 0xdb/0xe3 read branch). */

/* ---- headset_query producer harness (same shape as test_headset_query.c) ---- */
static struct { uint8_t bytes[32];unsigned used,budget;int drained; } bus;
static uint32_t qtoken=500u;
static int qtx(void *ctx,uint8_t b)
{ (void)ctx;if(!bus.budget) return 0;--bus.budget;assert(bus.used<sizeof(bus.bytes));bus.bytes[bus.used++]=b;return 1; }
static int qdrain(void *ctx) { (void)ctx;return bus.drained; }
static void qpoll(uint32_t now,bool allowed,unsigned budget)
{ bus.budget=budget;omni_headset_query_poll(now,allowed,(omni_headset_query_io){0,qtx,qdrain}); }
static void qfeed(const uint8_t *p,unsigned n,uint32_t now)
{ for(unsigned i=0;i<n;++i) omni_headset_query_observe(p[i],now); }
static uint32_t qphase(void)
{ uint32_t w[15];assert(omni_headset_query_status(0,0,w));return w[4]; }
static void qbegin(unsigned selected,uint32_t now)
{
    omni_headset_query_release(now);omni_headset_query_acquire();++qtoken;
    assert(omni_headset_query_request(qtoken,selected,now));
    memset(&bus,0,sizeof(bus));bus.drained=1;
}
static void query_roundtrip(unsigned selected,const uint8_t *request,unsigned rlen,
                            const uint8_t *reply,unsigned plen)
{
    uint8_t raw[60];
    qbegin(selected,0);
    for(uint32_t now=1;now<=6;++now) qpoll(now,now==1,5); /* start+send+drain at now==1 */
    assert(bus.used==rlen && !memcmp(bus.bytes,request,rlen));
    assert(qphase()==HEADSET_QUERY_WAIT); /* the local DD ACK alone never completes */
    qfeed(reply,plen,7);qpoll(7,false,0);
    assert(qphase()==HEADSET_QUERY_DONE);
    /* Raw evidence retained verbatim - metadata tail included, not truncated. */
    assert(omni_headset_query_reply(qtoken,raw) && raw[8]==plen);
    assert(!memcmp(raw+12,reply,plen));
    for(unsigned i=12u+plen;i<60u;++i) assert(!raw[i]);
}
static void query_whitelist(void)
{
    const uint8_t noise_req[]={0xbd,4,0xdb,2};
    const uint8_t noise_reply[]={0xdb,7,0xdb,3,1,3,0};
    query_roundtrip(12,noise_req,4,noise_reply,7);
    const uint8_t e3_req[]={0xbd,5,0xe3,2,1};
    const uint8_t e3_reply[]={0xdb,9,0xe3,3,1,1,7,2,0xaa}; /* meta byte 0xaa */
    query_roundtrip(13,e3_req,5,e3_reply,9);
    assert(!omni_headset_query_request(1u,14u,0)); /* boundary still closed */
}

/* ---- dsp_settings passive consumer harness ---- */
static void dfeed(const uint8_t *p,unsigned n,uint32_t now)
{ for(unsigned i=0;i<n;++i) omni_dsp_settings_observe(p[i],now); }
static uint32_t dcache(unsigned control,uint8_t out[60])
{ uint32_t flags;assert(omni_dsp_settings_value(control,0,out));memcpy(&flags,out+16,4);return flags; }
static void observe_mic_noise(void)
{
    uint8_t out[60];
    omni_dsp_settings_release(0);omni_dsp_settings_acquire();
    assert(dcache(DSP_SETTING_MIC_NOISE,out)==0u);
    const uint8_t good[]={0xdb,7,0xdb,3,1,3,0};dfeed(good,7,1);
    assert(dcache(DSP_SETTING_MIC_NOISE,out)==5u); /* valid|DB-observed, never DD */
    assert(out[24]==1u && out[25]==3u);            /* {enabled,level} like bulk20 */
    const uint8_t tail[]={0xdb,7,0xdb,3,0,2,0x7f};dfeed(tail,7,2); /* nonzero tail decodes */
    assert(dcache(DSP_SETTING_MIC_NOISE,out)==5u && out[24]==0u && out[25]==2u);
    const uint8_t lvl0[]={0xdb,7,0xdb,3,1,0,0};dfeed(lvl0,7,3);   /* level 0 */
    const uint8_t lvl4[]={0xdb,7,0xdb,3,1,4,0};dfeed(lvl4,7,3);   /* level 4 */
    const uint8_t echo[]={0xdb,7,0xdb,1,1,3,0};dfeed(echo,7,3);   /* p[3]!=3 SET echo */
    const uint8_t shortf[]={0xdb,6,0xdb,3,1,3};dfeed(shortf,6,3); /* wrong length */
    assert(dcache(DSP_SETTING_MIC_NOISE,out)==5u && out[24]==0u && out[25]==2u);
}
static void observe_e3(void)
{
    uint8_t out[60];
    omni_dsp_settings_release(0);omni_dsp_settings_acquire();
    for(unsigned id=DSP_SETTING_BT_STARTUP;id<=DSP_SETTING_BT_CALL;++id)
        assert(dcache(id,out)==0u);
    const uint8_t good[]={0xdb,9,0xe3,3,1,1,7,2,0xaa};dfeed(good,9,1);
    const uint8_t tuple[3]={1,7,2};
    for(unsigned id=DSP_SETTING_BT_STARTUP;id<=DSP_SETTING_BT_CALL;++id) {
        assert(dcache(id,out)==5u);assert(!memcmp(out+24,tuple,3));
    }
    /* Malformed / non-selected frames must neither replace the tuple nor
     * zero-fill siblings. othersel/setecho carry all-zero tuples on purpose. */
    const uint8_t bad_startup[]={0xdb,9,0xe3,3,1,2,7,2,0};dfeed(bad_startup,9,2);
    const uint8_t bad_led[]={0xdb,9,0xe3,3,1,1,11,2,0};dfeed(bad_led,9,2);
    const uint8_t bad_call[]={0xdb,9,0xe3,3,1,1,7,3,0};dfeed(bad_call,9,2);
    const uint8_t othersel[]={0xdb,9,0xe3,3,2,0,0,0,0};dfeed(othersel,9,2);
    const uint8_t setecho[]={0xdb,9,0xe3,1,1,0,0,0,0};dfeed(setecho,9,2);
    const uint8_t shortf[]={0xdb,8,0xe3,3,1,1,7,2};dfeed(shortf,8,2);
    for(unsigned id=DSP_SETTING_BT_STARTUP;id<=DSP_SETTING_BT_CALL;++id) {
        assert(dcache(id,out)==5u);assert(!memcmp(out+24,tuple,3));
    }
}
int main(void)
{
    query_whitelist();
    observe_mic_noise();
    observe_e3();
    puts("A5 compact readbacks: mic-noise + selected-field E3 whitelist and cache decoders passed");
    return 0;
}
