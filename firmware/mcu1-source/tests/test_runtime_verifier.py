"""Malformed/stale HID responses must never produce a verified runtime."""
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from flash_source import exchange, verify_runtime

class Device:
    def __init__(self, mutate=lambda x:x, ack=1, driver=0, capabilities=11):
        self.mutate=mutate
        self.ack=ack
        self.driver=driver
        self.capabilities=capabilities
    def send_feature_report(self,p):
        self.p=p
        return len(p)
    def get_feature_report(self,report,length):
        r=bytearray(64)
        r[:3]=self.p[:3]
        if self.p[1]==1:
            r[4:10]=b'OMNI\x01\x01'
            r[10]=self.capabilities
            struct.pack_into('<II',r,12,0xc000,0xc008)
            r[20:25]=b'test\0'
        elif self.p[1]==4:
            struct.pack_into('<Ii',r,4,self.ack,self.driver)
        else:
            r[4]=8; r[8:16]=b'12345678'
        return self.mutate(bytes(r))

class Tests(unittest.TestCase):
    def test_os_read_failure_retries_bounded_and_records(self):
        device=Device()
        original=device.get_feature_report
        attempts=[]
        def transient(*args):
            attempts.append(1)
            if len(attempts)<3: raise OSError('transient')
            return original(*args)
        device.get_feature_report=transient
        with patch('flash_source.time.sleep'):
            result=verify_runtime(device,{'build_id':'test','code_length':8},SimpleNamespace(data=b'12345678'))
        self.assertEqual(len(result['transport_retries']),2)
        log=[]
        with patch.object(device,'get_feature_report',side_effect=OSError('persistent')) as get, patch('flash_source.time.sleep'):
            with self.assertRaises(OSError):
                verify_runtime(device,{'build_id':'test','code_length':8},SimpleNamespace(data=b'12345678'),log)
        self.assertEqual(get.call_count,3)
        self.assertEqual(len(log),3)
    def test_mutation_transport_failure_never_retries(self):
        device=Device()
        with patch.object(device,'get_feature_report',side_effect=OSError('failure')) as get:
            with self.assertRaises(OSError): exchange(device,5,1,b'BOOT')
        self.assertEqual(get.call_count,1)
    def test_negative_write_transport_failure_and_partial_write_rejection(self):
        for count,exception in ((-1,OSError),(4,RuntimeError)):
            device=Device()
            with patch.object(device,'send_feature_report',return_value=count) as send:
                with self.assertRaises(exception): exchange(device,5,1,b'BOOT')
            self.assertEqual(send.call_count,1)
    def test_valid_readback(self):
        self.assertEqual(verify_runtime(Device(),{'build_id':'test','code_length':8},
                         SimpleNamespace(data=b'12345678'))['code_length'],8)
    def test_bad_replies(self):
        for mutate in (lambda x:x[:10],lambda x:b'\x02'+x[1:],
                       lambda x:x[:2]+b'\xff'+x[3:],lambda x:x[:3]+b'\x01'+x[4:]):
            with self.subTest(mutate=mutate), self.assertRaises(RuntimeError):
                exchange(Device(mutate),1,1)
    def test_wrong_build(self):
        with self.assertRaises(RuntimeError):
            verify_runtime(Device(),{'build_id':'other','code_length':8},SimpleNamespace(data=b'12345678'))
    def test_wrong_readback(self):
        with self.assertRaises(RuntimeError):
            verify_runtime(Device(),{'build_id':'test','code_length':8},SimpleNamespace(data=b'87654321'))
    def test_acknowledgement_required(self):
        manifest={'build_id':'test','code_length':8,'capabilities':['boot_start_ack']}
        for ack in (1,2):
            result=verify_runtime(Device(ack=ack),manifest,SimpleNamespace(data=b'12345678'))
            self.assertEqual(result['boot_start_ack'],{'status':ack,'driver_status':0})
        with patch('flash_source.time.sleep'):
            for ack,driver,cap in [(0,0,11),(1,-1003,11),(2,101,11),(1,0,3)]+[(s,0,11) for s in range(3,11)]:
                with self.subTest(ack=ack,driver=driver,cap=cap), self.assertRaises(RuntimeError):
                    verify_runtime(Device(ack=ack,driver=driver,capabilities=cap),manifest,
                                   SimpleNamespace(data=b'12345678'))
    def test_acknowledgement_wait_then_success(self):
        calls=[]
        def mutate(reply):
            if reply[1]==4:
                calls.append(reply[1])
                if len(calls)==1: return reply[:4]+bytes(8)+reply[12:]
            return reply
        with patch('flash_source.time.sleep') as sleep:
            result=verify_runtime(Device(mutate),
                {'build_id':'test','code_length':8,'capabilities':['boot_start_ack']},
                SimpleNamespace(data=b'12345678'))
        self.assertEqual(result['boot_start_ack']['status'],1)
        self.assertEqual(len(calls),2)
        self.assertEqual([call.args[0] for call in sleep.call_args_list].count(.05),1)

if __name__=='__main__': unittest.main()
