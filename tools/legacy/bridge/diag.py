import time, hid

VID, PID = 0x1038, 0x2290

def paths():
    out = {}
    for d in hid.enumerate(VID, PID):
        out.setdefault(d["usage_page"], d)
    return out

def open_up(up):
    p = paths().get(up)
    if not p:
        return None, None
    dev = hid.device()
    dev.open_path(p["path"])
    return dev, p

def dump(dev, secs):
    end = time.time() + secs
    seen = 0
    while time.time() < end:
        data = dev.read(64, 200)
        if data:
            seen += 1
            print("  RX:", " ".join(f"{b:02X}" for b in data[:12]), "...")
    return seen

for up in (0xFF00, 0xFFC0):
    dev, p = open_up(up)
    if not dev:
        print(f"up=0x{up:04X}: not found"); continue
    print(f"== up=0x{up:04X}  if={p['interface_number']} ==")
    # 1) write-channel check: send GET (07 24); report the byte count
    try:
        n = dev.write(bytes([0x07, 0x24]) + bytes(62))
        print(f"  write(07 24) returned {n}")
    except Exception as e:
        print(f"  write error: {e}")
    # 2) listen briefly for any telemetry (turn the knob 1 notch during this window)
    print("  listening 3s (turn the volume knob one notch now)...")
    got = dump(dev, 3)
    print(f"  telemetry frames: {got}")
    dev.close()
