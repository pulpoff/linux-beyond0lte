#!/bin/sh
# scrub.sh ROOT: turn a copy of the dev S10e's root file system into a shareable image (run as root, on a COPY).
# The system, the launcher, its icon packs and wallpaper, and the installed apps stay. This phone's credentials,
# pairings, modem data and personal files go, and so do Samsung's firmware (s10e-firstboot copies it from each
# phone's own vendor partition) and what the POCO the image began as left behind.
set -eu
ROOT=$1
[ -x "$ROOT/usr/local/sbin/exynos-cpd" ] || { echo "not the S10e root: $ROOT" >&2; exit 1; }
cd "$ROOT"

# --- credentials and pairings of this phone
rm -f etc/NetworkManager/system-connections/*            # Wi-Fi networks and their passwords, the hotspot, the APN
rm -rf var/lib/NetworkManager/*                          # leases, secret key, timestamps
rm -rf var/lib/bluetooth/*                               # paired devices
rm -f etc/ssh/ssh_host_*                                 # the ssh.service drop-in makes new ones
rm -f etc/systemd/system/multi-user.target.wants/ssh.service etc/systemd/system/sshd.service
: > etc/machine-id                                       # a new one on first boot
rm -f var/lib/dbus/machine-id var/lib/systemd/random-seed var/lib/systemd/credential.secret
rm -rf var/lib/fwupd/* var/lib/upower/* var/lib/geoclue/* var/lib/AccountsService/icons/* 2>/dev/null || true
rm -rf var/lib/colord/*.db var/lib/udisks2/* var/lib/wtmpdb/* var/lib/usb_modeswitch/* 2>/dev/null || true  # monitors, drives, logins

# --- the modem: this phone's NV data (IMEI...), its remote files, SMS; s10e-firstboot seeds each phone from its efs
rm -rf var/lib/exynos-cp; mkdir -m 700 var/lib/exynos-cp

# --- Samsung's firmware: never shipped (s10e-firstboot)
F=usr/lib/firmware
rm -f $F/*_AUDIO_SLSI.bin $F/calliope*.bin $F/calliope*.dt $F/rxse.bin $F/txse*.bin $F/SoundBoosterParam.bin \
      $F/s5p-mfc-v13.fw $F/brcm/bcm4375B1_*.hcd $F/brcm/BCM4362A2.hcd $F/brcm/nvram.txt_* \
      $F/brcm/brcmfmac4375b1-pcie.* $F/brcm/bcm4775-shub.patch

# --- the POCO's leftovers: Qualcomm/Xiaomi firmware and DSP libraries no package owns, its DSP state
rm -rf $F/qcom/sm7150 $F/ath10k/WCN3990 $F/qca/crbtfw01.tlv $F/qca/crnv01.bin usr/share/qcom var/lib/hexagonrpc
rm -f usr/share/alsa/ucm2/Xiaomi/surya/VoiceCall.conf usr/share/feedbackd/themes/xiaomi,surya.json

# --- the user: a known password (lock screen, sudo); personal data out of the home
HASH=$(openssl passwd -6 147147)
sed -i "s|^user:[^:]*:|user:$HASH:|" etc/shadow
H=home/user
rm -rf $H/.cache $H/.purple $H/.mms $H/.vvm $H/.ssh $H/.bash_history $H/.lesshst $H/.python_history \
       $H/.local/share/keyrings $H/.local/share/recently-used.xbel $H/.local/share/Trash \
       $H/.local/share/chatty $H/.local/share/calls $H/.local/share/evolution $H/.config/evolution \
       $H/.config/goa-1.0 $H/.local/share/tracker3 $H/.local/share/gvfs-metadata $H/.local/state/wireplumber \
       $H/.local/share/gnome-shell/notifications $H/.local/share/waydroid $H/.var \
       $H/.local/share/neobrowser $H/.local/share/de.yesman.NeoBrowser $H/.config/yesman \
       $H/.local/share/webkitgtk $H/.local/share/epiphany $H/.config/epiphany $H/.local/share/geary \
       $H/.config/pulse/cookie $H/.local/state/neo-launcher 2>/dev/null || true
# what the apps remember: recent places, media and file databases, received cell broadcasts, app usage,
# the monitors this phone was plugged into, Quake's per-install key, its screenshots and log
L=$H/.local/share
rm -rf $L/gnome-maps $L/grilo-plugins $L/nautilus $L/cellbroadcastd $L/icc $L/folks $L/flatpak/db \
       $L/gnome-shell/session-active-history.json $L/gnome-shell/application_state \
       $L/q3demo/qkey $L/q3demo/demoq3/screenshots $L/q3demo/demoq3/games.log
rm -f $H/.local/share/applications/waydroid.*.desktop
# where the phone has been: the user's settings keep it (dconf, written with the host's dconf in a private session)
if [ -f $H/.config/dconf/user ]; then
	D=$(mktemp -d); mkdir -p $D/dconf; cp $H/.config/dconf/user $D/dconf/user
	XDG_CONFIG_HOME=$D dbus-run-session -- sh -c '
		dconf reset -f /de/yesman/app-devel/
		for k in last-lat last-lng last-location-name; do dconf reset /de/yesman/app/$k; done
		dconf reset /org/gnome/Weather/locations
		dconf reset /org/gnome/shell/weather/locations
		dconf reset /org/gnome/settings-daemon/plugins/color/night-light-last-coordinates
		dconf reset /org/gnome/maps/last-viewed-location
		dconf reset -f /org/gnome/clocks/
		dconf reset /org/gnome/control-center/last-panel'
	cp $D/dconf/user $H/.config/dconf/user; rm -rf $D
fi
for d in Pictures Videos Downloads Documents Music; do rm -rf $H/$d; mkdir -p $H/$d; done
chown -R 10000:10000 $H

# --- development: root's files, sources, old kernels and modules, backups of binaries and configs
find root -mindepth 1 -maxdepth 1 ! -name .bashrc ! -name .profile -exec rm -rf {} +
rm -rf usr/src/* var/lib/waydroid/* 2>/dev/null || true
for m in usr/lib/modules/*; do [ "${m##*/}" = 7.2.13-s10e ] || rm -rf "$m"; done
find usr/local etc/udev/rules.d etc/s10e -name '*.prev' -o -name '*.pre*' -o -name '*.before*' -o -name '*.orig' -o -name '*.bak' | xargs -r rm -rf
rm -f usr/local/bin/s10e-dsi etc/s10e-NOTES etc/s10e/NOTES-* etc/s10e/pid1 etc/s10e/pid1-args etc/s10e/pid1.off
rm -f core core.[0-9]*

# --- /boot: the kernel, the previous kernel (the same one: U-Boot's second menu entry), the public initramfs
B=boot/s10e
find $B -mindepth 1 ! -name Image ! -name initramfs.cpio.gz ! -name bootargs.env -exec rm -rf {} +
rm -f boot/*surya* boot/config-* boot/System.map-* boot/vmlinuz-* boot/initrd.img-*
ln -f $B/Image $B/Image.prev
sed -i 's| dyndbg="[^"]*"||' $B/bootargs.env

# --- logs, caches, temp, the first-boot markers
rm -rf var/log/journal/* var/lib/systemd/coredump/* var/lib/systemd/pstore/* tmp/* var/tmp/* var/lib/s10e/*
find var/log -type f \( -name '*.log' -o -name '*.gz' -o -name '*.[0-9]' -o -name '*.old' \) -delete 2>/dev/null || true
: > var/log/wtmp; : > var/log/btmp; : > var/log/lastlog 2>/dev/null || true
rm -f var/cache/apt/archives/*.deb var/cache/apt/*.bin
echo scrubbed
