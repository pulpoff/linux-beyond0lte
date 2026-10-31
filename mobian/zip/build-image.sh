#!/bin/sh
# build-image.sh BACKUP.tar.zst IMG: the public root file system from a full tar of the dev S10e (run as root).
# Unpacks into a new ext4 image, adds the public parts (first-boot import, public initramfs, fstab by
# partition label), slims (slim.sh) and scrubs (scrub.sh) it, and shrinks it to its content; the initramfs
# grows it to the whole "linux" partition on the first boot.
#
# Conservative ext4 features: LineageOS recovery's e2fsck checks the image after it is written.
# Mount it ONLY through this script (a service with a private copy of the mount once kept a loop device
# attached, and a later mount wrote stale metadata over the shrunk file system: see the S5e kit).
set -eu
TAR=$1
IMG=$2
Z=$(cd "$(dirname "$0")" && pwd)
K=${S10E:-$HOME/S10e}/kit
INITRD=${S10E:-$HOME/S10e}/out/initramfs-public.cpio.gz
M=$(dirname "$IMG")/mnt.$$
[ ! -e "$IMG" ] || { echo "$IMG exists" >&2; exit 1; }
[ -s "$INITRD" ] || { echo "no $INITRD (kit/build.sh with PUBLIC=1 INIT=init-mainline RAW=...)" >&2; exit 1; }

truncate -s 16G "$IMG"
mke2fs -q -t ext4 -L s10e_root -b 4096 -I 256 -O ^orphan_file,^metadata_csum_seed "$IMG"
mkdir -p "$M"
mount -o loop,noatime "$IMG" "$M"
trap 'umount "$M" 2>/dev/null || true' EXIT

echo "== unpacking $TAR"
zstd -dc "$TAR" | tar -C "$M" --numeric-owner --xattrs --xattrs-include='*' --acls -xpf - \
	--exclude=./root/* --exclude='./root/.[!.]*' --exclude=./usr/src/* --exclude=./var/lib/waydroid/* \
	--exclude=./var/cache/apt/archives/*.deb --exclude=./var/log/journal/* --exclude=./var/lib/exynos-cp/* \
	--exclude=./home/user/.cache/* --exclude=./home/user/Pictures/* --exclude=./home/user/Videos/* \
	--exclude=./usr/share/qcom --exclude=./usr/lib/firmware/qcom/sm7150 \
	--exclude='./usr/lib/modules/7.2.0-*' --exclude=./usr/lib/modules/7.2.10-surya \
	--exclude='./boot/s10e/Image.*' --exclude='./boot/s10e/initramfs.cpio.gz.*' --exclude=./tmp/* --exclude=./var/tmp/*
[ -x "$M/usr/local/sbin/exynos-cpd" ] || { echo "unpack failed" >&2; exit 1; }

echo "== public parts"
install -m 755 "$K/public/s10e-firstboot" "$M/usr/local/sbin/s10e-firstboot"
install -m 644 "$K/public/s10e-firstboot.service" "$M/etc/systemd/system/s10e-firstboot.service"
mkdir -p "$M/etc/systemd/system/sysinit.target.wants"
ln -sf ../s10e-firstboot.service "$M/etc/systemd/system/sysinit.target.wants/s10e-firstboot.service"
install -m 644 "$INITRD" "$M/boot/s10e/initramfs.cpio.gz"
printf '# the initramfs mounts the partition named "linux" (rw) and grows it to the whole partition\nPARTLABEL=linux  /  ext4  defaults,noatime  0 1\n' > "$M/etc/fstab"

echo "== slim"; sh "$Z/slim.sh" "$M"
echo "== scrub"; sh "$Z/scrub.sh" "$M"
du -sh "$M"

echo "== shrink"
umount "$M"; rmdir "$M"; trap - EXIT
sleep 1
if losetup -a | grep -qF "($IMG)"; then
	echo "a loop device still holds $IMG: not shrinking" >&2; losetup -a | grep -F "($IMG)" >&2; exit 1
fi
e2fsck -fy "$IMG" >/dev/null || [ $? -le 1 ]
resize2fs -M "$IMG" >/dev/null 2>&1; resize2fs -M "$IMG" >/dev/null 2>&1
BC=$(dumpe2fs -h "$IMG" 2>/dev/null | awk -F: '/^Block count/{print $2+0}')
truncate -s $((BC * 4096)) "$IMG"
e2fsck -fn "$IMG"
[ -n "${SUDO_USER:-}" ] && chown "$SUDO_USER": "$IMG"
ls -la "$IMG"
