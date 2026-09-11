#ifndef OMNI_USB_ISO_LPC5528_H
#define OMNI_USB_ISO_LPC5528_H
#include <stdbool.h>
#include <stdint.h>
#include "audio_format.h"
/* Dedicated full-speed EP3 OUT / EP84 feedback service. SDK remains owner of
 * endpoint allocation and all other endpoints. The generated, hash-checked
 * DCI overlay routes these endpoints' completion/cancel/reset here. */
bool omni_usb_iso_managed(uint8_t endpoint);
bool omni_usb_iso_open(void *device, uint8_t endpoint);
bool omni_usb_iso_open_format(void *device,uint8_t endpoint,const omni_audio_format *format);
bool omni_usb_iso_preinit(void *controller,uint8_t endpoint);
bool omni_usb_iso_interrupt(void *controller, uint8_t physical_endpoint);
/* Return false without allowing SDK descriptor writes if cancellation fails. */
bool omni_usb_iso_cancel(void *controller, uint8_t endpoint);
void omni_usb_iso_bus_reset(void *controller);
bool omni_usb_iso_failed(void);
bool omni_usb_iso_status(uint8_t endpoint, uint32_t out[15]);
enum {
    OMNI_ISO_FAULT_NO_ARM=1u, OMNI_ISO_FAULT_CONTROL=2u,
    OMNI_ISO_FAULT_LENGTH=4u, OMNI_ISO_FAULT_ADDRESS=8u,
    OMNI_ISO_FAULT_PCM_LENGTH=16u, OMNI_ISO_FAULT_FEEDBACK_LENGTH=32u
};
/* Read-only first invalid completion, retained across endpoint reopen.
 * Suggested diagnostic48 page1 (request endpoint, page1):
 * [version1,endpoint,reason_bits(0=no_failure),
 * bank|(expected_bank<<8)|(last_arm_valid_mask<<16),
 * raw_failed,last_CPU_arm,raw_peer,last_CPU_peer_arm,
 * INFO,INTSTAT,EPINUSE,EPBUFCFG,EPSKIP,milliseconds,total_failures].
 * INFO bits10:0 contain the contemporaneous USB frame. Register/peer values
 * are separate volatile observations, not an atomic hardware snapshot.
 * The failed word is exactly the triggering read, before any cleanup;
 * IRQ masking in this reader serializes with the same USB IRQ writer.
 * No descriptor mutation or automatic recovery is performed. */
bool omni_usb_iso_failure_status(uint8_t endpoint,uint32_t out[15]);
#endif
