#!/usr/bin/env python3
# decode Samsung IPC frames (u16 len, u8 mseq, u8 aseq, u8 main, u8 sub, u8 type, data) from a rild strace
import re,sys
MAIN={1:'PWR',2:'CALL',4:'SMS',5:'SEC',6:'PB',7:'DISP',8:'NET',9:'SND',0xa:'MISC',0xb:'SVC',0xc:'SS',0xd:'GPRS',0xe:'SAT',0xf:'CFG',0x10:'IMEI',0x11:'GPS',0x12:'SAP',0x80:'GEN'}
TYPE={1:'EXEC',2:'GET',3:'SET',4:'CFRM',5:'EVENT',3:'SET'}
pat=re.compile(r'^(\d+)\s+(\S+) (read|write)\(\d+</dev/(umts_ipc[01])>, "((?:\\x[0-9a-f]{2})*)"')
for l in open(sys.argv[1]):
    m=pat.match(l)
    if not m: continue
    b=bytes.fromhex(m.group(5).replace('\\x',''))
    if len(b)<7: continue
    print(m.group(2)[:12], 'RX' if m.group(3)=='read' else 'TX', m.group(4)[-1],
          'seq=%02x/%02x %-4s %02x:%02x t=%x'%(b[2],b[3],MAIN.get(b[4],'%02x'%b[4]),b[4],b[5],b[6]), b[7:40].hex())
