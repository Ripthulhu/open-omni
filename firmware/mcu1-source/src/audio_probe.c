#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"
#include "usb_device_audio.h"
#include "usb_device_dci.h"
#include "usb_device_lpcip3511.h"
#include "fsl_device_registers.h"
#include "omni_core.h"
#include "audio_probe.h"
#include "usb_iso_lpc5528.h"
#include "usb_audio_ring.h"
#include "volume_scale.h"
#include "native_gain.h"
#include "ui.h"
#include "microphone.h"
#include <string.h>

/* Source-owned UAC2 adapter on the pinned NXP DCI. SDK's stock adapter stores
 * one streaming alternate, whereas this function has two independent streams.
 * USB callbacks serialize control mutations. Main masks IRQs around polling;
 * no audio callback writes flash or calls the retained stock application. */
static usb_device_handle audio_device;
static omni_volume volume;
static uint8_t configured, alternate[4], notify_busy;
static omni_uac2_clocks clocks={48000u,true};
static uint32_t format_epoch=1u;
static omni_audio_format frontend_format;
static uint8_t playback_failed;
/* Endpoint ownership survives failed close and partial stream initialization.
 * Bit order matches endpoint_audits: 03, 82, 83, 84. Closing also blocks
 * callbacks from rearming transfers while the DCI drains the endpoint. */
static uint8_t endpoint_open, endpoint_closing;
#define PLAYBACK_ENDPOINTS 0x09u
#define MICROPHONE_ENDPOINT 0x04u
#define NOTIFICATION_ENDPOINT 0x02u
static uint8_t microphone[98] __attribute__((aligned(4)));
static uint8_t control[32] __attribute__((aligned(4)));
static uint8_t notification[8] __attribute__((aligned(4)));
static uint32_t sent_revision, playback_packets, microphone_packets, errors, notifications;
static uint32_t notify_started;
/* Legacy SDK endpoint accounting. Dedicated ISO bank counters are opcode48. */
typedef struct {
    uint32_t submits,callbacks,cancels,opens,closes,halts,unhalts;
    uint32_t last_queue,last_length;
} endpoint_audit;
static endpoint_audit endpoint_audits[4];
static int audit_index(uint8_t endpoint)
{ return endpoint==3?0:endpoint==0x82?1:endpoint==0x83?2:endpoint==0x84?3:-1; }
static uint8_t request_trace[32][32];
static uint32_t request_count;
/* Error-only ring: no printing/allocation, 16 records, USB frame timestamps.
 * kind: 1 notification completion, 2 OUT length, 3 IN length,
 * 4 stream queue, 5 notification queue, 6 initial stream queue,
 * 7 feedback length, 8 OUT completion ownership. */
static uint32_t error_trace[16][8];
static void record_error(uint32_t kind,uint8_t endpoint,uint32_t length,usb_status_t status)
{
    uint32_t *entry=error_trace[errors%16U];
    entry[0]=errors+1U; entry[1]=USB0->INFO;
    entry[2]=kind; entry[3]=endpoint; entry[4]=length;
    entry[5]=(uint32_t)status; entry[6]=USB0->DEVCMDSTAT; entry[7]=USB0->INTSTAT;
    ++errors;
}
int audio_probe_alternate(uint8_t iface) { return iface<4 ? alternate[iface] : -1; }
int audio_probe_playback_failed(void) { return playback_failed || omni_usb_iso_failed(); }

static void next_format_epoch(void)
{
    if(++format_epoch==0u) ++format_epoch;
}
bool audio_probe_playback_format(omni_audio_format *out)
{
    if(!out) return false;
    uint32_t mask=__get_PRIMASK();__disable_irq();
    bool valid=omni_audio_format_make(out,clocks.playback_rate,
        alternate[OMNI_PLAYBACK_AS_INTERFACE]==2u?24u:16u,format_epoch);
    bool wanted=valid && configured && alternate[OMNI_PLAYBACK_AS_INTERFACE] &&
        !playback_failed && !(endpoint_closing&PLAYBACK_ENDPOINTS);
    __set_PRIMASK(mask);return wanted;
}
void audio_probe_format_status(uint32_t out[15])
{
    omni_audio_format format;bool wanted=audio_probe_playback_format(&format);
    uint32_t status[15]={1u,0u,wanted?1u:0u,format.sample_rate,format.sample_bits,
        format.epoch,format.frame_bytes,format.max_packet,format.frames_per_ms,
        OMNI_PLAYBACK_AC_INTERFACE,OMNI_PLAYBACK_AS_INTERFACE,
        OMNI_MICROPHONE_AC_INTERFACE,OMNI_MICROPHONE_AS_INTERFACE,OMNI_HID_INTERFACE,
        clocks.playback_valid?1u:0u};
    memcpy(out,status,sizeof(status));
}

static usb_status_t queue_stream(uint8_t endpoint)
{
    if(endpoint!=0x83u) return kStatus_USB_InvalidRequest;
    endpoint_audit *audit=&endpoint_audits[2];
    ++audit->submits;
    usb_status_t result=USB_DeviceSendRequest(audio_device,0x83,microphone,omni_microphone_packet(microphone));
    audit->last_queue=(uint32_t)result;
    return result;
}
void audio_probe_iso_receive(const uint8_t *pcm,uint16_t length)
{
    audio_probe_iso_receive_format(pcm,length,frontend_format.epoch);
}
void audio_probe_iso_receive_format(const uint8_t *pcm,uint16_t length,uint32_t epoch)
{
    if(!configured || !alternate[1] || epoch!=format_epoch ||
       (endpoint_closing&PLAYBACK_ENDPOINTS)) return;
    usb_audio_ring_observe_packet_format(length,USB0->INFO,omni_ui_milliseconds(),epoch);
    ++playback_packets; ++endpoint_audits[0].callbacks;
    endpoint_audits[0].last_length=length;
    usb_audio_ring_write_format(pcm,length,epoch);
}

static usb_status_t endpoint_callback(usb_device_handle handle,
    usb_device_endpoint_callback_message_struct_t *message, void *context)
{
    (void)handle;
    uint8_t endpoint=(uint8_t)(uintptr_t)context;
    int index=audit_index(endpoint);
    if (index<0 || omni_usb_iso_managed(endpoint)) return kStatus_USB_InvalidRequest;
    endpoint_audit *audit=&endpoint_audits[index];
    ++audit->callbacks;
    audit->last_length=message?message->length:UINT32_MAX;
    if (message && message->length==UINT32_MAX) ++audit->cancels;
    if (!(endpoint_open&(1u<<(unsigned)index)) ||
        (endpoint_closing&(1u<<(unsigned)index))) return kStatus_USB_Success;
    if (endpoint==0x82) {
        notify_busy=0;
        if (message && message->length==6) {
            omni_volume_notification_complete(&volume,notification[3],sent_revision);
            ++notifications;
        } else record_error(1,endpoint,message?message->length:UINT32_MAX,kStatus_USB_Error);
    } else if (configured && alternate[OMNI_MICROPHONE_AS_INTERFACE]) {
        if (!message || message->length==UINT32_MAX) return kStatus_USB_Success;
        ++microphone_packets;
        if (message->length<94u || message->length>98u || (message->length&1u)) record_error(3,endpoint,message->length,kStatus_USB_Error);
        usb_status_t result=queue_stream(endpoint);
        if (result!=kStatus_USB_Success) record_error(4,endpoint,message->length,result);
    }
    return kStatus_USB_Success;
}

static usb_status_t open_endpoint(uint8_t endpoint, uint16_t size, uint8_t type, uint8_t interval)
{
    int index=audit_index(endpoint);
    if(index<0) return kStatus_USB_InvalidRequest;
    uint8_t bit=(uint8_t)(1u<<(unsigned)index);
    if(endpoint_open&bit) return kStatus_USB_Error;
    usb_device_endpoint_init_struct_t ep={size,endpoint,type,0,interval};
    usb_device_endpoint_callback_struct_t cb={endpoint_callback,(void *)(uintptr_t)endpoint,0};
    ++endpoint_audits[index].opens;
    usb_status_t result=USB_DeviceInitEndpoint(audio_device,&ep,&cb);
    if(result==kStatus_USB_Success) endpoint_open|=bit;
    return result;
}
static usb_status_t close_endpoint(uint8_t endpoint)
{
    int index=audit_index(endpoint);
    if(index<0) return kStatus_USB_InvalidRequest;
    uint8_t bit=(uint8_t)(1u<<(unsigned)index);
    if(!(endpoint_open&bit)) return kStatus_USB_Success;
    endpoint_closing|=bit;
    ++endpoint_audits[index].closes;
    usb_status_t result=USB_DeviceDeinitEndpoint(audio_device,endpoint);
    if(result==kStatus_USB_Success) {
        endpoint_open&=(uint8_t)~bit;
        endpoint_closing&=(uint8_t)~bit;
        if(endpoint==0x82u) notify_busy=0;
    }
    return result;
}
static usb_status_t close_endpoints(uint8_t mask)
{
    static const uint8_t addresses[]={3u,0x82u,0x83u,0x84u};
    usb_status_t result=kStatus_USB_Success;
    endpoint_closing|=(uint8_t)(endpoint_open&mask);
    for(unsigned i=0;i<sizeof(addresses);++i) {
        if((mask&(1u<<i)) && close_endpoint(addresses[i])!=kStatus_USB_Success)
            result=kStatus_USB_Error;
    }
    return result;
}
static usb_status_t set_stream_interface(uint8_t iface,uint8_t next,uint32_t rate)
{
    bool playback=iface==OMNI_PLAYBACK_AS_INTERFACE;
    bool microphone_stream=iface==OMNI_MICROPHONE_AS_INTERFACE;
    if(!configured || (!playback && !microphone_stream) ||
       next>(playback?2u:1u)) return kStatus_USB_InvalidRequest;
    omni_audio_format format;
    if(playback && !omni_audio_format_make(&format,rate,next==2u?24u:16u,format_epoch))
        return kStatus_USB_InvalidRequest;
    uint8_t endpoints=playback?PLAYBACK_ENDPOINTS:MICROPHONE_ENDPOINT;
    if(alternate[iface]==next && (!playback || rate==clocks.playback_rate) &&
       !(endpoint_closing&endpoints) && (next || !(endpoint_open&endpoints)))
        return kStatus_USB_Success;
    if(playback) next_format_epoch();
    alternate[iface]=0; /* No cancellation callback may rearm the old stream. */
    if(close_endpoints(endpoints)!=kStatus_USB_Success) {
        if(playback) playback_failed=1;
        return kStatus_USB_Error;
    }
    if(!next) {
        if(playback) {clocks.playback_rate=rate;playback_failed=0;}
        return kStatus_USB_Success;
    }
    if(playback) {
        (void)omni_audio_format_make(&format,rate,next==2u?24u:16u,format_epoch);
        playback_failed=0;
        if(open_endpoint(3,format.max_packet,USB_ENDPOINT_ISOCHRONOUS,1)!=kStatus_USB_Success ||
           open_endpoint(0x84,4,USB_ENDPOINT_ISOCHRONOUS,1)!=kStatus_USB_Success ||
           !omni_usb_iso_open_format(audio_device,0x84,&format) ||
           !omni_usb_iso_open_format(audio_device,3,&format)) {
            playback_failed=1;(void)close_endpoints(PLAYBACK_ENDPOINTS);
            record_error(6,3,0,kStatus_USB_Error);return kStatus_USB_Error;
        }
        frontend_format=format;clocks.playback_rate=rate;alternate[iface]=next;
    } else {
        if(open_endpoint(0x83,98,USB_ENDPOINT_ISOCHRONOUS,1)!=kStatus_USB_Success)
            return kStatus_USB_Error;
        alternate[iface]=next;
        usb_status_t result=queue_stream(0x83);
        if(result!=kStatus_USB_Success) {
            alternate[iface]=0;(void)close_endpoints(MICROPHONE_ENDPOINT);
            record_error(6,0x83,0,result);return kStatus_USB_Error;
        }
    }
    return kStatus_USB_Success;
}

static usb_status_t deconfigure(void)
{
    /* Logical stop precedes any cancellation callback. Retain only physical
     * ownership for failed endpoints so another deconfigure/deinit can retry. */
    configured=0;
    next_format_epoch();
    memset(alternate,0,sizeof(alternate));
    usb_status_t result=close_endpoints(0x0fu);
    playback_failed=(uint8_t)(result!=kStatus_USB_Success);
    return result;
}

usb_status_t USB_DeviceAudioInit(uint8_t controller, usb_device_class_config_struct_t *config, class_handle_t *handle)
{
    (void)config;
    if (!omni_volume_init(&volume,OMNI_NATIVE_MIN_DB,0,OMNI_NATIVE_STEP_DB,-30*256)) return kStatus_USB_Error;
    /* Seed the master UNMUTED. omni_volume_init defaults muted=true (belt-and-
     * braces against a startup blast), and the host is expected to push its
     * remembered mute state on device arrival. A cold replug gets that push and
     * comes up correctly; a firmware flash re-enumerates too quickly for Windows
     * to re-run arrival init, so the device would otherwise sit stuck at the
     * power-up muted=true. No blast risk: no USB stream flows until the host has
     * set the real volume/mute, and line-in to the headset isn't gated here. */
    omni_mute_set(&volume,false,false);
    *handle=&volume;
    return USB_DeviceClassGetDeviceHandle(controller,&audio_device);
}

usb_status_t USB_DeviceAudioDeinit(class_handle_t handle)
{
    (void)handle;
    return deconfigure();
}

usb_status_t USB_DeviceAudioEvent(void *handle, uint32_t event, void *param)
{
    (void)handle;
    if (event==kUSB_DeviceClassEventDeviceReset) {
        /* Generated DCI propagates every endpoint cancellation failure from
         * SetDefaultStatus. This event is dispatched only after that succeeds
         * and the SDK clears endpoint callbacks, so controller ownership ended. */
        configured=0; playback_failed=0;
        next_format_epoch();clocks.playback_rate=48000u;clocks.playback_valid=true;
        endpoint_open=0; endpoint_closing=0;
        memset(alternate,0,sizeof(alternate)); notify_busy=0;
        return kStatus_USB_Success;
    }
    if (!param) return kStatus_USB_InvalidRequest;
    if (event==kUSB_DeviceClassEventSetEndpointHalt ||
        event==kUSB_DeviceClassEventClearEndpointHalt) {
        /* Chapter 9 delegates endpoint halt requests to the owning class.
         * Windows clears the notification pipe before starting UAC2. Only
         * our configured interrupt endpoint supports halt; ISO streams do
         * not, and HID owns 0x81. Preserve the full wIndex for validation. */
        uint16_t endpoint=*(uint16_t *)param;
        if (endpoint!=0x82) return kStatus_USB_Error;
        if (!configured) return kStatus_USB_InvalidRequest;
        if(event==kUSB_DeviceClassEventSetEndpointHalt) ++endpoint_audits[1].halts;
        else ++endpoint_audits[1].unhalts;
        return event==kUSB_DeviceClassEventSetEndpointHalt ?
            USB_DeviceStallEndpoint(audio_device,0x82) :
            USB_DeviceUnstallEndpoint(audio_device,0x82);
    }
    if (event==kUSB_DeviceClassEventSetConfiguration) {
        uint8_t next=*(uint8_t *)param;
        if (next>1) return kStatus_USB_InvalidRequest;
        if (next==configured && !endpoint_closing && (next || !endpoint_open))
            return kStatus_USB_Success;
        if(deconfigure()!=kStatus_USB_Success) return kStatus_USB_Error;
        if (next && open_endpoint(0x82,6,USB_ENDPOINT_INTERRUPT,4)!=kStatus_USB_Success)
            return kStatus_USB_Error;
        configured=next;
        return kStatus_USB_Success;
    }
    if (event==kUSB_DeviceClassEventSetInterface) {
        uint16_t value=*(uint16_t *)param;
        uint8_t iface=(uint8_t)(value>>8),next=(uint8_t)value;
        if(!configured) return kStatus_USB_InvalidRequest;
        if(iface==OMNI_PLAYBACK_AC_INTERFACE || iface==OMNI_MICROPHONE_AC_INTERFACE)
            return next? kStatus_USB_InvalidRequest:kStatus_USB_Success;
        return set_stream_interface(iface,next,clocks.playback_rate);
    }
    if (event==kUSB_DeviceClassEventClassRequest) {
        usb_device_control_request_struct_t *r=param;
        if (!r->setup) return kStatus_USB_InvalidRequest;
        if ((r->setup->wIndex&255U)!=OMNI_PLAYBACK_AC_INTERFACE &&
            (r->setup->wIndex&255U)!=OMNI_MICROPHONE_AC_INTERFACE) return kStatus_USB_Error;
        uint8_t *trace=request_trace[request_count%32];
        if (r->isSetup) {
            memset(trace,0,32); memcpy(trace,r->setup,8); trace[8]=255;
            ++request_count;
        }
        omni_setup s={r->setup->bmRequestType,r->setup->bRequest,r->setup->wValue,r->setup->wIndex,r->setup->wLength};
        if (s.type==0x21 && r->isSetup) {
            bool feature=s.index==0x0500u &&
                ((s.value==0x0200u && s.length==2u)||(s.value==0x0100u && s.length==1u));
            bool rate=s.index==0x0a00u && s.value==0x0100u && s.length==4u;
            if(s.request!=1u || (!feature && !rate)) return kStatus_USB_InvalidRequest;
            r->buffer=control; r->length=s.length; return kStatus_USB_Success;
        }
        omni_uac2_clocks trial=clocks;
        int length=omni_uac2_control_format(&volume,&trial,s,s.type==0x21?r->buffer:NULL,
            s.type==0x21?r->length:0,control,sizeof(control));
        if(length>=0 && trial.playback_rate!=clocks.playback_rate) {
            if(set_stream_interface(OMNI_PLAYBACK_AS_INTERFACE,alternate[1],
                                    trial.playback_rate)!=kStatus_USB_Success)
                return kStatus_USB_Error;
        }
        if (r->isSetup) {
            trace[8]=(uint8_t)length;
            if (length>0 && length<=16) memcpy(trace+12,control,(size_t)length);
        }
        if (length<0) return kStatus_USB_InvalidRequest;
        r->buffer=control; r->length=(uint32_t)length; return kStatus_USB_Success;
    }
    return kStatus_USB_InvalidRequest;
}

void audio_probe_poll(void)
{
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    if (configured && (endpoint_open&NOTIFICATION_ENDPOINT) &&
        !(endpoint_closing&NOTIFICATION_ENDPOINT) && !notify_busy &&
        omni_volume_notification(&volume,notification)) {
        sent_revision=volume.revision;
        notify_busy=1;
        notify_started=omni_ui_milliseconds();
        ++endpoint_audits[1].submits;
        usb_status_t result=USB_DeviceSendRequest(audio_device,0x82,notification,6);
        endpoint_audits[1].last_queue=(uint32_t)result;
        if (result!=kStatus_USB_Success) {
            notify_busy=0; record_error(5,0x82,6,result);
        }
    }
    __set_PRIMASK(mask);
}

static void put32(uint8_t *p,uint32_t x) { for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(x>>(8*i)); }
static int snapshot_ram(uintptr_t p,size_t bytes,unsigned alignment)
{
    return p>=0x20000000U && p<=0x20030000U && bytes<=0x20030000U-p &&
        (p&(alignment-1U))==0U;
}
int audio_probe_endpoint_snapshot(uint8_t endpoint,uint8_t page,uint8_t out[60])
{
    int index=audit_index(endpoint);
    memset(out,0,60);
    if(index<0 || page>3) return -1;
    uint32_t values[15]={1U,page,endpoint,0U};
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    uint32_t before_info=USB0->INFO,before_interrupts=USB0->INTSTAT,before_use=USB0->EPINUSE;
    uint32_t eplist=USB0->EPLISTSTART,descriptor0=0,descriptor1=0;
    uint32_t physical=((endpoint&15U)<<1)|((endpoint>>7)&1U);
    volatile uint32_t *list=NULL;
    const usb_device_lpc3511ip_state_struct_t *controller=NULL;
    const usb_device_lpc3511ip_endpoint_state_struct_t *ep=NULL;
    if(snapshot_ram((uintptr_t)audio_device,sizeof(usb_device_struct_t),4U)) {
        const usb_device_struct_t *device=(const usb_device_struct_t *)audio_device;
        if(snapshot_ram((uintptr_t)device->controllerHandle,sizeof(*controller),4U)) {
            controller=(const usb_device_lpc3511ip_state_struct_t *)device->controllerHandle;
            if(controller->registerBase==USB0) {
                ep=&controller->endpointState[physical]; values[3]|=1U;
            } else controller=NULL;
        }
    }
    if(snapshot_ram(eplist,USB_DEVICE_IP3511_ENDPOINTS_NUM*16U,256U)) {
        list=(volatile uint32_t *)(uintptr_t)eplist;
        descriptor0=list[physical*2U]; descriptor1=list[physical*2U+1U];
        values[3]|=2U;
    }
    if(page==0) {
        values[4]=before_info; values[6]=before_interrupts; values[8]=before_use;
        values[10]=descriptor0; values[11]=descriptor1;
        values[12]=USB0->EPBUFCFG; values[13]=USB0->EPSKIP; values[14]=USB0->EPTOGGLE;
    } else if(page==1) {
        values[4]=USB0->INTEN; values[5]=eplist;
        if(ep) {
            values[6]=(uint32_t)(uintptr_t)controller->epCommandStatusList;
            values[7]=ep->stateUnion.state; values[8]=ep->transferLength;
            values[9]=ep->transferDone; values[10]=ep->transferPrimedLength;
            values[11]=ep->epBufferStatusUnion[0].epBufferStatus|
                ((uint32_t)ep->epBufferStatusUnion[1].epBufferStatus<<16);
        }
        values[12]=endpoint_audits[index].submits; values[13]=endpoint_audits[index].callbacks;
        values[14]=endpoint_audits[index].cancels;
    } else if(page==2) {
        values[4]=notify_busy; values[5]=volume.pending; values[6]=sent_revision;
        values[7]=volume.revision; values[8]=notifications; values[9]=notification[3];
        values[10]=notify_busy?(uint32_t)(omni_ui_milliseconds()-notify_started):0U;
        values[11]=endpoint_audits[index].last_queue; values[12]=endpoint_audits[index].last_length;
        values[13]=configured; values[14]=alternate[1]|((uint32_t)alternate[OMNI_MICROPHONE_AS_INTERFACE]<<8);
    } else {
        values[4]=endpoint_audits[index].opens; values[5]=endpoint_audits[index].closes;
        values[6]=endpoint_audits[index].halts; values[7]=endpoint_audits[index].unhalts;
        values[8]=endpoint_audits[index].submits; values[9]=endpoint_audits[index].callbacks;
        values[10]=endpoint_audits[index].cancels;
        values[11]=endpoint==3?playback_failed:0u;
        values[12]=endpoint_open; values[13]=endpoint_closing;
    }
    uint32_t after_use=USB0->EPINUSE,after_interrupts=USB0->INTSTAT,after_info=USB0->INFO;
    /* Hardware still runs while CPU interrupts are masked. Matching reads are
     * a best-effort stability check, not an atomic controller snapshot. */
    if(before_use==after_use && before_interrupts==after_interrupts && eplist==USB0->EPLISTSTART &&
       (!list || (descriptor0==list[physical*2U] && descriptor1==list[physical*2U+1U]))) values[3]|=4U;
    if(page==0) { values[5]=after_info; values[7]=after_interrupts; values[9]=after_use; }
    __set_PRIMASK(mask);
    for(unsigned i=0;i<15;++i) put32(out+4U*i,values[i]);
    return 0;
}
void audio_probe_error_trace(uint8_t index,uint8_t out[60])
{
    memset(out,0,60); put32(out,errors);
    put32(out+4,errors>16U?errors-16U:0U); put32(out+8,16U);
    if (index<16U) memcpy(out+12,error_trace[index],sizeof(error_trace[index]));
}
void audio_probe_clock_snapshot(uint8_t out[60])
{
    /* Fixed allowlist of clock/status registers. No arbitrary address access
     * and no writes to analog trim, clocks, endpoint state or flash. */
    const uint32_t values[]={1U,ANACTRL->ANALOG_CTRL_CFG,ANACTRL->FRO192M_CTRL,
        ANACTRL->FRO192M_STATUS,SYSCON->MAINCLKSELA,SYSCON->MAINCLKSELB,
        SYSCON->AHBCLKDIV,SYSCON->USB0CLKSEL,SYSCON->USB0CLKDIV,
        USB0->INFO,USB0->DEVCMDSTAT,USB0->INTSTAT,USB0->EPLISTSTART,USB0->DATABUFSTART};
    memset(out,0,60);
    for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i) put32(out+4*i,values[i]);
}
void audio_probe_request_trace(uint8_t index, uint8_t out[48])
{
    memset(out,0,48); put32(out,request_count);
    if (index<32) memcpy(out+8,request_trace[index],32);
}
void audio_probe_status(uint8_t out[32])
{
    memset(out,0,32);
    out[0]=(uint8_t)volume.current; out[1]=(uint8_t)((uint16_t)volume.current>>8);
    out[2]=(uint8_t)volume.muted; out[3]=volume.pending;
    put32(out+4,volume.revision); put32(out+8,playback_packets);
    put32(out+12,microphone_packets); put32(out+16,errors); put32(out+20,notifications);
    out[24]=configured; out[25]=alternate[1]; out[26]=alternate[OMNI_MICROPHONE_AS_INTERFACE];
}
static int local_volume(int16_t db,uint8_t mute)
{
    if(mute>1u || !omni_volume_set(&volume,db,true)) return -1;
    omni_mute_set(&volume,mute!=0u,true);return 0;
}
int audio_probe_local(int16_t db,uint8_t mute)
{
    uint32_t mask=__get_PRIMASK();__disable_irq();
    int result=local_volume(db,mute);__set_PRIMASK(mask);return result;
}
int audio_probe_peer_volume(int16_t db,uint8_t mute,uint32_t expected_revision)
{
    uint32_t mask=__get_PRIMASK();__disable_irq();
    int result=volume.revision==expected_revision?local_volume(db,mute):-2;
    __set_PRIMASK(mask);return result;
}
void audio_probe_master_snapshot(int16_t *db,uint8_t *mute,uint32_t *revision)
{
    uint32_t mask=__get_PRIMASK();__disable_irq();
    *db=volume.current;*mute=(uint8_t)volume.muted;*revision=volume.revision;
    __set_PRIMASK(mask);
}
void audio_probe_volume_snapshot(int16_t *db,uint8_t *mute)
{
    uint32_t revision;audio_probe_master_snapshot(db,mute,&revision);
}
void audio_probe_dial(int step)
{
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    omni_volume_dial(&volume,step);
    __set_PRIMASK(mask);
}
