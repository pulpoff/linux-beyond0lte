#!/usr/bin/python3
# S10e BCM4775 sensor hub, bring-up stage S3 (SENSORHUB-SPEC.md sections 2.4, 3):
# boot the hub (shub-s2), replay the kernel's initialize_mcu() over RPC 32, then
# enable accel / light / proximity and print their samples.
#
# usage: shub-s3.py PATCH.h [seconds]
import os, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tl
from shub_s2 import boot

AP2HUB_READ, AP2HUB_WRITE, HUB2AP_WRITE, AP2HUB_RETURN = 0, 1, 2, 4

# bytes of sensor data in a 0x37 BYPASS_DATA item, before its u64 timestamp (sensor_list.h, this build)
DATA_LEN = {0: 6, 1: 12, 2: 13, 3: 6, 4: 8, 5: 6, 7: 7, 9: 21, 10: 2, 13: 1, 14: 1, 15: 24,
            16: 17, 17: 17, 18: 4, 26: 1, 27: 1, 29: 3, 30: 2, 33: 12, 34: 3, 35: 3, 36: 1,
            37: 1, 38: 1, 39: 6, 40: 21, 42: 1, 48: 1, 50: 1}
NAME = {0: 'accel', 1: 'gyro', 4: 'mag', 5: 'pressure', 7: 'prox', 9: 'light', 16: 'game_rv',
        17: 'rv', 48: 'autorot'}


class Ssp:
    def __init__(self, link):
        self.L = link
        self.state = 'hdr'
        self.need = 4
        self.buf = b''
        self.pending = []           # [msg dict] in send order
        self.hub_frames = 0
        self.samples = []

    # --- transport
    def record(self, b):
        self.L.send(tl.rpc(32, struct.pack('<H', len(b)) + bytes(b)))

    def pump(self, timeout=0.0):
        t0 = time.time()
        while True:
            got = self.L.poll()
            while self.L.inbox:
                pk = self.L.inbox.pop(0)
                if pk['flags'] & tl.INTERNAL:
                    continue
                for rid, d in tl.parse_rpcs(pk['payload']):
                    if rid == 33:
                        n = struct.unpack_from('<H', d)[0]
                        self.feed(d[2:2 + n])
                    elif rid != 3:
                        print('  rpc%d: %s' % (rid, d[:40].hex()))
            if time.time() - t0 >= timeout:
                return
            if not got:
                time.sleep(0.001)

    # --- callback_bbd_on_packet
    def feed(self, data):
        while data:
            k = min(self.need, len(data))
            self.buf += data[:k]
            data = data[k:]
            self.need -= k
            if self.need:
                continue
            if self.state == 'hdr':
                opts, ln = struct.unpack('<HH', self.buf)
                typ = opts & 3
                self.buf = b''
                if typ == HUB2AP_WRITE:
                    self.state, self.need = 'frame', ln
                    self.hub_frames += 1
                    if self.hub_frames % 3 == 0:
                        self.timestamp()
                    continue
                m = next((m for m in self.pending if m['opts'] == opts), None)
                if not m:
                    print('  SSP: no pending msg for opts %04x (len %d)' % (opts, ln))
                    self.need = 4
                    continue
                if typ == AP2HUB_READ:
                    self.state, self.need, self.cur = 'read', m['len'], m
                elif typ == AP2HUB_WRITE:
                    self.pending.remove(m)
                    self.record(m['buf'])
                    if opts & AP2HUB_RETURN:
                        m['opts'] = AP2HUB_READ | AP2HUB_RETURN
                        m['len'] = 1
                        self.pending.append(m)
                    else:
                        m['done'] = True
                    self.need = 4
                else:
                    print('  SSP: bad type %d' % typ)
                    self.need = 4
            elif self.state == 'read':
                self.cur['reply'] = self.buf
                self.cur['done'] = True
                self.pending.remove(self.cur)
                self.buf, self.state, self.need = b'', 'hdr', 4
            else:
                frame, self.buf, self.state, self.need = self.buf, b'', 'hdr', 4
                self.parse_frame(frame)

    def parse_frame(self, f):
        i = 0
        while i < len(f):
            inst = f[i]; i += 1
            if inst == 0x03:                                   # debug text
                n = struct.unpack_from('<H', f, i)[0]; i += 2
                txt = f[i:i + n].split(b'\0')[0].decode('latin1').rstrip()
                for l in txt.splitlines():
                    print('  MCU:', l)
                i += n
            elif inst == 0x0a:
                print('  MCU: SENSOR INIT DONE')
            elif inst == 0xa8:
                ts = struct.unpack_from('<Q', f, i)[0]; i += 8
                print('  MCU: timestamp offset req, mcu ts %d' % ts)
            elif inst in (0x37, 0x38):
                t = f[i]; i += 1
                if t not in DATA_LEN:
                    print('  data: unknown sensor %d, rest %s' % (t, f[i:].hex())); return
                n = DATA_LEN[t]
                d = f[i:i + n]; i += n
                ts = struct.unpack_from('<Q', f, i)[0]; i += 8
                self.samples.append((t, d, ts))
                self.show(t, d, ts)
            elif inst == 0x06:
                pass                                           # TIME_SYNC
            elif inst == 0x0b:                                 # COLLECT_BIGDATA: stats, ends the frame
                return
            else:
                print('  frame: inst %02x unhandled, rest %s' % (inst, f[i - 1:i + 31].hex()))
                return

    def show(self, t, d, ts):
        if t == 0:
            x, y, z = struct.unpack('<hhh', d)
            s = 'x %6.2f y %6.2f z %6.2f m/s2' % tuple(v * 9.80665 / 4096 for v in (x, y, z))
        elif t == 9:
            lux, cct, r, g, b, w, ag, at, br = struct.unpack('<IiHHHHHHB', d)
            s = 'lux %d cct %d rgbw %d %d %d %d gain %d time %d' % (lux, cct, r, g, b, w, ag, at)
        elif t == 7:
            det, adc, light = struct.unpack('<BHI', d)
            s = 'detect %d adc %d' % (det, adc)        # polarity not verified yet
        else:
            s = d.hex()
        print('  %-8s %s  (ts %.3f)' % (NAME.get(t, 'type%d' % t), s, ts / 1e9))

    # --- ssp_spi_sync / bbd_do_transfer
    def msg(self, cmd, opts, length, buf=b'', timeout=1.0):
        m = {'cmd': cmd, 'opts': opts, 'len': length, 'buf': bytes(buf), 'done': False}
        if length:
            self.pending.append(m)
        self.record(struct.pack('<BHHI', cmd, length, opts, 0))
        if not length:
            return b''
        t0 = time.time()
        while not m['done'] and time.time() - t0 < timeout:
            self.pump(0.005)
        if not m['done']:
            if m in self.pending:
                self.pending.remove(m)
            raise TimeoutError('SSP cmd %02x timed out' % cmd)
        return m.get('reply', b'')

    def read(self, cmd, n):
        return self.msg(cmd, AP2HUB_READ, n)

    def write(self, cmd, data=b''):
        return self.msg(cmd, AP2HUB_WRITE, len(data), data)

    def timestamp(self):
        ns = time.clock_gettime_ns(time.CLOCK_BOOTTIME)
        self.record(struct.pack('<BHHI', 0xa7, 8, AP2HUB_WRITE, 0) + struct.pack('<q', ns))


# initialize_mcu() as captured on stock (TRACE): data blocks are this phone's DT / EFS values
INIT = [
    ('w', 0xb6, '001a'),                      # AP_INFORMATION
    ('w', 0xf1, '070707'),                    # SENSOR_FORMATION (DT positions)
    ('w', 0xec, '00'),                        # GLASS_TYPE
    ('r', 0xf4, 8),                           # SCANNING: bitmask of present sensors
    ('r', 0x01, 3),                           # FUSEROM
    ('w', 0xfd, '86545b3fedde0045d236987bf0a400f9ff1e3e29befc5ffd0ab8f9'),   # MAG_STATIC_MATRIX
    ('w', 0x46, '010526fec0028e062201a1f89e'),                             # MAG_CAL_PARAM
    ('w', 0xb3, '1a042104'),                  # THERMISTOR_TABLE
    ('r', 0xf0, 4),                           # FIRMWARE_REV
    ('r', 0xe1, 1),                           # ACCEL_RANGE
    ('w', 0x91, '0000000051260000'),          # PANEL_INFO
    ('w', 0x96, '00'),
    ('w', 0xcd, '00960000e9e0ffff88f0ffff'),  # GYRO_CAL
    ('w', 0xce, '000000000000'),              # ACCEL_CAL
    ('w', 0xf2, '000000003ff00000'),          # PROX thresholds
    ('w', 0x49, '88fcffffd900000044feffff5b040000590e0000580400005a050000'),  # LIGHT_COEF
    ('w', 0xdb, ''),
]


def main():
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10
    L = boot(sys.argv[1])
    S = Ssp(L)
    try:
        L.send(tl.rpc(43, bytes.fromhex('010021000000')))    # as lhd does right after boot
        S.pump(0.9)
        S.timestamp()
        who = S.read(0x0f, 1)
        print('WHOAMI: %s' % who.hex(), 'OK' if who == b'\x55' else '(want 55)')
        present = 0
        for kind, cmd, arg in INIT:
            if kind == 'r':
                r = S.read(cmd, arg)
                print('read  %02x -> %s' % (cmd, r.hex()))
                if cmd == 0xf4:
                    present = int.from_bytes(r, 'little')
                    print('  sensors present: %s' % [i for i in range(64) if present >> i & 1])
            else:
                S.write(cmd, bytes.fromhex(arg))
                print('write %02x ok' % cmd)
        S.pump(0.5)
        for t, ms in ((0, 100), (9, 200), (7, 200)):
            if not present >> t & 1:
                print('%s not present, skipping' % NAME[t]); continue
            S.write(0xa1, bytes([t]) + struct.pack('<IIB', ms, 0, 0))
            print('enabled %s @ %d ms' % (NAME[t], ms))
        S.pump(secs)
        for t in (0, 9, 7):
            print('%s: %d samples' % (NAME[t], sum(1 for s in S.samples if s[0] == t)))
            if present >> t & 1:
                S.write(0xa2, bytes([t]) + struct.pack('<IIB', 0, 0, 0))
    finally:
        L.h.bye()


if __name__ == '__main__':
    main()
