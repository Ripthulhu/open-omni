"""Create a portable flasher bundle without local firmware dumps or restore data."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def package(release, output):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        bundle = Path(temporary)
        for path in (ROOT / 'tools/flash').iterdir():
            if path.is_file():
                shutil.copy2(path, bundle / path.name)
        (bundle / 'firmware').mkdir()
        for name in ('application.bin', 'manifest.json', 'omni_mcu1.elf', 'omni_mcu1.map', 'dependencies.lock.json'):
            shutil.copy2(release / name, bundle / 'firmware' / name)
        (bundle / 'lib').mkdir()
        for name in ('flash_source.py', 'enter_recovery.py'):
            shutil.copy2(ROOT / 'firmware/mcu1-source/tools' / name, bundle / 'lib' / name)
        for name in ('mcu1_update.py', 'read_bootloader.py', 'flash_verified_mcu1.py'):
            shutil.copy2(ROOT / 'firmware/rebuild-re' / name, bundle / 'lib' / name)
        archive = Path(shutil.make_archive(str(output / 'open-omni'), 'zip', bundle))
        (output / 'SHA256SUMS').write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
        return archive


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('release', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    args = parser.parse_args()
    print(package(args.release, args.output))
