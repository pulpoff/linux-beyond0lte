#!/usr/bin/python3
# appmem.py APP_ID…: each app's memory as JSON {app_id: bytes}, for Neo's task view. Run apart from Neo, under a
# timeout: reading /proc of a process stuck in the kernel blocks the reader, and that must never be Neo's main loop
# (its system gestures stopped while a camera stream was stuck).
import json, os, re, subprocess, sys


def read(path):
    try:
        with open(path, 'rb') as f:
            return f.read()
    except OSError:
        return None


def bus_pid(app_id):
    # every GApplication owns its app id on the session bus
    try:
        out = subprocess.run(['busctl', '--user', 'call', 'org.freedesktop.DBus', '/org/freedesktop/DBus',
                              'org.freedesktop.DBus', 'GetConnectionUnixProcessID', 's', app_id],
                             capture_output=True, text=True, timeout=0.5).stdout.split()
        return out[1] if len(out) == 2 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def main(ids):
    uid = os.getuid()
    kids, procs = {}, []
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        stat = read(f'/proc/{pid}/stat')
        if not stat:
            continue
        ppid = stat[stat.rfind(b')') + 2:].split()[1].decode()
        kids.setdefault(ppid, []).append(pid)
        try:
            if os.stat(f'/proc/{pid}').st_uid == uid:
                procs.append(pid)
        except OSError:
            pass
    result = {}
    for app_id in ids:
        if app_id.startswith('waydroid.'):
            # An Android app (Waydroid): its processes run in the container, under Android's own uids, and the
            # process that opened the window is only waydroid-open (about 20 MB, the same for every app). Count
            # the package's processes ("pkg" and "pkg:service"); their smaps are not ours to read, so the private
            # memory from status (RssAnon), which anyone may read.
            pkg = app_id[len('waydroid.'):].encode()
            total = 0
            for p in os.listdir('/proc'):
                if not p.isdigit():
                    continue
                name = (read(f'/proc/{p}/cmdline') or b'').split(b'\0')[0]
                if name == pkg or name.startswith(pkg + b':'):
                    m = re.search(rb'^RssAnon:\s+(\d+) kB', read(f'/proc/{p}/status') or b'', re.M)
                    if m:
                        total += int(m.group(1)) * 1024
            if total:
                result[app_id] = total
            continue
        pid = bus_pid(app_id)
        if not pid:
            for p in procs:
                env = read(f'/proc/{p}/environ') or b''
                if f'/{app_id}.desktop\0'.encode() in env and b'GIO_LAUNCHED_DESKTOP_FILE=' in env:
                    pid = p
                    break
        if not pid:
            for p in procs:
                if app_id.encode() in (read(f'/proc/{p}/cmdline') or b''):
                    pid = p
                    break
        if not pid:
            continue
        total, todo = 0, [pid]
        while todo:
            p = todo.pop()
            m = re.search(rb'^Pss:\s+(\d+) kB', read(f'/proc/{p}/smaps_rollup') or b'', re.M)
            if m:
                total += int(m.group(1)) * 1024
            todo.extend(kids.get(p, []))
        if total:
            result[app_id] = total
    print(json.dumps(result))


main(sys.argv[1:])
