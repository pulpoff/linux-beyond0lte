#!/bin/sh
# pack.sh IMG OUTDIR: s10e-mobian-<date>.zip for LineageOS recovery's "Apply update" (adb sideload):
#   META-INF/.../update-binary   LineageOS's own (static aarch64 edify interpreter)
#   META-INF/.../updater-script  this folder's updater-script
#   install.sh, s10e-part        the shell half and the partitioning tool (tools/, static)
#   rootfs.img                   IMG (build-image.sh: the shrunk ext4 root)
#   recovery.img                 U-Boot (S10e U-Boot v46), written to the recovery partition
# Deflate: package_extract_file inflates rootfs.img straight onto the partition; zip 3.0 switches to zip64
# by itself for an entry over 4 GB.
set -eu
IMG=$1; OUT=$2
Z=$(cd "$(dirname "$0")" && pwd)
UBOOT=${S10E:-$HOME/S10e}/out/v46-dp.img
BIN=$HOME/yesman-zip/lineage-bin/update-binary
W=$OUT/zip.$$
mkdir -p "$W/META-INF/com/google/android"
cp "$BIN" "$W/META-INF/com/google/android/update-binary"
cp "$Z/updater-script" "$W/META-INF/com/google/android/updater-script"
cp "$Z/install.sh" "$W/install.sh"
cp "$Z/tools/s10e-part" "$W/s10e-part"
ln "$IMG" "$W/rootfs.img" 2>/dev/null || cp "$IMG" "$W/rootfs.img"
cp "$UBOOT" "$W/recovery.img"
F=$OUT/s10e-mobian-$(date +%Y%m%d).zip
rm -f "$F"
(cd "$W" && zip -q -r -6 -X "$F" META-INF install.sh s10e-part recovery.img rootfs.img)
rm -rf "$W"
(cd "$OUT" && sha256sum "$(basename "$F")" > "$(basename "$F").sha256")
ls -la "$F"; cat "$F.sha256"; unzip -l "$F"
