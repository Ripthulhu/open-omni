#include "mixer_control.h"
#include "mixer_ui.h"
#include "mcu2_probe.h"
#include "usb_audio_ring.h"
#include "native_gain_adapter.h"
#include "dsp_settings.h"
#include <stdatomic.h>
#include <string.h>
static volatile uint32_t requested;
static uint32_t applied,submitted_token,submitted_revision;
static uint32_t submitted_epoch,submitted_ms,finished_ms,phase;
static uint8_t submitted_mode;
static bool waiting,have_submission,have_link,connected;
bool omni_mixer_control_request(unsigned mode,unsigned position)
{
    if(mode>1u || position>24u) return false;
    /* Generation in upper bits permits reapplying an identical host request
     * after intervening physical changes; always publish the entire tuple. */
    uint32_t generation=((requested>>8)+1u)&0xffffffu;
    if(!generation) generation=1u;
    requested=(generation<<8)|(mode<<5)|position;return true;
}
void omni_mixer_control_service(uint32_t now,bool allowed)
{
    uint32_t desired=requested;
    if(desired!=applied) {
        (void)omni_mixer_ui_bias_set((desired&32u)!=0u,desired&31u);applied=desired;
    }
    omni_source_mix mix;omni_source_mix_gains gains;
    omni_mixer_ui_bias_snapshot(&mix,&gains);
    usb_audio_ring_source_gain(gains.q14[0]);
    if(allowed) (void)omni_mcu2_set_source_gain(gains.index[1]);
    uint32_t w[15]={0};(void)omni_native_headset_gain_read(0,w);
    bool known=(w[3]&256u)!=0u,peer=known && (w[3]&512u)!=0u;
    if(known && (!have_link || peer!=connected)) {
        /* Only an actual first/link transition enables retry after a failed
         * dispatch; repeated reports of connected never form a retry loop. */
        ++submitted_epoch;have_submission=false;have_link=true;connected=peer;
    }
    if(waiting) {
        uint32_t result[15]={0};
        bool valid=omni_dsp_settings_status(0,result);
        /* Observe terminal/cancelled work even during USB teardown. Another
         * settings owner can replace the single backend status before the
         * next allowed loop; loss of that result must not wedge this owner. */
        if(valid && result[2]!=submitted_token) {
            phase=OMNI_MIXER_CONTROL_RESULT_REPLACED;finished_ms=now;waiting=false;
        } else if(valid && !(result[5]&1u)) {
            phase=result[4];finished_ms=now;waiting=false;
        } else if((uint32_t)(now-submitted_ms)>=OMNI_MIXER_CONTROL_WAIT_MS) {
            phase=DSP_SETTINGS_TIMEOUT;finished_ms=now;waiting=false;
        }
        return;
    }
    if(!allowed) return;
    if(!peer || !(w[3]&8u) || omni_dsp_settings_busy()) return;
    if(have_submission && submitted_mode==(uint8_t)mix.bias_mode) return;
    uint8_t mode=(uint8_t)mix.bias_mode;
    uint32_t next=omni_dsp_settings_next_token();
    if(omni_dsp_settings_request(next,DSP_SETTING_HOME_MODE,&mode,1u,now)) {
        submitted_token=next;submitted_mode=mode;submitted_revision=mix.revision;
        submitted_ms=now;phase=DSP_SETTINGS_QUEUED;waiting=have_submission=true;
    }
}
void omni_mixer_control_status(uint32_t out[15])
{
    omni_source_mix mix;omni_source_mix_gains gains;
    omni_mixer_ui_bias_snapshot(&mix,&gains);
    uint32_t w[15]={1u,mix.revision,mix.bias_mode,mix.position,gains.index[0],gains.index[1],
        gains.q14[0],gains.q14[1],submitted_token,phase,submitted_mode,
        submitted_revision,submitted_ms,finished_ms,submitted_epoch};
    /* phase ACCEPTED proves local DSP dispatch only: stock D209 suppresses
     * the RF consumer's failure. MCU2 transmission is separate HID66/page4. */
    memcpy(out,w,60);
}
