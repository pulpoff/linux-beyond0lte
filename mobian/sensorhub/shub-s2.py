#!/usr/bin/python3
# S10e BCM4775 sensor hub, bring-up stage S2 (SENSORHUB-SPEC.md sections 2.1-2.3, 7):
# power-cycle, sync with the ROM, download the patch (RPC 5 records), jump to it
# (RPC 46 PatchAddress), re-sync and check that the patched firmware reports its
# version over RPC 3. This mirrors lhd-android-start.strace frame for frame.
#
# usage: shub-s2.py PATCH.h      (bbd_patch_file_beyond.h, vendor: keep it on the device)
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tl
from shub_s1 import Hub, hexs

SSI_TX = 0x58             # full duplex | 2-byte length | flow control | write
SSI_RX = 0x50
WINDOW = 8                # stock keeps 8 reliable packets in flight
MAX_PAYLOAD = 921         # stock packs RPC 5 records up to this size
VERBOSE = int(os.environ.get('SHUB_V', '0'))
FW_VERSION = 545126


def load_patch(path):
    txt = open(path, errors='replace').read()
    if txt.lstrip().startswith('"'):      # C string literal lines
        txt = ''.join(l.strip().strip('"').replace('\\n', '\n') for l in txt.splitlines())
    hdr = dict(kv.split('=', 1) for kv in
               (x.strip('<>') for x in txt.split('\n') if x.startswith('<') and '=' in x
                and not x.startswith('<Blob=')))
    blob_hex = txt[txt.index('<Blob=') + 6:]
    blob_hex = blob_hex[:blob_hex.index('>')].replace('\n', '')
    blob = bytes.fromhex(blob_hex)
    recs, i = [], 0
    while i < len(blob):
        assert blob[i] == 0xf1, 'record tag %02x at %d' % (blob[i], i)
        n = blob[i + 1] | blob[i + 2] << 8
        addr = blob[i + 3:i + 7]
        data = blob[i + 7:i + 3 + n]
        recs.append(addr + bytes([len(data) & 0xff, len(data) >> 8]) + data)
        i += 3 + n
    return hdr, blob, recs


class Link:
    """SSI full-duplex transport + TL sequencing."""
    def __init__(self, hub):
        self.h = hub
        self.p = tl.Parser()
        self.seq = 0
        self.rseq = self.rseq0 = 0
        self.inbox = []           # decoded chip packets
        self.chip_seq = None      # last chip packet seen that carried a payload
        self.need_ack = False
        self.rack = None          # highest rseq the chip acknowledged
        self.racked = 0           # reliable packets acknowledged since the last sync (unwrapped)
        self.stats = {'tx': 0, 'fc_retry': 0, 'rx': 0}

    def _got(self, data):
        for pk in self.p.feed(data):
            if VERBOSE:
                print('   <<', tl.fmt(pk))
            if 'err' in pk or not pk['ok']:
                print('   << BAD', tl.fmt(pk))
                continue
            self.stats['rx'] += 1
            if pk['rack'] is not None:
                prev = (self.rseq0 - 1) & 0xff if self.rack is None else self.rack
                d = (pk['rack'] - prev) & 0xff
                if d < 0x80:                  # an older ack repeated is not progress
                    self.racked += d
                    self.rack = pk['rack']
            if pk['payload'] and not pk['flags'] & tl.INTERNAL:
                self.chip_seq = pk['seq']
                self.need_ack = True
            self.inbox.append(pk)

    def write(self, data):
        """bcm_ssi_tx: resend whatever the chip's flow control says it did not take."""
        data = bytes(data)
        for _ in range(3):
            m = len(data)
            r = self.h.xfer(bytes([SSI_TX, m & 0xff, m >> 8]) + data + b'\0\0')
            nread = r[1] | r[2] << 8
            if nread:
                self._got(r[3:3 + min(nread, m)])
            bw = r[3 + m] | r[4 + m] << 8
            self.stats['tx'] += 1
            if bw >= m:
                return
            self.stats['fc_retry'] += 1
            data = data[bw:]
        raise IOError('flow control: chip took %d of %d bytes' % (bw, m))

    def poll(self):
        """bcm_ssi_rx: status | len16, then read that many bytes."""
        r = self.h.xfer([SSI_RX, 0, 0])
        n = r[1] | r[2] << 8
        if not n:
            return False
        n = min(n, 2045)
        r = self.h.xfer([SSI_RX, 0, 0] + [0] * n)
        n = min(n, r[1] | r[2] << 8)
        self._got(r[3:3 + n])
        return n > 0

    def wait(self, pred, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if pred():
                return True
            if not self.poll():
                time.sleep(0.0005)
        return pred()

    def send(self, payload=b'', reliable=False, internal=False, ignore_seq=False):
        kw = {}
        if self.need_ack and not internal:
            kw['ack'] = self.chip_seq
            self.need_ack = False
        if reliable:
            kw['rseq'] = self.rseq
            self.rseq = (self.rseq + 1) & 0xff
        if self.p.garbage and not internal:
            kw['garbage'] = min(self.p.garbage, 0xff)
            self.p.garbage = 0
        f = tl.frame(self.seq, payload, internal=internal, ignore_seq=ignore_seq, **kw)
        if VERBOSE:
            print('   >>', tl.fmt(tl.Parser().feed(f)[0]))
        self.seq = (self.seq + 1) & 0xff
        self.write(f)
        return kw.get('rseq')

    def take_rpc(self, rid):
        for i, pk in enumerate(self.inbox):
            if pk['flags'] & tl.INTERNAL:
                continue
            for r, d in tl.parse_rpcs(pk['payload']):
                if r == rid:
                    return d
        return None

    def sync(self):
        """Autobaud + internal sync; returns the chip's internal reply."""
        self.inbox.clear()
        self.p.garbage = 0
        self.write(b'\x80' * 36)
        time.sleep(0.11)
        self.send(b'\x00\x01', internal=True, ignore_seq=True)
        ok = self.wait(lambda: any(pk['flags'] & tl.INTERNAL for pk in self.inbox), 0.5)
        if not ok:
            return None
        rep = [pk for pk in self.inbox if pk['flags'] & tl.INTERNAL][0]
        self.inbox.remove(rep)
        self.rseq = self.rseq0 = 0
        self.rack, self.racked = None, 0
        self.need_ack = False
        return rep

    def version(self):
        """RPC 0 (reliable) -> RPC 3: asic, rom/fw version, 4 more bytes."""
        self.inbox.clear()
        self.send(tl.rpc(0), reliable=True)
        if not self.wait(lambda: self.take_rpc(3) is not None, 1.0):
            return None
        d = self.take_rpc(3)
        return (int.from_bytes(d[0:4], 'little'), int.from_bytes(d[4:8], 'little'), d[8:])


def boot(path):
    """Power-cycle the hub, download the patch, start it. Returns a Link (MCU_REQ held)."""
    hdr, blob, recs = load_patch(path)
    print('patch: %s asic %s blob %d B (header says %s), %d records, entry %s' %
          (hdr.get('Name'), hdr.get('AsicVersion'), len(blob), hdr.get('BlobLength'),
           len(recs), hdr.get('PatchAddress')))
    if len(blob) != int(hdr['BlobLength']):
        sys.exit('blob length mismatch')

    h = Hub()
    L = Link(h)
    h.set('ap_sleep', 1)
    h.set('pwr_en', 0)
    time.sleep(0.02)
    h.set('pwr_en', 1)
    time.sleep(0.02)
    print('hello:', 'ok' if h.hello() else 'TIMEOUT (continuing)')
    L.write(b'\x80' * 16)
    rep = L.sync()
    print('sync 1:', rep and rep['payload'].hex())
    if not rep:
        sys.exit('no ROM sync reply')
    v = L.version()
    if not v:
        sys.exit('no RPC 3 from ROM')
    print('ROM: asic 0x%08x rom 0x%08x %s' % (v[0], v[1], v[2].hex()))
    if v[0] != int(hdr['AsicVersion'], 16):
        sys.exit('asic mismatch')

    # pack the records like lhd: as many RPC 5s per packet as fit in MAX_PAYLOAD
    pkts, cur = [], b''
    for r in recs:
        enc = tl.rpc(5, r)
        if cur and len(cur) + len(enc) > MAX_PAYLOAD:
            pkts.append(cur); cur = b''
        cur += enc
    pkts.append(cur)
    print('download: %d packets' % len(pkts))
    t0 = time.time()
    base = L.racked           # the RPC 0 of the version query is acked too
    acked = lambda: L.racked - base
    for i, pl in enumerate(pkts):
        if i - acked() >= WINDOW and not L.wait(lambda: i - acked() < WINDOW, 1.0):
            sys.exit('download stalled at packet %d (rack %s)' % (i, L.rack))
        L.send(pl, reliable=True)
        L.poll()
        if i % 100 == 0 and VERBOSE:
            print('  %3d/%d  rack %s  %.2fs' % (i, len(pkts), L.rack, time.time() - t0))
    if not L.wait(lambda: acked() >= len(pkts), 2.0):
        sys.exit('last packets not acked (rack %s)' % L.rack)
    print('download done in %.2fs, %s' % (time.time() - t0, L.stats))

    entry = int(hdr['PatchAddress'], 16)
    L.send(tl.rpc(46, entry.to_bytes(4, 'little')))
    time.sleep(0.045)
    rep = L.sync()
    print('sync 2:', rep and rep['payload'].hex())
    if not rep:
        sys.exit('no sync reply from patched firmware')
    v = L.version()
    if not v:
        sys.exit('no RPC 3 from patched firmware')
    print('FW: asic 0x%08x version %d %s' % (v[0], v[1], v[2].hex()))
    if v[1] != FW_VERSION:
        sys.exit('unexpected firmware version (want %d)' % FW_VERSION)
    return L


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: shub-s2.py PATCH.h')
    L = boot(sys.argv[1])
    try:
        print('S2 PASSED')
        t0 = time.time()
        while time.time() - t0 < 2.0:      # show whatever the firmware says unprompted
            L.poll()
        for pk in L.inbox:
            print('  later:', tl.fmt(pk), pk['payload'][:32].hex())
    finally:
        L.h.bye()


if __name__ == '__main__':
    main()
