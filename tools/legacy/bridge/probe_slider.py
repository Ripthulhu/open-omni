import warnings; warnings.simplefilter("ignore")
import comtypes, time
from ctypes import cast, POINTER
from comtypes import GUID, CLSCTX_INPROC_SERVER, CLSCTX_ALL
from pycaw.pycaw import IMMDeviceEnumerator, IAudioEndpointVolume, AudioUtilities, AudioDeviceState
CLSID=GUID('{BCDE0395-E52F-467C-8E3D-C4579291692E}')
enum=comtypes.CoCreateInstance(CLSID,IMMDeviceEnumerator,CLSCTX_INPROC_SERVER)
oid=None;name=None
for d in AudioUtilities.GetAllDevices():
    fn=getattr(d,"FriendlyName",None) or ""
    if getattr(d,"state",None)==AudioDeviceState.Active and "Headphones" in fn and ("Omni" in fn or "Arctis Nova Pro" in fn):
        oid,name=d.id,fn; break
dev=enum.GetDevice(oid) if oid else enum.GetDefaultAudioEndpoint(0,1)
vol=cast(dev.Activate(IAudioEndpointVolume._iid_,CLSCTX_ALL,None),POINTER(IAudioEndpointVolume))
print("watching:",name or "(default)","-- move the Windows slider now")
for i in range(20):
    print("  scalar=%.3f  hw=%d"%(vol.GetMasterVolumeLevelScalar(), round((1-vol.GetMasterVolumeLevelScalar())*56)))
    time.sleep(0.5)
