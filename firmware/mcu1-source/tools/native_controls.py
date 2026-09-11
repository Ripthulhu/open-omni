"""OS-independent native firmware controls. Requires matching release identity.

No GG or volume bridge. All writes enqueue bounded main-loop transactions.
ACK acceptance is reported separately from independent DSP readback.
"""
import argparse,json,secrets,struct,time
from pathlib import Path
from flash_source import exchange

SETTINGS={'limiter':1,'mic-volume':2,'sidetone':3,'mic-noise':4,'anc-state':5,
          'anc-level':6,'transparency':7,'bt-startup':8,'mic-led':9,'bt-call':10,
          'auto-off':11,'eq-wireless':12,'eq-mic':13,'eq-bt':14,'output-mode':15}
CACHES={**SETTINGS,'mic-state':16,'bt-state':17}

class Native:
 def __init__(self,release):
  import hid
  self.manifest=json.loads((Path(release)/'manifest.json').read_text());self.seq=0
  ds=[d for d in hid.enumerate(0x1038,0x2290) if d['usage_page']==0xffc0]
  if len(ds)!=1 or ds[0].get('serial_number')!=self.manifest['build_id']:raise RuntimeError('Expected matching native build')
  self.h=hid.device();self.h.open_path(ds[0]['path'])
  ident=self.call(1)
  if ident[4:10]!=b'OMNI\x01\x01' or ident[20:].split(b'\0')[0].decode()!=self.manifest['build_id']:
   self.close();raise RuntimeError('Native MCU1 identity mismatch')
 def close(self):self.h.close()
 def call(self,op,body=b''):
  self.seq=(self.seq+1)%256
  return exchange(self.h,op,self.seq,body)
 def words(self,op,page=0):return list(struct.unpack_from('<15I',self.call(op,bytes([page])),4))
 def wait_setting(self,token,seconds=4):
  end=time.monotonic()+seconds
  while time.monotonic()<end:
   w=self.words(62)
   if w[2]!=token:raise RuntimeError('Setting transaction replaced by another owner')
   if not w[5]&1:
    if w[4]!=6:raise RuntimeError('Setting failed: '+json.dumps(w))
    return {'status':w,'details':self.words(62,1),'accepted':True}
   time.sleep(.02)
  raise TimeoutError('Setting deadline exceeded')
 def setting(self,name,values):
  control=SETTINGS[name];blob=bytes(values);token=secrets.randbelow(0x7fffffff)+1
  if len(blob)<=54:self.call(61,struct.pack('<IBB',token,control,len(blob))+blob)
  else:
   for offset in range(0,len(blob),52):
    part=blob[offset:offset+52]
    self.call(64,struct.pack('<IBBBB',token,control,len(blob),offset,len(part))+part)
   self.call(65,struct.pack('<I',token))
  return self.wait_setting(token)
 def preset(self,name,preset):
  token=secrets.randbelow(0x7fffffff)+1
  self.call(70,struct.pack('<IBB',token,SETTINGS[name],preset));return self.wait_setting(token)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('release',type=Path)
 p.add_argument('--output',type=Path)
 sub=p.add_subparsers(dest='action',required=True)
 sub.add_parser('status')
 s=sub.add_parser('set');s.add_argument('setting',choices=SETTINGS);s.add_argument('values',type=int,nargs='+')
 s=sub.add_parser('preset');s.add_argument('setting',choices=['eq-wireless','eq-mic','eq-bt']);s.add_argument('index',type=int)
 s=sub.add_parser('eq-blob');s.add_argument('setting',choices=['eq-wireless','eq-mic','eq-bt']);s.add_argument('file',type=Path)
 s=sub.add_parser('cache');s.add_argument('setting',choices=CACHES)
 s=sub.add_parser('bias');s.add_argument('mode',type=int,choices=[0,1]);s.add_argument('position',type=int,choices=range(25))
 s=sub.add_parser('input');s.add_argument('side',type=int,choices=[0,1])
 s=sub.add_parser('display');s.add_argument('timeout_index',type=int,choices=range(7));s.add_argument('brightness',type=int,choices=range(1,11));s.add_argument('saver',type=int,choices=[0,1]);s.add_argument('simple',type=int,choices=[0,1])
 s=sub.add_parser('mixer');s.add_argument('input',type=int,choices=[0,3]);s.add_argument('level',type=int,choices=range(101));s.add_argument('--linked',action='store_true');s.add_argument('--muted',action='store_true')
 a=p.parse_args();n=Native(a.release)
 try:
  if a.action=='status':result={'build':n.manifest['build_id'],'mcu2':[n.words(66,i) for i in range(4)],'gain':[n.words(50,i) for i in range(3)],'headset':n.words(57),'settings':n.words(62),'menu':[n.words(71,i) for i in range(2)],'display':list(struct.unpack_from('<15I',n.call(68),4))}
  elif a.action=='set':result=n.setting(a.setting,a.values)
  elif a.action=='preset':result=n.preset(a.setting,a.index)
  elif a.action=='eq-blob':result=n.setting(a.setting,a.file.read_bytes())
  elif a.action=='cache':result={'pages':[n.call(63,bytes([CACHES[a.setting],i]))[4:].hex() for i in range(4 if a.setting=='eq-wireless' else 3 if a.setting.startswith('eq-') else 1)]}
  elif a.action=='bias':result={'queued':n.call(73,bytes([1,a.mode,a.position]))[3]==0,'bias':n.words(73)}
  elif a.action=='input':result={'queued':n.call(67,bytes([a.side]))[3]==0,'selection':n.words(66,2),'mcu2':n.words(66,1)}
  elif a.action=='display':result={'display':list(struct.unpack_from('<15I',n.call(68,bytes([1,a.timeout_index,a.brightness,a.saver,a.simple,0])),4))}
  else:result={'mixer':list(struct.unpack_from('<15I',n.call(69,bytes([a.input,a.level,int(a.linked)|int(a.muted)<<1,1])),4))}
  if a.action=='status' and 'headset_gain_v1' in n.manifest.get('capabilities',[]):result['headset_gain']=[n.words(72,i) for i in range(2)]
  if a.action=='status' and 'source_bias_v1' in n.manifest.get('capabilities',[]):result.update(bias=n.words(73),mcu2_source_gain=n.words(66,4))
  if a.output:a.output.write_text(json.dumps(result,indent=2)+'\n')
  print(json.dumps(result,indent=2))
 finally:n.close()
if __name__=='__main__':main()
