#!/usr/bin/python3
# S10e BCM4775 sensor hub, bring-up stage S1 (SENSORHUB-SPEC.md section 7):
# power-cycle the hub, autobaud, send the transport-layer internal sync, and
# print what the ROM answers. Then RPC 0 -> expect RPC 3 (AsicVersion 0x004775B0).
#
# Needs: spidev bound to the sensorhub@0 node (driver_override), python3-libgpiod
# 2.x, python3-spidev. Run as root on the phone.
import glob, os, sys, time

import gpiod
import spidev
from gpiod.line import Direction, Value

SPEED = int(os.environ.get('SHUB_HZ', '1000000'))

# SSI command bytes (bcm_gps_spi.h): full duplex, 2-byte length, flow control
SSI_TX = 0x58
SSI_RX = 0x50

CRC_TABLE = bytes.fromhex(
    '004d9ad77934e3aef2bf68258bc6115c'
    'a9e4337ed09d4a075b16c18c226fb8f5'
    '1f5285c8662bfcb1eda0773a94d90e43'
    'b6fb2c61cf8255184409de933d70a7ea'
    '3e73a4e9470add90cc81561bb5f82f62'
    '97da0d40eea374396528ffb21c5186cb'
    '216cbbf65815c28fd39e4904aae7307d'
    '88c5125ff1bc6b267a37e0ad034e99d4'
    '7c31e6ab05489fd28ec31459f7ba6d20'
    'd5984f02ace1367b276abdf05e13c489'
    '632ef9b41a5780cd91dc0b46e8a5723f'
    'ca87501db3fe29643875a2ef410cdb96'
    '420fd8953b76a1ecb0fd2a67c984531e'
    'eba6713c92df0845195483ce602dfab7'
    '5d10c78a2469bef3afe23578d69b4c01'
    'f4b96e238dc0175a064b9cd17f32e5a8'
)
assert len(CRC_TABLE) == 256


def crc8(data):
    c = 0
    for b in data:
        c = CRC_TABLE[c ^ b]
    return ((c & 0x0f) << 4) | (c >> 4)


def tl_frame(seq, flags, details, payload):
    """Transport-layer packet: seq | size | flags_lo | details... | payload | crc."""
    body = bytes([len(payload) & 0xff, flags & 0xff]) + bytes(details) + bytes(payload)
    body = bytes([seq]) + body + bytes([crc8(body)])
    out = bytearray(b'\xb0\x00')
    for b in body:
        if b == 0xb0:
            out += b'\xb0\x03'
        elif b == 0x11:
            out += b'\xb0\x04'
        elif b == 0x13:
            out += b'\xb0\x05'
        else:
            out.append(b)
    return bytes(out + b'\xb0\x01')


def find_line(bank, pin):
    for path in sorted(glob.glob('/dev/gpiochip*')):
        with gpiod.Chip(path) as c:
            if c.get_info().label == bank:
                return path, pin
    sys.exit('no gpiochip labelled %s' % bank)


class Hub:
    def __init__(self):
        self.req = {}
        for name, bank, pin, out in (('pwr_en', 'gpg0', 2, True),
                                     ('mcu_req', 'gpg4', 1, True),
                                     ('mcu_resp', 'gpg4', 2, False),
                                     ('host_req', 'gpa1', 7, False),
                                     ('ap_sleep', 'gpa0', 5, True)):
            path, off = find_line(bank, pin)
            s = gpiod.LineSettings(direction=Direction.OUTPUT if out else Direction.INPUT,
                                   output_value=Value.INACTIVE)
            r = gpiod.request_lines(path, consumer='shub-s1', config={off: s})
            self.req[name] = (r, off)
        dev = sorted(glob.glob('/sys/bus/spi/devices/*/of_node'))
        node = [d for d in dev if open(os.path.join(d, 'compatible'), 'rb').read()
                .startswith(b'brcm,bcm4775-shub')]
        if not node:
            sys.exit('sensorhub spi device not found')
        spi = os.path.basename(os.path.dirname(node[0]))      # spiB.C
        bus, cs = spi[3:].split('.')
        self.spi = spidev.SpiDev()
        self.spi.open(int(bus), int(cs))
        self.spi.mode = 3
        self.spi.bits_per_word = 8
        self.spi.max_speed_hz = SPEED
        print('spi%s.%s mode 3 @ %d Hz' % (bus, cs, SPEED))

    def set(self, name, v):
        r, off = self.req[name]
        r.set_value(off, Value.ACTIVE if v else Value.INACTIVE)

    def get(self, name):
        r, off = self.req[name]
        return r.get_value(off) == Value.ACTIVE

    def hello(self):
        self.set('mcu_req', 1)
        for n in range(100):
            if self.get('mcu_resp'):
                return True
            time.sleep(0.001)
            if n % 20 == 19:
                self.set('mcu_req', 0)
                time.sleep(0.001)
                self.set('mcu_req', 1)
                time.sleep(0.001)
        self.set('mcu_req', 0)
        return False

    def bye(self):
        self.set('mcu_req', 0)

    def xfer(self, data):
        return bytes(self.spi.xfer2(list(data)))

    def rx(self):
        """Full-duplex read: status | len16 | data."""
        r = self.xfer([SSI_RX, 0, 0])
        n = r[1] | (r[2] << 8)
        if not n:
            return r[0], b''
        r = self.xfer([SSI_RX, 0, 0] + [0] * min(n, 2045))
        return r[0], r[3:3 + min(n, r[1] | (r[2] << 8))]


def hexs(b):
    return ' '.join('%02x' % x for x in b)


def main():
    h = Hub()
    print('lines: mcu_resp=%d host_req=%d' % (h.get('mcu_resp'), h.get('host_req')))
    # power cycle via nSTDBY (vdd_mcu_1p8 stays on, as on stock in practice)
    h.set('ap_sleep', 1)
    h.set('pwr_en', 0)
    time.sleep(0.05)
    h.set('pwr_en', 1)
    time.sleep(0.05)
    print('after power-up: mcu_resp=%d host_req=%d' % (h.get('mcu_resp'), h.get('host_req')))

    ok = h.hello()
    print('hello:', 'MCU_RESP high' if ok else 'TIMEOUT (continuing like FW4775-1147)')
    # autobaud: debug-mode write of raw 0x80 bytes (bcm_gps_regs.c transaction_1st)
    r = h.xfer([0x80] * 32)
    print('autobaud rx:', hexs(r))
    time.sleep(0.01)
    # half-duplex raw write of the internal sync (transaction_2nd)
    sync = tl_frame(0, 0x80, [0x03, 0x00, 0x00], [0x00, 0x01])
    assert sync == bytes.fromhex('b0000002800300000001b1b001'), hexs(sync)
    r = h.xfer(b'\x00' + sync)
    print('sync rx:', hexs(r))
    t0 = time.time()
    while time.time() - t0 < 1.0:
        time.sleep(0.02)
        st, data = h.rx()
        if data:
            print('%5.0f ms host_req=%d stat %02x len %d: %s' %
                  ((time.time() - t0) * 1000, h.get('host_req'), st, len(data), hexs(data)))
    h.bye()


if __name__ == '__main__':
    main()
