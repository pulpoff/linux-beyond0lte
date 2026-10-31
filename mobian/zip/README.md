# Mobian for the Galaxy S10e: a LineageOS-recovery zip

Debian (Mobian) with Phosh and the Neo launcher on mainline Linux 7.2.13, next to Android, on the Galaxy S10e
(SM-G970F, **beyond0lte**, Exynos 9820). For **adb sideload** from LineageOS recovery.

## What the zip does

1. Checks the device is `beyond0lte`.
2. Shrinks `userdata` and adds a 40 GiB `linux` partition in the last 40 GiB of it (`tools/s10e-part`: reads
   and checks the GPT before writing, touches no other partition, refuses when userdata would drop under 16 GiB).
   A phone that already has `linux` keeps it, and only Linux is replaced.
3. Writes the Debian root file system to `linux`.
4. Writes U-Boot to the `recovery` partition (it boots Linux from `linux`; its menu, Volume Down at boot, can
   start Android or download mode).
5. **On a first install, formats Android's data** (`userdata` and `/metadata`, as Lineage's "Format data" does):
   shrinking userdata breaks the old ones. **Back up Android first.** Android then starts like a new phone.
6. Reboots into Linux.

## Install

1. LineageOS for beyond0lte, with its recovery on the `recovery` partition.
2. Recovery → Apply update → Apply from ADB, then on a computer: `adb sideload s10e-mobian-<date>.zip`.
   Accept the "Signature verification failed" prompt.
3. It reboots into Linux. The first boot copies the phone's own firmware (below) and restarts once.
   Lock screen: swipe up. User `user`, password **147147** (change it: Settings → Users, or `passwd`).

Power on from off starts Android. Android's "Restart → Recovery" (Lineage: Advanced restart) starts Linux, and
so does every restart from Linux.

## The phone's own firmware

The image carries no Samsung files. On the first boot `s10e-firstboot` copies them, read-only, from this
phone's partitions: audio DSP, Wi-Fi, Bluetooth and the video decoder from `vendor` (Lineage's or Samsung's),
and the modem's NV data from `efs` into `/var/lib/exynos-cp` (the modem daemon writes only that copy, never
`efs`). It needs no network.

## Not in this build

* The **sensor hub** (accelerometer, compass, light): its firmware is not on the phone's partitions.
* **Recovery**: U-Boot takes the recovery partition, so LineageOS recovery is gone. To sideload again, flash
  Lineage's recovery.img (Odin/Heimdall, or `dd` to the recovery partition from Linux) first.
* Wi-Fi and Bluetooth use the **Semco** module's NVRAM; phones with the Murata module may need
  `/usr/lib/firmware/brcm/brcmfmac4375b1-pcie.txt` pointed at another `nvram.txt_*`.

## Build (dev box, ~30 GB free on a real disk)

    tar of the dev phone's / (tar --one-file-system --numeric-owner --xattrs --acls) > BACKUP.tar.zst
    kit: PUBLIC=1 INIT=init-mainline RAW=out/initramfs-public.cpio.gz sh build.sh   # no developer key
    sudo sh build-image.sh BACKUP.tar.zst rootfs.img     # slim.sh + scrub.sh, shrunk
    sh pack.sh rootfs.img OUTDIR                          # s10e-mobian-<date>.zip + .sha256
