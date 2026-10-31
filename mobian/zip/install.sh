#!/system/bin/sh
# install.sh prepare|finish: the shell half of the S10e Mobian zip, run by updater-script in LineageOS
# recovery (toybox, mksh with 32-bit arithmetic: sizes are never computed here).
#   prepare  unmount /data and /metadata, carve "linux" (40 GiB) from the end of userdata with s10e-part,
#            and point /tmp/s10e-linux at it. An earlier install keeps its partition: only Linux is replaced.
#   finish   check the written file system; on a first install, format userdata and /metadata the way
#            Lineage's "Format data" does (shrinking userdata broke the old ones), so Android starts clean.
# A child of the updater cannot print to the recovery screen: on failure this writes error=<why> to
# /tmp/s10e.prop and exits 1, and updater-script aborts with it.
set -u
DISK=/dev/block/sda
BY=/dev/block/platform/13d60000.ufs/by-name
[ -e $BY/userdata ] || BY=/dev/block/by-name
P=/tmp/s10e.prop
LINUX_GIB=40

die() { echo "error=$*" >> $P; echo "s10e install: $*" >&2; exit 1; }

case "$1" in
prepare)
	: > $P
	for m in /sdcard /data /metadata; do
		grep -q " $m " /proc/mounts && { umount $m 2>/dev/null || umount -l $m 2>/dev/null; }
	done
	grep -q " /data " /proc/mounts && die "cannot unmount /data"
	chmod 755 /tmp/s10e-part
	OUT=$(/tmp/s10e-part apply $DISK $LINUX_GIB 2>/tmp/s10e-part.err)
	RC=$?
	[ $RC = 0 ] || die "partitioning failed ($RC): $(cat /tmp/s10e-part.err)"
	set -- $OUT
	case "$1" in
	created) echo "new=1" >> $P ;;
	exists)  echo "new=0" >> $P ;;
	*)       die "s10e-part said: $OUT" ;;
	esac
	N=$2
	DEV=$DISK$N
	# the kernel knows the partition (BLKPG); ueventd may be slow to make its node
	i=0; while [ ! -b $DEV ] && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
	if [ ! -b $DEV ]; then
		MM=$(cat /sys/class/block/sda$N/dev 2>/dev/null) || die "no /sys/class/block/sda$N"
		mknod $DEV b ${MM%%:*} ${MM##*:} || die "cannot make $DEV"
	fi
	echo "dev=$DEV" >> $P
	ln -sf $DEV /tmp/s10e-linux || die "cannot link $DEV"
	;;
finish)
	DEV=$(readlink /tmp/s10e-linux)
	[ -b "$DEV" ] || die "no Linux partition"
	sync
	e2fsck -fy $DEV > /tmp/s10e-fsck.log 2>&1
	[ $? -lt 4 ] || die "the written file system does not check: $(tail -n 2 /tmp/s10e-fsck.log)"
	if grep -q "^new=1" $P; then
		CF=
		[ "$(getprop external_storage.casefold.enabled)" = 1 ] && CF="-O casefold -C utf8"
		/system/bin/make_f2fs -f -g android -O project_quota,extra_attr $CF -O compression -O extra_attr \
			$BY/userdata > /tmp/s10e-f2fs.log 2>&1 || die "cannot format userdata: $(tail -n 2 /tmp/s10e-f2fs.log)"
		/system/bin/mke2fs -F -t ext4 -b 4096 $BY/omr > /tmp/s10e-omr.log 2>&1 ||
			die "cannot format /metadata: $(tail -n 2 /tmp/s10e-omr.log)"
		echo "formatted=1" >> $P
	fi
	sync
	;;
esac
exit 0
