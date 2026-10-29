#!/usr/bin/env python3
# Decode rild's umts_ipc0/ipc1/rfs0 traffic from an `strace -f -xx` capture.
#   tracedec.py trace ipc0_fd ipc1_fd rfs0_fd
import re, sys
MAIN = {1:'PWR',2:'CALL',4:'SMS',5:'SEC',6:'PB',7:'DISP',8:'NET',9:'SND',0xa:'MISC',0xb:'SVC',
        0xc:'SS',0xd:'GPRS',0xe:'SAT',0xf:'CFG',0x10:'IMEI',0x11:'GPS',0x12:'SAP',0x80:'GEN',0x42:'RFS'}
TYPE = {1:'EXEC',2:'GET',3:'SET',4:'CFRM',5:'EVENT'}
fds = {int(sys.argv[2]): 'ipc0', int(sys.argv[3]): 'ipc1', int(sys.argv[4]): 'rfs0'}
pend = {}
full = re.compile(r'^(\d+)\s+(\S+) (read|write)\((\d+), "((?:\\x[0-9a-f]{2})*)"')
unf = re.compile(r'^(\d+)\s+(\S+) read\((\d+), <unfinished')
res = re.compile(r'^(\d+)\s+(\S+) <\.\.\. read resumed>"((?:\\x[0-9a-f]{2})*)"')
def out(ts, op, fd, b):
    ch = fds[fd]; d = 'RX' if op == 'read' else 'TX'
    if ch == 'rfs0':
        if len(b) >= 6:
            L = int.from_bytes(b[0:4], 'little')
            print(ts, d, ch, 'len=%-6d rfslen=%-6d idx=%02x id=%02x' % (len(b), L, b[4], b[5]), b[6:60].hex())
        else:
            print(ts, d, ch, 'len=%d' % len(b), b.hex())
    elif len(b) >= 7:
        print(ts, d, ch, 'len=%-5d seq=%02x/%02x %-4s %02x:%02x %-5s' % (len(b), b[2], b[3],
              MAIN.get(b[4], '?'), b[4], b[5], TYPE.get(b[6], b[6])), b[7:60].hex())
for l in open(sys.argv[1], errors='replace'):
    m = full.match(l)
    if m:
        fd = int(m.group(4))
        if fd in fds:
            out(m.group(2), m.group(3), fd, bytes.fromhex(m.group(5).replace('\\x', '')))
        continue
    m = unf.match(l)
    if m:
        pend[m.group(1)] = int(m.group(3)); continue
    m = res.match(l)
    if m and m.group(1) in pend:
        fd = pend.pop(m.group(1))
        if fd in fds:
            out(m.group(2), 'read', fd, bytes.fromhex(m.group(3).replace('\\x', '')))
