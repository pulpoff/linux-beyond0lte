#!/usr/bin/env python3
# Minimal userspace for the Exynos 9820 CP on Mobian, in place of Samsung's rild:
#  - serves the CP's remote file system (umts_rfs0) from a private directory,
#    and NV writes into our own copy of nv_data.bin (never the EFS partition)
#  - talks Samsung IPC on umts_ipc0: sends rild's power-on sequence, logs
#    everything the CP says, and takes commands from a FIFO
#
#   cpd.py [--state /var/lib/exynos-cp] [--quiet-rfs]
#
# Protocols are from the Android capture (kit/modem/private/rfsboot.*) and
# libsamsung-ipc: IPC frame = u16 len, u8 mseq, u8 aseq, u8 group, u8 index,
# u8 type, data; RFS frame = u32 len, u8 index, u8 id, data.
import argparse, errno, hashlib, os, re, select, struct, subprocess, sys, time

GROUP = {1: 'PWR', 2: 'CALL', 4: 'SMS', 5: 'SEC', 6: 'PB', 7: 'DISP', 8: 'NET', 9: 'SND',
         0xa: 'MISC', 0xb: 'SVC', 0xc: 'SS', 0xd: 'GPRS', 0xe: 'SAT', 0xf: 'CFG',
         0x10: 'IMEI', 0x11: 'GPS', 0x12: 'SAP', 0x13: 'OEM', 0x80: 'GEN'}
TYPE = {1: 'EXEC', 2: 'GET', 3: 'SET', 4: 'CFRM', 5: 'EVENT', 6: 'INDI', 7: 'RESP', 8: 'NOTI'}
EXEC, GET, SET, CFRM, EVENT = 1, 2, 3, 4, 5
# frames whose payload identifies the phone or the SIM: logged without data
PRIVATE = {(0xa, 0x03), (0x5, 0x05), (0x5, 0x02), (0x5, 0x07), (0xc, 0x06), (0x2, 0x06), (0x4, 0x02), (0x4, 0x01), (0x2, 0x01)}

NV_SIZE = 0x100000
NV_SECRET = b'Samsung_Android_RIL'

RFS_NV_READ, RFS_NV_WRITE, RFS_READ, RFS_WRITE, RFS_LSEEK, RFS_CLOSE = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
RFS_UNLINK, RFS_OPEN = 0x0b, 0x11
MAX_FRAME = 2044            # one UL slot minus the SIPC5 header

t0 = time.time()


def log(*a):
    print('%8.3f' % (time.time() - t0), *a, flush=True)


class Rfs:
    def __init__(self, fd, state, quiet):
        self.fd, self.quiet = fd, quiet
        self.root = os.path.join(state, 'rfs')
        self.nv = os.path.join(state, 'nv_data.bin')
        self.buf = b''
        self.files = {}
        os.makedirs(self.root, mode=0o700, exist_ok=True)

    def path(self, name):
        # every name lands inside our root; no absolute paths, no '..'
        name = name.replace('\\', '/').lstrip('/')
        parts = [p for p in name.split('/') if p not in ('', '.')]
        if not parts or '..' in parts:
            return None
        return os.path.join(self.root, *parts)

    def send(self, index, rid, data):
        msg = struct.pack('<IBB', 6 + len(data), index, rid) + data
        for i in range(0, len(msg), MAX_FRAME):
            os.write(self.fd, msg[i:i + MAX_FRAME])

    def reply(self, index, rid, ret, err=0, data=b''):
        self.send(index, rid, struct.pack('<iI', ret, err) + data)

    def feed(self, chunk):
        self.buf += chunk
        while len(self.buf) >= 6:
            total = struct.unpack_from('<I', self.buf)[0]
            if total < 6 or total > 4 << 20:
                log('rfs: bad length %d, dropping %d bytes' % (total, len(self.buf)))
                self.buf = b''
                return
            if len(self.buf) < total:
                return
            msg, self.buf = self.buf[:total], self.buf[total:]
            try:
                self.handle(msg[4], msg[5], msg[6:])
            except Exception as e:      # never let one request kill the server
                log('rfs: idx %02x failed: %r' % (msg[4], e))
                self.reply(msg[4], msg[5], -1, errno.EIO)

    def handle(self, idx, rid, d):
        if idx == RFS_NV_WRITE:
            off, ln = struct.unpack_from('<II', d)
            data = d[8:8 + ln]
            if off + len(data) > NV_SIZE or len(data) != ln:
                log('rfs: NV write out of range off %#x len %#x' % (off, ln))
                self.send(idx, rid, struct.pack('<BII', 0, off, ln))
                return
            with open(self.nv, 'r+b') as f:
                f.seek(off)
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            with open(self.nv, 'rb') as f:
                md5 = hashlib.md5(f.read(NV_SIZE) + NV_SECRET).hexdigest()
            with open(self.nv + '.md5', 'w') as f:
                f.write(md5)
            log('rfs: NV write off %#x len %#x' % (off, ln))
            self.send(idx, rid, struct.pack('<BII', 1, off, ln))
        elif idx == RFS_NV_READ:
            off, ln = struct.unpack_from('<II', d)
            with open(self.nv, 'rb') as f:
                f.seek(off)
                data = f.read(min(ln, max(0, NV_SIZE - off)))
            log('rfs: NV read off %#x len %#x' % (off, ln))
            self.send(idx, rid, struct.pack('<BII', 1, off, len(data)) + data)
        elif idx == RFS_OPEN:
            flags, nlen = struct.unpack_from('<II', d)
            name = d[8:8 + nlen].split(b'\0')[0].decode(errors='replace')
            p = self.path(name)
            if p is None:
                self.reply(idx, rid, -1, errno.EACCES)
                return
            if flags & os.O_CREAT:
                os.makedirs(os.path.dirname(p), mode=0o700, exist_ok=True)
            try:
                fd = os.open(p, (flags & (os.O_ACCMODE | os.O_CREAT | os.O_TRUNC | os.O_APPEND)), 0o600)
            except OSError as e:
                if not self.quiet:
                    log('rfs: open %s flags %#x -> %s' % (name, flags, errno.errorcode.get(e.errno)))
                self.reply(idx, rid, -1, e.errno)
                return
            self.files[fd] = name
            log('rfs: open %s flags %#x -> fd %d' % (name, flags, fd))
            self.reply(idx, rid, fd)
        elif idx in (RFS_READ, RFS_WRITE, RFS_LSEEK, RFS_CLOSE):
            fd = struct.unpack_from('<i', d)[0]
            if fd not in self.files:
                self.reply(idx, rid, -1, errno.EBADF)
                return
            if idx == RFS_READ:
                ln = struct.unpack_from('<I', d, 4)[0]
                data = os.read(fd, min(ln, MAX_FRAME))
                self.reply(idx, rid, len(data), 0, data)
            elif idx == RFS_WRITE:
                ln = struct.unpack_from('<I', d, 4)[0]
                n = os.write(fd, d[8:8 + ln])
                self.reply(idx, rid, n)
            elif idx == RFS_LSEEK:
                off, whence = struct.unpack_from('<iI', d, 4)
                self.reply(idx, rid, os.lseek(fd, off, whence))
            else:
                os.close(fd)
                del self.files[fd]
                self.reply(idx, rid, 0)
        elif idx == RFS_UNLINK:
            nlen = struct.unpack_from('<I', d)[0]
            name = d[4:4 + nlen].split(b'\0')[0].decode(errors='replace')
            p = self.path(name)
            try:
                if p is None:
                    raise OSError(errno.EACCES, '')
                os.unlink(p)
                log('rfs: unlink %s' % name)
                self.reply(idx, rid, 0)
            except OSError as e:
                self.reply(idx, rid, -1, e.errno)
        else:
            log('rfs: unknown idx %02x id %02x' % (idx, rid), d[:32].hex())
            self.reply(idx, rid, -1, errno.ENOSYS)


class Ipc:
    def __init__(self, fd, name):
        self.fd, self.name, self.mseq = fd, name, 0

    def send(self, group, index, typ, data=b''):
        self.mseq = (self.mseq + 1) & 0xff or 1
        os.write(self.fd, struct.pack('<HBBBBB', 7 + len(data), self.mseq, 0, group, index, typ) + data)
        log('%s TX %02x %-4s %02x:%02x %-5s' % (self.name, self.mseq, GROUP.get(group, '?'), group,
                                               index, TYPE.get(typ, typ)),
            '<%d bytes>' % len(data) if (group, index) in PRIVATE else data.hex())
        return self.mseq

    def sms_in(self, data, state):
        # SMS_INCOMING_MSG: u8 type, u8 ?, u16 sim_index, u8 tpid, u8 len, pdu
        tpid, ln = data[4], data[5]
        pdu = data[6:6 + ln]
        # SMS_DELIVER_REPORT, as rild sends it: type 2, error 0, the tpid, 0
        self.send(0x04, 0x06, EXEC, bytes([2, 0, 0, tpid, 0]))
        d = os.path.join(state, 'sms')
        os.makedirs(d, mode=0o700, exist_ok=True)
        stamp = time.strftime('%Y%m%d-%H%M%S')
        os.umask(0o077)
        while os.path.exists(os.path.join(d, stamp + '.pdu')):
            stamp += 'x'
        with open(os.path.join(d, stamp + '.pdu'), 'w') as f:
            f.write(pdu.hex() + '\n')
        try:
            sender, text, _ = sms_deliver(pdu)
            with open(os.path.join(d, stamp + '.txt'), 'w') as f:
                f.write('From: %s\n\n%s\n' % (sender, text))
            masked = sender[:4] + '*' * max(0, len(sender) - 6) + sender[-2:]
            log('SMS from %s, %d chars -> %s/%s.txt' % (masked, len(text), d, stamp))
        except Exception as e:
            log('SMS stored as PDU only (%r)' % e)
        return stamp

    verbose = False
    NOISE = {(0x07, 0x01), (0x07, 0x06), (0x0b, 0x0d)}     # signal bars, RSSI, service-mode JSON

    def show(self, b):
        if len(b) >= 7 and (b[4], b[5]) in self.NOISE and not self.verbose:
            return
        if len(b) < 7:
            log('%s RX short' % self.name, b.hex())
            return
        ln, ms, aseq, g, i, t = struct.unpack_from('<HBBBBB', b)
        data = b[7:]
        txt = '<%d bytes>' % len(data) if (g, i) in PRIVATE else data[:64].hex()
        if g == 0x80 and i == 0x01 and len(data) >= 5:      # GEN_PHONE_RES
            rg, ri, rt, code = struct.unpack_from('<BBBH', data)
            txt = 'res for %s %02x:%02x %s: 0x%04x' % (GROUP.get(rg, '?'), rg, ri, TYPE.get(rt, rt), code)
        elif (g, i) == (0x04, 0x01) and t == SET and len(data) >= 4:   # network verdict on a send
            txt = 'sms send result: ack 0x%04x mr %d' % struct.unpack_from('<HB', data, 1)
        elif g == 0x13 or all(32 <= c < 127 or c in (9, 10, 13) for c in data[4:-1] or b'\0'):
            if g not in (0xa,) and len(data) > 4:
                txt += '  ' + repr(data.split(b'\0')[0][-60:])
        log('%s RX %02x/%02x %-4s %02x:%02x %-5s %s' % (self.name, ms, aseq, GROUP.get(g, '?'), g, i,
                                                       TYPE.get(t, t), txt))


GSM7 = ('@\u00a3$\u00a5\u00e8\u00e9\u00f9\u00ec\u00f2\u00c7\n\u00d8\u00f8\r\u00c5\u00e5'
        '\u0394_\u03a6\u0393\u039b\u03a9\u03a0\u03a8\u03a3\u0398\u039e\x1b\u00c6\u00e6\u00df\u00c9'
        ' !"#\u00a4%&\'()*+,-./0123456789:;<=>?'
        '\u00a1ABCDEFGHIJKLMNOPQRSTUVWXYZ\u00c4\u00d6\u00d1\u00dc\u00a7'
        '\u00bfabcdefghijklmnopqrstuvwxyz\u00e4\u00f6\u00f1\u00fc\u00e0')


def gsm7(data, n):
    out, acc, bits = [], 0, 0
    for byte in data:
        acc |= byte << bits
        bits += 8
        while bits >= 7 and len(out) < n:
            out.append(acc & 0x7f)
            acc >>= 7
            bits -= 7
    return ''.join(GSM7[c] for c in out)


def bcd(b):
    return ''.join('%d%d' % (x & 15, x >> 4) for x in b).rstrip('f').replace('f', '')


def sms_deliver(pdu):
    """SMS-DELIVER with leading SMSC -> (sender, text, single-part?)"""
    i = 1 + pdu[0]                      # skip SMSC
    first = pdu[i]; i += 1
    alen = pdu[i]; atype = pdu[i + 1]; i += 2
    araw = pdu[i:i + (alen + 1) // 2]; i += (alen + 1) // 2
    if (atype & 0x70) == 0x50:          # alphanumeric
        sender = gsm7(araw, alen * 4 // 7)
    else:
        sender = ('+' if (atype & 0x70) == 0x10 else '') + bcd(araw)[:alen]
    pid, dcs = pdu[i], pdu[i + 1]; i += 2 + 7      # + timestamp
    udl = pdu[i]; i += 1
    ud = pdu[i:]
    skip = 0
    if first & 0x40:                    # UDH (multipart etc.)
        udhl = ud[0]
        if dcs & 0x0c == 0:
            skip = (udhl + 1) * 8
            septets = skip // 7 + (1 if skip % 7 else 0)
            txt = gsm7(ud, udl)[septets:]
            return sender, txt, False
        ud = ud[udhl + 1:]
        udl -= udhl + 1
    if dcs & 0x0c == 0x08:
        txt = ud[:udl].decode('utf-16-be', errors='replace')
    elif dcs & 0x0c == 0:
        txt = gsm7(ud, udl)
    else:
        txt = '<%d bytes 8-bit data>' % udl
    return sender, txt, not first & 0x40



def ucs(s):
    return '"%s"' % s.replace('"', '')


class Atd:
    """AT commands from ModemManager (via /dev/wwan0at0 <-> /dev/umts_atbridge) to IPC."""

    def __init__(self, fd, ipc, state):
        self.fd, self.ipc, self.state = fd, ipc, state
        self.buf = b''
        self.echo = True
        self.cmee = 0
        self.creg = self.cgreg = self.cereg = 0
        self.cops_fmt = 0
        self.pdu_wait = None            # length of the PDU after AT+CMGS
        self.pending = {}               # mseq -> callback(code)
        self.pending_rsim = {}          # mseq -> callback(sw1 sw2 len data | None)
        self.st = dict(cs=(1, 0xff, 0, 0), ps=(1, 0xff, 0, 0), plmn='', name='', rssi=None,
                       sim=None, imei='', imsi='', version='', smsc=b'')
        self.calls = {}                 # id -> (term, status, number)
        self.audio = False              # CP voice path + AP call audio up
        self.ring_until = 0
        self.sms_wait = 0               # deadline for the network's verdict on +CMGS
        self.next_ring = 0
        self.apns = None                # attach APNs still to try for this SIM
        self.apn_at = 0                 # when to try the next one if still unregistered
        self.smsdir = os.path.join(state, 'sms')

    # --- output
    def out(self, txt):
        log('at >', re.sub(r'\d{9,}', lambda m: m.group(0)[:3] + '*' * (len(m.group(0)) - 3),
                           repr(txt[:120])))
        try:
            os.write(self.fd, txt.encode())
        except OSError as e:
            log('at: write failed %r' % e)

    def final(self, ok=True, err=None):
        if ok:
            self.out('\r\nOK\r\n')
        elif err is not None and self.cmee:
            self.out('\r\n+CME ERROR: %d\r\n' % err)
        else:
            self.out('\r\nERROR\r\n')

    def urc(self, txt):
        self.out('\r\n%s\r\n' % txt)

    # --- SMS store: one file per message in sms/; deleted ones move to sms/read/
    def sms_list(self):
        try:
            return sorted(f[:-4] for f in os.listdir(self.smsdir) if f.endswith('.pdu'))
        except FileNotFoundError:
            return []

    def sms_pdu(self, stamp):
        with open(os.path.join(self.smsdir, stamp + '.pdu')) as f:
            return f.read().strip()

    def sms_new(self, stamp):
        lst = self.sms_list()
        if stamp in lst:
            self.urc('+CMTI: "SM",%d' % (lst.index(stamp) + 1))

    # --- state from IPC
    @staticmethod
    def act_at(act):
        if act in (0x21, 0x20, 0x07, 0x0a):
            return 7                    # E-UTRAN
        if act in (0x03, 0x04, 0x05, 0x06):
            return 2                    # UTRAN
        return 0

    @staticmethod
    def stat_at(st):
        return {0x01: 0, 0x02: 1, 0x03: 2, 0x04: 3, 0x05: 4, 0x06: 5, 0x07: 2}.get(st, 4)

    def reg_txt(self, key, n):
        stat, act, lac, cid = self.st[key]
        s = self.stat_at(stat)
        if n >= 2 and s in (1, 5):
            return '%d,"%04X","%08X",%d' % (s, lac, cid, self.act_at(act))
        return '%d' % s

    # SIM file access (SEC_RSIM_ACCESS), in the layout rild uses on this modem:
    # cmd, 0, aid_len, aid[16], fid le16, path count, path le16[5], 0, 0, p1, p2, p3
    USIM_AID = bytes.fromhex('a0000000871002f230ff018907020000')

    def rsim(self, cmd, fid, p1, p2, p3, then):
        path = [0x3f00] if (fid >> 8) in (0x2f, 0x3f) else [0x3f00, 0x7fff]
        d = bytes([cmd, 0, 16]) + self.USIM_AID + struct.pack('<HB', fid, len(path))
        d += b''.join(struct.pack('<H', x) for x in path).ljust(10, b'\0')
        d += bytes([0, 0, p1, p2, p3])
        m = self.ipc.send(0x05, 0x05, GET, d)
        self.pending_rsim[m] = then

        def failed(code):
            self.pending_rsim.pop(m, None)
            then(None)
        self.pending[m] = failed

    def apn_tick(self):
        """Let the CP register with the attach APN it has stored; only while it
        stays unregistered or limited (state 7) do we set one, per SIM, and
        walk the candidates 30 s apart. Data is a separate step after this."""
        st, now = self.st, time.time()
        if any(st[k][0] in (1, 5) for k in ('cs', 'ps')):
            self.apn_at = now + 30
            return
        if not st['imsi'] or self.calls:
            return
        if self.apns is None:
            self.apns = sim_apns(self.state, st['imsi'])
            self.apn_at = now + 30
            log('SIM %s: attach APN candidates %s' % (st['imsi'][:5], self.apns))
        if self.apns and now >= self.apn_at:
            apn = self.apns.pop(0)
            log('still unregistered: initial-attach APN %r' % apn)
            self.ipc.send(*attach_apn(apn))
            self.apn_at = now + 30

    def load_imsi(self, then=None):
        def done(r):
            if r and r[0] == 0x90 and r[2] >= 9:
                raw = r[3:3 + r[2]]
                self.st['imsi'] = bcd(raw[1:1 + raw[0]])[1:]
            if then:
                then()
        self.rsim(0xb0, 0x6f07, 0, 0, 9, done)

    def ipc_rx(self, g, i, t, aseq, d):
        st = self.st
        if (g, i) == (0x05, 0x05) and aseq in self.pending_rsim:
            self.pending.pop(aseq, None)
            self.pending_rsim.pop(aseq)(d if len(d) >= 3 else None)
            return
        if g == 0x80 and i == 0x01 and len(d) >= 5:
            code = struct.unpack_from('<H', d, 3)[0]
            cb = self.pending.pop(aseq, None)
            if cb and (code != 0x8000 or aseq not in self.pending_rsim):
                cb(code)
            elif cb:
                self.pending[aseq] = cb     # a SIM read answers with data next
        elif (g, i) == (0x08, 0x05) and len(d) >= 10:
            act, dom, stat = d[0], d[1], d[2]
            lac, cid = struct.unpack_from('<HI', d, 4)
            key = 'ps' if dom == 3 else 'cs'
            old = st[key]
            st[key] = (stat, act, lac, cid)
            if old != st[key]:
                if key == 'cs' and self.creg:
                    self.urc('+CREG: ' + self.reg_txt('cs', self.creg))
                if key == 'ps':
                    if self.cgreg:
                        self.urc('+CGREG: ' + self.reg_txt('ps', self.cgreg))
                    if self.cereg:
                        self.urc('+CEREG: ' + self.reg_txt('ps', self.cereg))
        elif (g, i) == (0x04, 0x01) and t == SET and len(d) >= 4 and self.sms_wait:
            self.sms_done(struct.unpack_from('<H', d, 1)[0], d[3])
        elif (g, i) == (0x08, 0x03) and len(d) >= 9:
            st['plmn'] = d[3:9].split(b'#')[0].split(b'\0')[0].decode(errors='replace')
        elif (g, i) == (0x08, 0x0c) and len(d) > 5:
            st['name'] = d[5:].split(b'\0')[0].decode(errors='replace').strip()
        elif (g, i) == (0x07, 0x06) and d:
            st['rssi'] = d[0]
        elif (g, i) == (0x05, 0x01) and d:
            st['sim'] = d[0]
            if d[0] in (0x00, 0x82, 0x83) and not st['imsi'] and not self.pending_rsim:
                self.load_imsi()
            if d[0] in (0x00, 0x82, 0x83) and not st['smsc']:
                self.ipc.send(0x04, 0x0a, GET)      # fails (0x8003) until the SIM is up
        elif (g, i) == (0x0a, 0x01) and len(d) > 2:
            st['version'] = d[1:33].split(b'\0')[0].decode(errors='replace')
        elif (g, i) == (0x0a, 0x02) and len(d) > 1:
            st['imsi'] = d[1:1 + d[0]].decode(errors='replace').strip('\0')
        elif (g, i) == (0x0a, 0x03) and len(d) > 2 and d[0] == 0x01:
            st['imei'] = d[2:2 + d[1]].split(b'\0')[0].decode(errors='replace')[:15]
        elif (g, i) == (0x04, 0x0a) and t == GET and d:
            st['smsc'] = d[:1 + d[0]]       # length-prefixed SCA, zero-padded to 12
        elif (g, i) == (0x02, 0x02):
            self.ipc.send(0x02, 0x06, GET)          # who is calling: CALL_LIST
            self.ring_until = time.time() + 60
            self.next_ring = 0
        elif (g, i) == (0x02, 0x05) and len(d) >= 4:
            if d[3] == 0x04:                         # released
                self.calls.pop(d[2], None)
                if not self.calls:
                    self.ring_until = 0
                    self.audio_off()
                    self.urc('NO CARRIER')
            else:
                self.ring_until = 0 if d[3] == 0x03 else self.ring_until
            self.ipc.send(0x02, 0x06, GET)
        elif (g, i) == (0x02, 0x06) and t == GET and d:
            calls, off = {}, 1
            for _ in range(d[0]):
                if off + 8 > len(d):
                    break
                _, typ, cid, term, cst, mpty, nlen, pfx = d[off:off + 8]
                num = d[off + 8:off + 8 + nlen].decode(errors='replace')
                calls[cid] = (term, cst, num)
                off += 8 + nlen
            self.calls = calls
            if not calls:
                self.ring_until = 0
                self.audio_off()

    def tick(self):
        self.apn_tick()
        if self.sms_wait and time.time() > self.sms_wait:
            self.sms_done(0xffff, 0)
        now = time.time()
        if self.ring_until and now < self.ring_until and now >= self.next_ring:
            inc = [c for c in self.calls.values() if c[1] == 5]
            if inc:
                self.urc('RING')
                num = inc[0][2]
                self.urc('+CLIP: "%s",%d' % (num, 145 if num.startswith('+') else 129))
                self.next_ring = now + 3

    # --- commands
    def feed(self, data):
        self.buf += data
        while True:
            if self.pdu_wait is not None:
                for term in (b'\x1a', b'\x1b'):
                    k = self.buf.find(term)
                    if k >= 0:
                        pdu, self.buf = self.buf[:k], self.buf[k + 1:]
                        n, self.pdu_wait = self.pdu_wait, None
                        if term == b'\x1b':
                            self.final(True)
                        else:
                            self.send_sms(pdu.decode(errors='replace').strip(), n)
                        break
                else:
                    return
                continue
            k = min([x for x in (self.buf.find(b'\r'), self.buf.find(b'\n')) if x >= 0], default=-1)
            if k < 0:
                return
            line, self.buf = self.buf[:k], self.buf[k + 1:]
            line = line.decode(errors='replace').strip()
            if not line:
                continue
            if self.echo:
                self.out(line + '\r')
            try:
                self.command(line)
            except Exception as e:
                log('at: %r failed: %r' % (line, e))
                self.final(False)

    def command(self, line):
        u = line.upper()
        if not u.startswith('AT'):
            return self.final(False)
        body = line[2:]
        # basic commands that can share a line (ATE0V1Q0 etc.)
        if not body or not body.startswith(('+', '%', '^', '$', 'D', 'd', 'I', 'i')):
            ub = body.upper()
            if 'E0' in ub:
                self.echo = False
            if 'E1' in ub:
                self.echo = True
            if ub.startswith(('H', 'H0')):
                return self.hangup()
            if ub.startswith('A') and not ub.startswith('AT'):
                return self.answer()
            return self.final(True)
        if body[0] in 'Dd':
            return self.dial(body[1:])
        if body[0] in 'Ii':
            self.out('\r\nSamsung SM-G970F (Exynos 9820 CP)\r\n')
            return self.final(True)
        for cmd in [c for c in body.split(';') if c]:
            if not cmd.startswith(('+', '%', '^', '$')):
                cmd = '+' + cmd
            if self.ext(cmd) is False:
                return
        self.final(True)

    def ext(self, cmd):
        """returns False when it already sent the final result (or will, async)"""
        st = self.st
        name, _, arg = cmd.partition('=')
        q = name.endswith('?')
        name = name.rstrip('?').upper()
        test = arg == '?'
        info = lambda t: self.out('\r\n%s\r\n' % t)

        if name in ('+CGMI', '+GMI'):
            info('Samsung')
        elif name in ('+CGMM', '+GMM'):
            info('SM-G970F')
        elif name in ('+CGMR', '+GMR'):
            info(st['version'] or 'unknown')
        elif name in ('+CGSN', '+GSN'):
            if not st['imei']:
                self.final(False, 10)
                return False
            info(st['imei'])
        elif name == '+GCAP':
            info('+GCAP: +CGSM')
        elif name == '+CMEE':
            if q:
                info('+CMEE: %d' % self.cmee)
            elif not test:
                self.cmee = int(arg or 0)
        elif name == '+CPIN':
            if test:
                pass
            elif st['sim'] in (0x00, 0x82, 0x83):
                info('+CPIN: READY')
            elif st['sim'] == 0x03:
                info('+CPIN: SIM PIN')
            elif st['sim'] == 0x80:
                self.final(False, 10)
                return False
            else:
                self.final(False, 14)       # SIM busy
                return False
        elif name == '+CIMI':
            def answer():
                if st['imsi']:
                    info(st['imsi'])
                self.final(bool(st['imsi']), 10)
            if st['imsi']:
                info(st['imsi'])
            elif st['sim'] in (0x00, 0x82, 0x83):
                self.load_imsi(answer)
                return False
            else:
                self.final(False, 10)
                return False
        elif name == '+CRSM':
            a = [x.strip() for x in arg.split(',')]
            try:
                cmd, fid = int(a[0]), int(a[1])
                p1, p2, p3 = (int(a[2]), int(a[3]), int(a[4])) if len(a) >= 5 else (0, 0, 0)
            except (ValueError, IndexError):
                self.final(False, 50)
                return False
            if cmd not in (176, 178, 192, 242):

                self.final(False, 4)
                return False

            def crsm(r):
                if r is None:
                    self.final(False, 13)
                    return
                info('+CRSM: %d,%d,"%s"' % (r[0], r[1], r[3:3 + r[2]].hex().upper()))
                self.final(True)
            self.rsim(cmd, fid, p1, p2, p3, crsm)
            return False
        elif name == '+CFUN':
            if q:
                info('+CFUN: 1')
            elif test:
                info('+CFUN: (0,1,4),(0)')
        elif name == '+COPS':
            if test:
                info('+COPS: (2,%s,"","%s",7),,(0,1,3),(0,2)' % (ucs(st['name']), st['plmn']))
            elif q:
                stat, act = st['cs'][0], st['ps'][1] if st['ps'][0] in (2, 6) else st['cs'][1]
                if self.stat_at(st['cs'][0]) in (1, 5) or self.stat_at(st['ps'][0]) in (1, 5):
                    oper = st['plmn'] if self.cops_fmt == 2 else st['name'] or st['plmn']
                    info('+COPS: 0,%d,"%s",%d' % (self.cops_fmt, oper, self.act_at(act)))
                else:
                    info('+COPS: 0')
            else:
                a = arg.split(',')
                if len(a) >= 2 and a[0] == '3':
                    self.cops_fmt = int(a[1])
        elif name in ('+CREG', '+CGREG', '+CEREG'):
            attr = name[1:].lower()
            key = 'cs' if name == '+CREG' else 'ps'
            if test:
                info('%s: (0-2)' % name)
            elif q:
                info('%s: %d,%s' % (name, getattr(self, attr), self.reg_txt(key, getattr(self, attr))))
            else:
                setattr(self, attr, int(arg or 0))
        elif name == '+CSQ':
            r = st['rssi']
            v = 99 if r is None or r == 0 else max(0, min(31, (113 - r) // 2))
            info('+CSQ: %d,99' % v)
        elif name == '+CLCC':
            for cid, (term, cst, num) in sorted(self.calls.items()):
                state = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4, 6: 5}.get(cst, 0)
                info('+CLCC: %d,%d,%d,0,0,"%s",%d' % (cid + 1, 1 if term == 2 else 0, state, num,
                                                     145 if num.startswith('+') else 129))
        elif name == '+CHUP':
            self.hangup()
            return False
        elif name in ('+CLIP', '+COLP', '+CCWA', '+CRC', '+CSSN', '+CSCS', '+CNMI', '+CSMS',
                      '+CGEREP', '+CMER', '+CUSD', '+CTZR', '+CTZU', '+CLIR'):
            if q or test:
                info('%s: %s' % (name, {'+CSCS': '"IRA"', '+CSMS': '0,1,1,1', '+CNMI': '2,1,0,0,0'}
                                 .get(name, '0')))
        elif name == '+CMGF':
            if q:
                info('+CMGF: 0')
            elif test:
                info('+CMGF: (0)')
            elif arg.strip() != '0':
                self.final(False, 303)
                return False
        elif name == '+CPMS':
            n = len(self.sms_list())
            if test:
                info('+CPMS: ("SM"),("SM"),("SM")')
            elif q:
                info('+CPMS: "SM",%d,100,"SM",%d,100,"SM",%d,100' % (n, n, n))
            else:
                info('+CPMS: %d,100,%d,100,%d,100' % (n, n, n))
        elif name in ('+CMGL', '+CMGR'):
            lst = self.sms_list()
            if name == '+CMGR':
                idx = int(arg) - 1
                if not 0 <= idx < len(lst):
                    self.final(False, 321)
                    return False
                sel = [idx]
            else:
                if test:
                    info('+CMGL: (0-4)')
                    return
                sel = range(len(lst)) if arg.strip() in ('', '4', '0', '1') else []
            for idx in sel:
                pdu = self.sms_pdu(lst[idx])
                tl = len(pdu) // 2 - 1 - int(pdu[:2], 16)
                if name == '+CMGR':
                    info('+CMGR: 1,,%d\r\n%s' % (tl, pdu))
                else:
                    info('+CMGL: %d,1,,%d\r\n%s' % (idx + 1, tl, pdu))
        elif name == '+CMGD':
            if test:
                info('+CMGD: (1-%d),(0-4)' % max(1, len(self.sms_list())))
                return
            a = arg.split(',')
            lst = self.sms_list()
            flag = int(a[1]) if len(a) > 1 and a[1] else 0
            victims = lst if flag >= 1 else [lst[int(a[0]) - 1]] if 0 < int(a[0] or 0) <= len(lst) else []
            rd = os.path.join(self.smsdir, 'read')
            os.makedirs(rd, mode=0o700, exist_ok=True)
            for v in victims:
                for ext in ('.pdu', '.txt'):
                    src = os.path.join(self.smsdir, v + ext)
                    if os.path.exists(src):
                        os.rename(src, os.path.join(rd, v + ext))
        elif name == '+CMGS':
            if test:
                return
            self.pdu_wait = int(arg)
            self.out('\r\n> ')
            return False
        elif name == '+CSCA':
            if q:
                sc = st['smsc']
                info('+CSCA: "%s",145' % (('+' + bcd(sc[2:2 + sc[0] - 1])) if len(sc) > 2 else ''))
        elif name == '+CNUM':
            pass
        elif name == '+CLCK':
            if '2' in arg.split(',')[1:2]:
                info('+CLCK: 0')
        elif name in ('+CGDCONT', '+CGACT', '+CGATT'):
            if q and name == '+CGATT':
                info('+CGATT: %d' % (1 if self.stat_at(st['ps'][0]) in (1, 5) else 0))
        else:
            self.final(False, 4)
            return False

    # --- actions
    def wait_res(self, mseq, then=None):
        def cb(code):
            if then:
                then(code)
            else:
                self.final(code == 0x8000)
        self.pending[mseq] = cb

    # stock rild after answering or dialling, and 09:09 00 once the last call
    # is gone (rild-sms-call.decoded.txt); the AP half is exynos-callaudio
    CALLAUDIO = '/usr/local/sbin/exynos-callaudio'

    def audio_on(self):
        if self.audio:
            return
        self.audio = True
        for d in ((0x09, 0x02, SET, b'\x00'),                   # mic unmuted
                  (0x09, 0x03, SET, b'\x01\x00'),               # path: handset
                  (0x09, 0x09, EXEC, b'\x01'),                   # voice PCM clock on
                  (0x09, 0x01, SET, b'\x01\x02')):              # volume
            self.ipc.send(*d)
        self.callaudio('start')

    def audio_off(self):
        if not self.audio:
            return
        self.audio = False
        self.ipc.send(0x09, 0x09, EXEC, b'\x00')
        self.callaudio('stop')

    def callaudio(self, what):
        if os.access(self.CALLAUDIO, os.X_OK):
            log('call audio', what)
            subprocess.Popen([self.CALLAUDIO, what], stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def answer(self):
        m = self.ipc.send(0x02, 0x04, EXEC)

        def done(code):
            if code == 0x8000:
                self.ring_until = 0
                self.audio_on()
            self.final(code == 0x8000)
        self.wait_res(m, done)

    def hangup(self):
        self.ring_until = 0
        if not self.calls:              # MM probes voice support with a bare ATH
            return self.final(True)
        self.wait_res(self.ipc.send(0x02, 0x03, EXEC))

    def dial(self, s):
        num = s.rstrip(';').strip()
        digits = num.lstrip('+')
        if not digits or not all(c in '0123456789*#' for c in digits):
            return self.final(False)
        data = bytes([0, 1, 0, len(digits), 0x11 if num.startswith('+') else 0]) + digits.encode()
        data += b'\0' * (5 + 86 - len(data))
        m = self.ipc.send(0x02, 0x01, EXEC, data)

        def done(code):
            if code == 0x8000:
                self.audio_on()
            self.final(code == 0x8000)
        self.wait_res(m, done)

    def send_sms(self, pdu_hex, tpdu_len):
        try:
            pdu = bytes.fromhex(pdu_hex)
        except ValueError:
            return self.final(False, 304)
        if not self.st['smsc']:
            self.ipc.send(0x04, 0x0a, GET)
        if pdu and pdu[0] == 0:                     # no SMSC: use the SIM's
            pdu = (self.st['smsc'] or b'\x00') + pdu[1:]
        data = bytes([2, 2, 0, len(pdu)]) + pdu
        m = self.ipc.send(0x04, 0x01, EXEC, data)

        def done(code):
            if code == 0x8000:          # CP took it; the network answers in SMS 04:01 SET
                self.sms_wait = time.time() + 60
            else:
                self.final(False, 500)
        self.wait_res(m, done)

    def sms_done(self, ack, mr):
        self.sms_wait = 0
        if ack & 0x8000 == 0:
            self.out('\r\n+CMGS: %d\r\n' % mr)
            self.final(True)
        else:                           # 0x80xx: RP cause xx (0x801d: no credit on this SIM)
            self.out('\r\n+CMS ERROR: %d\r\n' % (ack & 0xff if ack != 0xffff else 500))

# rild's opening on ipc0 after a modem boot (kit/modem/private/rfsboot.decoded.txt)
POWER_ON = [
    (0x0f, 0x3b, GET, b'{"jver":"1.0"}'),
    (0x01, 0x07, EXEC, b'\x02\x02'),          # PWR_PHONE_STATE: normal (radio on)
    (0x08, 0x03, GET, b''),                    # NET_SERVING_NETWORK
    (0x05, 0x01, GET, b''),                    # SEC_PIN_STATUS
    (0x08, 0x02, GET, b''),                    # NET_PLMN_SEL
    (0x0a, 0x01, GET, b'\xff'),                # MISC_ME_VERSION
    (0x08, 0x05, GET, b'\xff\x03'),            # NET_REGIST (ps)
    (0x08, 0x05, GET, b'\xff\x02'),            # NET_REGIST (cs)
    (0x08, 0x11, GET, b''),
    (0x0a, 0x02, GET, b''),                    # MISC_ME_IMSI
    (0x0a, 0x03, GET, b'\x01'),                # MISC_ME_SN: IMEI
    (0x04, 0x0a, GET, b''),                    # SMS_SVC_CENTER_ADDR
]



def sim_apns(state, imsi):
    """Initial-attach APN candidates for this SIM, best first: the override in
    <state>/apn, the provider DB's internet APN for the SIM's MCC+MNC (2- then
    3-digit MNC), then "internet" (what 1&1 26223 wants; the DB only lists
    web.vodafone.de for it)."""
    out = []
    try:
        out.append(open(os.path.join(state, 'apn')).read().strip())
    except OSError:
        pass
    try:
        import xml.etree.ElementTree as ET
        root = ET.parse('/usr/share/mobile-broadband-provider-info/serviceproviders.xml').getroot()
        for mnc in (imsi[3:5], imsi[3:6]):
            for prov in root.iter('provider'):
                if any(n.get('mcc') == imsi[:3] and n.get('mnc') == mnc
                       for n in prov.iter('network-id')):
                    out += [a.get('value') for a in prov.iter('apn')
                            if any(u.get('type') == 'internet' for u in a.iter('usage'))][:1]
    except Exception as e:
        log('provider DB: %r' % e)
    out.append('internet')
    return [x for i, x in enumerate(out) if x and x not in out[:i]]


def attach_apn(apn):
    """GPRS 0d:14 SET, the LTE initial-attach APN, as rild sends it (kit/modem/
    private/rild-1and1.decoded.txt): cid 1, 1, PDP type 6 (IPv4v6), APN in a
    100-byte field, 88 zero bytes (auth), then 01 06 00. Without it the 1&1
    SIM never registers: the CP camps on 26202 in limited service (state 7)."""
    return (0x0d, 0x14, SET, b'\x01\x01\x06' + apn.encode()[:99].ljust(100, b'\0') +
            bytes(88) + b'\x01\x06\x00')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--state', default='/var/lib/exynos-cp')
    ap.add_argument('--quiet-rfs', action='store_true')
    ap.add_argument('--no-power-on', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true', help='also log signal-strength updates')
    a = ap.parse_args()
    Ipc.verbose = a.verbose

    fifo = os.path.join(a.state, 'cmd')
    if not os.path.exists(fifo):
        os.mkfifo(fifo, 0o600)
    cmd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
    rfsfd = os.open('/dev/umts_rfs0', os.O_RDWR | os.O_NONBLOCK)
    ipc0 = Ipc(os.open('/dev/umts_ipc0', os.O_RDWR | os.O_NONBLOCK), 'ipc0')
    ipc1 = Ipc(os.open('/dev/umts_ipc1', os.O_RDWR | os.O_NONBLOCK), 'ipc1')
    rfs = Rfs(rfsfd, a.state, a.quiet_rfs)
    # the AT bridge creates /dev/wwan0at0 for ModemManager while we hold it open;
    # open it last, once the IPC side is there to answer
    try:
        atfd = os.open('/dev/umts_atbridge', os.O_RDWR | os.O_NONBLOCK)
    except OSError as e:
        log('no AT bridge (%r): ModemManager will not see the modem' % e)
        atfd = None
    at = Atd(atfd, ipc0, a.state) if atfd is not None else None

    p = select.poll()
    for fd in (rfsfd, ipc0.fd, ipc1.fd, cmd) + ((atfd,) if at else ()):
        p.register(fd, select.POLLIN)
    todo = [] if a.no_power_on else list(POWER_ON)
    next_tx = time.time() + 1.0
    while True:
        for fd, ev in p.poll(100):
            if ev & (select.POLLHUP | select.POLLERR) and fd != cmd:
                log('fd %d HUP/ERR: CP gone' % fd)
                return 1
            if fd == rfsfd:
                rfs.feed(os.read(fd, 65536))
            elif at and fd == atfd:
                d = os.read(fd, 4096)
                log('at <', re.sub(rb'\d{9,}', b'<num>', d[:80]))
                at.feed(d)
            elif fd in (ipc0.fd, ipc1.fd):
                ch = ipc0 if fd == ipc0.fd else ipc1
                b = os.read(fd, 65536)
                try:
                    ch.show(b)
                    if ch is ipc0 and len(b) >= 7:
                        stamp = None
                        if len(b) > 13 and b[4] == 0x04 and b[5] == 0x02:
                            stamp = ch.sms_in(b[7:], a.state)
                        if at:
                            at.ipc_rx(b[4], b[5], b[6], b[3], b[7:])
                            if stamp:
                                at.sms_new(stamp)
                except Exception as e:
                    log('%s RX (undecoded: %r)' % (ch.name, e), b[:32].hex())
            elif fd == cmd:
                # "group index type hexdata", e.g. "05 01 02" = SEC_PIN_STATUS GET
                for line in os.read(fd, 4096).decode(errors='replace').splitlines():
                    w = line.split()
                    if len(w) >= 3:
                        ipc0.send(int(w[0], 16), int(w[1], 16), int(w[2], 16),
                                  bytes.fromhex(''.join(w[3:])))
        if at:
            at.tick()
        if todo and time.time() >= next_tx:
            ipc0.send(*todo.pop(0))
            next_tx = time.time() + 0.1


if __name__ == '__main__':
    sys.exit(main())
