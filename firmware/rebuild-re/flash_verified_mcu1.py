"""MCU1-only restore runner using bootloader ACK, CRC-pair and full readback.

Only the two previously verified reference images are accepted. Source-built
images need a separate runtime-identity verifier before this allowlist expands.
Physical recovery has been identified at 1038:2291 after a four-second dial hold.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time
from mcu1_update import BASE, ENVELOPE, validate_image, stage_image
from read_bootloader import read_region

ROOT=Path(__file__).resolve().parent
REFERENCES={
    'stock':(ROOT.parent.parent/'qr-patch/omni_5528.bin','75b41488edecbddfc672734f86f0b163b9486cd87de2aa28a656e61dcd80ca16'),
    'diagnostic':(ROOT/'build/omni_mcu1_5528_dump_v2.bin','3d0aee01218280cfade6eafc81c77e3a773c1bf6eb684cbbedc5c612214ef096'),
}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('reference',choices=REFERENCES)
    ap.add_argument('--execute',action='store_true')
    args=ap.parse_args()
    path,digest=REFERENCES[args.reference]
    manifest=dict(chip='LPC5528',role='MCU1',target=1,base=BASE,length=ENVELOPE,
        format='omni-app-be-crc32-v1',sha256=digest,build_id=args.reference)
    image=validate_image(path.read_bytes(),manifest)
    print('Validated',args.reference,image.sha256,flush=True)
    if not args.execute:
        print('Dry run only. Execute requires the unique MCU1 1038:2291 bootloader.')
        return
    import hid
    ds=[d for d in hid.enumerate(0x1038,0x2291) if d['usage_page']==0xffc0]
    if len(ds)!=1: raise RuntimeError('Expected one MCU1 bootloader; no flash attempted')
    log=[]
    record={'timestamp':datetime.now(timezone.utc).isoformat(),'manifest':manifest,
            'application_readback_verified':False,'runtime_identity_verified':False,'events':log}
    h=hid.device()
    h.open_path(ds[0]['path'])
    h.set_nonblocking(False)
    try:
        def readback(address,length):
            print('CRC accepted; reading back complete staged application...',flush=True)
            return read_region(h,address,length,log)
        print('Writing MCU1 application with per-block ACK validation...',flush=True)
        stage_image(h,image,readback,log)
        record['application_readback_verified']=True
        print('Exact readback verified. Returning to application...',flush=True)
        # Recovered command disables USB, flushes metadata/cache, then schedules
        # the application handoff. It does not have a conventional ACK.
        packet=bytes([1,1,0,1]).ljust(64,b'\0')
        record['commit_issued']=True
        if h.write(packet)!=64: raise RuntimeError('Short commit write; outcome uncertain')
        h.close()
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            if any(d['usage_page']==0xffc0 for d in hid.enumerate(0x1038,0x2290)):
                record['application_enumerated']=True
                print('Application enumerated; functional confirmation remains required.',flush=True)
                break
            time.sleep(.2)
        else: raise RuntimeError('Application did not enumerate after verified staging')
    except Exception as exc:
        record['error']=str(exc)
        raise
    finally:
        h.close()
        output=ROOT/'logs'/('verified-restore-'+args.reference+'-'+datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'.json')
        output.write_text(json.dumps(record,indent=2)+'\n')
        print('Evidence:',output,flush=True)

if __name__=='__main__': main()
