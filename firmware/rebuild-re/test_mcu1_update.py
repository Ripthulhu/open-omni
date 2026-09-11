import hashlib
import struct
import unittest
import zlib
from mcu1_update import *

def fixture():
    data = bytearray(b'\xff'*ENVELOPE)
    struct.pack_into('<II',data,0,0x20030000,0xc181)
    data[-4:] = struct.pack('>I',zlib.crc32(data[:-4]))
    manifest = dict(chip='LPC5528',role='MCU1',target=1,base=BASE,length=ENVELOPE,
        format='omni-app-be-crc32-v1',sha256=hashlib.sha256(data).hexdigest(),build_id='offline-test')
    return data,manifest

class FakeHid:
    def __init__(self,image,fail=None):
        self.image=image; self.fail=fail; self.queue=[]; self.writes=[]
    def read(self,*args):
        return self.queue.pop(0) if self.queue else []
    def send_feature_report(self,packet):
        self.writes.append(packet)
        self.queue.append(bytes([1,3,3 if self.fail=='status' else 0,0,0,0]))
        return len(packet)-1 if self.fail=='short' else len(packet)
    def write(self,packet):
        self.writes.append(packet)
        self.queue.append(bytes([1,0x84,0])+struct.pack('>II',self.image.crc32,self.image.crc32 ^ (self.fail=='crc')))
        return len(packet)

class UpdateTests(unittest.TestCase):
    def test_valid_and_wrong_target(self):
        data,m=fixture(); validate_image(data,m)
        for key,bad in [('target',2),('target',True),('base',0),('chip','LPC5516'),('format','raw')]:
            with self.subTest(key=key,bad=bad), self.assertRaises(UpdateError):
                validate_image(data,{**m,key:bad})
    def test_corruption_and_raw_dump(self):
        data,m=fixture()
        with self.assertRaises(UpdateError): validate_image(b'\0'*0xc000+data,m)
        data[512]^=1
        with self.assertRaises(UpdateError): validate_image(data,m)
        m['sha256']=hashlib.sha256(data).hexdigest()
        with self.assertRaises(UpdateError): validate_image(data,m)
    def test_vectors(self):
        for sp,reset in [(0x20040000,0xc181),(0x20030000,0x181),(0x20030000,0xc180)]:
            data,m=fixture(); struct.pack_into('<II',data,0,sp,reset)
            data[-4:]=struct.pack('>I',zlib.crc32(data[:-4])); m['sha256']=hashlib.sha256(data).hexdigest()
            with self.assertRaises(UpdateError): validate_image(data,m)
    def test_block_bounds(self):
        self.assertEqual(block_report(0,b'abc')[:13],bytes.fromhex('01030101030000000000616263'))
        for off,payload in [(1,b'a'),(-1012,b'a'),(0,b''),(0,b'a'*1013),(ENVELOPE,b'a')]:
            with self.assertRaises(UpdateError): block_report(off,payload)
    def test_staging_never_commits(self):
        image=validate_image(*fixture()); h=FakeHid(image); log=[]
        self.assertEqual(stage_image(h,image,lambda a,n:image.data,log),image.sha256)
        self.assertFalse(log[-1]['running_verified'])
        self.assertTrue(all(p[1] in (3,0x84) for p in h.writes))
    def test_failures_stop(self):
        image=validate_image(*fixture())
        for fault in ['short','status','crc']:
            h=FakeHid(image,fault)
            with self.subTest(fault=fault), self.assertRaises(UpdateError):
                stage_image(h,image,lambda a,n:image.data,[])
            if fault in ('short','status'): self.assertEqual(len(h.writes),1)
        with self.assertRaises(UpdateError): stage_image(FakeHid(image),image,lambda a,n:b'',[])
    def test_reply_filtering(self):
        h=FakeHid(None); h.queue=[b'\x07\x25\x20',b'\x01\x03\0\0\0\0']
        self.assertEqual(await_reply(h,3,6)[1],3)
        for response in [b'\x01\x84\0',b'\x01\x03\0',b'\0'*64]:
            h.queue=[response]
            with self.assertRaises(UpdateError): await_reply(h,3,6)
        with self.assertRaises(UpdateError): await_reply(h,3,6,timeout=0)
    def test_stale_ack_refused_before_writes(self):
        image=validate_image(*fixture()); h=FakeHid(image); h.queue=[b'\x01\x03\0\0\0\0']
        with self.assertRaises(UpdateError): stage_image(h,image,lambda a,n:image.data,[])
        self.assertEqual(h.writes,[])

if __name__=='__main__': unittest.main()
