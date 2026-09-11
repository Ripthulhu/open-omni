"""Stream a 128x64 1-bit framebuffer to the Omni MCU1 OLED over HID.

Frame format: SSD1306 page layout, 1024 bytes = 8 pages x 128 columns; byte
(page*128+col) holds 8 vertical pixels, bit b = row page*8+b. This is exactly
what the 1-bit-doom-stream browser capture emits, so its frames stream as-is.

Transport: HID feature reports to the native firmware.
  opcode 76: frame chunk  [1,76,seq,0, off_lo, off_hi, count, bytes...]  (<=57 B)
  opcode 77: SPI divider  [1,77,seq,0, div_lo, div_hi]  (SCK = 12MHz/(div+1))

Modes:
  selftest         bouncing box animation (no browser needed)
  bridge           WebSocket server; forwards each 1024-byte binary frame to HID
"""
import argparse, struct, sys, time

VID, PID, USAGE_PAGE = 0x1038, 0x2290, 0xffc0
W, H, FRAME = 128, 64, 1024


def open_device():
    import hid
    devs = [d for d in hid.enumerate(VID, PID) if d.get("usage_page") == USAGE_PAGE]
    if len(devs) != 1:
        raise SystemExit("expected exactly one native Omni HID device, found %d" % len(devs))
    h = hid.device(); h.open_path(devs[0]["path"])
    return h, devs[0].get("serial_number")


class Stream:
    def __init__(self):
        self.h, self.serial = open_device()
        self.seq = 0

    def _report(self, opcode, body):
        self.seq = (self.seq + 1) % 256
        pkt = bytes([1, opcode, self.seq, 0]) + body
        if len(pkt) > 64:
            raise ValueError("report too long")
        self.h.send_feature_report(pkt.ljust(64, b"\0"))

    def set_div(self, div):
        self._report(77, struct.pack("<H", div))

    def frame(self, buf):
        if len(buf) != FRAME:
            raise ValueError("frame must be %d bytes" % FRAME)
        off = 0
        while off < FRAME:
            n = min(57, FRAME - off)
            self._report(76, struct.pack("<HB", off, n) + buf[off:off + n])
            off += n

    def close(self):
        self.h.close()


def set_pixel(buf, x, y):
    if 0 <= x < W and 0 <= y < H:
        buf[(y >> 3) * W + x] |= 1 << (y & 7)


def selftest(args):
    s = Stream()
    print("device serial:", s.serial)
    if args.div:
        s.set_div(args.div); print("SPI divider set to", args.div, "(SCK ~= %.0f kHz)" % (12000.0 / (args.div + 1)))
    bx, by, dx, dy, bw, bh = 10, 10, 3, 2, 24, 16
    period = 1.0 / args.fps
    frames = 0; t0 = time.monotonic()
    try:
        while True:
            buf = bytearray(FRAME)
            for x in range(W):  # top/bottom border
                set_pixel(buf, x, 0); set_pixel(buf, x, H - 1)
            for y in range(H):  # left/right border
                set_pixel(buf, 0, y); set_pixel(buf, W - 1, y)
            for x in range(bx, bx + bw):  # bouncing filled box
                for y in range(by, by + bh):
                    set_pixel(buf, x, y)
            s.frame(bytes(buf))
            frames += 1
            bx += dx; by += dy
            if bx <= 1 or bx + bw >= W - 1: dx = -dx
            if by <= 1 or by + bh >= H - 1: dy = -dy
            if frames % 30 == 0:
                fps = frames / (time.monotonic() - t0)
                print("streamed %d frames, %.1f fps" % (frames, fps))
            if args.frames and frames >= args.frames:
                print("done: %d frames, %.1f fps avg" % (frames, frames / (time.monotonic() - t0)))
                break
            time.sleep(period)
    except KeyboardInterrupt:
        pass
    finally:
        s.close()


def bridge(args):
    import asyncio, websockets
    s = Stream()
    print("device serial:", s.serial, "| WebSocket ws://127.0.0.1:%d" % args.port)
    if args.div:
        s.set_div(args.div); print("SPI divider set to", args.div)

    async def handler(ws):
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)) and len(msg) == FRAME:
                s.frame(bytes(msg))

    async def run():
        async with websockets.serve(handler, "127.0.0.1", args.port, max_size=None):
            await asyncio.Future()
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass
    finally:
        s.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="mode", required=True)
    st = sub.add_parser("selftest"); st.add_argument("--fps", type=float, default=20.0); st.add_argument("--div", type=int, default=0); st.add_argument("--frames", type=int, default=0)
    br = sub.add_parser("bridge"); br.add_argument("--port", type=int, default=8765); br.add_argument("--div", type=int, default=0)
    a = p.parse_args()
    (selftest if a.mode == "selftest" else bridge)(a)


if __name__ == "__main__":
    main()
