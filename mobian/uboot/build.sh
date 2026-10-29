#!/bin/sh
# S10e debug recovery: LineageOS's recovery image with our initramfs instead
# of its ramdisk. Kernel and recovery_dtbo are Lineage's, byte for byte.
set -eu
TOP=${TOP:-$HOME/S10e}
K=$TOP/kit
BK=$TOP/backup
DEBS=$HOME/gts4lv/debs/root
OUT=$TOP/out
W=$(mktemp -d $OUT/work.XXXX)
R=$W/rootfs
mkdir -p $R/bin $R/usr/sbin $R/usr/lib/aarch64-linux-gnu $R/lib $R/etc $R/root/.ssh
cp $DEBS/usr/bin/busybox $R/bin/busybox
cp $K/${INIT:-init} $R/init; chmod 755 $R/init
for f in usr/sbin/dropbear usr/sbin/kexec usr/sbin/mke2fs usr/sbin/e2fsck usr/sbin/resize2fs usr/bin/tar; do
	cp $DEBS/$f $R/usr/sbin/
done
while read l || [ -n "$l" ]; do cp -L $DEBS/usr/lib/aarch64-linux-gnu/$l $R/usr/lib/aarch64-linux-gnu/; done < $HOME/gts4lv/debs/idx/libs.txt
aarch64-linux-gnu-gcc -static -Os -s -o $R/usr/sbin/dmlinear $HOME/gts4lv/kit/tools/dmlinear.c
ln -s ../usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 $R/lib/ld-linux-aarch64.so.1
ln -s ../usr/lib/aarch64-linux-gnu $R/lib/aarch64-linux-gnu
cp $DEBS/etc/mke2fs.conf $R/etc/mke2fs.conf
echo "root:x:0:0:root:/root:/bin/sh" > $R/etc/passwd
echo "root::0::::::" > $R/etc/shadow
echo "/bin/sh" > $R/etc/shells
cp ${AUTHORIZED_KEYS:-$HOME/.ssh/id_ed25519.pub} $R/root/.ssh/authorized_keys
chmod 700 $R/root/.ssh; chmod 600 $R/root/.ssh/authorized_keys
(cd $R && find . | cpio -o -H newc -R 0:0 --quiet | gzip -9) > $W/initramfs.cpio.gz
[ -n "${RAW:-}" ] && { cp $W/initramfs.cpio.gz $RAW; echo "initramfs -> $RAW"; rm -rf $W; exit 0; }
python3 $K/repack.py $BK/recovery.img $W/initramfs.cpio.gz $OUT/recovery-s10e-debug.img --pad $(stat -c %s $BK/recovery.img)
sha256sum $OUT/recovery-s10e-debug.img | tee $OUT/recovery-s10e-debug.img.sha256
rm -rf $W
