Linux 7.2.13 for the Samsung Galaxy S10e (beyond0lte, SM-G970F)
================================================================

Mainline Linux for the Samsung Galaxy S10e (SM-G970F, codename beyond0lte, Exynos 9820). It runs the
phone on **Mobian** (Debian trixie with Mobian's repository) with Phosh, next to LineageOS.

The tree is stable **v7.2.13**, plus the Exynos 9820 work of
[chiffathefox/exynos-9820-mainline-linux](https://github.com/chiffathefox/exynos-9820-mainline-linux)
(7.2-rc2 based, rebased here onto 7.2.13), plus the S10e changes listed below. The device tree is not in
this tree: it is built into U-Boot and handed to Linux from there (see `mobian/uboot/`).

What works
==========

| Part | State |
| --- | --- |
| Display | 1080x2280 AMOLED (S6E3FA7) on DECON, taken over from the bootloader; brightness through `s10e_panel_bl` |
| GPU | Mali-G76 MP12 with panfrost, OpenGL ES 3.1, DVFS 156–702 MHz (simple_ondemand) |
| Touchscreen | S6SY761 |
| Wi-Fi | BCM4375 B1 on PCIe, 2.4 and 5 GHz (new Exynos 9820 PCIe driver, brcmfmac support for the 4375) |
| Bluetooth | BCM4375 B1 on UART, Bluetooth LE included |
| Sound | speaker and microphones through the CS47L93 codec and CS35L41 amps; media playback drives ABOX without its DSP firmware, calls use it |
| Modem | Shannon CP boots from the phone's own `radio` partition; voice calls with audio, SMS receive, LTE registration |
| Sensors | accelerometer (screen rotation), ambient light, proximity and compass through the BCM4775 sensor hub |
| Video | MFC hardware decode |
| USB | device mode (networking), host mode (USB 2) with Type-C orientation from the MAX77705 |
| Battery | MAX77705 charger and fuel gauge; Qi wireless charging at 5 V through the charger's WCIN input |
| RTC | S2MPS19 real-time clock |
| Buttons | power, volume, haptics |

Not done: mobile data (the modem registers on LTE, the data path is missing), cameras, GPS,
NFC, USB 3 SuperSpeed (the PHY trains, link polling does not finish yet), suspend (s2idle resets the SoC,
so it is masked), CPU idle states, MFC encode, the flash LED, fast (10 V) wireless charging, Wireless PowerShare and DisplayPort.

Booting it
==========

The phone keeps LineageOS. **U-Boot goes into the recovery partition**, repacked into LineageOS's
recovery image header (`mobian/uboot/repack.py`). It loads `/boot/s10e/Image`, `initramfs.cpio.gz` and
`bootargs.env` from the partition labelled `linux` (the Debian root filesystem; on the test phone it was
carved out of `userdata`) and passes its own device tree. `adb reboot recovery` or a plain `reboot` from
Linux gets there. Volume Down in U-Boot opens the menu: Mobian, Mobian with the previous kernel
(`Image.prev`), a USB rescue shell, LineageOS, download mode, console, power off.

Build with:

```
make ARCH=arm64 LLVM=1 LOCALVERSION= olddefconfig      # starting from mobian/kernel.config as .config
make ARCH=arm64 LLVM=1 LOCALVERSION= Image modules
```

The kernel release is `7.2.13-s10e`. Pass `LOCALVERSION=` to every make, or it gains a `+`.

Changes for the Galaxy S10e
===========================

On top of chiffathefox's Exynos 9820 tree (clocks, pinctrl, UFS, ACPM, the PMIC, simple display):

* **Display:** `drivers/gpu/drm/tiny/exynos9820-decon-kms.c` keeps the bootloader's DECON/DSIM setup and
  turns it into a real KMS device; `panel-samsung-s6e3fa7-beyond0.c`; simpledrm reports a DSI connector
  so Phosh treats the panel as built-in.
* **GPU:** panfrost on the G76 (the G3D power domain stays on: re-powering it needs a secure-world
  restore that mainline does not do).
* **Wi-Fi:** `drivers/pci/controller/dwc/pcie-exynos9820.c` (root complex and PHY), and brcmfmac support
  for the BCM4375 B1, including the shared `host_cap` the v7 PCIe protocol needs before the firmware
  consumes receive buffers.
* **Bluetooth:** CMU_PERIC1 clocks for UART_BT, the USI wrapper, hci_bcm power sequencing.
* **Sound:** `sound/soc/samsung/exynos9820-abox.c` drives ABOX's DMA and routing directly for media,
  and loads its DSP firmware (calliope) for the voice path of calls; `s10e_madera.c` machine driver; CS35L41 changes for the S10e amps.
* **Modem:** `drivers/misc/exynos9820-cp.c`, the boot and shared-memory link to the Shannon CP. The radio
  stack runs in userspace (`exynos-cpd`, `mobian/modem/`): ModemManager talks AT over a WWAN AT bridge,
  and `exynos-cpd` translates that to Samsung IPC.
* **Sensor hub:** `drivers/iio/common/bcm4775_shub/`. The BCM4775 boots from ROM and needs a RAM patch on
  every power-up; the driver loads it (`brcm/bcm4775-shub.patch`, from the phone's Android), speaks
  Broadcom's transport layer and Samsung's SSP protocol on top, and exposes the accelerometer, light and
  proximity sensors as IIO devices for iio-sensor-proxy. Each sensor runs only while it is read. The
  compass is a tilt-compensated heading worked out from the hub's calibrated magnetometer and the
  accelerometer, as the triggered buffer iio-sensor-proxy expects; it appears once the hub has booted.
  `mobian/sensorhub/` has the protocol notes and the userspace scripts it was worked out with.
* **USB:** `drivers/usb/typec/max77705.c` (Type-C port and orientation); the USB 3 DRD PHY for the
  Exynos 9820 with orientation-aware lane setup; dwc3 quirks stock sets in GUSB3PIPECTL (elastic buffer
  mode, U-state exit in P1-P3, failed U1/U2 exit to Recovery).
* **Video:** MFC v12 decode in s5p-mfc.
* **Power:** MAX77705 charger and MAX17042-family fuel gauge changes, including the WCIN input the IDT
  wireless receiver feeds (selected, limited to 820 mA after a ramp from 480 mA as stock does, reported
  as online); S2MPS19 RTC and 32 kHz clock over
  ACPM; Exynos 9820 PMU and power domains.

Firmware
========

None is included; it comes from the phone's own Android (`/vendor/firmware`, `/vendor/etc`): the
BCM4375 Wi-Fi firmware, CLM blob and NVRAM, the Bluetooth `.hcd`, `s5p-mfc-v13.fw`, the ABOX
`calliope_*` files, and the sensor hub patch (`bbd_patch_file_beyond.h` in Samsung's kernel source,
converted to text). The modem image is read from the `radio` partition at boot.

mobian/
=======

The userspace half: U-Boot's boot menu and the S10e device tree, the initramfs init, the kernel config,
and the services and configuration the hardware needs on Mobian. See `mobian/README.md`.
