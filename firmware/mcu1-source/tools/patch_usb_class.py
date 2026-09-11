"""Generate pinned composite lifecycle fixes without changing vendor sources."""
import argparse
import hashlib
import re
from pathlib import Path

PINS = {
    'usb_device_ch9.c': '1322bbc30080e30d26aef8ded12160440382fd43bd26af613208b6b70518e35a',
    'class/usb_device_class.c': '4f1259532ecbbd79388258591232b57f9a7723f617f4dc69d941aafe416dc1a7',
    'class/usb_device_hid.c': 'e1fa0365abc0d06c26f5eba1191d49b198f9cacbf55ac7c2cb0d511bda9e551a',
}


def function(source, name, transform):
    matches = list(re.finditer(r'(?m)^(?:static )?usb_status_t ' + re.escape(name) + r'\([^;]*?\n\{', source))
    if len(matches) != 1:
        raise ValueError('Function anchor mismatch: ' + name)
    start = matches[0].start()
    body = matches[0].end() - 1
    depth = 1
    end = body + 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[:start] + transform(source[start:end], body - start) + source[end:]


def ch9(source):
    configuration = '''{
    (void)buffer; (void)length;
    uint8_t state = 0U;
    if (setup->bmRequestType != 0U || setup->wIndex != 0U ||
        setup->wLength != 0U || setup->wValue > 1U)
        return kStatus_USB_InvalidRequest;
    usb_status_t error = USB_DeviceGetStatus(classHandle->handle, kUSB_DeviceStatusDeviceState, &state);
    if (error != kStatus_USB_Success) return error;
    if (state != (uint8_t)kUSB_DeviceStateAddress && state != (uint8_t)kUSB_DeviceStateConfigured)
        return kStatus_USB_InvalidRequest;
    /* A composite success may never hide a failed class teardown. Do not
     * publish the new device/application configuration before classes succeed. */
    error = USB_DeviceClassEvent(classHandle->handle, kUSB_DeviceClassEventSetConfiguration, &setup->wValue);
    if (error != kStatus_USB_Success) return error;
    state = setup->wValue ? (uint8_t)kUSB_DeviceStateConfigured : (uint8_t)kUSB_DeviceStateAddress;
    error = USB_DeviceSetStatus(classHandle->handle, kUSB_DeviceStatusDeviceState, &state);
    if (error != kStatus_USB_Success) return error;
    return USB_DeviceClassCallback(classHandle->handle, (uint32_t)kUSB_DeviceEventSetConfiguration, &setup->wValue);
}'''
    interface = '''{
    (void)buffer; (void)length;
    uint8_t state = 0U;
    if (setup->bmRequestType != USB_REQUEST_TYPE_RECIPIENT_INTERFACE ||
        setup->wLength != 0U || setup->wIndex > 255U || setup->wValue > 255U)
        return kStatus_USB_InvalidRequest;
    usb_status_t error = USB_DeviceGetStatus(classHandle->handle, kUSB_DeviceStatusDeviceState, &state);
    if (error != kStatus_USB_Success) return error;
    if (state != (uint8_t)kUSB_DeviceStateConfigured) return kStatus_USB_InvalidRequest;
    classHandle->standardTranscationBuffer = (uint16_t)((setup->wIndex << 8U) | setup->wValue);
    error = USB_DeviceClassEvent(classHandle->handle, kUSB_DeviceClassEventSetInterface,
                                &classHandle->standardTranscationBuffer);
    if (error != kStatus_USB_Success) return error;
    return USB_DeviceClassCallback(classHandle->handle, (uint32_t)kUSB_DeviceEventSetInterface,
                                   &classHandle->standardTranscationBuffer);
}'''
    source = function(source, 'USB_DeviceCh9SetConfiguration', lambda old, body: old[:body] + configuration)
    return function(source, 'USB_DeviceCh9SetInterface', lambda old, body: old[:body] + interface)


def common(source):
    dispatch = '''    /* Omni has one configuration. Lifecycle events have stricter semantics
     * than generic composite requests: every class must accept configuration,
     * and only the descriptor-declared owner handles a SET_INTERFACE. */
    if (event == kUSB_DeviceClassEventSetConfiguration || event == kUSB_DeviceClassEventSetInterface)
    {
        usb_status_t result = kStatus_USB_Success;
        uint8_t handled = 0U;
        for (classIndex = 0U; classIndex < classHandle->configList->count; ++classIndex)
        {
            usb_device_class_struct_t *info = classHandle->configList->config[classIndex].classInfomation;
            if (info == NULL || info->configurations != 1U || info->interfaceList == NULL)
                return kStatus_USB_InvalidParameter;
            if (event == kUSB_DeviceClassEventSetInterface)
            {
                uint16_t requested = *((uint16_t *)param);
                uint8_t owns = 0U, supported = 0U;
                const usb_device_interface_list_t *list = &info->interfaceList[0];
                for (uint32_t i = 0U; i < list->count; ++i)
                {
                    const usb_device_interfaces_struct_t *item = &list->interfaces[i];
                    if (item->interfaceNumber != (uint8_t)(requested >> 8U)) continue;
                    owns = 1U;
                    for (uint32_t j = 0U; j < item->count; ++j)
                        if (item->interface[j].alternateSetting == (uint8_t)requested) supported = 1U;
                }
                if (!owns) continue;
                if (!supported) return kStatus_USB_InvalidRequest;
            }
            uint8_t mapped = 0U;
            for (mapIndex = 0U; mapIndex < (ARRAY_SIZE(s_UsbDeviceClassInterfaceMap) - 1U); ++mapIndex)
            {
                if (s_UsbDeviceClassInterfaceMap[mapIndex].type != info->type) continue;
                mapped = 1U; handled = 1U;
                errorReturn = s_UsbDeviceClassInterfaceMap[mapIndex].classEventCallback(
                    (void *)classHandle->configList->config[classIndex].classHandle, event, param);
                if (event == kUSB_DeviceClassEventSetInterface) return errorReturn;
                if (errorReturn != kStatus_USB_Success) result = errorReturn;
                break;
            }
            if (!mapped) result = kStatus_USB_InvalidRequest;
        }
        return handled ? result : kStatus_USB_InvalidRequest;
    }

'''
    def transform(old, body):
        anchor = '    for (classIndex = 0U; classIndex < classHandle->configList->count; classIndex++)'
        if old.count(anchor) != 1:
            raise ValueError('Class event loop anchor mismatch')
        return old.replace(anchor, dispatch + anchor)
    return function(source, 'USB_DeviceClassEvent', transform)


def hid(source):
    deinit = '''{
    usb_status_t status = kStatus_USB_Success;
    if (hidHandle->interfaceHandle == NULL) return status;
    for (uint32_t count = 0U; count < hidHandle->interfaceHandle->endpointList.count; ++count)
    {
        usb_status_t result = USB_DeviceDeinitEndpoint(hidHandle->handle,
            hidHandle->interfaceHandle->endpointList.endpoint[count].endpointAddress);
        if (result != kStatus_USB_Success) status = result;
    }
    if (status == kStatus_USB_Success) hidHandle->interfaceHandle = NULL;
    return status;
}'''
    source = function(source, 'USB_DeviceHidEndpointsDeinit', lambda old, body: old[:body] + deinit)
    def event(old, body):
        start = old.index('        case kUSB_DeviceClassEventSetConfiguration:')
        end = old.index('        case kUSB_DeviceClassEventSetInterface:', start)
        return old[:start] + '''        case kUSB_DeviceClassEventSetConfiguration:
            temp8 = ((uint8_t *)param);
            if (hidHandle->configStruct == NULL ||
                *temp8 > hidHandle->configStruct->classInfomation->configurations)
                return kStatus_USB_InvalidRequest;
            /* Preserve a failed close for retry; configuration zero is a
             * successful shutdown and must not call EndpointsInit(0). */
            error = USB_DeviceHidEndpointsDeinit(hidHandle);
            if (error != kStatus_USB_Success) return error;
            hidHandle->configuration = *temp8;
            hidHandle->alternate = 0U;
            if (*temp8 == 0U) return kStatus_USB_Success;
            error = USB_DeviceHidEndpointsInit(hidHandle);
            if (error != kStatus_USB_Success) hidHandle->configuration = 0U;
            break;
''' + old[end:]
    return function(source, 'USB_DeviceHidEvent', event)


def generate(device, output):
    output.mkdir(parents=True, exist_ok=True)
    for name, transform in (('usb_device_ch9.c', ch9), ('class/usb_device_class.c', common), ('class/usb_device_hid.c', hid)):
        raw = (device / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != PINS[name]:
            raise ValueError('Pinned USB input differs: ' + name)
        rendered = transform(raw.decode().replace('\r\n', '\n'))
        target = output / (Path(name).stem + '_omni.c')
        if target.resolve() == (device / name).resolve():
            raise ValueError('Refusing vendor overwrite')
        target.write_text(rendered, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('device', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    generate(args.device, args.output)
