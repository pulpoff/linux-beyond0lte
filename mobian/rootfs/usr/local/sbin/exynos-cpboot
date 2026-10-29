#!/usr/bin/env python3
# Boot the Exynos 9820 CP the way Samsung's cbd does (see MODEM-SPEC.md),
# against the exynos9820-cp driver's /dev/umts_boot0.
#
#   cpboot.py [--radio /dev/disk/by-partlabel/radio] [--nv nv_data.bin]
#             [--until reset|load|on|boot_on|all]
import argparse, ctypes, fcntl, os, struct, sys

def _IO(nr):
    return (ord('o') << 8) | nr

MODEM_ON, MODEM_RESET, MODEM_BOOT_ON, MODEM_BOOT_OFF = _IO(0x19), _IO(0x21), _IO(0x22), _IO(0x23)
MODEM_STATUS, MODEM_DL_START, XMIT_BOOT, SECURITY_REQ = _IO(0x27), _IO(0x28), _IO(0x40), _IO(0x53)

def log(*a):
    print('cpboot:', *a, flush=True)

def parse_toc(img):
    # 32-byte entries: char name[12]; u32 b_off, m_off, size, crc, entries/reserved
    toc = {}
    for i in range(16):
        e = img[i * 32:(i + 1) * 32]
        name = e[:12].split(b'\0')[0].decode(errors='replace')
        if not name:
            break
        b_off, m_off, size, crc, rsv = struct.unpack('<5I', e[12:32])
        toc[name] = (b_off, m_off, size, crc)
        log('TOC %-6s b_off 0x%08x m_off 0x%08x size 0x%08x crc 0x%08x' % (name, b_off, m_off, size, crc))
    return toc

def xmit(fd, data, m_offset, b_offset=0):
    buf = ctypes.create_string_buffer(data, len(data))
    mf = struct.pack('<QIIIII', ctypes.addressof(buf), len(data), m_offset, b_offset, 0, len(data))
    fcntl.ioctl(fd, XMIT_BOOT, mf)

def sec(fd, mode, p2=0, p3=0):
    log('security request mode %d 0x%x 0x%x' % (mode, p2, p3))
    fcntl.ioctl(fd, SECURITY_REQ, struct.pack('<4I', mode, p2, p3, 0))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--radio', default='/dev/disk/by-partlabel/radio')
    ap.add_argument('--nv', default='/var/lib/exynos-cp/nv_data.bin')
    ap.add_argument('--until', default='all', choices=['reset', 'load', 'on', 'boot_on', 'all'])
    a = ap.parse_args()

    with open(a.radio, 'rb') as f:
        head = f.read(0x1000)
        toc = parse_toc(head)
        need = max(b + s for (b, m, s, c) in toc.values() if s and b)
        f.seek(0)
        img = f.read(need)
    nv = open(a.nv, 'rb').read()
    cp_base = toc['BOOT'][1]          # CP address of the carve-out start (0x40000000)

    fd = os.open('/dev/umts_boot0', os.O_RDWR)

    log('reset'); fcntl.ioctl(fd, MODEM_RESET)
    sec(fd, 2)                         # re-init: opens the carve-out to the AP
    if a.until == 'reset':
        return

    for name in ('TOC', 'BOOT', 'MAIN', 'VSS'):
        b, m, s, c = toc[name]
        log('load %s 0x%x bytes at +0x%x' % (name, s, m - cp_base))
        xmit(fd, img[b:b + s], m - cp_base, b)
    b, m, s, c = toc['NV']
    log('load NV %d bytes at +0x%x' % (len(nv), m - cp_base))
    xmit(fd, nv[:s], m - cp_base)
    if a.until == 'load':
        return

    sec(fd, 7, toc['TOC'][1] - cp_base, toc['TOC'][2])          # verify TOC
    sec(fd, 0, toc['BOOT'][2], toc['MAIN'][2])                  # verify BOOT+MAIN, lock
    log('modem on'); fcntl.ioctl(fd, MODEM_ON)
    if a.until == 'on':
        return
    log('boot on'); fcntl.ioctl(fd, MODEM_BOOT_ON)
    log('status', fcntl.ioctl(fd, MODEM_STATUS))
    if a.until == 'boot_on':
        return
    log('dl start'); fcntl.ioctl(fd, MODEM_DL_START)
    for req, want in ((0x900D, 0xA00D), (0x9F00, 0xAF00)):
        os.write(fd, struct.pack('<I', req))
        got = struct.unpack('<I', os.read(fd, 4)[:4].ljust(4, b'\0'))[0]
        log('handshake 0x%04x -> 0x%04x' % (req, got))
        if got != want:
            sys.exit('cpboot: expected 0x%04x' % want)
    log('boot off (waits for INIT_START)'); fcntl.ioctl(fd, MODEM_BOOT_OFF)
    log('CP up')

if __name__ == '__main__':
    main()
