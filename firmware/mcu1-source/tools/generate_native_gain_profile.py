"""Generate DSP inverse and UI taper tables from the hash-checked stock image."""
import argparse
import hashlib
import math
from pathlib import Path
import struct

SHA='320dace4ddd866e30787c2ac4220f0b5b4e6d51c48dc2568ce3f6566c5b2c5f2'
ROOT=Path(__file__).resolve().parents[1]

def array(name,kind,values):
    rows=[','.join(str(x) for x in values[i:i+10]) for i in range(0,len(values),10)]
    return f'static const {kind} {name}[]={{\n    '+',\n    '.join(rows)+'\n};\n'

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dsp',required=True,type=Path)
    args=ap.parse_args(); blob=args.dsp.read_bytes()
    assert hashlib.sha256(blob).hexdigest()==SHA
    e089=blob[0x1ff92f:0x1ff92f+61]; e05d=blob[0x1ff184:0x1ff184+204]
    assert hashlib.sha256(e089).hexdigest()=='adfa43084e1ce128b2e36aaeee528fc196b6b8f19d88b370205c1d41fa6ce8b5'
    assert hashlib.sha256(e05d).hexdigest()=='45e6ef0954fecc2aa9c6e353ccdb95c2db10828a2dad1b745a1534fc00566bd0'
    table=struct.unpack_from('<101h',e05d,2)
    curve=[table[min(e089[4+(wire*56+50)//100]*101//100,100)] for wire in range(101)]
    inverse=[curve.index(db*100-600) for db in range(-49,1)]
    out='#include "native_gain.h"\n/* Generated from hash-checked AB15 E089/E05D, stock DSP '+SHA+'. */\n'
    out+=array('wire_by_db','uint8_t',inverse)
    out+='''bool omni_native_gain_wire(int16_t db,bool muted,uint8_t *wire)
{
    if(!wire || db<OMNI_NATIVE_MIN_DB || db>0 || db%256) return false;
    *wire=muted?0u:wire_by_db[(db-OMNI_NATIVE_MIN_DB)/256]; return true;
}
'''
    (ROOT/'src/native_gain_curve.c').write_text(out,encoding='utf8')
    # 35*log10 law reproduces every integer point in the existing measured
    # -60dB Windows profile. Applying it to -49dB is an inference to validate
    # against the new endpoint, not a claim that other OS percentages match.
    def db256(percent):
        floor=10**(-49/35)
        return 35*math.log10(floor+(1-floor)*percent/100)*256
    points=[max(-49*256,min(0,round(db256(p)/256)*256)) for p in range(101)]
    halves=[round(db256(p+.5)) for p in range(100)]
    out='''#include "volume_scale.h"
/* Generated Windows UI taper profile for -49..0dB, whole-dB hardware steps.
 * The 35*log10 taper fits the old measured -60dB profile. This new range
 * needs live Windows verification; percentages on other OSes can differ.
 * USB dB and DSP gain mapping remain OS independent. */
'''+array('percent_db','int16_t',points)+array('half_percent_db','int16_t',halves)+'''
unsigned omni_volume_percent(int16_t db)
{
    unsigned lo=0,hi=100;
    while(lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if(db>=half_percent_db[mid])lo=mid+1;else hi=mid;
    }
    return lo;
}
int16_t omni_volume_percent_db(unsigned percent)
{ return percent_db[percent>100?100:percent]; }
'''
    (ROOT/'src/volume_scale.c').write_text(out,encoding='utf8')
    print('Generated50 exact DSP steps and quantized Windows UI taper profile')

if __name__=='__main__': main()
