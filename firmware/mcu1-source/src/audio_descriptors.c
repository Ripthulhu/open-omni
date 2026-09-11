#include "audio_probe.h"
#include "usb_names.h"
#include <stddef.h>

const char *omni_usb_string(uint8_t index,const char *build_id)
{
    switch(index) {
    case OMNI_USB_STRING_MANUFACTURER: return "Omni";
    case OMNI_USB_STRING_PRODUCT: return "Omni USB1";
    case OMNI_USB_STRING_SERIAL: return build_id;
    case OMNI_USB_STRING_AUDIO: return "USB1 Audio";
    case OMNI_USB_STRING_PLAYBACK: return "USB1 Playback";
    case OMNI_USB_STRING_MICROPHONE: return "USB1 Microphone";
    case OMNI_USB_STRING_CONTROL: return "USB1 Control";
    default: return NULL;
    }
}
/* Two independent UAC2 functions: playback AC0/AS1 has a programmable 48/96k
 * clock; microphone AC2/AS3 keeps its stock 48k clock. Windows binds one clock
 * per function. HID4 diagnostics is independent. Playback16/24 uses stereo
 * packed PCM with explicit feedback; microphone remains mono16 silence pending
 * the actual capture path. Keep topology constants in audio_usb_layout.h. */
uint8_t omni_audio_config[OMNI_AUDIO_CONFIG_LENGTH] = {
    9,2,0x63,1,5,1,0,0x80,50,
    8,11,0,2,1,0,0x20,OMNI_USB_STRING_PLAYBACK,
    9,4,0,0,1,1,1,0x20,OMNI_USB_STRING_PLAYBACK,
    9,0x24,1,0,2,1,64,0,0,
    8,0x24,10,10,3,7,0,0, /* Internal programmable, frequencyRW / validityRO. */
    17,0x24,2,1,1,1,0,10,2,3,0,0,0,0,0,0,OMNI_USB_STRING_PLAYBACK,
    18,0x24,6,5,1,15,0,0,0,0,0,0,0,0,0,0,0,0,
    12,0x24,3,2,1,3,0,5,10,0,0,OMNI_USB_STRING_PLAYBACK,
    7,5,0x82,3,6,0,4,
    9,4,1,0,0,1,2,0x20,OMNI_USB_STRING_PLAYBACK,
    9,4,1,1,2,1,2,0x20,OMNI_USB_STRING_PLAYBACK,
    16,0x24,1,1,0,1,1,0,0,0,2,3,0,0,0,0,
    6,0x24,2,1,2,16,
    7,5,3,0x05,0x84,1,1, /* PCM16: up to 97 stereo frames 388 bytes. */
    8,0x25,1,0,0,0,0,0,
    7,5,0x84,0x11,4,0,1,
    9,4,1,2,2,1,2,0x20,OMNI_USB_STRING_PLAYBACK,
    16,0x24,1,1,0,1,1,0,0,0,2,3,0,0,0,0,
    6,0x24,2,1,3,24,
    7,5,3,0x05,0x46,2,1, /* PCM24: up to 97 stereo frames 582 bytes. */
    8,0x25,1,0,0,0,0,0,
    7,5,0x84,0x11,4,0,1,
    8,11,2,2,1,0,0x20,OMNI_USB_STRING_MICROPHONE,
    9,4,2,0,0,1,1,0x20,OMNI_USB_STRING_MICROPHONE,
    9,0x24,1,0,2,1,46,0,0,
    8,0x24,10,11,5,5,0,0, /* Separate fixed 48k, USB-frame-paced silent capture. */
    17,0x24,2,3,1,2,0,11,1,0,0,0,0,0,0,0,OMNI_USB_STRING_MICROPHONE,
    12,0x24,3,4,1,1,0,3,11,0,0,OMNI_USB_STRING_MICROPHONE,
    9,4,3,0,0,1,2,0x20,OMNI_USB_STRING_MICROPHONE,
    9,4,3,1,1,1,2,0x20,OMNI_USB_STRING_MICROPHONE,
    16,0x24,1,4,0,1,1,0,0,0,1,0,0,0,0,0,
    6,0x24,2,1,2,16,
    7,5,0x83,0x0d,98,0,1,
    8,0x25,1,0,0,0,0,0,
    9,4,4,0,1,3,0,0,OMNI_USB_STRING_CONTROL,
    9,0x21,0x11,1,0,1,0x22,27,0,
    7,5,0x81,3,64,0,10
};
