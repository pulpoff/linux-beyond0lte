Linux 7.2.15 for the Samsung Galaxy S10e (beyond0lte, SM-G970F)
================================================================

Mainline Linux for the Samsung Galaxy S10e (SM-G970F, codename beyond0lte, Exynos 9820). It runs the
phone on **Mobian** (Debian trixie with Mobian's repository) with Phosh, next to LineageOS.

The tree is stable **v7.2.15**, plus the Exynos 9820 work of
[chiffathefox/exynos-9820-mainline-linux](https://github.com/chiffathefox/exynos-9820-mainline-linux)
(7.2-rc2 based, rebased here onto 7.2.13), plus the S10e changes listed below. The device tree is not in
this tree: it is built into U-Boot and handed to Linux from there (see `mobian/uboot/`).

7.2.15 is the last 7.2 stable release: kernel.org has marked 7.2 end of life, and this tree stays on it.
Use the `main` branch; every update is one commit of the whole tree, this README and `mobian/`.

New in this version
===================

* **Linux 7.2.15.** 7.2.14 and 7.2.15 change only core code (io_uring, keys, signals and exit, tracing, the
  scheduler, Bluetooth, SMB, exec), none of the S10e's drivers.
* **Suspend to idle is steadier.**
  * A sleep within a few seconds of a sound crashed the phone: the sound block's power domain went off
    under a running ABOX, and the secure-world restore after the failed power-off was an SError. ABOX now
    refuses a system sleep while it is active, for a few seconds after each sound. This is a stopgap (see
    Sound).
  * A video playing through an s2idle left the MFC dead until the player closed. Fixed (see Video).
  * The CS47L93 codec never powered down, and every s2idle disabled its core supply a second time. Fixed
    (see Sound).
  * While the phone sleeps, Wi-Fi is in strict power save (PM_MAX): 203 mA against 214 mA on battery,
    mostly asleep.
  * The sensor hub's resume recovery could poll without end, so the phone never slept and ran hot. Fixed.
* **The SoC's own sleep state is in the kernel, but not used yet** (`exynos9820-sleep`, off by default). It
  drew 78 mA, against about 190 mA in s2idle. Entering it can take up to 8 s, and Wi-Fi does not come back
  from it without a driver reload. See Suspend.
* **Rear flash LED** (torch and flash) and **NFC** drivers. Their device tree nodes are not yet in
  `mobian/uboot/`.

What works
==========

| Part | State |
| --- | --- |
| Display | 1080x2280 AMOLED (S6E3FA7) on DECON, taken over from the bootloader; brightness through `s10e_panel_bl`; calibrated "Natural" colour (the panel's mDNIe, set by the panel driver); panel self-refresh, so an idle screen sends no frames; an Always On Display whose analog clock the panel draws and ticks by itself in its low power mode, so the phone sleeps through it |
| GPU | Mali-G76 MP12 with panfrost, OpenGL ES 3.1, DVFS 156–702 MHz (simple_ondemand) |
| Touchscreen | S6SY761, double tap to wake |
| Wi-Fi | BCM4375 B1 on PCIe, 2.4 and 5 GHz (new Exynos 9820 PCIe driver, brcmfmac support for the 4375); stays associated while the phone sleeps and wakes it for unicast traffic |
| Bluetooth | BCM4375 B1 on UART, Bluetooth LE included |
| Sound | speaker and microphones through the CS47L93 codec and CS35L41 amps; media playback drives ABOX without its DSP firmware, calls use it; a call's downlink and uplink as PCMs of their own, at the rates the modem's voice path uses, for a GSM gateway; a system sleep waits until ABOX has been idle for a few seconds (a stopgap, see Sound below) |
| Modem | Shannon CP boots from the phone's own `radio` partition; voice calls with audio, SMS, LTE data (`rmnet0`, ~47/34 Mbit/s), APN from the SIM |
| Sensors | accelerometer (screen rotation), ambient light, proximity and compass through the BCM4775 sensor hub, through suspend and resume |
| Cameras | rear wide S5K2L4 with AK7375 autofocus, rear ultra-wide S5K3P9, front S5K3J1, through a new CSIS/write-DMA driver and libcamera's software ISP (tuning in `mobian/camera/`) |
| Video | MFC hardware decode: H.264, HEVC, VP9; playback carries on through an s2idle |
| DisplayPort | USB-C DP alt mode to HDMI docks: 1080p60/120 with audio, a second CRTC on DECON2, HDMI CEC through docks that tunnel it |
| USB | device mode (networking), host mode with hubs, USB 3 SuperSpeed through docks, Type-C orientation from the MAX77705 |
| Battery | MAX77705 charger and fuel gauge; Qi wireless charging at 5 V through the charger's WCIN input; on a 0.5 A USB port it shows as charging, as Android does; without USB PD the input limit follows the Type-C current and BC1.2 detection (up to 1.8 A), as stock does |
| Suspend | the phone sleeps (s2idle) a few seconds after the screen goes off, whenever nothing needs it awake (`s10e-sleepd`); calls, SMS, Wi-Fi, the power key, a double tap and the charger wake it. Kernel timers stop while it sleeps (a WFI idle state for suspend to idle). About 200 mA asleep with Wi-Fi associated. The SoC's own sleep state is in the kernel but not used yet (below) |
| Flash | rear flash LED as torch and flash (`white:flash`); needs a device tree node that `mobian/uboot/` does not have yet |
| NFC | the S3NRN82 probes as `nfc0` (s3fwrn5); reading tags is untested; needs device tree nodes that `mobian/uboot/` does not have yet |
| Watchdog | the SoC watchdog, fed by systemd: a stalled SoC resets by itself, and the kernel log of a panic survives in pstore |
| RAM | 5464 MiB usable, after handing four of the vendor's camera and secure carve-outs (449 MiB) back to the kernel |
| RTC | S2MPS19 real-time clock |
| Buttons | power, volume, haptics |

Not done: GPS, the fingerprint sensor (S-Boot locks its SPI bus to the secure world, so Linux cannot reach it
at all), NFC beyond the driver probing, VoLTE, IPv6 on mobile data, the front camera's tuning, everyday use of
the SoC's own sleep state (s2idle keeps the SoC's rails up, so the phone still draws about 0.7 W asleep, far
from Android's standby), CPU idle states beyond WFI (C2 hangs the SoC), the display link's power-down in the
AOD (Samsung's DOZE_SUSPEND), MFC encode, fast (10 V) wireless charging and Wireless PowerShare. The charger's BC1.2
limits are checked on a PC port only.

Installing it
=============

`mobian/zip/` builds the LineageOS recovery zip (`adb sideload`): it moves the end of `userdata` into a 40 GiB
`linux` partition, writes the root filesystem and U-Boot, formats Android's data on a first install, and
reboots into Linux. `s10e-firstboot` then copies the phone's own firmware from `/vendor` (see Firmware).

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

The kernel release is `7.2.15-s10e`. Pass `LOCALVERSION=` to every make, or it gains a `+`.

Changes for the Galaxy S10e
===========================

On top of chiffathefox's Exynos 9820 tree (clocks, pinctrl, UFS, ACPM, the PMIC, simple display):

* **Display:** `drivers/gpu/drm/tiny/exynos9820-decon-kms.c` keeps the bootloader's DECON/DSIM setup and
  turns it into a real KMS device; `panel-samsung-s6e3fa7-beyond0.c`, which also loads the panel's mDNIe
  colour tables (Samsung's "Natural" mode: the bootloader leaves a violet-tinted one); simpledrm reports a DSI connector
  so Phosh treats the panel as built-in. Panel self-refresh (`exynos9820-decon-kms.psr`, on by default): the TE
  trigger starts a frame only when the picture has changed, so a still screen or the AOD clock costs no frames
  between updates.
* **Always On Display:** the panel driver's `lpm` (sysfs: 0, or 2, 10, 30 or 60 nit) puts the S6E3FA7 in Samsung's
  HLPM, where it refreshes from its own memory at a low rate and voltage; `self_clock` (`analog X Y UTCOFF [12|24]`,
  `off`, `reload`) starts the panel's own analog clock there: it draws hour and minute hands over its frame memory and
  moves them on its own timer, so the AP sleeps for hours with the time right. The hands are a firmware image
  (`samsung/s6e3fa7-beyond0-aod-hands.bin`) made from Samsung's GPL source (`mobian/display/aod/`); a PM notifier
  sets the time again after a resume. The panel's self move is a module parameter (`self_move`, off). On the
  panel's clock the AOD costs 29 mA (about 0.12 W) over the panel being off. Commands go out through DECON's
  command FIFO, and a frame's packets could land between two of them: a B0h offset then went to the frame, and
  the hands came up out of shape in the top left corner. The panel now holds DECON's frames
  (`exynos9820_decon_hold_frames()`) around its offset commands and the side RAM upload, and sends each
  clock block as one packet.
* **Touchscreen:** `s6sy761.c` gets the controller's low power mode with its double tap gesture (`wake_gesture`
  in sysfs). Userspace turns it on when it blanks the screen; the double tap comes from a second input device as
  `KEY_WAKEUP`.
* **GPU:** panfrost on the G76 (the G3D power domain stays on: re-powering it needs a secure-world
  restore that mainline does not do).
* **Wi-Fi:** `drivers/pci/controller/dwc/pcie-exynos9820.c` (root complex and PHY), and brcmfmac support
  for the BCM4375 B1, including the shared `host_cap` the v7 PCIe protocol needs before the firmware
  consumes receive buffers. The 4375's firmware takes mailbox data (D3 inform, D0 inform) as control ring
  messages, as Broadcom's dhd sends it, not through the TCM mailbox, so suspend gets its D3 acknowledgement.
  Its deep sleep requests are left unanswered: brcmfmac has no device-wake handshake to wake it again.
  The 4375's firmware has no wowl, so the dongle wakes the host over its out-of-band line instead
  (`brcmfmac host_wake=gpa0:7`, as Broadcom's `BCMPCIE_OOB_HOST_WAKE`): the link stays up through a suspend,
  wowlan "any" is on by default, and while the phone sleeps the dongle passes only unicast frames (the firmware
  answers ARP and ND) and sends the access point a keepalive every 30 s. For that suspend the radio goes to
  strict power save (PM_MAX), as the WOWL path already did, and back to its earlier mode on resume. The PCIe
  driver can be unloaded: it had no remove, and the root port stayed on the bus unmapped.
* **Bluetooth:** CMU_PERIC1 clocks for UART_BT, the USI wrapper, hci_bcm power sequencing.
* **Sound:** `sound/soc/samsung/exynos9820-abox.c` drives ABOX's DMA and routing directly for media,
  and loads its DSP firmware (calliope) for the voice path of calls; `s10e_madera.c` machine driver; CS35L41 changes for the S10e amps.
  For a GSM gateway, two more PCMs work during a call: "Call Downlink" (`hw:0,2`, capture: the far end's voice
  as the CP sends it, before anything is mixed in) and "Call Uplink" (`hw:0,3`, playback: with the "Call Uplink
  Switch" control on it goes to the CP in place of the microphones). The downlink is 16 kHz, as the CP's voice
  frames are, and the uplink takes 4 channels, as ABOX's mixer reads them.
  Power: ABOX writes CMU_AUD's controller option and Q-channel settings back after every power-up, as stock's
  pmucal does, and switches PLL_AUD0 off at runtime suspend; before, only the first AUD power-off of a boot
  completed. `s10e_madera.c` starts the codec's FLL1 and SYSCLK only while the codec's DAPM context is up (as
  tm2_wm5110 does): a running FLL held a runtime PM reference, so the CS47L93 and its core supply never powered
  down. `mfd: madera` gets system sleep callbacks (`pm_runtime_force_suspend`/`_resume`): with runtime
  callbacks only, on a bus without PM callbacks, the PM core marked the codec active after every s2idle without
  resuming it, and the next runtime suspend disabled its supply a second time ("unbalanced disables for
  vdd_codec_1p2").
  **Stopgap:** ABOX refuses a system sleep (`-EBUSY` in `.prepare`) while it is runtime-active, which is for a
  few seconds after every sound (PipeWire keeps the PCM open about 5 s, then a 3 s autosuspend); `s10e-sleepd`
  tries again a second later. Without it, a sleep in that window powered AUD off under a running ABOX, and the
  failed power-off ended in an SError panic or a hard lockup. Force-suspending ABOX stops the crash too, but the
  next stream then never starts, so the refusal stays until streams survive a sleep. A program that keeps a
  PCM open keeps the phone awake.
* **Modem:** `drivers/misc/exynos9820-cp.c`, the boot and shared-memory link to the Shannon CP. The radio
  stack runs in userspace (`exynos-cpd`, `mobian/modem/`): ModemManager talks AT over a WWAN AT bridge,
  and `exynos-cpd` translates that to Samsung IPC. The CP's mailbox interrupt is a wakeup source: every
  message from the CP keeps the phone awake for `wake_ms` (1 s), the CP's own wakelock bit holds it awake as
  long as it is set, and `exynos-cpd` holds a kernel wakelock while a call rings or is up and for 10 s after an
  SMS, as rild and Android's in-call screen do. Around every suspend the driver tells the CP that the AP sleeps
  (stock's `pda_active`), so it holds back what can wait.
* **Suspend:** suspend to idle works. Pin-controller 7 (VTS) is left out of pinctrl-exynos: it sits in a power
  domain that is off, and saving its banks in the late suspend phase stalled the SoC. genpd no longer powers on
  the domain of a runtime-suspended device in resume: on the 9820 the power-off after it failed and left MFC and
  the ISP domains half on, and the next touch of them stalled the bus. With no idle states in the DT,
  cpuidle-psci registers a WFI state with `enter_s2idle`, so suspend to idle freezes the tick and kernel timers
  (and systemd's watchdogs) stop while the phone sleeps. The USB PHY's own supplies (S2MPS19 LDO10M-12M) go
  down with it, which saves some 150 mW in every sleep. The pin controllers save and restore their banks in
  the noirq phases, and the AUD banks (gpb0-2), whose domain is off whenever ABOX is idle, are typed without
  power-down registers so that the save leaves them alone (reading them with the domain off stalled the bus).
  The policy is userspace's: `mobian/sleepd/`, which uses s2idle. With Wi-Fi associated the phone draws about
  200 mA asleep.
* **SoC sleep (in the kernel, not used yet):** `drivers/soc/samsung/exynos9820-sleep.c` offers `deep` in
  `/sys/power/mem_sleep`. The kernel arms the PMU as stock's exynos-pm and PMUCAL SYS_SLEEP do and makes a PSCI
  CPU_SUSPEND for the whole system; EL3 and the APM take the SoC down, the MIF with it when no other master holds
  it. It is off until `exynos9820_sleep.mode=3` (1 and 2 are dry runs). By default the RTC alarm and the power
  key wake it (`eint_wake`); `wakeup_int_en=0x1500` adds the modem's mailboxes, so a call or an SMS wakes it.
  debugfs shows the PMU's sleep registers and the firmware's counters. What it took for every CPU and block to
  come back:
  * PSCI CPU_OFF passes affinity level 1 for the last CPU of a power cluster, and the PMU gets the matching CPD
    hint; CMU_CPUCL2's option goes back from a CPU hotplug callback (from syscore resume it was an SError).
  * Every memblock range is cleaned to the point of coherency first (about 0.75 s): the caches do not survive.
  * The UFS host's IO coherency setting is written again on resume.
  * The PCIe link to the Wi-Fi chip is taken down with PME_Turn_Off and PERST# and trained again after the wake.
    Its pads (PERST#, CLKREQ#) are let out of retention early for that
    (`exynos9820_sleep_release_pads()`), and WLAN_REG_ON keeps its level, so the BCM4375 stays powered. In
    s2idle the link stays up as before.
  * The watchdog uses its normal suspend and resume: the SoC sleep resets the block, and one left running bites
    about 5 s after the wake.

  On battery, with Wi-Fi and the sensor hub unloaded, a 600 s sleep cost 13 mAh, about 78 mA (0.32 W), against
  about 190 mA in s2idle. It is not ready for everyday use, and `mobian/sleepd/` does not use it:
  * Entering it takes up to 8 s, between the file system sync and the freezing of userspace (s2idle: under
    0.7 s). The cause is not found yet. Power key presses in that window are lost.
  * The modem's mailbox wakes it every 10 s or so.
  * brcmfmac does not get the Wi-Fi firmware back after the PERST#: Wi-Fi needs a driver reload after each
    SoC sleep, which takes about 10 s. The sensor hub's driver has to be unbound and bound again too.
  * Sound after a SoC sleep needs the stock power-down settings of the amps' and codec's reset pins in the
    device tree; `mobian/uboot/` does not have them yet, and without them the amps fail after the sleep.
* **Watchdog:** `s3c2410_wdt` knows the 9820's CLUSTER0 watchdog; U-Boot's DT has the node, and systemd feeds it.
* **RAM:** `reserved_mem_skip=<node>[,...]` hands those `/reserved-memory` carve-outs to the kernel without
  changing the boot loader's DT. The phone's `bootargs.env` frees `sec_rdx_bootdev`, `secure_camera`,
  `camera_contig` and `camera`; freeing the debug-log regions as well moved CMA and swiotlb and crashed two boots in
  five.
* **Sensor hub:** `drivers/iio/common/bcm4775_shub/`. The BCM4775 boots from ROM and needs a RAM patch on
  every power-up; the driver loads it (`brcm/bcm4775-shub.patch`, from the phone's Android), speaks
  Broadcom's transport layer and Samsung's SSP protocol on top, and exposes the accelerometer, light and
  proximity sensors as IIO devices for iio-sensor-proxy. Each sensor runs only while it is read. The
  compass is a tilt-compensated heading worked out from the hub's calibrated magnetometer and the
  accelerometer, as the triggered buffer iio-sensor-proxy expects; it appears once the hub has booted.
  `mobian/sensorhub/` has the protocol notes and the userspace scripts it was worked out with. Through a
  suspend it follows stock's handshake (an AP_SLEEP pulse, then AP_STATUS_RESUME), and when its SPI
  controller comes back from a resume clocking nothing, the driver lets the controller autosuspend and boots
  the hub again, holding the phone awake meanwhile. That recovery now starts as soon as a resume's first
  transfer fails: it used to wait for more errors, a transfer that worked in between cleared the count, and
  the thread then polled without end with a wakeup source held, so the phone never slept and ran hot.
* **USB:** `drivers/usb/typec/max77705.c` (Type-C port and orientation); the USB 3 DRD PHY for the
  Exynos 9820 with orientation-aware lane setup; dwc3 quirks stock sets in GUSB3PIPECTL (elastic buffer
  mode, U-state exit in P1-P3, failed U1/U2 exit to Recovery).
* **Video:** MFC v12 decode in s5p-mfc. After a system sleep the wakeup path writes MFC_CLOCK_OFF (0x7120)
  = 0 with RISC_ON, as the init does on v10 and later (and = 1 after the sleep command, as the vendor driver
  does): an s2idle in the middle of a video left the MFC dead until the last instance closed. A real-time
  decode with an s2idle in it now plays through.
* **Flash LED:** `drivers/leds/flash/leds-s2mpb02.c`, the S2MPB02's flash LED as a flash class LED
  (`white:flash`): torch through its brightness (12 steps of 20 mA, up to 240 mA), flash through `flash_strobe`
  (150-1250 mA, up to 1 s), the ceilings Samsung's kernel uses on this board. It is an MFD cell of the s2mpb02
  regulator driver. The device tree in `mobian/uboot/` does not have its node yet.
* **NFC:** the S3NRN82 (`s3fwrn5-i2c`) on hsi2c_20, which is USI08 in its USI configuration (CMU_PERIC1 gets
  its clocks); its 26 MHz reference is the PMU's CLKOUT1, which `exynos-clkout` now offers. s3fwrn5 asks for
  its firmware without the user-mode helper fallback, which stalled every power-up for 60 s; without the file
  the chip runs the firmware in its flash. The chip probes as `nfc0`; reading tags is untested. The device
  tree in `mobian/uboot/` does not have its nodes yet.
* **Cameras:** `drivers/media/platform/samsung/exynos9820-csis/` (MIPI CSI-2 receivers, the PHYs and the write
  DMA, as a V4L2 capture device), `drivers/media/i2c/s5k3p9.c` (data-driven: S5K2L4, S5K3P9 and S5K3J1, with
  register tables), AK7375 changes for the wide camera's lens (powered only while the camera streams), the S2MPB02/S2MPB03 camera PMICs, and CMU_ISPPRE.
  libcamera's simple pipeline and software ISP do the rest (`mobian/camera/libcamera-0.7.2-s10e.patch`).
* **DisplayPort:** `drivers/gpu/drm/tiny/exynos9820-dp.c` (link controller, AUX, EDID, training, CEC over AUX)
  as a bridge on a second CRTC (DECON2) in `exynos9820-decon-kms.c`; DP alt mode and HPD in `max77705.c`.
* **USB hubs:** dwc3 on the 9820 resets GFLADJ with REFCLK_LPM_SEL set while no 24 MHz reference is there, so the
  xHCI frame counter ran almost 3x too fast and every hub failed to enumerate; the bit is cleared now.
* **Power:** MAX77705 charger and MAX17042-family fuel gauge changes, including the WCIN input the IDT
  wireless receiver feeds (selected, limited to 820 mA after a ramp from 480 mA as stock does, reported
  as online). Without a PD contract the input limit is the Type-C current the source advertises or what the
  chip's BC1.2 detection found (a charging port 1.5 A, a dedicated charger 1.8 A), as stock's cable table has it.
  A supplied battery that is not full reports Charging even while a 0.5 A port cannot keep up
  with the screen (`max17042_battery.supplied_is_charging`), as Android shows "charging slowly". S2MPS19 RTC
  and 32 kHz clock over ACPM; Exynos 9820 PMU and power domains.

Firmware
========

None is in this tree. On the phone, `s10e-firstboot` copies it from the phone's own Android
(`/vendor/firmware`) on the first boot: the BCM4375 Wi-Fi firmware, CLM blob and NVRAM, the Bluetooth `.hcd`,
`s5p-mfc-v13.fw`, and the ABOX `calliope_*` and audio files. The sensor hub patch
(`bbd_patch_file_beyond.h` in Samsung's kernel source, converted to text) is not on `/vendor`, so the release
zip carries it. The modem image is read from the `radio` partition at boot. The AOD's hand image is made from
Samsung's GPL kernel source by `mobian/display/aod/` (hour and minute hands only: `hands-clean-second.py ... none`)
into `/lib/firmware/samsung/s6e3fa7-beyond0-aod-hands.bin`; without it the AOD shows the time as text.

mobian/
=======

The userspace half: U-Boot's boot menu and the S10e device tree, the initramfs init, the kernel config,
the services and configuration the hardware needs on Mobian, the sleep daemon, the recovery zip, the camera
stack, and the Neo launcher and apps the image ships. See `mobian/README.md`.

`mobian/` is as it was in the 7.2.13 release (2026-11-03), apart from `kernel.config`, which is now the 7.2.15
configuration the phone runs (it has `CONFIG_EXYNOS9820_SLEEP` and the flash LED driver). Its device tree
therefore still lacks the flash LED and NFC nodes and the stock power-down pin settings mentioned above.
