#ifndef OMNI_USB_GUARD_H
#define OMNI_USB_GUARD_H
#include <stdint.h>
void omni_usb_guard_poll(uint32_t now);
void omni_usb_guard_status(uint8_t out[60]);
#endif
