import comtypes
comtypes.CoInitialize()
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume, IMMDeviceEnumerator
from pycaw.constants import CLSID_MMDeviceEnumerator, EDataFlow, DEVICE_STATE
from ctypes import POINTER, cast
from comtypes import CLSCTX_ALL

enum = comtypes.CoCreateInstance(CLSID_MMDeviceEnumerator, IMMDeviceEnumerator,
                                 comtypes.CLSCTX_INPROC_SERVER)
coll = enum.EnumAudioEndpoints(EDataFlow.eRender.value, DEVICE_STATE.ACTIVE.value)
print("Active render endpoints:")
for i in range(coll.GetCount()):
    dev = coll.Item(i)
    name = AudioUtilities.CreateDevice(dev).FriendlyName
    vol = cast(dev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None),
               POINTER(IAudioEndpointVolume))
    mn, mx, inc = vol.GetVolumeRange()
    print(f"  - {name!r}")
    print(f"      range dB: min={mn:.2f} max={mx:.2f} inc={inc:.2f} | "
          f"scalar={vol.GetMasterVolumeLevelScalar():.3f} mute={vol.GetMute()}")
