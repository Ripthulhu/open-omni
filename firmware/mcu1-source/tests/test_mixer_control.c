#include "mixer_control.h"
#include "mixer_ui.h"
#include "dsp_settings.h"
#include "native_gain_adapter.h"
#include "mcu2_probe.h"
#include "usb_audio_ring.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static omni_source_mix mix;
static unsigned ui_calls,pcm_calls,pcm_target,aa_calls,aa_transmissions,aa_target;
static unsigned request_calls,accepted_requests,last_mode,inject_tuple;
static uint32_t peer_flags,backend[15],now;
static bool admit=true,read_valid=true,have_aa;

bool omni_mixer_ui_bias_set(bool enabled,unsigned position)
{
    ++ui_calls;
    assert(position<=24u);
    if(mix.bias_mode!=enabled) omni_source_mix_toggle(&mix);
    assert(omni_source_mix_configure(&mix,position));
    if(inject_tuple) {
        unsigned packed=inject_tuple;inject_tuple=0;
        /* Model the USB IRQ publishing while main applies its captured tuple. */
        assert(omni_mixer_control_request((packed>>5)&1u,packed&31u));
    }
    return true;
}
void omni_mixer_ui_bias_snapshot(omni_source_mix *out,omni_source_mix_gains *gains)
{ *out=mix;assert(omni_source_mix_snapshot(&mix,gains)); }
void usb_audio_ring_source_gain(unsigned q14)
{ ++pcm_calls;pcm_target=q14; }
bool omni_mcu2_set_source_gain(uint8_t index)
{
    ++aa_calls;assert(index<=12u);
    /* The UART7 wrapper owns wire coalescing; this service publishes once/loop. */
    if(!have_aa || aa_target!=index) {++aa_transmissions;have_aa=true;aa_target=index;}
    return true;
}
bool omni_native_headset_gain_read(unsigned page,uint32_t out[15])
{ assert(page==0u);memset(out,0,60u);out[3]=peer_flags;return true; }
bool omni_dsp_settings_status(unsigned page,uint32_t out[15])
{ assert(page==0u);memcpy(out,backend,60u);return read_valid; }
bool omni_dsp_settings_busy(void) {return (backend[5]&1u)!=0u;}
bool omni_dsp_settings_request(uint32_t token,unsigned control,const uint8_t *value,size_t length,uint32_t at)
{
    ++request_calls;
    assert(control==DSP_SETTING_HOME_MODE && length==1u && value && value[0]<=1u);
    assert(at==now && !omni_dsp_settings_busy());
    if(!admit) return false;
    ++accepted_requests;last_mode=value[0];backend[2]=token;
    backend[4]=DSP_SETTINGS_QUEUED;backend[5]=1u;return true;
}
static void step(bool allowed)
{
    unsigned before=aa_calls;
    ++now;omni_mixer_control_service(now,allowed);
    assert(aa_calls-before==(allowed?1u:0u));
}
static uint32_t status(unsigned index)
{ uint32_t out[15];omni_mixer_control_status(out);assert(index<15u);return out[index]; }
static void terminal(unsigned result,bool allowed)
{backend[4]=result;backend[5]=0u;step(allowed);assert(status(9)==result);}

int main(void)
{
    omni_source_mix_init(&mix);
    step(false);assert(!request_calls && !aa_calls && pcm_target==16384u);
    assert(!omni_mixer_control_request(2,0) && !omni_mixer_control_request(0,25));
    assert(omni_mixer_control_request(1,2));
    assert(omni_mixer_control_request(0,23));
    step(false);assert(ui_calls==1u && !mix.bias_mode && mix.position==23u);
    assert(pcm_target==206u && !request_calls);
    /* Mid-service IRQ cannot tear mode/position. It becomes the next tuple. */
    assert(omni_mixer_control_request(1,3));inject_tuple=22u;
    step(false);assert(mix.bias_mode && mix.position==3u);
    step(false);assert(!mix.bias_mode && mix.position==22u);
    unsigned count=ui_calls;step(false);assert(ui_calls==count);
    /* Identical host value has a new generation after a physical UI change. */
    omni_source_mix_toggle(&mix);assert(omni_source_mix_configure(&mix,4));
    assert(omni_mixer_control_request(0,22));step(false);
    assert(!mix.bias_mode && mix.position==22u);
    step(true);assert(!request_calls); /* Unknown peer. */
    peer_flags=256u|8u;step(true);assert(!request_calls); /* Known absent. */
    peer_flags=256u|512u;step(true);assert(!request_calls); /* Connected unready. */
    peer_flags|=8u;backend[5]=1u;step(true);assert(!request_calls);
    backend[5]=0u;step(false);assert(!request_calls); /* Ready but disallowed. */
    admit=false;step(true);assert(request_calls==1u && !accepted_requests);
    admit=true;step(true);assert(accepted_requests==1u && last_mode==0u);
    uint32_t first_token=status(8);
    for(unsigned i=0;i<5u;++i) step(true);
    assert(accepted_requests==1u && status(9)==DSP_SETTINGS_QUEUED);
    /* Main publishes each loop, UART7 wrapper coalesces unchanged AA. */
    unsigned transmissions=aa_transmissions;
    for(unsigned i=0;i<5u;++i) step(true);
    assert(aa_transmissions==transmissions);
    assert(omni_mixer_control_request(1,2));step(true);
    assert(last_mode==0u && accepted_requests==1u && aa_target==2u);
    terminal(DSP_SETTINGS_ACCEPTED,true); /* Local dispatch only, no peer claim. */
    step(true);assert(accepted_requests==2u && last_mode==1u && status(8)!=first_token);
    terminal(DSP_SETTINGS_NACK,true);
    for(unsigned i=0;i<5u;++i) step(true);
    assert(accepted_requests==2u); /* Duplicate connected/unchanged mode cannot retry. */
    assert(omni_mixer_control_request(1,5));step(true);
    assert(accepted_requests==2u && aa_target==5u); /* Bias-only change isn't mode retry. */
    assert(omni_mixer_control_request(0,5));step(true);
    assert(accepted_requests==3u && last_mode==0u);
    /* A completion must be consumed even while the UART is disallowed. */
    terminal(DSP_SETTINGS_CANCELLED,false);step(true);
    assert(accepted_requests==3u);
    peer_flags=256u;step(true);assert(accepted_requests==3u);
    peer_flags=256u|512u|8u;step(true);assert(accepted_requests==4u);
    /* Backend status replacement is a local terminal loss, never an ACK. */
    backend[2]=123u;backend[4]=DSP_SETTINGS_ACCEPTED;backend[5]=0u;
    step(false);assert(status(9)==OMNI_MIXER_CONTROL_RESULT_REPLACED);
    for(unsigned i=0;i<5u;++i) step(true);
    assert(accepted_requests==4u);
    assert(omni_mixer_control_request(1,12));step(true);assert(accepted_requests==5u);
    /* Waiting can time out if the backend is never polled; no cancellation or
     * replay of its in-flight work. Changed desire waits for backend idle. */
    uint32_t started=status(12);
    now=started+OMNI_MIXER_CONTROL_WAIT_MS-1u;step(false);
    assert(status(9)==DSP_SETTINGS_TIMEOUT && backend[5]==1u);
    assert(omni_mixer_control_request(0,12));step(true);assert(accepted_requests==5u);
    backend[5]=0u;step(true);assert(accepted_requests==6u);
    terminal(DSP_SETTINGS_TIMEOUT,true);
    /* An unknown status transient is not a new physical peer epoch. */
    uint32_t epoch=status(14);peer_flags=0;step(true);
    peer_flags=256u|512u|8u;step(true);
    assert(status(14)==epoch && accepted_requests==6u);
    /* Actual peer edge while a request is active preserves one re-dispatch. */
    assert(omni_mixer_control_request(1,12));step(true);assert(accepted_requests==7u);
    peer_flags=256u;step(true);peer_flags=256u|512u|8u;step(true);
    terminal(DSP_SETTINGS_ACCEPTED,true);step(true);assert(accepted_requests==8u);
    terminal(DSP_SETTINGS_ACCEPTED,true);step(true);assert(accepted_requests==8u);
    assert(pcm_calls>aa_calls);
    puts("mixer control: packed IRQ tuple, coalesced owners, peer epochs and bounded terminals pass");
    return 0;
}
