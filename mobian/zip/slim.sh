#!/bin/sh
# slim.sh ROOT: in an arm64 chroot of the image (binfmt qemu), take out what only development needed:
# compilers, -dev packages, the GTK demos, and the POCO's kernel package. Run as root, on a copy, before
# scrub.sh. No autoremove: the -dev packages are what keeps some runtime libraries marked manual.
set -eu
R=$1
[ -x "$R/usr/local/sbin/exynos-cpd" ] || { echo "not the S10e root: $R" >&2; exit 1; }
for d in proc sys dev; do mount --bind /$d "$R/$d"; done
trap 'rm -f "$R/usr/sbin/policy-rc.d"; for d in dev sys proc; do umount "$R/$d" 2>/dev/null || true; done' EXIT
# no service starts or restarts inside the chroot
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"; chmod 755 "$R/usr/sbin/policy-rc.d"
ch() { chroot "$R" env DEBIAN_FRONTEND=noninteractive LC_ALL=C "$@"; }

PKGS=$(ch dpkg-query -W -f '${Package}\n' | grep -E '^(gcc|g\+\+|build-essential|dpkg-dev|make|cmake|meson|ninja-build|autoconf|automake|libtool|pkgconf|pkg-config|gdb|strace|ltrace|valgrind|clang|llvm)(-[0-9]+)?(-aarch64-linux-gnu)?$|-dev$|^gtk-4-examples$|^gtk-3-examples$|^linux-image-.*surya|^atomix-data$' || true)
echo "removing: $(echo $PKGS | wc -w) packages"
# purge also takes what depends on them: refuse when that is more than the list (KEEP_GOING=1 to accept)
if [ -n "$PKGS" ]; then
	EXTRA=$(ch apt-get -s purge $PKGS | sed -n 's/^Purg \([^ ]*\).*/\1/p' | sed 's/:arm64$//' | sort |
		comm -23 - "$(printf '%s\n' $PKGS | sort > /tmp/slim.$$; echo /tmp/slim.$$)")
	rm -f /tmp/slim.$$
	if [ -n "$EXTRA" ]; then
		echo "purging these would also remove: $EXTRA" >&2
		[ -n "${KEEP_GOING:-}" ] || exit 1
	fi
	ch apt-get purge -y $PKGS
fi
ch apt-get clean
echo slimmed
