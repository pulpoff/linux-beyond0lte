Mobian on the Galaxy S10e (beyond0lte, SM-G970F)
===============================================

The userspace half of running this kernel on the phone: the boot loader, the initramfs, and the
services and configuration the hardware needs on Mobian (Debian trixie with Mobian's repository, Phosh).
The kernel is the rest of this repository; see the top-level README.md.

`rootfs/` mirrors where each file goes on the phone.

U-Boot (`uboot/`)
=================

Built from [chiffathefox/u-boot](https://github.com/chiffathefox/u-boot) (branch exynos9820) with
`u-boot-s10e.patch` applied. The patch adds `exynos9820-beyond0lte.dts` (copied here too, with the
dtsi files it changes) and the boot environment `exynos-mobile.env`.

* The DTB U-Boot is built with is the one Linux gets (`booti ... ${fdtcontroladdr}`).
* `repack.py` wraps `u-boot.bin` into LineageOS's recovery image header, padded to the partition size;
  write the result to the recovery partition (`dd of=/dev/disk/by-partlabel/recovery`) from Linux, or
  from Android with root.
* The kernel command line lives in `/boot/s10e/bootargs.env` (`s10e_args=...`), so it changes without
  reflashing U-Boot. The rescue entry ignores that file.
* Samsung's S-Boot follows PMU INFORM3 for the next boot; only Samsung's kernel clears it, so the
  environment sets it before every reset (normal, download, Android).

initramfs (`initramfs/init`)
============================

Brings up the USB gadget (NCM + RNDIS, fixed MACs, ssh on 172.16.42.1 for the rescue shell), finds the
root by `s10e.root=PARTLABEL=linux`, and switches to it. A failed boot leaves
`/var/lib/s10e/boot-attempt` behind (`s10e-boot-ok.service` clears it on success), and the next boot
stops in the rescue shell. `uboot/build.sh` packs it with busybox and dropbear (`RAW=initramfs.cpio.gz`);
the rescue shell's ssh key is `$AUTHORIZED_KEYS`. The script was written for one build machine and its
paths are absolute: adjust them before using it elsewhere.

Kernel
======

`kernel.config` is the configuration the phone runs. Modules install to `/lib/modules/7.2.13-s10e`.

Services and configuration
==========================

| File | What it does |
| --- | --- |
| `exynos-modem.service` | Loads `exynos9820-cp`, boots the CP from the `radio` partition (`exynos-cpboot`), then runs `exynos-cpd`. |
| `exynos-cpd` | The radio daemon in place of Samsung's rild: Samsung IPC over the shared-memory link, the CP's remote file system (NV writes go to its own copy, never the EFS partition), and an AT bridge (`/dev/wwan0at0`) through which ModemManager places calls and reads SMS. Sets the LTE attach APN only when the SIM does not register within 30 s; the APN comes from `/var/lib/exynos-cp/apn`, else from the provider database by the SIM's MCC/MNC. |
| `exynos-callaudio`, `exynos-callaudiod`, `org.mobian_project.CallAudio.service` | Call audio. In a call the voice path runs in ABOX's firmware (calliope) with PipeWire stopped; `exynos-callaudio start\|stop` (called by `exynos-cpd`) sets that up, and `exynos-callaudiod` replaces callaudiod for Phosh's speaker and mute buttons. |
| `s10e-gpu-load(.service)`, `s10e-gpu-manual.conf`, `phosh.service.d/gpu.conf` | panfrost is blacklisted and loaded only after checking that the G3D power domain is up, then phoc renders on it (`/run/s10e-gpu.env`). |
| `s10e-touch-bind(.service)` | The S6SY761 can NAK its first probe on a fast boot; this rebinds it. |
| `s10e-bt-addr(.service)` | Sets the Bluetooth address from the `efs` partition's `bt_addr`. |
| `s10e-usb-net.service`, `s10e-usb-watch(.service)`, `s10e-usb-reconnect` | usb0 networking (172.16.42.1), and the gadget rebound after the port was in host mode. |
| `s10e-cpuinfo(.service)` | Adds a `model name` to `/proc/cpuinfo` so Settings names the processor. |
| `s10e-boot-ok.service` | Clears the boot-attempt marker the initramfs sets. |
| `s10e-tone` | The codec's tone generator on the 3.5 mm jack, for testing. |
| `61-s10e-sensors.rules` | Mount matrix for the hub's accelerometer (it reports X and Y in Android's sign) and the proximity near level. |
| `ucm2/conf.d/S10e` | ALSA UCM for the speaker and microphone. |
| `NetworkManager/conf.d` | Wi-Fi uses the chip's own MAC (the one Android uses); usb0/usb1 stay with the gadget. |
| `gmobile/display-panels/samsung,beyond0lte.json`, `phosh.service.d/user.conf` | The camera hole for Phosh (G_RESOURCE_OVERLAYS, since gmobile has no S10e entry). |
| `boot/s10e/bootargs.env` | The kernel command line U-Boot reads. |

Out-of-tree modules (`modules/`)
================================

* `s10e-panel-bl`: panel brightness (AOR) over the DSIM command FIFO, as `/sys/class/backlight/s10e-panel`.
* `s10e-aud-probe`: a register probe for the audio block, for debugging.

Build against the kernel with `make -C <build> M=$PWD LOCALVERSION=`.

Bring-up notes (`docs/`, `modem/`, `sensorhub/`)
================================================

How each part was worked out, with the register and protocol details: display (DPU), GPU, MFC, sound
and ABOX, the modem (IPC, shared-memory link, `cpboot.py`, `cpd.py`, trace decoders), the sensor hub
(the transport layer and SSP, and the userspace scripts `shub-s1/s2/s3.py` that proved it before the
kernel driver), the RTC, USB 3, and a survey of the sensors, cameras and NFC the phone has.

Firmware
========

None is included. Copy it from the phone's own Android:

* `/lib/firmware/brcm/`: `brcmfmac4375b1-pcie.bin`, `.clm_blob`, `.txt` (Wi-Fi), the BCM4375 `.hcd`
  (Bluetooth), `bcm4775-shub.patch` (the sensor hub RAM patch, as text: Samsung's
  `bbd_patch_file_beyond.h` with the C string quoting removed).
* `/lib/firmware/`: `s5p-mfc-v13.fw`, `calliope_*.bin`, `calliope2.dt`.
