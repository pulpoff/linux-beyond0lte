#!/usr/bin/env python3
# Listen on /dev/umts_ipc0 + /dev/umts_rfs0 (opening both lets the driver send
# INIT_END) and print what the CP sends. Samsung IPC: u16 len, u8 mseq, u8 aseq,
# u8 main, u8 sub, u8 type, data.
#   ipcmon.py [seconds]
import os, select, sys, time
MAIN = {1:'PWR',2:'CALL',4:'SMS',5:'SEC',6:'PB',7:'DISP',8:'NET',9:'SND',0xa:'MISC',
        0xb:'SVC',0xc:'SS',0xd:'GPRS',0xe:'SAT',0xf:'CFG',0x10:'IMEI',0x11:'GPS',0x12:'SAP',0x80:'GEN'}
TYPE = {1:'EXEC',2:'GET',3:'SET',4:'CFRM',5:'EVENT'}
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 30
ipc = os.open('/dev/umts_ipc0', os.O_RDWR | os.O_NONBLOCK)
rfs = os.open('/dev/umts_rfs0', os.O_RDWR | os.O_NONBLOCK)
names = {ipc: 'ipc0', rfs: 'rfs0'}
t0 = time.time()
p = select.poll()
for fd in names:
    p.register(fd, select.POLLIN)
while time.time() - t0 < dur:
    for fd, ev in p.poll(500):
        if ev & (select.POLLHUP | select.POLLERR):
            print('%7.3f %s HUP/ERR' % (time.time() - t0, names[fd])); sys.exit(1)
        b = os.read(fd, 65536)
        ts = '%7.3f %s' % (time.time() - t0, names[fd])
        if fd == ipc and len(b) >= 7:
            print(ts, 'len=%d seq=%02x/%02x %-4s %02x:%02x %-5s' % (len(b), b[2], b[3],
                  MAIN.get(b[4], '?'), b[4], b[5], TYPE.get(b[6], b[6])), b[7:48].hex(), flush=True)
        else:
            print(ts, 'len=%d' % len(b), b[:48].hex(), flush=True)
