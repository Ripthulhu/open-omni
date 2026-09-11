#!/usr/bin/env python3
r"""
Omni Volume Bridge — two-way sync between the native Windows volume slider and the Arctis Nova Pro
Omni transmitter's real 0-56 hardware volume. No GG, no OSD overlay, no media-key hooking.

Requires the range-unlock firmware (so the Windows slider is *movable* instead of pinned at 100%).

  * Move the Windows slider, or press the media volume keys  -> the bridge sends [0x01 0x25 HW]
    on the 0xFFC0 command collection -> the transmitter's real hardware volume changes.
  * Turn the physical knob -> the transmitter pushes [0x07 0x25 VAL] telemetry on 0xFF00 -> the
    bridge moves the Windows slider to match.

Device scale is ATTENUATION: HW=0 is LOUDEST, HW=56 is silent. Windows scalar 0.0..1.0 maps to
HW = round((1 - scalar) * 56).

Run in the background:   pythonw omni_volume_bridge.py      (or the .cmd launcher)
Console (debug):         python  omni_volume_bridge.py --verbose
"""
from __future__ import annotations
import sys, time, argparse
import comtypes
from ctypes import cast, POINTER
from comtypes import GUID, CLSCTX_INPROC_SERVER, CLSCTX_ALL
from pycaw.pycaw import IMMDeviceEnumerator, IAudioEndpointVolume, AudioUtilities, AudioDeviceState
import hid

VID, PID = 0x1038, 0x2290
VMAX = 56
OP_VOLUME = 0x25
CMD_PAGE, TEL_PAGE = 0xFFC0, 0xFF00
CLSID_ENUM = GUID('{BCDE0395-E52F-467C-8E3D-C4579291692E}')
VERBOSE = False


def log(*a):
    if VERBOSE:
        print("[%s]" % time.strftime("%H:%M:%S"), *a, flush=True)


# ---------- Windows audio endpoint (the movable Omni slider) ----------
def get_omni_endpoint_volume():
    """Return (IAudioEndpointVolume, name) for the Omni render endpoint (falls back to default)."""
    enum = comtypes.CoCreateInstance(CLSID_ENUM, IMMDeviceEnumerator, CLSCTX_INPROC_SERVER)
    omni_id, omni_name = None, None
    try:
        for dev in AudioUtilities.GetAllDevices():
            fn = getattr(dev, "FriendlyName", None) or ""
            if getattr(dev, "state", None) == AudioDeviceState.Active and "Headphones" in fn \
               and ("Omni" in fn or "Arctis Nova Pro" in fn):
                omni_id, omni_name = dev.id, fn
                break
    except Exception as e:
        log("device scan failed:", e)
    if omni_id:
        immdev = enum.GetDevice(omni_id)
    else:
        immdev = enum.GetDefaultAudioEndpoint(0, 1)   # eRender, eMultimedia
        omni_name = "(default render endpoint)"
    vol = cast(immdev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None), POINTER(IAudioEndpointVolume))
    return vol, omni_name


# Windows scalar is linear in dB for this endpoint (-96..0 dB), so scalar->HW here is mathematically
# identical to the firmware wake hook's dB->att map -- the OLED number the firmware draws on a slider
# move matches the loudness this bridge sets. (Verified: the range matches the hardware 100%.)
def scalar_to_hw(scalar: float) -> int:
    return max(0, min(VMAX, round((1.0 - max(0.0, min(1.0, scalar))) * VMAX)))


def hw_to_scalar(hw: int) -> float:
    return (VMAX - max(0, min(VMAX, hw))) / VMAX


# ---------- HID transport to the transmitter ----------
class Transmitter:
    def __init__(self):
        cp = self._path(CMD_PAGE)
        if not cp:
            raise RuntimeError("Omni 0xFFC0 command collection not found")
        self.cmd = hid.device(); self.cmd.open_path(cp); self.cmd.set_nonblocking(True)
        self.tel = None
        tp = self._path(TEL_PAGE)
        if tp:
            self.tel = hid.device(); self.tel.open_path(tp); self.tel.set_nonblocking(True)

    @staticmethod
    def _path(up):
        for d in hid.enumerate(VID, PID):
            if d["usage_page"] == up:
                return d["path"]
        return None

    def set_hw(self, hw: int):
        hw = max(0, min(VMAX, int(hw)))
        self.cmd.write(bytes([0x01, OP_VOLUME, hw]) + bytes(61))   # raises OSError if unplugged

    def poll_knob(self):
        """Return the newest knob HW value from 07 25 telemetry, or None."""
        if self.tel is None:
            return None
        val = None
        while True:
            r = self.tel.read(64, 0)
            if not r:
                break
            if len(r) >= 3 and r[0] == 0x07 and r[1] == 0x25:
                val = r[2]
        return val

    def close(self):
        for h in (getattr(self, "cmd", None), getattr(self, "tel", None)):
            try:
                if h: h.close()
            except Exception:
                pass


# ---------- bridge loop ----------
def run():
    comtypes.CoInitialize()
    vol, name = get_omni_endpoint_volume()
    log("endpoint:", name)
    tx = None
    last_hw = None          # the HW value we consider "current" (shared by both sides)
    while True:
        try:
            if tx is None:
                tx = Transmitter()
                # startup: align the transmitter to the current Windows slider position
                last_hw = scalar_to_hw(vol.GetMasterVolumeLevelScalar())
                tx.set_hw(last_hw)
                log("connected; synced transmitter to Windows HW=%d" % last_hw)

            # (1) knob -> Windows slider
            knob = tx.poll_knob()
            if knob is not None and knob != last_hw:
                last_hw = knob
                try:
                    vol.SetMasterVolumeLevelScalar(hw_to_scalar(knob), None)
                except Exception as e:
                    log("set slider failed:", e)
                log("knob -> HW=%d (slider %.0f%%)" % (knob, hw_to_scalar(knob) * 100))

            # (2) Windows slider / media keys -> transmitter
            cur_hw = scalar_to_hw(vol.GetMasterVolumeLevelScalar())
            if cur_hw != last_hw:
                last_hw = cur_hw
                tx.set_hw(cur_hw)
                log("windows -> HW=%d" % cur_hw)

            time.sleep(0.03)
        except OSError:
            log("transmitter lost; reconnecting...")
            try: tx.close()
            except Exception: pass
            tx = None
            time.sleep(1.5)
        except KeyboardInterrupt:
            break
        except Exception as e:
            log("loop error:", e)
            time.sleep(0.5)
    if tx: tx.close()


def main():
    global VERBOSE
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true", help="log to console")
    VERBOSE = ap.parse_args().verbose
    run()


if __name__ == "__main__":
    main()
