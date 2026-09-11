#ifndef OMNI_USB_NAMES_H
#define OMNI_USB_NAMES_H
#include <stdint.h>

enum {
    OMNI_USB_STRING_MANUFACTURER = 1,
    OMNI_USB_STRING_PRODUCT = 2,
    OMNI_USB_STRING_SERIAL = 3,
    OMNI_USB_STRING_AUDIO = 4,
    OMNI_USB_STRING_PLAYBACK = 5,
    OMNI_USB_STRING_MICROPHONE = 6,
    OMNI_USB_STRING_CONTROL = 7
};

/* Serial stays the exact build identity used by update and diagnostic tools.
 * Unknown indices return null; index0 language descriptor is handled by USB. */
const char *omni_usb_string(uint8_t index,const char *build_id);
#endif
