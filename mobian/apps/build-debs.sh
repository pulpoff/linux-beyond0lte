#!/bin/sh
# build-debs.sh OUT [PHONE]: the Neo-on-Phosh packages for the Mobian image, into OUT:
#   neo-launcher_<v>_all.deb                 this tree's neo-launcher/ (+ the h2o and oneplus icon packs from
#                                            gnome/launcher/iconpacks, which are not in git: third-party art)
#   phosh-mobile-settings_0.58.0-1+suryaN   Mobile Settings with the POCO page (launcher, screen lock) and the About fix
#   libcamera*_0.7.2-1+suryaN               libcamera with the soft-ISP patches (gnome/device/camera)
#   phosh*/libphosh*_0.58.0-1+neoN           Phosh with debian/patches/0100-0103, as built on the phone
#   phoc_0.58.0-1+neoN                       Phoc with phoc/debian/patches (rotation without a dark panel, cross-faded)
#   *webkit*6.0*/*javascriptcore*6.0*_2.54.1-1+neoN   WebKitGTK 6.0 with touch-scroll resampling and 120 Hz pages
#                                            (gnome/device/webkit, cross-built on gpu3), kept on the phone in /root/webkit-neo
# The arm64 parts are built on the phone (root@PHONE, default 10.66.0.1) and fetched from there:
#   Phosh in /usr/src/neo/phosh-0.58.0 (see README), the plugin in /usr/src/neo/phosh-mobile-settings-0.58.0.
set -eu
OUT=$1 PHONE=${2:-10.66.0.1}
HERE=$(cd "$(dirname "$0")" && pwd)
PACKS=$HERE/../launcher/iconpacks
NEO_VERSION=1.0.5
PLUGIN_VERSION=0.58.0-1+surya1
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$PHONE"
SCP="scp -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$OUT"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT

# --- neo-launcher (arch all) -----------------------------------------------------------------------------------
P=$WORK/neo-launcher N=$HERE/neo-launcher
install -d "$P/DEBIAN" "$P/usr/bin" "$P/usr/share/neo-launcher/iconpacks" "$P/usr/lib/systemd/user/neo-launcher.service.d" \
    "$P/usr/share/glib-2.0/schemas"
install -m 644 "$N"/src/*.js "$N"/src/*.css "$P/usr/share/neo-launcher/"
install -m 755 "$N"/src/appmem.py "$P/usr/share/neo-launcher/"
install -m 755 "$N/neo-launcher" "$P/usr/bin/neo-launcher"
install -m 644 "$N/data/neo-launcher.service" "$P/usr/lib/systemd/user/"
install -m 644 "$N/data/renderer.conf" "$P/usr/lib/systemd/user/neo-launcher.service.d/"
install -m 644 "$N/data/de.yesman.neo.gschema.xml" "$P/usr/share/glib-2.0/schemas/"
for pack in oneui h2o oneplus; do
    [ -d "$PACKS/$pack" ] && cp -r --no-preserve=ownership "$PACKS/$pack" "$P/usr/share/neo-launcher/iconpacks/" || echo "no icon pack $pack" >&2
done
cat > "$P/DEBIAN/control" <<EOF
Package: neo-launcher
Version: $NEO_VERSION
Architecture: all
Maintainer: pulpoff <pashkovsky@gmail.com>
Depends: gjs, python3, gir1.2-gtk-4.0, gir1.2-adw-1, libgtk4-layer-shell0, gir1.2-gtk4layershell-1.0, grim, wtype, systemd, phosh (>= 0.58.0-1+neo13)
Section: x11
Priority: optional
Description: Neo, a Launcher3-like home screen for Phosh
 Pages, dock, an app drawer that follows the finger, icon packs and shapes, the
 back gesture from the screen edges and a One UI task view. Replaces Phosh's
 app grid; Phosh keeps the lock screen, notifications and quick settings.
EOF
cat > "$P/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
glib-compile-schemas /usr/share/glib-2.0/schemas
systemctl --global enable neo-launcher.service >/dev/null 2>&1 || true
EOF
cat > "$P/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
[ "$1" = remove ] || [ "$1" = purge ] || exit 0
systemctl --global disable neo-launcher.service >/dev/null 2>&1 || true
glib-compile-schemas /usr/share/glib-2.0/schemas || true
EOF
chmod 755 "$P/DEBIAN/postinst" "$P/DEBIAN/postrm"
dpkg-deb --root-owner-group -Zxz --build "$P" "$OUT/neo-launcher_${NEO_VERSION}_all.deb" >/dev/null

# --- Mobile Settings with the POCO page (plugins/surya in its tree) and the About patch, built on the phone ---------
for pkg in phosh-mobile-settings_ libpms-1.0-0_ libpms-1.0-common_; do
    $SCP "root@$PHONE:/usr/src/neo/${pkg}0.58.0-1+surya*_*.deb" "$OUT/" 2>/dev/null || true
done

# --- Phosh with the Neo patches: the newest build on the phone ------------------------------------------------
V=$($SSH 'cd /usr/src/neo && ls phosh_0.58.0-1+neo*_arm64.deb | sed "s/.*+neo\([0-9]*\)_.*/\1/" | sort -n | tail -1')
for pkg in phosh_ phosh-common_ phosh-plugins_ phosh-mobile-tweaks_ libphosh-0.45-0_; do
    $SCP "root@$PHONE:/usr/src/neo/${pkg}0.58.0-1+neo${V}_*.deb" "$OUT/"
done
# --- Stevia with neo-phosh/stevia's patch (slide down to close, a tap brings it back): the newest build on the phone
V=$($SSH 'cd /usr/src/neo/stevia 2>/dev/null && ls phosh-osk-stevia_0.58.0-1+neo*_arm64.deb | sed "s/.*+neo\([0-9]*\)_.*/\1/" | sort -n | tail -1')
if [ -n "$V" ]; then
    for pkg in phosh-osk-stevia_ phosh-osk-stevia-common_; do
        $SCP "root@$PHONE:/usr/src/neo/stevia/${pkg}0.58.0-1+neo${V}_*.deb" "$OUT/"
    done
fi
# --- Phoc with phoc/debian/patches: the newest build on the phone (/usr/src/neo/phoc-0.58.0) ----------------------
V=$($SSH 'cd /usr/src/neo && ls phoc_0.58.0-1+neo*_arm64.deb | sed "s/.*+neo\([0-9]*\)_.*/\1/" | sort -n | tail -1')
$SCP "root@$PHONE:/usr/src/neo/phoc_0.58.0-1+neo${V}_arm64.deb" "$OUT/"
# --- WebKitGTK 6.0 with gnome/device/webkit's patches (smooth touch scrolling, pages at 120 Hz) ---------------------
for pkg in libwebkitgtk-6.0-4_ libjavascriptcoregtk-6.0-1_ gir1.2-webkit-6.0_ gir1.2-javascriptcoregtk-6.0_; do
    $SCP "root@$PHONE:/root/webkit-neo/${pkg}2.54.1-1+neo*_arm64.deb" "$OUT/" 2>/dev/null || echo "note: no ${pkg%_} build on the phone"
done
# --- libcamera with gnome/device/camera/libcamera-patches (soft ISP exposure/white balance, the GPU released when a
# stream stops): each package's newest +surya build on the phone (/usr/src/neo; libcamera0.7 can be newer than the
# rest, it is the one the EGL fix changed)
for pkg in libcamera0.7 libcamera-ipa libcamera-v4l2 gstreamer1.0-libcamera; do
    f=$($SSH "cd /usr/src/neo && ls ${pkg}_0.7.2-1+surya*_arm64.deb 2>/dev/null | sort -V | tail -1")
    if [ -n "$f" ]; then $SCP "root@$PHONE:/usr/src/neo/$f" "$OUT/"; else echo "note: no $pkg build on the phone"; fi
done
# --- PipeWire's libcamera plugin with frame rates (gnome/device/camera/pipewire-libcamera): without them a hardware
# encoder cannot negotiate with a camera and Snapshot records 0-byte files
f=$($SSH "cd /usr/src/neo && ls libspa-0.2-libcamera_*+neo*_arm64.deb 2>/dev/null | sort -V | tail -1")
if [ -n "$f" ]; then $SCP "root@$PHONE:/usr/src/neo/$f" "$OUT/"; else echo "note: no libspa-0.2-libcamera build on the phone"; fi
# --- GNOME Snapshot (the Camera) with pinch-to-zoom (gnome/device/camera/snapshot): the newest +neo build on the phone
f=$($SSH "cd /usr/src/neo/snapshot && ls gnome-snapshot_*+neo*_arm64.deb 2>/dev/null | sort -V | tail -1")
if [ -n "$f" ]; then $SCP "root@$PHONE:/usr/src/neo/snapshot/$f" "$OUT/"; else echo "note: no gnome-snapshot build on the phone"; fi
ls -l "$OUT"
