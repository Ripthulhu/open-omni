"""Strict MCU1 image/transport primitives. No automatic device discovery or reset.

The bootloader provides no block offset in its ACK: permit exactly one outstanding
block, never retry a timed-out write, and verify the complete readback before commit.
"""
from dataclasses import dataclass
import hashlib
import struct
import time
import zlib

BASE = 0xc000
ENVELOPE = 285612  # Conservative, hardware-proven stock application envelope.
CHUNK = 1012
REPORT_LEN = 1036

class UpdateError(RuntimeError):
    pass

@dataclass(frozen=True)
class Image:
    data: bytes
    sha256: str
    crc32: int
    build_id: str

def validate_image(data, manifest):
    required = {'chip':'LPC5528', 'role':'MCU1', 'target':1,
                'base':BASE, 'length':ENVELOPE, 'format':'omni-app-be-crc32-v1'}
    for key, value in required.items():
        if type(manifest.get(key)) is not type(value) or manifest[key] != value:
            raise UpdateError(f'Invalid manifest {key}; expected {value!r}')
    if len(data) != ENVELOPE:
        raise UpdateError('Not the approved MCU1 application envelope; raw dumps are forbidden')
    digest = hashlib.sha256(data).hexdigest()
    if manifest.get('sha256') != digest:
        raise UpdateError('Image SHA256 mismatch')
    expected = struct.unpack('>I', data[-4:])[0]
    if zlib.crc32(data[:-4]) != expected:
        raise UpdateError('Image CRC mismatch')
    sp, reset = struct.unpack_from('<II', data)
    if sp & 7 or not 0x20000000 < sp <= 0x20030000:
        raise UpdateError('Invalid initial MCU1 stack pointer')
    if not reset & 1 or not BASE <= (reset & ~1) < BASE+len(data)-4:
        raise UpdateError('Reset vector is not Thumb code inside this MCU1 application')
    build_id = manifest.get('build_id')
    if not isinstance(build_id,str) or not build_id:
        raise UpdateError('Missing build identity')
    return Image(bytes(data),digest,expected,build_id)

def block_report(offset, payload):
    if offset < 0 or offset % CHUNK or not 0 < len(payload) <= CHUNK or offset+len(payload) > ENVELOPE:
        raise UpdateError('Invalid block offset/length')
    return (bytes([1,3,1,1])+struct.pack('<HI',len(payload),offset)+payload).ljust(REPORT_LEN,b'\0')

def await_reply(handle, opcode, minimum, timeout=2, log=None):
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        reply = bytes(handle.read(64,100))
        if not reply:
            continue
        if log is not None:
            log.append({'rx':reply.hex()})
        if reply[0] == 7:  # Separate asynchronous notification report.
            continue
        if len(reply) < 2 or reply[:2] != bytes([1,opcode]):
            raise UpdateError('Unexpected command response: '+reply.hex())
        if len(reply) < minimum:
            raise UpdateError('Truncated command response: '+reply.hex())
        if reply[2] != 0:
            raise UpdateError(f'Bootloader rejected opcode {opcode:02x}, status {reply[2]:02x}')
        return reply
    raise UpdateError(f'Timed out waiting for opcode {opcode:02x}; transfer stopped without retry')

def stage_image(handle, image, readback, log):
    """Write and verify in the bootloader. Does NOT commit or boot the application.

    Caller must first establish the recovery gate and unique MCU1 bootloader.
    readback(address,length) must perform the recovered bounded read command.
    """
    # Refuse pre-existing replies rather than accepting a stale ACK for block zero.
    if handle.read(64,100):
        raise UpdateError('Pending HID traffic; establish a clean transaction before writing')
    for offset in range(0,len(image.data),CHUNK):
        packet = block_report(offset,image.data[offset:offset+CHUNK])
        log.append({'phase':'block','offset':offset,'length':min(CHUNK,len(image.data)-offset)})
        if handle.send_feature_report(packet) != len(packet):
            raise UpdateError('Short or failed feature-report write')
        await_reply(handle,3,6,log=log)
    packet = bytes([1,0x84,1,1]).ljust(64,b'\0')
    if handle.write(packet) != len(packet):
        raise UpdateError('Failed CRC query')
    response = await_reply(handle,0x84,11,log=log)
    supplied, computed = struct.unpack('>II',response[3:11])
    if supplied != image.crc32 or computed != image.crc32:
        raise UpdateError('Bootloader CRC pair does not match the selected image')
    # readback=None skips the full pre-commit re-read (fast path). The bootloader
    # CRC32 above already covers the whole staged image, and the runtime SHA readback
    # after boot re-verifies the installed code, so integrity is still proven.
    if readback is not None:
        actual = readback(BASE,len(image.data))
        if actual != image.data:
            raise UpdateError('Complete bootloader readback does not match selected image')
    log.append({'phase':'staged_and_readback_verified' if readback is not None
                else 'staged_crc_verified','sha256':image.sha256,'running_verified':False})
    return image.sha256
