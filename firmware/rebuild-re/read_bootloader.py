"""Read-only MCU1 bootloader protocol 0x83, recovered from boot function 0x5210.

Never sends begin, erase, write, finalize, or reset. Requires unique 1038:2291.
First verify against captured application bytes before acquiring unknown regions.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time

def read_region(handle, address, length, transcript):
    if not 0xc000 <= address < 0x80000 or not 0 < length <= 0x80000-address:
        raise ValueError('Read must stay within MCU1 [0xc000,0x80000)')
    result = bytearray()
    while len(result) < length:
        count = min(32, length-len(result))
        offset = address+len(result)-0xc000
        request = bytes([1, 0x83, 1, 1])+struct.pack('<HI', count, offset)
        packet = request.ljust(64, b'\0')
        if handle.write(packet) != len(packet):
            raise IOError('Short HID request write')
        deadline = time.monotonic()+2
        while True:
            reply = bytes(handle.read(64, 100))
            if reply:
                transcript.append({'address': hex(address+len(result)), 'tx': packet.hex(), 'rx': reply.hex()})
            if reply[:2] == b'\x01\x83':
                if len(reply) < 3+count or reply[2] != 0:
                    raise IOError('Rejected or short bootloader read: '+reply.hex())
                result.extend(reply[3:3+count])
                break
            if time.monotonic() >= deadline:
                raise TimeoutError('No matching 0x83 reply; no writes or retries attempted')
    return bytes(result)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--address', type=lambda n:int(n,0), default=0xc000)
    ap.add_argument('--length', type=lambda n:int(n,0), default=32)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--compare', type=Path)
    args = ap.parse_args()
    if args.output.exists() or args.output.with_suffix('.json').exists():
        raise SystemExit('Refusing to overwrite existing evidence')
    import hid
    devices = [d for d in hid.enumerate(0x1038,0x2291) if d['usage_page']==0xffc0]
    if len(devices) != 1:
        raise SystemExit(f'Expected one MCU1 bootloader command collection, found {len(devices)}')
    h = hid.device()
    transcript = []
    try:
        h.open_path(devices[0]['path'])
        h.set_nonblocking(False)
        blob = read_region(h,args.address,args.length,transcript)
        if args.compare and blob != args.compare.read_bytes()[:len(blob)]:
            raise ValueError('Read does not match reference; acquisition refused')
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_bytes(blob)
        print(f'Read {len(blob)} bytes; SHA256 {hashlib.sha256(blob).hexdigest()}')
    finally:
        h.close()
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.with_suffix('.json').write_text(json.dumps({'address':hex(args.address),
            'requested_length':args.length,'transcript':transcript},indent=2)+'\n')

if __name__ == '__main__':
    main()
