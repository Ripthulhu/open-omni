import sys
import unittest
from pathlib import Path
from unittest.mock import patch
import struct

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import enter_recovery


class RecoveryToolTests(unittest.TestCase):
    def run_prepare(self, states, capability=True):
        calls=[]
        def exchange(handle,op,seq,body=b''):
            calls.append(op)
            result=bytearray(64)
            if op==1:
                result[4:10]=b'OMNI\x01\x01'; result[10]=16 if capability else 0
            elif op==5:
                self.assertEqual(body,struct.pack('<I',123)+b'BOOT')
            elif op==6:
                struct.pack_into('<IIIi',result,4,*states.pop(0))
            else: self.fail('Preparation must never issue reset')
            return result
        with patch.object(enter_recovery,'exchange',side_effect=exchange), patch.object(enter_recovery.time,'sleep'):
            return enter_recovery.prepare(object(),123),calls

    def test_ready_after_pending(self):
        result,calls=self.run_prepare([(1,123,0,0),(2,123,1,0)])
        self.assertEqual(result['result'],1)
        self.assertEqual(calls,[1,5,6,6])

    def test_idempotent_ready(self):
        self.assertEqual(self.run_prepare([(2,123,2,0)])[0]['result'],2)

    def test_failed_or_inconsistent_states(self):
        for state in ((3,123,7,-1),(2,123,1,-1),(2,123,0,0),(0,123,0,0),(2,999,1,0)):
            with self.subTest(state=state),self.assertRaises(RuntimeError): self.run_prepare([state])

    def test_timeout(self):
        with self.assertRaisesRegex(RuntimeError,'timed out'): self.run_prepare([(1,123,0,0)]*50)

    def test_old_firmware(self):
        with self.assertRaisesRegex(RuntimeError,'capability'): self.run_prepare([],False)


if __name__=='__main__': unittest.main()
