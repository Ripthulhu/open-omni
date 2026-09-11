"""Enter MCU1 recovery after verifying the installed source release; dry-run by default."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import secrets
import struct
import time

from flash_source import exchange, verify_runtime
from mcu1_update import validate_image
from read_bootloader import read_region


def prepare(handle, token):
    identity=exchange(handle,1,1)
    if identity[4:10]!=b'OMNI\x01\x01' or not identity[10]&16:
        raise RuntimeError('MCU1 source software recovery capability missing')
    exchange(handle,5,2,struct.pack('<I',token)+b'BOOT')
    for attempt in range(50):
        answer=exchange(handle,6,(attempt+3)%256)
        state,echo,result,driver=struct.unpack_from('<IIIi',answer,4)
        if echo!=token: raise RuntimeError('Recovery session token mismatch')
        if state==2 and result in (1,2) and driver==0:
            return {'token':token,'state':state,'result':result,'driver':driver}
        if state!=1: raise RuntimeError(f'Recovery preparation failed: {state}, {result}, {driver}')
        time.sleep(.05)
    raise RuntimeError('Recovery preparation timed out; reset not issued')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('release',type=Path)
    ap.add_argument('--execute',action='store_true')
    args=ap.parse_args()
    manifest=json.loads((args.release/'manifest.json').read_text())
    image=validate_image((args.release/'application.bin').read_bytes(),manifest)
    if 'software_recovery' not in manifest.get('capabilities',[]):
        raise RuntimeError('This release does not support software recovery')
    length=manifest['code_length']
    if not 304<=length<=len(image.data)-4 or hashlib.sha256(image.data[:length]).hexdigest()!=manifest['code_sha256']:
        raise RuntimeError('Invalid code bounds/hash')
    if not args.execute:
        print('Dry run passed:',image.build_id); return
    import hid
    log=[]
    record={'build_id':image.build_id,'bootloader_verified':False,'events':log}
    h=None
    try:
        devices=[d for d in hid.enumerate(0x1038,0x2290) if d['usage_page']==0xffc0]
        if len(devices)!=1 or devices[0].get('serial_number')!=image.build_id:
            raise RuntimeError('Expected unique matching MCU1 source device')
        if any(d['usage_page']==0xffc0 for d in hid.enumerate(0x1038,0x2291)):
            raise RuntimeError('Bootloader already present; refusing ambiguous transition')
        h=hid.device(); h.open_path(devices[0]['path'])
        record['runtime']=verify_runtime(h,manifest,image,record.setdefault('runtime_transport_retries',[]))
        token=secrets.randbelow(0xffffffff)+1
        record['preparation']=prepare(h,token)
        packet=(bytes([1,7,99,0])+struct.pack('<I',token)+b'BOOT').ljust(64,b'\0')
        record['reset_issued']=True
        # Disconnect can interrupt this transfer. Never retry the reset; prove
        # the resulting device and code through a separate read-only session.
        try: record['commit_write_count']=h.send_feature_report(packet)
        except OSError as exc: record['commit_disconnect']=str(exc)
        h.close(); h=None
        deadline=time.monotonic()+25
        while time.monotonic()<deadline:
            devices=[d for d in hid.enumerate(0x1038,0x2291) if d['usage_page']==0xffc0]
            if len(devices)>1: raise RuntimeError('Ambiguous bootloader devices')
            if len(devices)==1: break
            time.sleep(.2)
        else: raise RuntimeError('MCU1 bootloader did not enumerate; outcome uncertain')
        h=hid.device(); h.open_path(devices[0]['path'])
        code=read_region(h,0xc000,length,log)
        if code!=image.data[:length]: raise RuntimeError('Bootloader code readback mismatch')
        record['code_sha256']=hashlib.sha256(code).hexdigest()
        record['metadata_hex']=read_region(h,0x7f800,512,log).hex()
        record['bootloader_verified']=True
        print('MCU1 bootloader entry and installed code verified. No image flashed.')
    except Exception as exc:
        record['error']=str(exc); raise
    finally:
        if h: h.close()
        output=args.release/('software-recovery-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'.json')
        output.write_text(json.dumps(record,indent=2)+'\n')
        print('Evidence:',output)


if __name__=='__main__': main()
