"""Reproducible MCU1 build and release packaging."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT.parent/'rebuild-re'))
from flash_verified_mcu1 import REFERENCES
def run(*args):
    return subprocess.check_output(list(map(str,args)),text=True,stderr=subprocess.STDOUT)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--vendor',type=Path,required=True)
    ap.add_argument('--build',type=Path,required=True)
    ap.add_argument('--public',action='store_true',help='Package source firmware without private stock restore images')
    args=ap.parse_args()
    restores={}
    for name,(path,digest) in ({} if args.public else REFERENCES).items():
        data=path.read_bytes()
        if hashlib.sha256(data).hexdigest()!=digest:
            raise RuntimeError('Restore reference hash mismatch: '+name)
        restores[name]=(data,digest)
    lock=json.loads((ROOT/'dependencies.lock.json').read_text())
    for dep in lock:
        repo=args.vendor/dep['name']
        if run('git','-C',repo,'rev-parse','HEAD').strip()!=dep['revision'] or run('git','-C',repo,'status','--porcelain').strip():
            raise RuntimeError('Dependency differs from lock: '+dep['name'])
    sources={}
    # Generated tool environments/dependencies are not firmware source. Prune
    # before recursion so a host venv cannot enter the identity/restore package.
    def source_paths(directory):
        for path in sorted(directory.iterdir()):
            if path.is_dir():
                if path.name in ('releases','__pycache__','vendor','build','.venv') or path.name.startswith('build-'):
                    continue
                yield from source_paths(path)
            elif path.is_file():
                yield path
    for path in source_paths(ROOT):
        if path.suffix not in ('.c','.h','.inc','.S','.ld','.cmake','.py','.json') and path.name!='CMakeLists.txt': continue
        sources[path.relative_to(ROOT).as_posix()]=hashlib.sha256(path.read_bytes()).hexdigest()
    digest=hashlib.sha256(json.dumps(sources,sort_keys=True).encode()).hexdigest()
    build_id='omni-a-'+digest[:16]
    print(run('cmake','-S',ROOT,'-B',args.build,'-G','Ninja',
              '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'cmake/arm-none-eabi.cmake'),
              '-DOMNI_VENDOR='+str(args.vendor),'-DOMNI_BUILD_ID='+build_id))
    print(run('cmake','--build',args.build))
    elf=args.build/'omni_mcu1.elf'
    symbols={}
    for line in run('arm-none-eabi-nm','-n',elf).splitlines():
        fields=line.split()
        if len(fields)==3: symbols[fields[2]]=int(fields[0],16)
    raw=args.build/'application.code.bin'
    run('arm-none-eabi-objcopy','-O','binary',elf,raw)
    code=raw.read_bytes()
    assert len(code)==symbols['__image_end']-0xc000
    assert len(code)<=285608 and symbols['__bss_end']<=0x2002e000
    sp,reset=struct.unpack_from('<II',code)
    assert sp==0x20030000 and reset==symbols['Reset_Handler']|1
    vectors=struct.unpack_from('<76I',code)
    assert vectors[44]==symbols['USB0_IRQHandler']|1
    assert vectors[17]==symbols['DMA0_IRQHandler']|1
    assert not any(name.startswith('__atomic_') for name in symbols), 'IRQ atomics must be lock-free'
    assert all(v&1 and 0xc000<=v-1<symbols['__image_end'] for v in vectors[1:])
    image=code.ljust(285608,b'\xff')
    image+=struct.pack('>I',zlib.crc32(image))
    manifest=dict(chip='LPC5528',role='MCU1',target=1,base=0xc000,length=len(image),
        format='omni-app-be-crc32-v1',sha256=hashlib.sha256(image).hexdigest(),
        build_id=build_id,code_length=len(code),code_sha256=hashlib.sha256(code).hexdigest(),
        runtime='omni-hid-v1',source_sha256=digest,source_files=sources,
        compiler=run('arm-none-eabi-gcc','--version').splitlines()[0],
        capabilities=['identity','code_readback','fault_status','boot_start_ack','boot_ack_status','software_recovery','microphone_pcm16_48k_v1','audio_error_trace','clock_snapshot','dial_display_probe','volume_percent_profile','usb_setup_guard','usb_endpoint_snapshot','mcu2_status_probe','mcu2_probe_trace_v1','mcu2_detect_query_v1','dsp_status_probe','control_uart_snapshot','dsp_passive_capture','dsp_passive_capture_rearm','audio_clock_snapshot','dsp_status_full','headset_telemetry_v1','settings_query_v1','dsp_meter_v1','native_dsp_settings_v1','persistent_mcu2_v1','native_display_settings_v1'],
        runtime_metadata_write={'base':0x7f800,'length':512,'purpose':'stock boot startup acknowledgement and explicit recovery request',
            'preserves':'all bytes except force word +4..7 and acknowledgement byte +14'},
        hardware_validation='pending',
        limitations=['USB playback and microphone capture through DSP; full lifecycle and audio endurance unqualified',
                      'Prototype volume range is not measured hardware attenuation',
                      'Fault status is not persistent across watchdog reset',
                      'Metadata erase/program is not atomic against power loss',
                      'Cold boot and physical recovery remain unverified for this candidate'])
    manifest['capabilities'] += ['remote_menu_v1','audio_endpoint_lifetime_v1',
                                 'local_menu_hold_v1','dsp_output_mode_reconcile_v1','headset_gain_v1','technical_oled_v1','source_bias_v1',
                                 'playback_pcm16_pcm24_48k_96k_v1','audio_format_epoch_v1','audio_format_status_v1','dsp_audio_mode_ack_v1']
    out=ROOT/'releases'/build_id
    out.mkdir(parents=True,exist_ok=True)
    if (out/'application.bin').exists() and (out/'application.bin').read_bytes()!=image:
        raise RuntimeError('Non-reproducible image for this source identity')
    (out/'application.bin').write_bytes(image)
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (out/'symbols.json').write_text(json.dumps(symbols,indent=2)+'\n')
    shutil.copy2(elf,out/elf.name)
    shutil.copy2(args.build/'omni_mcu1.map',out/'omni_mcu1.map')
    shutil.copy2(ROOT/'dependencies.lock.json',out/'dependencies.lock.json')
    for relative in sources:
        dest=out/'source'/relative
        dest.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(ROOT/relative,dest)
    shutil.copy2(ROOT/'README.md',out/'source'/'README.md')
    restore_dir=out/'restore'
    restore_dir.mkdir(exist_ok=True)
    for name,(data,digest) in restores.items():
        (restore_dir/(name+'.bin')).write_bytes(data)
        restore_manifest=dict(chip='LPC5528',role='MCU1',target=1,base=0xc000,
            length=len(data),format='omni-app-be-crc32-v1',sha256=digest,build_id=name)
        (restore_dir/(name+'.manifest.json')).write_text(json.dumps(restore_manifest,indent=2)+'\n')
    (restore_dir/'README.md').write_text(
        '# MCU1 restore references\n\n'
        'These are the hash-pinned stock and diagnostic v2 APPLICATION images for target 1 at 0xC000.\n'
        'Never write them at address zero; they do not contain a bootloader.\n\n'
        'When MCU1 1038:2291 recovery is available, from the original project firmware/rebuild-re directory run '
        '`python flash_verified_mcu1.py diagnostic` for validation, then add `--execute` to restore. '
        'Use `stock` for stock firmware. The runner independently verifies its original references.\n\n'
        'Proceed only when the unique MCU1 bootloader is present. These files are a recovery package, '
        'not evidence that this candidate or its recovery path has passed hardware tests.\n')
    if args.public:
        (restore_dir/'README.md').write_text('Public build: no stock or per-unit restore images are distributed.\nThe public flasher saves the installed MCU1 application before writing.\n')
    print(run('arm-none-eabi-size',elf))
    print('RELEASE',out,'SHA256',manifest['sha256'])

if __name__=='__main__': main()
