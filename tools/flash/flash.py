"""Flash a packaged Open Omni MCU1 application on Windows, Linux or macOS."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import secrets
import struct
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path[:0] = [str(HERE / 'lib'), str(ROOT / 'firmware/mcu1-source/tools'),
                str(ROOT / 'firmware/rebuild-re')]
from mcu1_update import BASE, ENVELOPE, validate_image, stage_image
from read_bootloader import read_region
from flash_source import exchange, verify_runtime
from enter_recovery import prepare


def load_release(path):
    manifest = json.loads((path / 'manifest.json').read_text())
    image = validate_image((path / 'application.bin').read_bytes(), manifest)
    length = manifest.get('code_length')
    if (manifest.get('runtime') != 'omni-hid-v1' or type(length) is not int
            or not 304 <= length <= ENVELOPE - 4
            or hashlib.sha256(image.data[:length]).hexdigest() != manifest.get('code_sha256')):
        raise ValueError('Invalid source code bounds, runtime or hash')
    if not {'boot_start_ack', 'software_recovery'} <= set(manifest.get('capabilities', [])):
        raise ValueError('Firmware lacks required recovery capabilities')
    return manifest, image


def devices(hid, pid):
    return [d for d in hid.enumerate(0x1038, pid) if d['usage_page'] == 0xffc0]


def bootloader(hid):
    boot = devices(hid, 0x2291)
    running = devices(hid, 0x2290)
    if len(boot) + len(running) > 1:
        raise RuntimeError('Connect only one transmitter on USB1; MCU1 selection is ambiguous')
    if boot:
        return boot[0]
    if running:
        handle = hid.device()
        try:
            handle.open_path(running[0]['path'])
            identity = exchange(handle, 1, 1)
            if identity[4:10] != b'OMNI\x01\x01':
                raise RuntimeError('Not an Open Omni MCU1 application')
            token = secrets.randbelow(0xffffffff) + 1
            prepare(handle, token)
            packet = (bytes([1, 7, 99, 0]) + struct.pack('<I', token) + b'BOOT').ljust(64, b'\0')
            try:
                if handle.send_feature_report(packet) != 64:
                    raise RuntimeError('Recovery write was short; outcome uncertain')
            except OSError:
                pass  # A reset may disconnect before the write returns. Never retry it.
        finally:
            handle.close()
    else:
        print('Unplug all transmitter power. Hold the dial, connect USB1, keep holding five seconds, then release.', flush=True)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        boot = devices(hid, 0x2291)
        if len(boot) > 1:
            raise RuntimeError('Multiple MCU1 bootloaders connected')
        if boot:
            return boot[0]
        time.sleep(.25)
    raise RuntimeError('MCU1 recovery (1038:2291) not found. No application written.')


def flash(release, backup_root):
    manifest, image = load_release(release)
    import hid
    device = bootloader(hid)
    session = backup_root / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    session.mkdir(parents=True, exist_ok=False)
    log = []
    record = {'requested_build': image.build_id, 'runtime_verified': False}
    handle = hid.device()
    try:
        handle.open_path(device['path'])
        print('Saving the installed MCU1 application before writing...', flush=True)
        original = read_region(handle, BASE, ENVELOPE, log)
        backup = dict(chip='LPC5528', role='MCU1', target=1, base=BASE,
                      length=ENVELOPE, format='omni-app-be-crc32-v1',
                      sha256=hashlib.sha256(original).hexdigest(), build_id='pre-flash-backup')
        validate_image(original, backup)
        (session / 'application.bin').write_bytes(original)
        (session / 'manifest.json').write_text(json.dumps(backup, indent=2) + '\n')
        if (session / 'application.bin').read_bytes() != original:
            raise RuntimeError('Backup file verification failed')
        print(f'Backup verified: {session}\nFlashing {image.build_id} with block ACK and whole-image CRC checks...', flush=True)
        stage_image(handle, image, None, log)
        record['loader_crc_verified'] = True
        if handle.write(bytes([1, 1, 0, 1]).ljust(64, b'\0')) != 64:
            raise RuntimeError('Short finalisation write; outcome uncertain')
        handle.close()
        handle = None
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            found = devices(hid, 0x2290)
            if len(found) > 1:
                raise RuntimeError('Multiple running MCU1 devices')
            if found and found[0].get('serial_number') == image.build_id:
                break
            time.sleep(.25)
        else:
            raise RuntimeError('Firmware did not enumerate. Power-cycle USB1, then use --verify. Do not flash again blindly.')
        handle = hid.device()
        handle.open_path(found[0]['path'])
        record['runtime'] = verify_runtime(handle, manifest, image)
        record['runtime_verified'] = True
        print('Verified running build, complete code readback and boot acknowledgement.', flush=True)
    except Exception as exc:
        record['error'] = str(exc)
        raise
    finally:
        if handle:
            handle.close()
        record['events'] = log
        (session / 'flash-log.json').write_text(json.dumps(record, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('release', nargs='?', type=Path, default=HERE / 'firmware')
    parser.add_argument('--check', action='store_true', help='Validate package without USB access')
    parser.add_argument('--verify', action='store_true', help='Read back an already installed build without flashing')
    parser.add_argument('--backups', type=Path, default=Path.home() / 'OpenOmni-backups')
    args = parser.parse_args()
    manifest, image = load_release(args.release)
    if args.check:
        print(f'Valid MCU1 package: {image.build_id} {image.sha256}')
    elif args.verify:
        import hid
        found = devices(hid, 0x2290)
        if len(found) != 1:
            raise RuntimeError('Expected one running MCU1')
        handle = hid.device()
        try:
            handle.open_path(found[0]['path'])
            print(json.dumps(verify_runtime(handle, manifest, image), indent=2))
        finally:
            handle.close()
    else:
        flash(args.release, args.backups)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError) as exc:
        print(f'Flash stopped: {exc}', file=sys.stderr)
        sys.exit(1)
