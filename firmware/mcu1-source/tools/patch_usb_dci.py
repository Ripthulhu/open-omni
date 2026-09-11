"""Generate the narrowly hooked LPCIP3511 DCI; never modify the pinned checkout."""
import argparse
import hashlib
from pathlib import Path

PINNED_SHA256 = 'f8036bdc7287188b46b4452219af54d4d0feae77afb8f6f4929d4406807b820e'
HEADER_SHA256 = '3f5c35c422cc0c23f723c0fcb411a0fa690c8b47e648e38309ab831d9da9bb18'
DCI_SHA256 = '3f9172467ec71974f2a8620c223c436c7a489be3c6a7d2275942ed3eebd67de3'

def patch(raw):
    if hashlib.sha256(raw).hexdigest() != PINNED_SHA256:
        raise ValueError('LPCIP3511 source differs from the reviewed pinned input')
    source = raw.decode().replace('\r\n', '\n')
    def replace(old, new):
        nonlocal source
        if source.count(old) != 1:
            raise ValueError('DCI hook anchor is absent or ambiguous: ' + old[:100])
        source = source.replace(old, new)
    # Include after vendor declarations so their configuration remains intact.
    source += '\n'  # output is stable regardless of the input final newline
    replace('#include "usb_device_config.h"',
            '#include "usb_device_config.h"\n#include "usb_iso_lpc5528.h"')
    # Pinned SDK assigns len in the single-bank path too, but conditionally
    # declared it only for double buffering. Keep the shared local declared.
    replace('#if (defined USB_DEVICE_IP3511_DOUBLE_BUFFER_ENABLE) && (USB_DEVICE_IP3511_DOUBLE_BUFFER_ENABLE)\n'
            '    uint32_t len = 0;\n#endif', '    uint32_t len = 0;')
    replace('    lpc3511IpState->registerBase->EPINUSE &= (~((uint32_t)(0x01UL << endpointIndex)));',
            '    /* SDK single-bank selectors stay0; managed ISO retains its selector. */')
    anchor = ('static usb_status_t USB_DeviceLpc3511IpEndpointInit(usb_device_lpc3511ip_state_struct_t *lpc3511IpState,\n'
              '                                                    usb_device_endpoint_init_struct_t *epInit)\n{')
    replace(anchor, anchor + '\n    if (omni_usb_iso_managed(epInit->endpointAddress) &&\n'
            '        !omni_usb_iso_preinit(lpc3511IpState, epInit->endpointAddress)) return kStatus_USB_Error;')
    anchor = 'static usb_status_t USB_DeviceLpc3511IpEndpointDeinit(usb_device_lpc3511ip_state_struct_t *lpc3511IpState, uint8_t ep)\n{'
    replace(anchor, anchor + '\n    if (!omni_usb_iso_cancel(lpc3511IpState, ep)) return kStatus_USB_Error;')
    # This is ownership safety, not the SDK's optional return-value diagnostics.
    # Even single-bank microphone/notification cancellation can fail; never
    # release their packet buffer or clear descriptors after that failure.
    replace('''    /* Cancel the transfer of the endpoint */
#if (defined(USB_DEVICE_CONFIG_RETURN_VALUE_CHECK) && (USB_DEVICE_CONFIG_RETURN_VALUE_CHECK > 0U))
    if (kStatus_USB_Success != USB_DeviceLpc3511IpCancel(lpc3511IpState, ep))
    {
        return kStatus_USB_Error;
    }
#else
    (void)USB_DeviceLpc3511IpCancel(lpc3511IpState, ep);
#endif''', '''    /* Cancellation must succeed before releasing controller-owned memory. */
    if (kStatus_USB_Success != USB_DeviceLpc3511IpCancel(lpc3511IpState, ep))
    {
        return kStatus_USB_Error;
    }''')
    replace('    lpc3511IpState->registerBase->EPINUSE &= ~((uint32_t)(0x01UL << endpointIndex));',
            '    /* No EPINUSE RMW while other ISO banks can toggle in hardware. */')
    anchor = 'usb_status_t USB_DeviceLpc3511IpCancel(usb_device_controller_handle controllerHandle, uint8_t ep)\n{'
    replace(anchor, anchor + '\n    if (omni_usb_iso_managed(ep))\n'
            '        return omni_usb_iso_cancel(controllerHandle, ep) ? kStatus_USB_Success : kStatus_USB_Error;')
    a = source.index('                    if ((lpc3511IpState->registerBase->EPINUSE & (((uint32_t)0x00000001U << endpointIndex))) != 0U)')
    b = source.index('\n                }\n                else', a)
    source = source[:a] + ('                    /* Single-bank invariant violation: do not alter another endpoint selector. */\n'
                          '                    OSA_EXIT_CRITICAL();\n'
                          '                    return kStatus_USB_Error;') + source[b:]
    replace('''                    while (((lpc3511IpState->registerBase->EPSKIP & ((uint32_t)0x00000001U << endpointIndex)) != 0U) &&
                           ((lpc3511IpState->epCommandStatusList[(uint32_t)endpointIndex * 2U +
                                                                 ((lpc3511IpState->registerBase->EPINUSE &
                                                                   (((uint32_t)0x00000001U << endpointIndex))) >>
                                                                  endpointIndex)] &
                             USB_LPC3511IP_ENDPOINT_ACTIVE_MASK) != 0U))
                    {
                    }''', '''                    uint32_t omni_cancel_budget = 4096U;
                    while (((lpc3511IpState->registerBase->EPSKIP & ((uint32_t)0x00000001U << endpointIndex)) != 0U) &&
                           ((lpc3511IpState->epCommandStatusList[(uint32_t)endpointIndex * 2U +
                                                                 ((lpc3511IpState->registerBase->EPINUSE &
                                                                   (((uint32_t)0x00000001U << endpointIndex))) >>
                                                                  endpointIndex)] &
                             USB_LPC3511IP_ENDPOINT_ACTIVE_MASK) != 0U))
                    {
                        if (--omni_cancel_budget == 0U)
                        {
                            /* Keep the pending skip and ACTIVE descriptor owned. */
                            OSA_EXIT_CRITICAL();
                            return kStatus_USB_Error;
                        }
                    }''')
    anchor = 'static void USB_DeviceLpc3511IpInterruptReset(usb_device_lpc3511ip_state_struct_t *lpc3511IpState)\n{'
    replace(anchor, anchor + '\n    omni_usb_iso_bus_reset(lpc3511IpState);')
    replace('        case kUSB_DeviceControlSetDefaultStatus:',
            '        case kUSB_DeviceControlSetDefaultStatus:\n'
            '            /* Never overwrite custom ACTIVE banks after a failed cancel. */\n'
            '            if (!omni_usb_iso_cancel(lpc3511IpState, 3U) ||\n'
            '                !omni_usb_iso_cancel(lpc3511IpState, 0x84U)) return kStatus_USB_Error;\n'
            '            error = kStatus_USB_Success;')
    # Complete every endpoint's teardown, retaining failed ownership. A reset
    # notification may clear class state only if the whole controller reset
    # succeeds; the default SDK configuration ignored these return values.
    replace('''#if (defined(USB_DEVICE_CONFIG_RETURN_VALUE_CHECK) && (USB_DEVICE_CONFIG_RETURN_VALUE_CHECK > 0U))
                if ((kStatus_USB_Success !=
                     USB_DeviceLpc3511IpEndpointDeinit(lpc3511IpState, (uint8_t)(tmp32Value | (USB_IN << 0x07U)))) ||
                    (USB_DeviceLpc3511IpEndpointDeinit(lpc3511IpState, (uint8_t)(tmp32Value | (USB_OUT << 0x07U)))))
                {
                    return kStatus_USB_Error;
                }
#else
                (void)USB_DeviceLpc3511IpEndpointDeinit(lpc3511IpState, (uint8_t)(tmp32Value | (USB_IN << 0x07U)));
                (void)USB_DeviceLpc3511IpEndpointDeinit(lpc3511IpState, (uint8_t)(tmp32Value | (USB_OUT << 0x07U)));
#endif
            }
            USB_DeviceLpc3511IpSetDefaultState(lpc3511IpState);''', '''                if (kStatus_USB_Success != USB_DeviceLpc3511IpEndpointDeinit(
                        lpc3511IpState, (uint8_t)(tmp32Value | (USB_IN << 0x07U))))
                    error = kStatus_USB_Error;
                if (kStatus_USB_Success != USB_DeviceLpc3511IpEndpointDeinit(
                        lpc3511IpState, (uint8_t)(tmp32Value | (USB_OUT << 0x07U))))
                    error = kStatus_USB_Error;
            }
            if (error != kStatus_USB_Success) return error;
            USB_DeviceLpc3511IpSetDefaultState(lpc3511IpState);''')
    replace('                USB_DeviceLpc3511IpInterruptToken(lpc3511IpState, (uint8_t)devState, 0U, usbErrorCode);',
            '                if (!omni_usb_iso_interrupt(lpc3511IpState, (uint8_t)devState))\n'
            '                    USB_DeviceLpc3511IpInterruptToken(lpc3511IpState, (uint8_t)devState, 0U, usbErrorCode);')
    anchor = ('usb_status_t USB_DeviceLpc3511IpSend(usb_device_controller_handle controllerHandle,\n'
              '                                     uint8_t endpointAddress,\n'
              '                                     uint8_t *buffer,\n'
              '                                     uint32_t length)\n{')
    replace(anchor, anchor + '\n    /* The logical multi-packet SDK engine never owns these ISO banks. */\n'
            '    if (omni_usb_iso_managed(endpointAddress)) return kStatus_USB_InvalidRequest;')
    return source

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.source.resolve().parent == args.output.resolve().parent:
        raise ValueError('Generated files must be outside the pinned input directory')
    result = patch(args.source.read_bytes())
    header = args.source.with_suffix('.h').read_bytes()
    if hashlib.sha256(header).hexdigest() != HEADER_SHA256:
        raise ValueError('LPCIP3511 header differs from reviewed pin')
    header = header.decode().replace('\r\n', '\n')
    anchor = '#define USB_DEVICE_IP3511_DOUBLE_BUFFER_ENABLE (1U)'
    if header.count(anchor) != 1:
        raise ValueError('Single-buffer header anchor mismatch')
    header = header.replace(anchor, '#define USB_DEVICE_IP3511_DOUBLE_BUFFER_ENABLE (0U)')
    (args.output.parent/'usb_device_lpcip3511.h').write_text(header, encoding='utf-8', newline='\n')
    # Copy the DCI front end so its quoted header uses the same buffer policy.
    # Propagate a failed controller reset instead of clearing class ownership.
    dci = args.source.with_name('usb_device_dci.c').read_bytes()
    if hashlib.sha256(dci).hexdigest() != DCI_SHA256:
        raise ValueError('DCI front end differs from reviewed pin')
    dci = dci.decode().replace('\r\n', '\n')
    anchor = '    (void)USB_DeviceControl(handle, kUSB_DeviceControlSetDefaultStatus, NULL);'
    if dci.count(anchor) != 1:
        raise ValueError('DCI reset-result anchor mismatch')
    dci = dci.replace(anchor, '    usb_status_t omni_reset_status = USB_DeviceControl(handle, kUSB_DeviceControlSetDefaultStatus, NULL);\n'
                             '    if (omni_reset_status != kStatus_USB_Success) return omni_reset_status;')
    start = dci.index('usb_status_t USB_DeviceDeinitEndpoint(')
    end = dci.index('\n/*!', start)
    function = dci[start:end]
    anchor = '    if (endpoint < USB_DEVICE_CONFIG_ENDPOINTS)'
    if function.count(anchor) != 1:
        raise ValueError('DCI endpoint close-result anchor mismatch')
    function = function.replace(anchor,
        '    /* Failed cancellation retains endpoint callback/busy ownership. */\n'
        '    if (status != kStatus_USB_Success) return status;\n\n' + anchor)
    dci = dci[:start] + function + dci[end:]
    (args.output.parent/'usb_device_dci_omni.c').write_text(dci, encoding='utf-8', newline='\n')
    args.output.write_text(result, encoding='utf-8', newline='\n')
