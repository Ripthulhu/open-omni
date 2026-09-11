import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
import zlib

SCRIPT = Path(__file__).resolve().parents[1] / 'flash.py'
spec = importlib.util.spec_from_file_location('public_flash', SCRIPT)
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


class Handle:
    def __init__(self):
        self.writes = []

    def open_path(self, path):
        pass

    def close(self):
        pass

    def write(self, packet):
        self.writes.append(packet)
        return len(packet)


class FlashTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        data = bytearray(b'\xff' * flash.ENVELOPE)
        struct.pack_into('<II', data, 0, 0x20030000, 0xc181)
        data[-4:] = struct.pack('>I', zlib.crc32(data[:-4]))
        self.data = bytes(data)
        self.manifest = dict(chip='LPC5528', role='MCU1', target=1, base=flash.BASE,
                             length=flash.ENVELOPE, format='omni-app-be-crc32-v1',
                             sha256=hashlib.sha256(data).hexdigest(), build_id='test-build',
                             runtime='omni-hid-v1', code_length=512,
                             code_sha256=hashlib.sha256(data[:512]).hexdigest(),
                             capabilities=['boot_start_ack', 'software_recovery'])
        (self.root / 'application.bin').write_bytes(data)
        self.save()

    def save(self):
        (self.root / 'manifest.json').write_text(json.dumps(self.manifest))

    def test_manifest_rejection(self):
        flash.load_release(self.root)
        for key, value in [('target', 2), ('base', 0), ('code_length', -1), ('code_sha256', 'wrong')]:
            original = self.manifest[key]
            self.manifest[key] = value
            self.save()
            with self.assertRaises((RuntimeError, ValueError)):
                flash.load_release(self.root)
            self.manifest[key] = original

    def run_transport(self, failure=None, backup=None):
        handle = Handle()
        hid = types.SimpleNamespace(device=lambda: handle)
        with patch.dict(sys.modules, hid=hid), \
             patch.object(flash, 'bootloader', return_value={'path': b'test'}), \
             patch.object(flash, 'read_region', return_value=self.data if backup is None else backup), \
             patch.object(flash, 'stage_image', side_effect=failure) as stage, \
             patch.object(flash, 'devices', return_value=[{'path': b'test', 'serial_number': 'test-build'}]), \
             patch.object(flash, 'verify_runtime', return_value={'verified': True}) as verify:
            try:
                flash.flash(self.root, self.root / 'backups')
            except (OSError, RuntimeError, ValueError):
                if failure is None and backup is None:
                    raise
            return handle, stage, verify

    def test_negative_ack_never_finalises(self):
        for error in (RuntimeError('negative ACK'), TimeoutError('missing ACK')):
            handle, stage, verify = self.run_transport(error)
            self.assertEqual(handle.writes, [])
            verify.assert_not_called()

    def test_bad_backup_stops_before_erase(self):
        handle, stage, verify = self.run_transport(backup=b'\0' * flash.ENVELOPE)
        stage.assert_not_called()
        self.assertEqual(handle.writes, [])

    def test_success_requires_runtime_verification(self):
        handle, stage, verify = self.run_transport()
        self.assertEqual(len(handle.writes), 1)
        verify.assert_called_once()
        session = next((self.root / 'backups').iterdir())
        self.assertEqual((session / 'application.bin').read_bytes(), self.data)
        self.assertTrue(json.loads((session / 'flash-log.json').read_text())['runtime_verified'])


if __name__ == '__main__':
    unittest.main()
