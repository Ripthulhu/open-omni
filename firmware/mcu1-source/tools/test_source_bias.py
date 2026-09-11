"""Bounded native source-bias integration check; exact build, no GG/bridge.

Requires idle physical/Windows controls. Temporarily mutes the shared master,
checks four bias positions and a coalesced burst, then restores both tuples.
MCU2 TXIDLE and DSP local dispatch are not acoustic/readback proof. No flash.
"""
import argparse
import json
import struct
import time
from datetime import datetime, timezone
from pathlib import Path
from native_controls import Native


def run(release, output):
    n = Native(release)
    evidence = {'utc': datetime.now(timezone.utc).isoformat(), 'build': n.manifest['build_id'],
                'passed': False, 'checks': [], 'limits': __doc__}
    original = bias = None
    def save(): output.write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf8')
    def apply(mode, position): n.call(73, bytes([1, mode, position]))
    def settle(mode, position):
        end = time.monotonic()+4
        desired = (12 if position <= 12 else 24-position, position if position <= 12 else 12)
        while time.monotonic() < end:
            b = n.words(73);s = n.words(66, 4)
            if s[1]&2: raise RuntimeError('MCU2 fault: '+str(s))
            if b[2:6] == [mode, position, *desired] and s[1]&8 and s[2] == s[3] == desired[1] and s[4] == s[5]:
                if b[9] == 6 and b[10] == mode:
                    return {'bias': b, 'mcu2_transmitted': s}
                if b[9] >= 7: raise RuntimeError('Home context dispatch failed: '+str(b))
            time.sleep(.025)
        raise TimeoutError(f'Bias failed to settle: {b}, {s}')
    try:
        if 'source_bias_v1' not in n.manifest.get('capabilities', []): raise RuntimeError('Source-bias build required')
        headset = n.words(72)
        if headset[3]&0x308 != 0x308: raise RuntimeError('Connected, ready headset required')
        state = n.call(8);original = struct.unpack_from('<hB', state, 4)
        bias = n.words(73)[2:4]
        evidence.update(original_master=list(original), original_bias=bias, audio_before=n.words(44))
        n.call(9, struct.pack('<hB', original[0], 1))
        time.sleep(.3)
        for position in (0, 12, 24, 12):
            apply(1, position);evidence['checks'].append(settle(1, position));save()
        for index in range(60): apply(1, (index*7)%25)
        apply(0, 12);evidence['burst_final'] = settle(0, 12)
        evidence['audio_after'] = n.words(44)
        assert all(evidence['audio_after'][i] == evidence['audio_before'][i] for i in (2, 5, 6, 7, 8, 9))
        evidence['passed'] = True
    except Exception as error:
        evidence['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        if bias is not None:
            try:
                apply(*bias);evidence['restored_bias'] = settle(*bias)
            except Exception as error:
                evidence['bias_restore_error'] = f'{type(error).__name__}: {error}';evidence['passed'] = False
        if original is not None:
            try:
                n.call(9, struct.pack('<hB', *original));evidence['restored_master_request'] = list(original)
            except Exception as error:
                evidence['master_restore_error'] = f'{type(error).__name__}: {error}';evidence['passed'] = False
        n.close();save()
    return evidence


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('release', type=Path);p.add_argument('--output', type=Path, required=True)
    p.add_argument('--execute', action='store_true')
    a = p.parse_args()
    if not a.execute: print('Dry run: requires --execute, hands-off controls and a matching connected headset.')
    else: print(json.dumps(run(a.release, a.output), indent=2))
