# BCM4775 transport layer: frame build/parse shared by the shub-s* scripts.
CRC_TABLE = bytes.fromhex(
    '004d9ad77934e3aef2bf68258bc6115c' 'a9e4337ed09d4a075b16c18c226fb8f5'
    '1f5285c8662bfcb1eda0773a94d90e43' 'b6fb2c61cf8255184409de933d70a7ea'
    '3e73a4e9470add90cc81561bb5f82f62' '97da0d40eea374396528ffb21c5186cb'
    '216cbbf65815c28fd39e4904aae7307d' '88c5125ff1bc6b267a37e0ad034e99d4'
    '7c31e6ab05489fd28ec31459f7ba6d20' 'd5984f02ace1367b276abdf05e13c489'
    '632ef9b41a5780cd91dc0b46e8a5723f' 'ca87501db3fe29643875a2ef410cdb96'
    '420fd8953b76a1ecb0fd2a67c984531e' 'eba6713c92df0845195483ce602dfab7'
    '5d10c78a2469bef3afe23578d69b4c01' 'f4b96e238dc0175a064b9cd17f32e5a8')

ACK, RELIABLE, RELIABLE_ACK, NACK, MSG_LOST, GARBAGE, SIZE_EXT, EXTENDED = (1 << i for i in range(8))
INTERNAL, IGNORE_SEQID = 0x100, 0x200
NAMES = ['ACK', 'REL', 'RACK', 'NACK', 'LOST', 'GARB', 'SZX', 'EXT']


def crc8(data):
    c = 0
    for b in data:
        c = CRC_TABLE[c ^ b]
    return ((c & 0x0f) << 4) | (c >> 4)


def frame(seq, payload=b'', ack=None, rseq=None, rack=None, garbage=None, internal=False,
          ignore_seq=False):
    """Build an escaped packet. Detail bytes follow the flag bits from bit0 up."""
    flags, det = 0, []
    if ack is not None:
        flags |= ACK; det.append(ack & 0xff)
    if rseq is not None:
        flags |= RELIABLE; det.append(rseq & 0xff)
    if rack is not None:
        flags |= RELIABLE_ACK; det.append(rack & 0xff)
    if garbage is not None:
        flags |= GARBAGE; det.append(garbage & 0xff)
    n = len(payload)
    if n > 0xff:
        flags |= SIZE_EXT; det.append(n >> 8)
    ext = (INTERNAL if internal else 0) | (IGNORE_SEQID if ignore_seq else 0)
    if ext:
        # EXTENDED's detail is flags 8-15, and each of those bits has a detail byte too
        flags |= EXTENDED; det.append(ext >> 8)
        det += [0] * bin(ext).count('1')
    body = bytes([seq & 0xff, n & 0xff, flags]) + bytes(det) + bytes(payload)
    body += bytes([crc8(body[1:])])
    out = bytearray(b'\xb0\x00')
    for b in body:
        out += {0xb0: b'\xb0\x03', 0x11: b'\xb0\x04', 0x13: b'\xb0\x05'}.get(b, bytes([b]))
    return bytes(out + b'\xb0\x01')


def rpc(rid, data=b''):
    n = len(data)
    hdr = bytes([rid, n]) if n < 0x80 else bytes([rid, 0x80 | (n >> 8), n & 0xff])
    return hdr + bytes(data)


def parse_rpcs(p):
    out, i = [], 0
    while i + 2 <= len(p):
        rid, n = p[i], p[i + 1]
        i += 2
        if n & 0x80:
            n = ((n & 0x7f) << 8) | p[i]; i += 1
        out.append((rid, p[i:i + n]))
        i += n
    return out


class Parser:
    """Byte-stream deframer. feed() returns a list of packet dicts (or errors)."""
    def __init__(self):
        self.buf = None
        self.esc = False
        self.garbage = 0          # bytes seen outside any frame, reported in GARBAGE


    def feed(self, data):
        pk = []
        for b in data:
            if self.esc:
                self.esc = False
                if b == 0x00:
                    self.buf = bytearray()
                elif b == 0x01:
                    if self.buf is not None:
                        pk.append(self.decode(bytes(self.buf)))
                    self.buf = None
                elif self.buf is not None:
                    self.buf.append({3: 0xb0, 4: 0x11, 5: 0x13}.get(b, b))
            elif b == 0xb0:
                self.esc = True
            elif self.buf is not None:
                self.buf.append(b)
            else:
                self.garbage += 1
        return pk

    @staticmethod
    def decode(b):
        if len(b) < 4:
            return {'err': 'short', 'raw': b}
        seq, n, flags = b[0], b[1], b[2]
        i, d = 3, {}
        for bit in range(8):
            if flags & (1 << bit):
                d[bit] = b[i]; i += 1
        if 6 in d:
            n |= d[6] << 8
        ext = d.get(7, 0) << 8
        for bit in range(8, 16):
            if ext & (1 << bit):
                d[bit] = b[i]; i += 1
        payload = b[i:i + n]
        ok = (i + n + 1 == len(b)) and crc8(b[1:-1]) == b[-1]
        return {'seq': seq, 'flags': flags | ext, 'det': d, 'payload': payload, 'ok': ok,
                'ack': d.get(0), 'rseq': d.get(1), 'rack': d.get(2)}


def fmt(p):
    if 'err' in p:
        return 'ERR %s %s' % (p['err'], p['raw'].hex())
    f = '|'.join(NAMES[i] + ('=%02x' % p['det'][i] if i in p['det'] else '')
                 for i in range(8) if p['flags'] & (1 << i))
    if p['flags'] & INTERNAL:
        f += '|INT'
    s = 'seq %02x [%s]%s' % (p['seq'], f, '' if p['ok'] else ' BADCRC')
    if p['payload'] and not p['flags'] & INTERNAL:
        s += ' ' + ' '.join('rpc%d(%d)' % (r, len(d)) for r, d in parse_rpcs(p['payload']))
    elif p['payload']:
        s += ' ' + p['payload'].hex()
    return s
