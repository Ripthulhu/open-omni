"""MCU1 source application flasher with staged and running readback verification."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
RE=ROOT.parent/'rebuild-re'
sys.path.insert(0,str(RE))
from mcu1_update import validate_image, stage_image, BASE
from read_bootloader import read_region
from flash_verified_mcu1 import REFERENCES

def exchange(handle, opcode, sequence, body=b''):
    packet=(bytes([1,opcode,sequence,0])+body).ljust(64,b'\0')
    if len(packet)!=64: raise RuntimeError('Invalid diagnostic request size')
    sent=handle.send_feature_report(packet)
    if sent<0: raise OSError('Diagnostic transport write failed: '+str(sent))
    if sent!=64: raise RuntimeError('Short diagnostic request write: '+str(sent))
    answer=bytes(handle.get_feature_report(1,64))
    if len(answer)!=64 or answer[:3]!=packet[:3] or answer[3]:
        raise RuntimeError('Invalid diagnostic response: '+answer.hex())
    return answer

def verify_runtime(handle, manifest, image, transport_log=None):
    retries=[] if transport_log is None else transport_log
    def read(opcode,sequence,body=b''):
        # Only idempotent reads may retry OS transport failures. Malformed,
        # stale or negative replies still fail immediately. Recovery/update
        # mutations continue to use strict, single-attempt exchange().
        if opcode not in (1,2,4): raise ValueError('Runtime verifier read allowlist')
        time.sleep(.001) # pace bulk readback while the USB control endpoint is shared
        for attempt in range(3):
            try: return exchange(handle,opcode,sequence,body)
            except OSError as exc:
                retries.append({'opcode':opcode,'sequence':sequence,'body_hex':body.hex(),
                                'attempt':attempt+1,'error':str(exc)})
                if attempt==2: raise
                time.sleep(.02)
    identity=read(1,1)
    if identity[4:10]!=b'OMNI\x01\x01': raise RuntimeError('Wrong runtime protocol or MCU')
    base,end=struct.unpack_from('<II',identity,12)
    build=identity[20:].split(b'\0',1)[0].decode('ascii')
    if build!=manifest['build_id'] or base!=BASE or end!=BASE+manifest['code_length']:
        raise RuntimeError('Running build identity or address bounds mismatch')
    actual=bytearray()
    for offset in range(0,manifest['code_length'],56):
        count=min(56,manifest['code_length']-offset)
        reply=read(2,(offset//56+2)%256,struct.pack('<IB',BASE+offset,count))
        if reply[4]!=count: raise RuntimeError('Wrong readback length')
        actual.extend(reply[8:8+count])
    if bytes(actual)!=image.data[:manifest['code_length']]:
        raise RuntimeError('Running application code readback mismatch')
    result={'build_id':build,'code_length':len(actual),'code_sha256':hashlib.sha256(actual).hexdigest(),
            'transport_retries':list(retries)}
    if 'boot_start_ack' in manifest.get('capabilities',[]):
        if not identity[10] & 8: raise RuntimeError('Missing boot acknowledgement capability')
        for attempt in range(20):
            reply=read(4,attempt)
            status,driver=struct.unpack_from('<Ii',reply,4)
            if status!=0: break
            time.sleep(.05)
        if status not in (1,2) or driver!=0:
            raise RuntimeError(f'Boot acknowledgement not verified: status={status}, driver={driver}')
        result['boot_start_ack']={'status':status,'driver_status':driver}
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('release',type=Path)
    ap.add_argument('--execute',action='store_true')
    ap.add_argument('--verify-only',action='store_true')
    ap.add_argument('--fast',action='store_true',
                    help='skip the redundant full pre-commit staged readback; keep per-block '
                         'ACK, bootloader whole-image CRC pair, and the runtime SHA readback')
    args=ap.parse_args()
    manifest=json.loads((args.release/'manifest.json').read_text())
    image=validate_image((args.release/'application.bin').read_bytes(),manifest)
    n=manifest.get('code_length')
    if manifest.get('runtime')!='omni-hid-v1' or type(n)!=int or not 304<=n<=len(image.data)-4:
        raise RuntimeError('Invalid source runtime manifest')
    if hashlib.sha256(image.data[:n]).hexdigest()!=manifest.get('code_sha256'):
        raise RuntimeError('Code hash mismatch')
    for name,(path,digest) in REFERENCES.items():
        if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
            raise RuntimeError('Restore reference hash mismatch: '+name)
    print('Validated MCU1 source build',image.build_id,image.sha256,flush=True)
    if not args.execute and not args.verify_only:
        print('Dry run passed; restore references also verified.'); return
    import hid
    log=[]
    record={'manifest':manifest,'events':log,'runtime_verified':False}
    h=hid.device()
    try:
        if not args.verify_only:
            devices=[d for d in hid.enumerate(0x1038,0x2291) if d['usage_page']==0xffc0]
            if len(devices)!=1: raise RuntimeError('Expected unique MCU1 physical/software bootloader')
            h.open_path(devices[0]['path'])
            def readback(address,length):
                print('CRC accepted; verifying full staged image...',flush=True)
                return read_region(h,address,length,log)
            print('Writing MCU1 with per-block acknowledgements...',flush=True)
            stage_image(h,image,None if args.fast else readback,log)
            record['staged_readback_verified']=not args.fast
            record['staged_crc_verified']=True
            packet=bytes([1,1,0,1]).ljust(64,b'\0')
            record['commit_issued']=True
            if h.write(packet)!=64: raise RuntimeError('Commit result uncertain: short write')
            h.close()
        deadline=time.monotonic()+25
        while time.monotonic()<deadline:
            devices=[d for d in hid.enumerate(0x1038,0x2290)
                     if d['usage_page']==0xffc0 and d.get('serial_number')==image.build_id]
            if len(devices)==1: break
            if len(devices)>1: raise RuntimeError('Ambiguous source devices')
            time.sleep(.2)
        else: raise RuntimeError('Source application did not enumerate with matching build serial')
        h=hid.device()
        h.open_path(devices[0]['path'])
        record['runtime']=verify_runtime(h,manifest,image,record.setdefault('runtime_transport_retries',[]))
        record['runtime_verified']=True
        print('RUNNING BUILD AND CODE READBACK VERIFIED:',record['runtime'],flush=True)
    except Exception as exc:
        record['error']=str(exc)
        raise
    finally:
        h.close()
        name=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        output=args.release/('hardware-'+name+'.json')
        output.write_text(json.dumps(record,indent=2)+'\n')
        print('Evidence:',output,flush=True)

if __name__=='__main__': main()
