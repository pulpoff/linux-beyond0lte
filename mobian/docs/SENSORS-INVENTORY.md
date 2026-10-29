# S10e (beyond0lte, SM-G970F) sensors + GNSS inventory

Date 2026-10-29. Read-only research. Nothing was built and no device was touched.

Tags: **[C]** means certain, with the file:line given. **[I]** means inferred.

Paths used below:
- `LK/` = `src/lineage-kernel` (stock Samsung 4.14)
- `SDT/` = `LK/arch/arm64/boot/dts/samsung`, `XDT/` = `LK/arch/arm64/boot/dts/exynos`
- `SH/` = `LK/drivers/sensorhub/brcm` (the whole stock sensor stack is in here)
- `ML/` = `src/linux-9820` (our 7.2-rc2 tree)
- `UDT/` = `src/u-boot/dts/upstream/src/arm64/exynos`
- `TRACE` = `kit/sensorhub/lhd-android-start.strace`. This is a full-length strace of the
  vendor `lhd` daemon starting on the phone under Lineage (Android 16). It is the most useful single file here.

> Note on the DT location. `ML/arch/arm64/boot/dts/exynos/` contains **no** exynos9820 files at the moment, and
> git does not track any. The live DT the board boots is `UDT/exynos9820{.dtsi,-common.dtsi,-beyond0lte.dts}`,
> edited 2026-10-29 14:50. The board is "BEYOND0LTE EUR OPEN 25" (`s10e-live.dts:10`), so the
> stock files that apply are `SDT/exynos9820-beyond0lte_eur_open_25.dts` and what it includes.

---

## 1. Architecture

### 1.1 The headline: there is no Exynos CHUB on this phone. The sensor hub is the Broadcom GNSS chip.

- **[C]** The Exynos 9820 has no CHUB/nanohub block. `grep -i chub` over `LK/drivers/soc/samsung/cal-if/exynos9820/`
  and `XDT/` finds nothing. The same grep finds CHUB CMU/PMU entries in `cal-if/exynos9810/cmucal-node.c` and
  `flexpmu_cal_local_exynos9810.h`. The 9810 (S9) had a CHUB; the 9820 has VTS and CMGP in its place. There is no
  `drivers/staging/nanohub` and no `drivers/misc/samsung/chub*` in the stock tree.
- **[C]** The sensor hub is an MCU inside the **Broadcom BCM4775** (B0 silicon) GNSS + sensor-hub combo ("LHE").
  The stock kernel calls it "SSP over BBD":
  - DT: `SDT/exynos9820-sensorhub.dtsi:62-98`, node `BCM4773@0`, `compatible = "ssp,BCM4773"`, on `&spi_17`,
    25 MHz, CPOL=1 and CPHA=1. The board overrides are at `SDT/exynos9820-beyond0lte_eur_open_25.dts:166-177`
    (acc/mag position 7, a 27-byte `ssp-mag-array`, thermistor tables) and `SDT/exynos9820-beyond0lte_common.dtsi:2469-2473`
    (`ssp-ap-type = <0>`).
  - The firmware ("patch") header says `AsicVersion=0x004775B0`, `Name=bcm_sensorhub_a1`, `PatchAddress=0x00800161`
    (`SH/bbdpl/r_os/bbd_patch_file_beyond.h:1-11`). The chip's version reply in TRACE, at 13:28:32.605, contains
    `b0 75 47 00`, which is 0x004775B0. That confirms the silicon.
- **[C]** The accelerometer/gyro, magnetometer, pressure and light/prox chips hang off the **BCM4775's own buses**,
  not off the AP's. No board DT has an I2C or SPI node for LSM6DSO, AK0991x, LPS22H or TMD4910. They exist only as
  SSP "factory" sysfs drivers that are selected by Kconfig (`SH/Makefile`, `LK/arch/arm64/configs/exynos9820-beyond0lte_defconfig:5427-5499`).
  The AP only reaches them through the hub firmware.
- **[C]** Sensors that ARE on the AP side: SX9330 grip, TCS3407 rear ALS/flicker, the Hall switches, and the ET523
  fingerprint sensor. See section 2.

### 1.2 Stock data path

```
 Android sensor HAL (sensors.sensorhub.so) <-> IIO + /sys/class/sensors/ssp_sensor/*   (ssp_* in SH/)
                 SSP driver (ssp_dev.c, ssp_i2c.c, ssp_data.c, ssp_bbd.c)
          TX: bbd_send_packet() -> /dev/bbd_sensor ----> lhd (closed, userspace) wraps it in RPC 0x20 + TL frame
          RX: bcm_gps_spi RX -> bbd_parse_asic_data() (TL decode, in kernel) -> RPC 0x21 -> bbd_sensor_write() -> SSP
                 /dev/ttyBCM  (bcm_gps_spi.c, misc device)  <->  SPI17 (USI16)  <->  BCM4775
 GNSS: gpsd (closed) <-> lhd (closed) <-> /dev/ttyBCM. Non-sensor RPCs are passed up to lhd.
```

- **[C]** TX goes through lhd. `SH/bbdpl/bbd.c:371-395` says it plainly: "The sensor packet is pushed into
  /dev/bbd_sensor to be read by gpsd/lhd. gpsd/lhd wrap the packet into RPC and sends to chip directly." TRACE
  shows it in action: every `read(/dev/bbd_sensor)` is followed by a `write(/dev/ttyBCM)` carrying the same bytes
  inside a TL frame.
- **[C]** RX is decoded in the kernel. `SH/bbdpl/bbd_rpc_lh.c:278-293` (`BbdBridge_OnRpcReceived`, RPC
  `IRpcSensorResponse_Data`) and `:575` (`bbd_parse_asic_data`) are called from `bcm_gps_spi.c:720,734,938`. lhd
  makes no ttyBCM reads after `ESW:READY` (TRACE).
- **[C]** Char devices: `bbd_shmd`, `bbd_sensor`, `bbd_control`, `bbd_patch`, `bbd_urgent_patch` (`SH/bbdpl/bbd.c:113-121`),
  plus misc `/dev/ttyBCM` (`bcm_gps_spi.c:1283`). Control strings such as `ESW:READY`, `BBD:PassThru=1`,
  `SSI:PassThru=1` and `GPSD:SENSOR_ON` are listed in `SH/bbdpl/bbd.h:53-78`. They are handled in `bbd.c:494-579`.

### 1.3 Firmware ("patch"): file, loading, boot

- **[C]** No `/vendor/firmware` file is involved. The patch is **compiled into the kernel** as a hex-string header
  and served to lhd through `read(/dev/bbd_patch)` (`SH/bbdpl/bbd.c:133-181`, `bbd_patch_read` at `:821`). The
  variant depends on hw_rev (`SH/ssp_dev.c:1285-1310`): for beyond0lte, hw_rev > 22 selects `bbd_current`, which
  on Android 11+ is `r_os/bbd_patch_file_beyond.h`. The header gives BlobLength 445094 and Crc=236. The bytes
  uploaded in TRACE (`...e8 23 87 00 61 01 80 00 03 0c 01...`) match that file's `<Blob=F1940300008000E823870061...`.
  The patch ships in Samsung's GPL source drop, but it is an opaque Thumb binary. **Do not commit it.** Generate it
  at build time from the stock source.
- **[C]** lhd's power-up sequence, read from TRACE at 13:28:31.14-.27:
  1. `/sys/bbd/lk_enable=1`, `bbd_control <- "SSP:ANDROID_VERSION:16"`
  2. `GPS_PWR_EN(gpg0-2)=0`, then `mcu_power=0` (this is `regulator_disable(vdd_mcu_1p8)`, `SH/ssp_sysfs.c:587-614`), then 20 ms
  3. `mcu_power=1`, `GPS_PWR_EN=1`, then 20 ms
  4. open `/dev/ttyBCM`, `bbd_control <- "BBD:PassThru=1"`, 10 ms
  5. autobaud: 16 × `0x80`, then 36 × `0x80`. This matches `bcm477x_rpc_get_version()` in `SH/bbdpl/bcm_gps_regs.c:48-...`,
     which has the exact bytes.
  6. TL frames (see 1.4): RPC 0 (GetVersion) with an internal/ignore-seq flag, then **486 × RPC 5** "write memory".
     Each payload is `u32 addr LE (0x00800000 + n*0x390) | u16 0x0390 | 912 data bytes`, using the reliable
     channel with per-packet ACKs from the chip.
  7. RPC 46 (0x2e), payload `61 01 80 00`. This is a jump to 0x00800161, the PatchAddress.
  8. Autobaud again, RPC 0 → the version reply (0x4775B0), then RPC 43 (0x2b) `01 00 21 00 00 00`. [I] This
     subscribes to RPC 0x21, the sensor response.
  9. `BBD:PassThru=0`, then `ESW:READY` (13:28:32.659). The whole download takes **1.4 s**.
  10. The kernel SSP driver then runs `initialize_mcu()` (`SH/ssp_dev.c:311`). TRACE shows its traffic: `0xa7`
      timestamp, `0xf1` formation, the mag matrix (the 0x1b-byte packet equals the DT `ssp-mag-array`), the accel
      and prox thresholds, and so on.
- **[C]** No Exynos PMU or CMU step is involved beyond the USI16/SPI17 clocks. The hub's only power inputs are
  `vdd_mcu_1p8` (S2MPS19 **LDO21M**, `always-on`, `SDT/exynos9820-beyond0lte_common.dtsi:1632-1639`) and the
  `GPS_PWR_EN` (nSTBY) GPIO.

### 1.4 The wire protocol is fully recoverable (verified)

- **SPI ("SSI") layer [C]**: `SH/bbdpl/bcm_gps_spi.c` (GPL). Half-duplex frames start with a command byte (`0x80`
  = write) or a status byte. Flow control works through `MCU_REQ` (gpg4-1, AP output) and `MCU_RESP` (gpg4-2,
  input): see `bcm477x_hello`/`bye`, `:476-550`. `HOST_REQ` (gpa1-7) is the chip→AP "data ready" line, a
  level-high IRQ (`:1185,1271`).
- **Transport layer (TL) [C]**: the RX side is in `SH/bbdpl/bbd_rpc_lh.c:98-132,371-560`. The layout is
  `B0 00 | seq | len | flags | flag-details... | RPC* | crc | B0 01`. The escapes are `B0→B0 03`, `11→B0 04` and
  `13→B0 05`. CRC-8 uses the table at `:229-246`. It covers len…payload (not seq, not crc), and the result's nibbles
  are swapped. Flags: ACK=1, RELIABLE=2, R_ACK=4, R_NACK=8, LOST=0x10, GARBAGE=0x20, SIZE_EXT=0x40 (detail = len
  MSB), EXT=0x80 (detail = flags MSB), INTERNAL=0x100, IGNORE_SEQ=0x200. An RPC is `id (7/15-bit, 0x80 = ext) |
  len (7/15-bit) | data`.
  **I checked this against TRACE.** I decoded 4 frames from both directions (lhd TX and chip RX) with the kernel's
  CRC table, and all 4 CRCs matched. The TX side, which lhd implements, is the exact mirror of the RX parser.
- **RPC ids [C]**: the enum is at `bbd_rpc_lh.c:136-172`. Its names are obfuscated (`IRpcA..IRpcL`), but the
  order is fixed, so `IRpcSensorRequest_Data` = **0x20** and `IRpcSensorResponse_Data` = **0x21**. The payload
  is `u16 LE size + raw SSP packet`. TRACE frame `...20 13 11 00 a7 08 00 01 ...` is RPC 0x20, len 0x13, size 0x11,
  SSP cmd 0xa7.
- **SSP command layer [C]**: open and complete in `SH/`. The commands are in `SH/ssp.h:193-240` (e.g. `0xA1`
  BYPASS_SENSOR_ADD, `0xA2` REMOVE, `0xA4` CHANGE_DELAY, `0x0F` WHOAMI, `0xF0` FIRMWARE_REV, `0xF1` SENSOR_FORMATION,
  `0xDD` BATCH_FLUSH). The sensor ids are in `SH/sensor_list.h:22-75` (accel 0, gyro 1, mag-uncal 2, mag 4,
  pressure 5, prox 7, light 9, orientation 12, step 13/18, game-rot 16, rot 17, light-IR 24, …). The frame parser
  is `parse_dataframe()` in `SH/ssp_data.c:832`, with per-sensor decoders at `:187-361`. The transfer function is
  `bbd_do_transfer()` in `SH/ssp_bbd.c:76`. The hub also has a register passthrough command,
  `MSG2SSP_AP_REGISTER_SETTING` (`SH/ssp_sysfs.c:840-990`, under `CONFIG_SSP_REGISTER_RW`), which is a useful debug
  tool once the firmware is up.
- **What remains closed**: the GNSS RPCs (every id except 0/5/43/46/0x20/0x21) and lhd/gpsd themselves. The patch
  upload and the sensor plumbing are fully reproducible without lhd.

---

## 2. Per-sensor table

Bus bases and IRQs come from `XDT/exynos9820.dts`; the board wiring comes from `SDT/` (rev 25). "Hub" means the
part sits behind the BCM4775 and the AP cannot address it.

| Prio | Function | Part | Where / bus | Addr | IRQ / GPIO | Supply | Mainline driver |
|---|---|---|---|---|---|---|---|
| ★ | **Accel + gyro** | ST **LSM6DSO** [C] defconfig:5434, `SH/factory/accel_lsm6dsl.c:21-25` | Hub | – (hub) | via hub | hub | `st_lsm6dsx` (`st,lsm6dso`) exists, but it is unusable behind the hub |
| ★ | **Magnetometer** | AKM, `ssp-mag-type=1` [C] sensorhub.dtsi:91. The string says "AK09916C" because `CONFIG_SENSORS_SSP_AK09918` is unset (`factory/magnetic_common.c:24-28`). [I] The silicon is probably AK09918C | Hub | – | – | hub | `ak8975` (ak09916/ak09918) exists, unusable behind the hub |
| ★ | **Pressure** | ST **LPS22H(B)** [C] defconfig:5444, `factory/pressure_lps22h.c:17-19` | Hub | – | – | hub | `st_pressure` (lps22hb) exists, unusable behind the hub |
| ★ | **Light + proximity** (front, under display) | AMS **TMD4910** [C] defconfig:5453, `factory/light_tmg399x.c:37` | Hub. [I] Needs panel COPR data: the `shub_ap_int_copr` pin gpm9-0 (eur_open_25.dts:179-186) and the `sensorhub_ddi_spi_check` sysfs (Lineage `init.exynos9820.rc:166`) suggest the hub reads the DDI | – | – | hub | none |
| | Thermistors ×2, MoBeam barcode emulation | in hub [C] defconfig:5448,5492 | Hub | – | – | – | n/a |
| | Virtual: rotation vector, game-rot, step, sig-motion, tilt, pickup, pocket… | computed in hub fw [C] sensor_list.h | Hub | – | – | – | n/a (or do them in userspace with iio-sensor-proxy) |
| ☆ | **Grip / SAR** | Semtech **SX9330** [C] defconfig:5474 | AP **hsi2c_14 = USI05_USI (PERIC0) @0x104F0000**, GIC 392 [C] XDT:2797-2803; board `SDT/…common.dtsi:2408-2440`, pins gpp2-4/5 | 0x28 | NIRQ **gpa3-0** (ALIVE EINT) `:1136-1141,2425` | VDD_GRIP_3P3 = S2MPB02 LDO17, always-on + boot-on `:1817-1823` | none (sx9310/sx9324/sx9360 exist). The stock `SH/sx9330.c` is GPL and could be ported |
| ☆ | **Rear ALS / flicker** | AMS **TCS3407** [C] optics-r00.dtsi:34-52 | AP **hsi2c_20 = USI08_USI (PERIC1) @0x108E0000**, GIC 418 [C] XDT:2917-2934, pins gpp5-0/1. **Shared with the NFC chip @0x27** (eur_open_25.dts:138-164) | 0x29 | INT gpp3-0, LED_EN gpg2-4 | vdd S2MPB02_LDO9, i2c VDD_NFC_1P8 (LDO22M) | none (tcs3472 is a different chip) |
| ☆ | **Hall (flip cover)** | GPIO switch [C] common.dtsi:692-697,727-733 | AP GPIO **gpa3-6** | – | EINT | – | `gpio-keys` with `SW_LID`, DT only |
| | Hall "certify" (wallet/LED cover) | GPIO [C] `:699-704,735-741` | gpa0-2 | – | EINT | – | `gpio-keys` (SW_*/KEY_*) |
| | **Fingerprint** (side key) | Egis **ET523** [C] fingerprint_20.dtsi:102-121 | AP **spi_1 = USI00 (PERIC0) @0x10450000**, marked **`secure-mode`** (`:91`) | – | DRDY gpa1-3, sleep gpg0-7, LDO gph1-0 | – | none. The TEE owns this bus; see section 5 |

The hub's own pins (all [C], `SDT/exynos9820-sensorhub.dtsi:13-98`):

| Signal | Pin | Dir (AP) | Note |
|---|---|---|---|
| SPI17 CLK/MISO/MOSI | gph0-0/1/2 func 2 | – | PERIC1 (`XDT/exynos9820-pinctrl.dtsi:2105-2125`) |
| SPI17 CS | gph0-3 (`cs-gpio`) | out | |
| HOST_REQ (chip→AP data ready) | **gpa1-7**, ALIVE EINT, func 0xf | in/IRQ | the stock IRQ is `gpio_to_irq(host_req)`, level-high |
| MCU_REQ | gpg4-1 | out | wakes the chip |
| MCU_RESP | gpg4-2 | in | the chip says it is awake |
| GPS_PWR_EN (nSTBY) | gpg0-2 (PERIC0) | out | lhd's `GpioNStdbyPath` |
| AP_SLEEP ("batch-wake-irq") | gpa0-5, func 1 = **output** | out | AP suspend state to the hub (`SH/ssp_dev.c:456-458,501`) |
| SHUB_INT | gpa4-0, func 0xf | in/IRQ | hub→AP extra interrupt |
| (COPR to hub) | gpm9-0 (CMGP) | – | display COPR interrupt, rev-25 board |
| vdd_mcu_1p8 | S2MPS19 LDO21M, 1.8 V, always-on | – | `mcu_power` sysfs; lhd toggles it to reset the hub |

SPI17 controller [C]: `spi@10970000` = USI16_USI in PERIC1, GIC SPI 426 (`XDT/exynos9820.dts:3793-3807`,
`LK/include/dt-bindings/interrupt-controller/exynos9820.h:440`). The USI mode register is SYSREG_PERIC1+0x1048
(`usi_16 @0x10821048`, `XDT:2373-2378`), set to "spi" in `SDT/…common.dtsi:1252-1254`. The stock CMU_PERIC1
offsets are in `LK/drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c`: MUX_USER PLL_CON0/2 0x2a0/0x2a8 (`:333-334`),
DIV 0x1830 (`:495`), gates IPCLK 0x2108 / PCLK 0x210c / RSTNSYNC 0x2080 (`:1778-1781`), QCH 0x3070 (`:2609`).
USI08 (hsi2c_20) uses 0x220/0x228, 0x1820, 0x20c8/0x20cc/0x2070 and QCH 0x3050 (`:315-316,486,1725,1742,1764,2600`).

**What mainline already has [C]**: `exynos-usi.c` (exynos850/8895 compatibles), `spi-s3c64xx` (exynos850-spi
etc.) and `i2c-exynos5` all exist. The live DT already uses USI-mode SPI (usi1/spi2) and I2C (usi17/hsi2c_38),
so the plumbing is proven. **Missing**: `ML/include/dt-bindings/clock/exynos9820.h:131-143` gives CMU_PERIC1 only
UART_BT and USI17_I2C, so **USI16 (SPI17) and USI08 clocks must be added** to `clk-exynos9820.c`. PERIC0 USI00/USI05
IDs already exist (`:43-111`). There is no mainline S2MPB02 regulator driver. S2MPS19 is reached through
`mfd/sec-acpm.c`, and LDO21M is not in the live DT, so it stays however the bootloader left it ([I] on, because it
is always-on in stock).

---

## 3. Can we skip the firmware?

### 3.1 The "drive the CHUB directly" idea does not apply here

- **[C]** There is no CHUB domain on the 9820, so there is no CHUB CMU/PMU or CHUB-side USI to power from the AP.
  The sensors are not on any Exynos USI. They sit behind the BCM4775's internal I2C/SPI masters. This is not like
  the audio case, where ABOX's blocks were AP-mapped Exynos IP.
- **[I]** The only firmware-less route left would be to drive the **BCM4775's own** I2C master from the AP through
  the SSI debug window. `bcm_reg32Iwrite`/`bcm_reg32Iread` (`SH/bbdpl/bcm_gps_regs.c:275-419`) give 32-bit
  read/write access into the chip's address space (e.g. `HSI_*` at 0x401040xx, `bbd.h:88-93`). But the chip's
  peripheral map, I2C controller, clocks and pinmux are undocumented, and the ROM's state is unknown. Every
  sensor-to-pin mapping would have to be reverse-engineered from the 445 KB Thumb patch. That is weeks of work, has
  no upside over loading the patch, and loses the hub's sensor fusion. **Not recommended**, except as a debugging aid
  for reading chip registers.

### 3.2 Booting the vendor patch from Linux without lhd is cheap

What it takes:
1. Bring up SPI17 (clocks, USI mode, pins). Port `bcm_gps_spi.c`'s SSI framing and the MCU_REQ/RESP/HOST_REQ
   handshake. That is about 1.4 k lines of GPL code; it needs modernising (wakelocks, timeval).
2. Implement TL TX/RX. RX is copied from `bbd_rpc_lh.c`, and TX is its mirror, verified against TRACE. The reliable
   channel needs seq/ACK handling; TRACE shows how the chip ACKs each frame.
3. Patch download: power-cycle (LDO21M/GPS_PWR_EN), autobaud, RPC 0, 486 × RPC 5 (addr, 912 B), RPC 46 (jump to
   0x800161), autobaud, RPC 0, RPC 43. Everything comes from TRACE plus the GPL patch header. Convert the header to a
   `.bin` and load it with `request_firmware()`.
4. SSP layer: send the `initialize_mcu()` sequence (also captured in TRACE after 13:28:32.66), then
   `0xA1 BYPASS_SENSOR_ADD(type, delay)` per sensor, and parse with the `ssp_data.c` decoders. Expose IIO devices
   (accel/anglvel/magn/pressure/illuminance/proximity), which iio-sensor-proxy / GNOME already consume.

The protocol documentation is complete in the stock source except for the GNSS RPCs and lhd's patch-download
policy, which TRACE supplies. **Recommendation: load the vendor patch and talk to it directly, with no lhd.**
Prototype it in userspace first (section 6).

---

## 4. GNSS

- **Chip [C]**: the same **BCM4775** ("4773/4774/4775" family, `bbd_rpc_lh.c:14`, `bcm_gps_spi.c` "BCM477x"). It
  uses the same SPI17 link. There is no separate UART and no separate GNSS chip. GPS_PWR_EN is gpg0-2 and the
  host-wake line is HOST_REQ gpa1-7, both shared with the hub. lhd.conf has `LhePinMux=P17=M1` ("ExtLNA for GNSS")
  and `LheClockMHz=100` (`kit/sensorhub/lhd.conf`).
- **Stock stack [C]** (`android_device_samsung_beyond0lte/proprietary-files-vendor.txt:216-230`):
  `vendor/bin/hw/gpsd` (Broadcom position engine), `vendor/bin/hw/lhd` (chip/transport/patch daemon),
  `vendor.samsung.hardware.gnss@2.0-service`, `gps.default.so`, `libwrappergps.so`, `/vendor/etc/gnss/gps.xml` and
  `gps.cer`, `/vendor/etc/sensor/lhd.conf`, `init.gps.rc`, and `system/etc/init/init.gpscommon.rc`. lhd listens on
  `PortName=tcp:127.0.0.1:300` for gpsd. Lineage's `config/gps/gps_psds.conf` points at glpals LTO for XTRA.
  sepolicy labels `/sys/class/sec/gps/GPS_PWR_EN/value` (`…common/sepolicy/vendor/file_contexts:62-63`).
  gps.xml and the blobs are not on this disk. They have to be pulled from the phone's vendor partition.
- **Open source [C]/[I]**: mainline `drivers/gnss/` has only serial, sirf, ubx, mtk and usb drivers, with nothing
  for Broadcom (`ML/drivers/gnss/`). [I] The BCM477x does not emit NMEA. It speaks the opaque RPCs above, and the
  position fix is computed host-side by gpsd. I know of no open BCM4775 GNSS engine, and the GNSS RPC names are
  deliberately obfuscated in the GPL code.
- **Realistic path [I]**: run Samsung's `lhd` + `gpsd` blobs (bionic) under libhybris or a minimal Android
  container. They need the stock kernel ABI: `/dev/ttyBCM`, `/dev/bbd_{control,patch,sensor,…}` and the sysfs
  `mcu_power` / `GPS_PWR_EN`. Then pull NMEA or locations out through the GNSS HAL (or a gpsd NMEA output option, if
  gps.xml has one; unverified) into Linux gpsd and geoclue. This conflicts with an in-kernel lhd-less sensor driver,
  because lhd wants to own the chip, upload the patch and multiplex sensor RPC 0x20 itself. The clean way out is to
  have the new kernel driver keep a **stock-compatible ABI**: the `/dev/bbd_*` + `/dev/ttyBCM` semantics, with the
  kernel uploading the patch when no lhd is present. That keeps sensors working without lhd and still lets lhd/gpsd
  run for GNSS. **Effort: high; outcome uncertain.** GNSS should come last.

---

## 5. Risks

- **[I] Unclocked IP**: touching SPI17 (0x10970000) or HSI2C20 (0x108E0000) registers with their PERIC1 gate or
  Q-channel off can raise an SError or stall the bus. Add the CMU entries first. Do not hand-poke 0x2108/0x210c/
  0x3070 while audio's USI17_I2C (hsi2c_38, the amps) is in use, because it is the same CMU.
- **[C] Secure bus**: fingerprint `spi_1` (USI00) is `secure-mode` (`fingerprint_20.dtsi:91`), so the TEE owns it.
  Leave USI00, its pins (gpa1-3, gpg0-7, gph1-0) and `pdma0` channels 0/1 alone. Expect faults or TZ panics if they
  are touched.
- **[C] Shared I2C**: hsi2c_20 carries the NFC controller (0x27) as well as the TCS3407. Clock and pinctrl
  changes there affect NFC.
- **[C] Shared chip**: GNSS and the hub are one chip with one power rail. Toggling LDO21M or GPS_PWR_EN resets
  both. LDO21M is ACPM-managed. Writing an unknown S2MPS19 LDO by number is a real hazard: LDO20M/22M next to it
  feed other rails (LDO22M = VDD_NFC_1P8). Use DT `ldo21m` through `sec-acpm` only, and check the register index first.
- **[I] BCM debug writes**: `bcm_reg32Iwrite` into the chip can wedge it. That is recoverable by power-cycling
  GPS_PWR_EN/LDO21M. It cannot brick anything, because the patch lives in RAM and is re-uploaded on every boot.
- **[I] AP_SLEEP (gpa0-5) and the batch logic**: the hub batches and may stall or reset if AP_SLEEP misreports.
  Drive it low (awake) while bringing things up.
- **[I] Under-display light**: the TMD4910 values may be wrong or unstable unless the hub gets panel COPR and
  brightness. In stock, the SSP driver is notified of panel changes (`CONFIG_PANEL_NOTIFY`,
  `factory/light_tmg399x.c:42`).
- **[C] Licensing**: the patch blob is shipped in Samsung's kernel source but is not real source. Keep it out of
  the repo and extract it at build time.

---

## 6. Stage order (cheapest win first)

**S0: Hall sensors (about 1 h, DT only).** Add a `gpio-keys` node: gpa3-6 as `SW_LID` (flip cover; check the
polarity, since stock has `CONFIG_HALL_EVENT_REVERSE`) and gpa0-2 as a second switch. Test with `evtest`. No risk.

**S1: SPI17 bring-up and chip "hello" (no firmware).**
1. In `clk-exynos9820.c` CMU_PERIC1, add the USI16_USI mux/div/gates at the offsets in section 2. Export
   `CLK_GOUT_PERIC1_USI16_USI_{IPCLK,PCLK}`.
2. In `UDT/exynos9820.dtsi`, add `usi16: usi@109700c0` (`samsung,exynos850-usi`, `samsung,sysreg = <&sysreg_peric1 0x1048>`)
   with child `spi@10970000` (`samsung,exynos850-spi`, GIC 426), plus `spi17_bus`/cs pinctrl (gph0-0..3 func 2).
3. In the board DT, add `spidev`-style child at 25 MHz, mode 3, plus gpio-hogs or line names for gpg0-2, gpg4-1,
   gpg4-2, gpa1-7, gpa0-5 and gpa4-0.
4. From userspace (Python, spidev + libgpiod): set GPS_PWR_EN 0→1, raise MCU_REQ and wait for MCU_RESP, send 32 ×
   0x80 and then the `transaction_2nd` GetVersion bytes from `bcm_gps_regs.c:48-...`. **Success = a reply
   containing `b0 75 47 00`.** This proves the bus, clocks and power.

**S2: Patch upload from userspace ("lhd-lite").** Extend the script: build TL frames (CRC from `bbd_rpc_lh.c`), send
the RPC 5 chunks from the header blob (addr 0x800000 + 0x390·n), RPC 46 → 0x800161, then RPC 0 / RPC 43, all
following TRACE's order and ACK pattern. Success = a post-patch version reply. Diff against TRACE byte by byte;
the seq/flag details should match.

**S3: First sensor readings (still userspace).** Replay the post-READY `initialize_mcu` traffic from TRACE (it is
already TL-framed), then send `0xA1` for accel (type 0) and gyro (1) at 10 ms. Decode RPC 0x21 with the
`ssp_data.c` layouts. This is the first "gravity" win. Then add mag (4/2), pressure (5), light (9) and prox (7).

**S4: Kernel driver.** Write a fresh, compact driver, e.g. `drivers/iio/common/bcm4775-shub/`, as a
`spi_driver`. It needs: the power sequence (regulator + gpiod), the SSI transport with HOST_REQ IRQ, TL, a
`request_firmware("brcm/bcm4775-shub-beyond0lte.bin")` loader, a minimal SSP command set, and IIO channels with
triggers/buffers. Mount matrix: acc/mag position 7 (DT). Keep a stock-compatible `/dev/ttyBCM` + `/dev/bbd_*`
mode behind a module parameter so S6 stays possible. Hook up iio-sensor-proxy for auto-rotate and ambient light.

**S5: AP-side extras.** Port `SH/sx9330.c` to IIO proximity on hsi2c_14 (USI05, PERIC0 clocks already exist; add
the `usi5` node with sysreg 0x1042102C). For the TCS3407 on hsi2c_20, add USI08 clocks and write a small IIO light
driver. It needs S2MPB02 LDO9 (no mainline driver: confirm whether the bootloader leaves it on, or add a minimal
regulator). Fingerprint: skip (TEE-owned, closed matcher).

**S6: GNSS (last, optional).** Extract `lhd`, `gpsd`, `gps.xml`, `gps.cer`, `libwrappergps.so` and the GNSS HAL from
the phone. Run them in a libhybris or Android container against the S4 driver's stock-compatible ABI. Bridge
locations/NMEA to Linux gpsd/geoclue. Before investing, check whether `gps.xml` exposes an NMEA log/output.
