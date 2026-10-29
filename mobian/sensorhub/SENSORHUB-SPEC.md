# S10e (SM-G970F) sensor hub on mainline: replacing lhd

Date 2026-10-29. This is research only. No device was touched, and no blob was copied or published.

Tags: **[C]** means confirmed, with a file:line or trace line given. **[I]** means inferred or guessed.

This builds on `kit/sensors/SENSORS-INVENTORY.md`, written earlier today, and corrects two things in it. First,
the GNSS blobs are now on disk in `kit/sensorhub/private/`. Second, the chip *does* contain an on-chip PVT/NMEA
engine (see section 4).

Paths:
- `SH/` = `src/lineage-kernel/drivers/sensorhub/brcm`, the stock GPL sensor stack. `BB/` = `SH/bbdpl`.
- `ML/` = `src/linux-9820` (7.2-rc2). `UDT/` = `src/u-boot/dts/upstream/src/arm64/exynos`.
- `LIVE` = `s10e-live.dts` (the stock DT dumped from the phone).
- `TRACE` = `kit/sensorhub/lhd-android-start.strace` (cited as `TRACE:<line>`).
- `PRIV/` = `kit/sensorhub/private/` (lhd, gpsd, gps.default.so, gps.xml, init.gps.rc)

How the trace was decoded: a throwaway Python script (outside the kit) extracts every `read`/`write` on
`/dev/ttyBCM`. It then de-escapes the stream and parses the transport layer with the CRC table copied from
`BB/bbd_rpc_lh.c:232`, and splits each packet into RPCs. **All 1071 transport packets in TRACE pass the CRC check, in both
directions.** So the framing below is verified, not guessed.

---

## 0. The answer

* **For sensors, lhd adds almost nothing.** It powers the chip, uploads a RAM patch, and then relays bytes. In
  steady state it copies each record from `/dev/bbd_sensor` verbatim into an RPC 32 packet on `/dev/ttyBCM`, one
  transport packet per record. Hub→AP sensor data never reaches lhd at all, because the kernel strips RPC 33 out
  in `bbd_rpc_lh.c`. The whole SSP sensor protocol (init, enable, data parsing) already lives in GPL kernel code.
* **Nothing cryptographic sits on the sensor path.** The transport has a CRC-8 and seq/ack. The patch is a plain
  address+data record list that is written to RAM at 0x800000 and started at 0x800161 (Thumb). The only signature
  check anywhere is the *kernel-side*, optional "urgent patch" check (Samsung SPU), which we do not need.
* **The chip is a BCM4775 B0** (`AsicVersion=0x004775B0`). Its ROM version is 0x0004527C, and after the patch it
  reports 0x00085166 = 545126, which matches the patch string "Broadcom 4775.B0 FlagtailPatch ver. 545126".
* **Recommended design:** a new in-kernel SPI driver `bcm4775-shub`. It does the power and handshake, SSI framing,
  the transport layer, a `request_firmware()` patch loader, a minimal SSP client and IIO devices. It also keeps an
  optional stock-ABI char-device mode (`/dev/ttyBCM` + `/dev/bbd_*`), so the Broadcom `lhd`+`gpsd` blobs can still
  be run for GPS later. Prototype the protocol in userspace (spidev + libgpiod) first. Section 7 has the stages.
* **GPS:** the position engine (`glmeaseng`/`glposeng`) runs in gpsd on the host. A clean open GPS stack is not
  realistic. The two options are: (a) run the vendor `lhd`+`gpsd` in a bionic container against the stock-ABI mode,
  using gpsd's `NmeaOutName` NMEA output; or (b) research the chip's on-chip PVT engine (`pvt_task`,
  `GlOnChipReqMgr`, `$pglirm,req_breadcrumb,pvtOnChip,...`, on-chip `$GPRMC/$GPGGA`), which might give NMEA without
  gpsd. Option (b) is unproven.

---

## 1. The stock stack: who does what

### 1.1 Components

| Layer | Code | Role |
|---|---|---|
| SSI transport | `BB/bcm_gps_spi.c` (spi_driver, `compatible = "ssp,BCM4773"`, `:1408-1412`) | SPI framing, MCU_REQ/MCU_RESP/HOST_REQ handshake, misc device **`/dev/ttyBCM`** (`:1282-1286`). It feeds every received byte to `bbd_parse_asic_data()` (`:938`, and `:720,734` in full duplex) |
| TL parser (RX only) | `BB/bbd_rpc_lh.c` | Parses the escaped transport layer (`:575-674`), checks the CRC (`:371-387`), and walks the RPCs (`:328-369`). **RPC 33 = `IRpcSensorResponse.Data` goes to `bbd_sensor_write()` → SSP** (`:278-294`). It passes every other packet *raw and still escaped* to `bcm_on_packet_received()` → the `/dev/ttyBCM` read buffer (`:550-560`, `bcm_gps_spi.c:860-887`). The kernel has **no TL transmitter**. lhd builds every outgoing frame |
| BBD bridge | `BB/bbd.c` | Char devices `bbd_shmd`(0), **`bbd_sensor`**(1), **`bbd_control`**(2), **`bbd_patch`**(3) and `bbd_urgent_patch`(4) (`bbd.h:41-49`, `bbd.c:113-120`), plus sysfs `/sys/bbd/{bbd,lk_enable,lk_timeout,pl}`. `bbd_send_packet()` (SSP→hub) pushes `[u16 len][ssp bytes]` into the `/dev/bbd_sensor` read buffer for lhd (`:381-399`). `/dev/bbd_patch` serves the patch blob built into the kernel (`:161-184`, `:821-850`). `/dev/bbd_control` takes control strings (`:494-581`). The "lhd killer" SIGKILLs lhd when SSP wants an MCU reset or the patch download takes more than 10 s (`:457-481`, `:283-340`, `:555-566`) |
| SSP client | `SH/ssp_*.c` (about 12 k lines) plus `SH/factory/*` (about 18 k) | Samsung's sensor-hub protocol: msg header `{u8 cmd; u16 len; u16 opts; u32 data}`, 9 bytes (`ssp.h:684-704`, `SSP_INSTRUCTION_PACKET 9` `:118`). Request/response matching (`ssp_bbd.c:76-208`, `:703-831`), `initialize_mcu()` (`ssp_dev.c:311-...`), data frame parser (`ssp_data.c:832-...`), and Android-style IIO/input outputs. It is probed by `bbd_init()` on the same spi_device (`bbd.c:1353`) |
| **lhd** (closed, bionic) | `PRIV/lhd` 270 KB. Sources named in strings: `lhe2_dev/{transport/transport_layer.cpp, transport/rpc_engine.cpp, firmware_downloader/firmware_downloader.cpp, pe/pe.cpp (patch engine), fail_safe, interface/lhe_imp.cpp}`, `lhd2/hal/BrcmGpsHalGpioSysfs.cpp` | Power sequencing via sysfs, the autobaud/sync state machine (`FD:FD_WAIT4AUTOBAUD → FD_SENT_AUTOBAUD → FD_GOT_SYNC → FD_GOT_VERSION_INIT → FD_REQUEST_DOWNLOAD → FD_WAIT4DOWNLOAD_COMPLETE → FD_DONE`, from strings), patch upload, TL transmitter + reliable channel, sensor relay `/dev/bbd_sensor`→RPC 32, and the IPC server for gpsd (`/data/vendor/gps/.pipe.gpsd_to_lhd`) and CHRE/contexthub (`.pipe.chd_to_lhd`, HIDL `contexthub@1.0`) |
| **gpsd** (closed, bionic) | `PRIV/gpsd` 8.3 MB. Strings: `gll_dev/glmeaseng` (113 files), `gll_dev/glposeng` (111), SUPL/LPP, LTO | GNSS measurement + position engine on the host. It talks to lhd over named pipes (`gps.xml: PortName="lhd"`) and to the framework through `gps.default.so` → `/dev/socket/gps` |

### 1.2 Device nodes and data flow per direction

```
                    +-----------------------------+    /dev/bbd_control (strings both ways)
 lhd  <------------ | /dev/ttyBCM  (raw TL bytes)  |    /dev/bbd_patch  (read-only blob)
  |  \            +--+-----------------------------+    /dev/bbd_sensor (read: SSP->hub records)
  |   \  write    |  ^ read: non-sensor TL packets,       /sys/class/sec/sensorhub/mcu_power
  |    `--------> |  |       still escaped                /sys/class/sec/gps/GPS_PWR_EN/value
  |               v  |
  |         bcm_gps_spi.c (SSI frames over SPI17, GPIO handshake)
  |               |  ^
  |               v  | bbd_parse_asic_data(): RPC 33 -> bbd_sensor_write() -> ssp on_packet()
  |             BCM4775 (ROM + RAM patch: SensorHubTask, GNSS ME, PVT task)
  |
  `-- reads /dev/bbd_sensor  <-- bbd_send_packet() <-- ssp_bbd.c bbd_do_transfer()
```

* **AP SSP → hub [C]:** `ssp` builds a 9-byte header plus an optional payload and calls `bbd_send_packet()`. That
  yields a record `[u16 len LE][bytes]` in the `/dev/bbd_sensor` read buffer (`bbd.c:381-399`). lhd reads it
  (`TRACE:1423...`) and writes **one TL packet per record, carrying RPC 32 (`IRpcSensorRequest.Data`), whose payload is
  the record byte-for-byte**. Example from `TRACE:1427`: the record `09 00 b6 02 00 01 00 00 00 00 00` becomes the TL
  payload `20 0b 09 00 b6 02 00 01 00 00 00 00 00`. These packets are *unreliable* (flags 0), carry no acks, and use
  a running TX seq.
* **Hub → AP SSP [C]:** the chip sends TL packets containing RPC 33 `[u16 size][ssp bytes]`. The kernel consumes
  them and never shows them to lhd (`bbd_rpc_lh.c:280-291`, `:551-557`). In TRACE, after `ESW:READY` (13:28:32.66),
  **no hub→AP packet at all reaches lhd** in 38 s of runtime, even though SSP traffic is flowing.
* **Chip ↔ lhd for everything else [C]:** sync, version, patch ACKs, and (with gpsd running) all GNSS RPCs travel
  over `/dev/ttyBCM`.
* **Control strings [C]:** from lhd to the kernel: `SSP:ANDROID_VERSION:16`, `BBD:PassThru=1` (starts the 10 s killer timer),
  `BBD:PassThru=0`, `ESW:READY` (→ SSP `on_mcu_ready(true)` → `initialize_mcu()` 1 s later, `ssp_bbd.c:264-280`,
  `:387-438`), `ESW:NOTREADY` and `ESW:CRASHED` (`TRACE:341,1421,1422,1502,1504`). From the kernel to lhd:
  `BBD:RESET_REQ` (unused while the killer is on), `GPSD:SENSOR_ON/OFF`.

---

## 2. What lhd does, exactly

### 2.1 Startup sequence (TRACE, Android 16 / Lineage, with gpsd not running)

| t (s) | TRACE | Action |
|---|---|---|
| 31.129 | 295 | reads `lhd.conf` |
| 31.143 | 309 | `1` → `/sys/bbd/lk_enable` |
| 31.145 | 314 | `SSP:ANDROID_VERSION:16` → `/dev/bbd_control` |
| 31.149 | 322-326 | `GPS_PWR_EN` (gpio145 = **gpg0-2**, nSTDBY) ← 0, then `mcu_power` ← 0 (regulator `vdd_mcu_1p8` off, takes 28 ms) |
| 31.179 | 328 | sleep 20 ms |
| 31.201 | 329-336 | reads GPS_PWR_EN, then `mcu_power` ← 1, then `GPS_PWR_EN` ← 1 |
| 31.233 | 338 | sleep 20 ms |
| 31.254 | 339-341 | opens `/dev/bbd_control` and `/dev/ttyBCM`, writes `BBD:PassThru=1\0` |
| 31.270 | 343 | ttyBCM ← 16 × `0x80` (**autobaud**, `LheAutoBaudDelayMS=10`). Then 8 ms and later 36 × `0x80` (`:359`) |
| 31.507 | 360 | TL **internal sync** `b0 00 \| 00 02 80 03 00 00 00 01 b1 \| b0 01`. This is the same frame the kernel calls "Rpc GetVersion" (`BB/bcm_gps_regs.c:69-75`) |
| 31.509 | — | chip: internal reply, payload `01 01 00 00 ff 00 00 ff` (the third byte echoes the host seq [I]) |
| 31.512 | 362 | host: **RPC 0** (empty, *reliable*, rseq 0) |
| 31.514 | — | chip: ack, then **RPC 3** (12 B) `b0754700 7c520400 00000000` = AsicVersion 0x004775B0, ROM ver 0x0004527C |
| 31.515-32.427 | 364-1411 | reads `/dev/bbd_patch` (text format, see 2.2) and sends **486 × RPC 5** in 485 reliable TL packets (921 B each, rseq 1…, up to **7 unacked in flight**). The chip acks each one (`ACK=seq, RACK=rseq`) |
| 32.440 | 1412 | **RPC 46** `61 01 80 00` → jump to 0x00800161 (unreliable) |
| 32.485 | 1413 | 36 × `0x80` again (the patched firmware boots and does autobaud) |
| 32.597 | — | internal sync again (host seq e8). The chip replies with `01 01 e8 00 ff 00 00 ff` and restarts its seq at 0 |
| 32.601 | 1416 | RPC 0 (reliable, rseq **0** again, flag `GARBAGE=0x24` = the 36 autobaud bytes [I]) |
| 32.605 | — | chip RPC 3: `b0754700 66510800 …` → **patch version 545126** |
| 32.656 | 1419 | host **RPC 43** `01 00 21 00 00 00` [I: `RpcLogRequest:DebugFlag(0x1, 0x21)`, where 0x21 = 33 matches `Lhe477xDebugFlags=RPC:FACILITY=33`]. Probably optional |
| 32.659 | 1421-1422 | `BBD:PassThru=0`, then `ESW:READY` → the kernel SSP runs `initialize_mcu()` |
| 32.657… | 1420, 1423+ | opens `/dev/bbd_sensor` and starts relaying (2.4) |
| 70.82 | 1495-1511 | SIGTERM (from strace's parent): `ESW:CRASHED`, `ESW:NOTREADY`, GPS_PWR_EN←0, mcu_power←0 |

Notes:
* **[C]** `LhePinMux=P17=M1` and `LheClockMHz=100` do **not** produce any message in this trace. [I] They are used
  only when GNSS starts (gpsd), or folded into the patch. You can ignore them for sensors.
* **[I]** `mcu_power` toggles LDO21M (`ssp_sysfs.c:587-626`), but LIVE marks LDO21M `regulator-always-on`
  (`LIVE:12468-12477`), so the real reset is probably GPS_PWR_EN (nSTDBY). Keep both in the sequence.
* **[C]** The patch that was used is `BB/r_os/bbd_patch_file_beyond.h` (BlobLength 445094 matches TRACE). It is
  selected by `get_patch_version(ap_type=0, hw_rev=26)` → `bbd_current` (`ssp_dev.c:1285-1310`). hw_rev 0x1a is
  visible in the SSP AP_INFORMATION payload at `TRACE:1429`.

### 2.2 Patch file format (`/dev/bbd_patch` == `BB/r_os/bbd_patch_file_beyond.h`) [C]

ASCII header lines, then a hex blob:
```
<FormatVersion=0x00010003> <Crc=236> <BlobLength=445094> <AsicVersion=0x004775B0>
<ChangeList=4294967295> <PatchLevel=0> <PostPatchLevel=1> <CustomerVersion=None>
<Name=bcm_sensorhub_a1> <Type=Application> <PatchAddress=0x00800161>
<Blob=F1940300008000E8238700...(hex, 46 chars per line)...>
```
The decoded blob (445094 bytes) is **486 records**, each `[u8 0xF1][u16 LE n][u32 LE addr][n-4 data bytes]`.
That is 485 × 912 B plus one 16 B record, covering 0x00800000-0x0086BD5C (441692 B) with no gaps. Overhead
486 × 7 = 3402 = 445094 - 441692.
`Crc=236` is not CRC-8 (with the TL table), sum, xor or crc32 over either the blob or the data [I: an lhd-side
integrity check; it is never sent to the chip]. The firmware contains readable strings (2129 of 6+ chars): an RTOS,
`SensorHubTask`, `sensorhub.c`, `pvt_task.cpp`, `gnss_onchip.cpp`, and drivers for TCS3701/TCS3407.

### 2.3 Wire format of `/dev/ttyBCM` (Broadcom "TransportLayer") [C]

From `BB/bbd_rpc_lh.c:98-132, 371-674`, verified against every packet in TRACE.

**Escaping (byte stream):**
* A frame is `B0 00 <body> B0 01`. ESC = 0xB0, SOP = 0x00, EOP = 0x01.
* Inside the body: `B0`→`B0 03`, `11`(XON)→`B0 04`, `13`(XOFF)→`B0 05`. Raw 0x11/0x13 are dropped by the parser.

**Body:** `seq u8 | size_lo u8 | flags_lo u8 | flag-detail bytes… | payload[size] | crc u8`
* The flag bits are walked from bit 0 upward, and each set bit consumes one detail byte:
  - 0 `ACK` (detail = acked peer seq)
  - 1 `RELIABLE` (detail = own reliable seq)
  - 2 `RELIABLE_ACK` (detail = acked peer reliable seq)
  - 3 `RELIABLE_NACK`
  - 4 `MSG_LOST`
  - 5 `GARBAGE` (count)
  - 6 `SIZE_EXTENDED` (detail = size MSB)
  - 7 `EXTENDED` (detail = flags bits 8-15)
  - 8 `INTERNAL` (detail is unused, always 00)
  - 9 `IGNORE_SEQID` (detail unused)
* **CRC:** CRC-8 using `crc_table[]` (`:232-249`, poly 0x4D [I]), init 0. It covers the body from `size` (excluding
  `seq`) up to the byte before the CRC. Then the nibbles are swapped: `crc = (c<<4)|(c>>4)` (`:378-383`).
* `seq` increments by 1 per packet in each direction, independently. The reliable seq is separate. Max packet size
  is 2048 (`:66-71`). Limits are 10 retries and a 1000 ms timeout (`:73-79`).
* The **payload** is a concatenation of RPCs `id | len | data`. `id` is 1 byte (or 2 bytes if bit 7 is set: 15-bit,
  big-endian with the top bit stripped), and `len` is encoded the same way (`:299-323`).

**RPC ids seen [C]** (names come from the kernel's enum `:137-172`, which deliberately obfuscates them):

| id | direction | name / meaning |
|---|---|---|
| 0 | H→C, reliable | `IRpcA.A`, the "hello"/version request [I] |
| 3 | C→H | `IRpcB.A`, the version reply: `u32 asic, u32 version, u32 0` |
| 5 | H→C, reliable | `IRpcC.A`, **memory write**: `u32 addr, u16 n, n bytes` |
| 32 | H→C | `IRpcSensorRequest.Data`: `u16 n, n bytes of SSP` |
| 33 | C→H | `IRpcSensorResponse.Data`: `u16 n, n bytes of SSP` (`:278-294`) |
| 43 | H→C | debug/log flags [I] |
| 46 | H→C | **jump/start** `u32 entry` (Thumb, odd address) |
| internal (flag 0x100) | both | sync. Host payload `00 01`. Chip payload `01 01 <host seq> 00 ff 00 00 ff` [I meaning] |

Ids 34 and up are beyond the kernel's enum, so the real RPC table is larger (GNSS lives there).

**Reliable channel [C]:** the host sets `RELIABLE` with an incrementing rseq (starting at 0 after each sync). The chip
answers each one with an empty packet carrying `ACK=<host seq>` and `RELIABLE_ACK=<rseq>`. lhd keeps at most 7 in
flight. The host piggybacks `ACK=<chip seq>` on its next packet when the chip sent payload, for example the first
RPC 5 acks the chip's RPC 3 packet (`TRACE:371`). After READY, nobody acks the chip's unreliable sensor packets;
the kernel computes ack flags (`:488-505`) but never sends them.

### 2.4 Steady state [C]

lhd is **a pure relay with no transformation**. It reads records from `/dev/bbd_sensor` (possibly several per
`read()`), wraps each one as a separate TL packet with a single RPC 32 and an unchanged payload, and writes it to
`/dev/ttyBCM`. It also drains `/dev/ttyBCM` (only GNSS/ESW traffic would arrive there). Example records from
`TRACE:1423-1500`, all of them created by the kernel's `initialize_mcu()`:
`0F WHOAMI(read 1)`, `B6 AP_INFORMATION(00 1a)`, `F1 SENSOR_FORMATION(07 07 07)`, `EC GLASS_TYPE`,
`F4 SCANNING(read 8)`, `01 FUSEROM(read 3)`, `FD MAG_STATIC_MATRIX(27 B = DT ssp-mag-array)`, `46 MAG_CAL_PARAM`,
`B3 THERMISTOR_TABLE`, `F0 FIRMWARE_REV(read 4)`, `E1 ACCEL_RANGE(read 1)`, `91 PANEL_INFO`, `96`,
`CD GYRO_CAL`, `CE ACCEL_CAL`, `F2 PROXTHRESHOLD`, `49 LIGHT_COEF(28 B)`, `DB`, `D2`, and every ~3 s
`A7 CURRENT_TIMESTAMP(u64 boottime ns)` (sent by `ssp_bbd.c:681-699,766`).

---

## 3. Can lhd be reimplemented for sensors? Yes, and it is small.

**Nothing cryptographic or obfuscated is in the way [C].** The CRC is a table CRC-8. The RPC names are obfuscated
but the numbers are plain. The patch is unsigned RAM records. The only signature (SPU, `bbd.c:852-...`) protects the
optional `/spu/sensorhub/urgent.patch`. **[I]** The ROM might check something we cannot see, but it accepted RPC 46 and
the version changed, so no host-supplied signature is involved.

**Minimum message set for sensors (host side):**
1. Power: GPS_PWR_EN=0, (vdd_mcu_1p8 off/on), 20 ms, GPS_PWR_EN=1, 20 ms.
2. Autobaud: 16 × 0x80, about 130 ms, then 36 × 0x80, about 100 ms (TRACE timing; [I] the ROM may need only one burst).
3. Internal sync `00 01`. Wait for the chip's internal reply.
4. RPC 0 (reliable). Wait for RPC 3 and check asic == 0x004775B0 (and ROM 0x0004527C).
5. 486 × RPC 5 (reliable, window ≤ 7, retry on a missing RELIABLE_ACK after 1 s).
6. RPC 46 `0x00800161`.
7. Autobaud 36 × 0x80, internal sync, RPC 0 → RPC 3 must report version 545126.
8. (RPC 43, optional [I].)
9. Steady state: wrap SSP records in RPC 32. Unwrap RPC 33 into the SSP parser.
10. Recovery: on an SSP timeout or a crash indication [I: some C→H RPC reports ESW panic, see the strings "MCU SW PANIC" and
    "ESW:CRASHED"], redo steps 1-8.

The **SSP layer** (`SH/ssp_*`, which is GPL and already in the kernel) then needs, for accel + light + prox:
* `initialize_mcu()` core: WHOAMI 0x0F must return **0x55** (`ssp.h:85`), plus `B6` AP info, `F1` sensor position
  (DT 7/7/7), `F4` scanning info (a bitmask of the sensors present), and `A7` timestamps. Wait for `MSG2AP_INST_SENSOR_INIT_DONE 0x0a`
  (`ssp_data.c:851-854`). Then send `F2` prox thresholds and `49` light coefficients [I: without them prox/lux may be
  uncalibrated. The values come from the DT and `factory/prox_autocal_ams.c`/`light_colorid.c`].
* Enable: `A1 BYPASS_SENSOR_ADD`, payload `[type][u32 delay_ms][u32 max_batch_latency][u8 batch_opts]`
  (`ssp_i2c.c:211-300`, `ssp_sysfs.c:102-210`). Disable with `A2`. Change delay with `A4`. Each is an AP2HUB_WRITE (opts=1).
* Data: HUB2AP_WRITE frames (`[u16 opts=2][u16 len][frame]`, `ssp_bbd.c:763-821`) containing
  `0x37 BYPASS_DATA, type, payload, u64 timestamp` (`ssp_data.c:857-893`, ts at `:165-184`).
  Payload sizes for this build (`sensor_list.h`, defconfig):
  - accel (type 0): `s16 x,y,z`. ±8 g, so **4096 LSB/g** (`factory/accel_lsm6dsl.c:39-43`) → scale 0.0023942 m/s²
  - gyro (1): `s32 x,y,z` (12 B, different from mainline's 6 B)
  - mag cal (4): 8 B
  - pressure (5): 6 B
  - **light (9): 21 B** `u32 lux, s32 cct, u16 r,g,b,w, u16 a_gain, u16 a_time, u8 brightness` (lux comes straight
    from the hub, because `CONFIG_SENSORS_SSP_LIGHT_REPORT_LUX=y`; `ssp.h:523-540`, `ssp_input.c:392-414`)
  - **proximity (7): 7 B** `u8 detect, u16 adc, u32 light` (`ssp.h:574-583`, `ssp_data.c:349-358`)
  - game rotation vector (16) and rotation vector (17): 17 B quaternion `s32 a,b,c,d, u8 acc`
  - auto-rotation (48): 1 B. This is the hub's own screen-rotation result [I: 0-3].

**Where it can run:**
* *Userspace* (spidev + libgpiod + a ~1000-line C/Python daemon) is good for proving the protocol. To feed
  iio-sensor-proxy without a kernel driver, it would have to implement the `net.hadess.SensorProxy` D-Bus API
  itself, or use uinput for accel only (iio-sensor-proxy's `input-accel`). Light and proximity are IIO-only in
  iio-sensor-proxy.
* *In-kernel* is the recommended end state (section 5). The patch upload is about 200 lines with `request_firmware()`.

---

## 4. GPS

**What gpsd is [C]:** gpsd is the host GNSS stack. It holds Broadcom's measurement engine (`glmeaseng`, 113
source files referenced in strings), position engine (`glposeng`, 111), SUPL/LPP, LTO download (`gllto.glpals.com`)
and SSL (SUPL). `gps.xml` gives the RF config the engines need (`FrqPlan=FRQ_PLAN_26MHZ_2PPM_49_152MHZ_300PPB`,
`RfType=GL_RF_4755_BRCM_EXT_LNA`, L1 only). gpsd connects to lhd over named pipes (`.pipe.gpsd_to_lhd.to_server`,
`TRACE` thread 8517), and lhd carries its RPCs over the same TL. gpsd links `libwrappergps.so`,
`android.frameworks.sensorservice@1.0.so`, `libhidlbase.so` and `libssl/libcrypto` (readelf). Of these,
`libwrappergps.so` is not in `PRIV/`.

**On-chip PVT exists [C, strings in the patch]:** `pvt_task.cpp`, `gnss_onchip.cpp`, `GlOnChipReqMgr::*`
(geofence, batching, indoor detection), `pGONCHIP_NMEA`, `$GPRMC,%s,` / `$GPGGA,...`, `$PGLOR,...,FIX`, and the
host command `$pglirm,req_breadcrumb,pvtOnChip,periodSec,%d,batchOption,%d` / `$pglirm,stop,pvtOnChip`. gpsd has
the matching `CustomerModule::... onChipPvt request`. This is the low-power FLP/batching/geofence mode. [I] It almost
certainly still needs gpsd to configure and start the measurement engine (RF/frequency plan, time/aiding), and it
reports batched fixes, not a live 1 Hz stream.

**gpsd can emit NMEA [C, strings]:** `NmeaOutName` / `acNmeaOutName`, "[cat %s] to stream out NMEA",
`NmeaToLogCat`. [I] Setting `NmeaOutName="/data/vendor/gps/nmea_out"` in `<hal>` of gps.xml should make it write a
NMEA FIFO or file.

**Options, honestly:**
1. **Blob fallback (recommended for GPS): feasible, medium-high effort.** Run `lhd` + `gpsd` (+ `libwrappergps.so`
   from the phone) as bionic executables in a minimal Android userspace: an LXC/chroot with Lineage `/system`
   libraries and linker64, or the Waydroid image already used on the Poco. libhybris alone is not enough, because
   these are executables that expect `/dev/binder` and servicemanager (lhd does binder calls at `TRACE:251-290`,
   for the contexthub and suspend HALs). They also need the **stock kernel ABI**: `/dev/ttyBCM`, `/dev/bbd_{control,patch,sensor}`,
   `/sys/bbd/lk_enable`, `/sys/class/sec/sensorhub/mcu_power`, `/sys/class/sec/gps/GPS_PWR_EN/value`, and an
   in-kernel SSP that still consumes RPC 33. NMEA then goes to Linux `gpsd` and geoclue. The sensors keep working
   through our own kernel SSP even in this mode, because in the stock design sensor data never passes through lhd.
2. **On-chip PVT without gpsd: research, low-to-medium odds.** Capture a full strace of lhd *with gpsd getting a
   fix* (ttyBCM bytes plus the gpsd↔lhd pipes). Identify the GNSS RPCs that gpsd sends at session start (RF config,
   `LhePinMux`), then replay the minimal set from our driver followed by `pvtOnChip`. Unknowns: the opaque binary
   aiding/config structures, and whether the on-chip PVT runs with no host ME at all. ("GNSS Host disconnect detected -
   sensor only processing begin" hints the chip can run alone, at least for sensors.)
3. **Open host ME/PE: not realistic.** The chip streams raw measurement-engine data. Reimplementing Broadcom's
   tracking/measurement engine is a research project.

---

## 5. Porting the SSP driver to mainline

**The stock SSP is about 12.2 k lines of core plus 18.2 k of factory code [C]**, built around Samsung-isms:
`<linux/wakelock.h>` (6 files), `sec_class`/`sec_device_create` (`bcm_gps_spi.c:1218`, `ssp_sysfs.c:1458`),
`ssp_platformdata.h`, `sec_debug`, `sec_batt`, `earlysuspend`, `spu-verify`, `set_fs()`/`vfs_read` firmware reading,
`do_gettimeofday`/`timespec`, `setup_timer`, legacy `of_get_named_gpio`, input devices with REL_* abuse
(`ssp_input.c`, 53 `input_report_*`), and IIO used as an opaque byte pipe for the Android HAL
(`ssp_iio.c:704-765`, all channels are `IIO_TIMESTAMP` with byte-blob scan types). **iio-sensor-proxy can use none of
it.** Do not port it. Write a new driver.

Mainline already has `ML/drivers/iio/common/ssp_sensors/` (Galaxy S6, STM32 hub) with the same message header,
command numbers and enable payload (`ssp_dev.c:217-250`). But it assumes a 4-byte timestamp (`ssp_sensors.h:10`),
a 6-byte gyro, an STM32 SPI handshake and firmware update, so it is incompatible on the wire. It is a good
**template** for structure (the `ssp_sensor_data` / `process_data` split and `ssp_common_buffer_postenable`), and
possibly for extension with a variant/transport ops struct. **[I]** A separate driver is cleaner than bending it.

**Recommended minimal design: `drivers/iio/common/bcm4775-shub/` [I, design]**

```
bcm4775_core.c   spi_driver: gpiod (gps-pwr-en, mcu-req, mcu-resp, host-req IRQ, ap-sleep), regulator
                 vdd-mcu-1p8; SSI framing (port of bcm_gps_spi.c :660-1031, 8-bit words, no swap-mode);
                 kthread/workqueue on HOST_REQ level IRQ; runtime PM later
bcm4775_tl.c     TL encode/decode (CRC, escape, seq, reliable window 7, 1 s retry), RPC split
bcm4775_fw.c     request_firmware("brcm/bcm4775b0-beyond0lte.patch") -> parse header + 0xF1 records
                 -> sync/RPC0/RPC5.../RPC46/resync; verify version 545126
bcm4775_ssp.c    SSP client: msg queue, pending list keyed by opts, HUB2AP parser, init sequence,
                 A1/A2/A4, A7 timestamp every few s, MCU reset on timeout
bcm4775_iio.c    accel (IIO_ACCEL x/y/z raw+scale+mount_matrix, kfifo buffer + timestamp),
                 anglvel, magn, pressure, light (IIO_LIGHT in_illuminance_input, + intensity r/g/b/clear),
                 proximity (IIO_PROXIMITY in_proximity_raw = adc or detect, near_level)
bcm4775_cdev.c   optional (module param stock_abi=1): /dev/ttyBCM + /dev/bbd_{control,sensor,patch}
                 + sysfs mcu_power / GPS_PWR_EN, with exactly the stock semantics, for blob lhd/gpsd
```
iio-sensor-proxy mapping:
* `iio-poll-accel` / `iio-buffer-accel` → accel. The hub data is already rotated by position 7 into the Android frame
  (x right, y up, z out of the screen), which is the same convention iio-sensor-proxy uses [I]. So use an identity mount-matrix, and
  check it with `monitor-sensor`.
* `iio-poll-als` → `in_illuminance_input` (lux, scale 1).
* `iio-poll-proximity` → `in_proximity_raw` + udev `PROXIMITY_NEAR_LEVEL`. Expose `detect × near_level` or the ADC,
  and use the hub's `detect` bit for the call-screen blanking.
* Orientation: iio-sensor-proxy derives it from accel. Gyro, rotation vector and compass are extras.
Polled reads need the hub sensor enabled. [I] The simplest approach is to enable it on the first `read_raw` and auto-disable it
about 5 s after the last read (prox and light are ON_CHANGE, so cache the last event). Buffered mode would enable it in
`postenable`.

Size estimate [I]: core+TL+fw about 1.2 k lines, SSP about 0.8 k, IIO about 0.8 k. cdev compat about 0.6 k
(mostly lifted from `bbd.c`/`bcm_gps_spi.c`, which are GPL).

---

## 6. Mainline prerequisites

| Item | Stock | Mainline 7.2 upstream | Local `ML/` + `UDT/` working tree |
|---|---|---|---|
| SPI block | "spi17" = **PERIC1 USI16** @ 0x10970000, GIC SPI 0x1aa=**426**, fifo 64, `swap-mode` (`LIVE:11360-11407`, `exynos/exynos9820.dts:3793-3807`) | `spi-s3c64xx` has `samsung,exynos850-spi` (`ML/drivers/spi/spi-s3c64xx.c:1561-1571,1642`); spi2 already works this way on 9820 (`UDT/exynos9820.dtsi:788-822`) | **added today 20:04-20:05, uncommitted:** `usi16: usi@109700c0` + `spi17: spi@10970000` (backups in `kit/sensorhub/bak/*.before-spi17`) |
| USI mode | sysreg PERIC1 + **0x1048**, `usi_v2_mode="spi"` (`LIVE:9071-9077`) | `exynos-usi.c` `samsung,exynos850-usi` (USI v2) | `samsung,sysreg = <&sysreg_peric1 0x1048>`, board `&usi16 { samsung,mode = <USI_MODE_SPI>; }` |
| Clocks | GATE_USI16_USI + DOUT_CLK_PERIC1_USI16_USI. SFRs: mux 0x2a0, div 0x1830, gates 0x2080/0x2108/0x210c, QCH 0x3070 (`cal-if/exynos9820/cmucal-sfr.c:333,495,1778-1781,2609`) | CMU_PERIC1 has only UART_BT + USI17_I2C | **added today**: `CLK_{MOUT,DOUT}_PERIC1_USI16_USI*`, `CLK_GOUT_PERIC1_USI16_USI_{CLK,IPCLK,PCLK}` (ids 12-16) |
| Pins | gph0-0..2 func 2 (bus), gph0-3 CS (func 2 = native, or GPIO) (`LIVE:3277-3301`) | pinctrl peric1 exists | `spi17_bus`, `spi17_cs` (GPIO CS, `cs-gpios = <&gph0 3 …>`) |
| Device node | `BCM4773@0`: 25 MHz (`0x17d7840`), **mode 3** (cpol+cpha) (`LIVE:11376-11405`) | — | **missing**: no child under `&spi17` yet |
| HOST_REQ (chip→AP IRQ) | **gpa1-7**, ALIVE, func 0xf (EINT), pull-down, level-high IRQ (`bcm_gps_spi.c:1185,1271`, `LIVE:181-188`) | ALIVE pinctrl + exynos7 wakeup EINT (`UDT/exynos9820.dtsi:1156-1162`) | needs DT |
| MCU_REQ (AP→chip wake) | **gpg4-1**, output (`LIVE:2291-2298`) | peric0 pinctrl | needs DT |
| MCU_RESP | **gpg4-2**, input | | needs DT |
| GPS_PWR_EN (nSTDBY) | **gpg0-2**, output, gpio145 (`LIVE:2269-2277`, `TRACE:322`) | | needs DT |
| AP_SLEEP ("batch-wake-irq") | **gpa0-5**, output from the AP, driven on suspend/resume (`ssp_dev.c:501-...`) | | needs DT; drive it high (awake) [I] |
| SHUB_INT | **gpa4-0**, read only (`ssp_dev.c:515`) | | optional |
| vdd_mcu_1p8 | S2MPS19 **LDO21M** 1.8 V, always-on (`LIVE:12468-12477`) | `s2mps11.c` has S2MPS19 via ACPM; the board DT lists only ldo17m/18m (`UDT/exynos9820-beyond0lte.dts:427-444`) | add `ldo21m` (always-on). Mind LDO22M = VDD_NFC_1P8 next to it |
| DMA | none in the stock node | PIO is fine | stay in PIO. Use 8-bit words. Avoid the vendor's 32-bit swap-mode trick (`bcm_gps_spi.c:665`, vendor `spi-s3c64xx.c:775-800`) |

SSI framing details for the port [C] (`bcm_gps_spi.c:59-63, 214-329`, `bcm_gps_spi.h:25-41,119-120`):
* Stock settings: full duplex, 2-byte length, flow control with 3 retries. TX command byte **0x58**
  (`FULL_DUPLEX|2B_LEN|FC`) followed by `len16 LE`, data and a 2-byte FC field. RX command **0x50**, and the response
  is `status` + `len16` + data. The status byte is PZC: buffer count/size/PE bits (`:171-206`). The frame is at most 8 KB
  (`MAX_SPI_FRAME_LEN = 8*PAGE/4`).
* Every transfer is wrapped in `hello`: set MCU_REQ=1, poll MCU_RESP for up to 100 ms (toggling REQ every 20 ms), and set
  MCU_REQ=0 afterwards (`:501-550`). A HOST_REQ high-level IRQ triggers RX (`:1039-1065`). The RX/TX loop is at `:889-1031`.
* In full duplex, the chip's TX bytes arrive during our writes and must be parsed too (`:710-739`).

---

## 7. Staged bring-up plan

**S1 – Bus alive (DT only, plus spidev).** Add the `ldo21m` regulator, line names or hogs for the six GPIOs, and a
temporary `spidev` child on `&spi17` (mode 3, start at 1-5 MHz). From userspace (libgpiod + spidev): run the power
sequence, then MCU_REQ↑ and wait for MCU_RESP. Send an SSI write (0x58) with the 0x80 autobaud bytes, then the
internal sync frame. **Success = the chip's internal reply** `b0 00 00 08 80 01 00 01 01 00 00 ff 00 00 ff 43 b0 01`
(the same bytes as in `bcm_gps_regs.c` and TRACE), followed after RPC 0 by RPC 3 with `b0 75 47 00`.
Risk: the clocks/Q-channel for USI16 being wrong → SError. The new CMU entries are untested, so check that `clk_summary` shows them.

**S2 – Patch boots (userspace "lhd-lite").** Convert `r_os/bbd_patch_file_beyond.h` into a local binary. Keep it out of
any repo or upload. Send the RPC 5 records with the reliable window, then RPC 46, resync and RPC 0. **Success = version
545126.** Compare the frame bytes against TRACE: seq, flags and CRC should match exactly.

**S3 – First sensor data (still userspace).** Replay the SSP init that the kernel would send. The records are verbatim
in TRACE after line 1423, so they can be copied straight from it. Wait for `0x0a INIT_DONE`, then send `A1` for accel (type 0, 66 ms)
and decode `0x37` frames from RPC 33. Then do light (9) and prox (7). Optionally publish them over a minimal
`net.hadess.SensorProxy` D-Bus shim to get auto-rotate in Phosh early.

**S4 – Kernel driver `bcm4775-shub`** (section 5): IIO accel, light and prox, then gyro, magn and pressure. Move the patch to
`/lib/firmware/brcm/`, extracted locally from the Samsung GPL source tree, which the user already has. Add
iio-sensor-proxy udev tags. Test with `monitor-sensor`.

**S5 – Power and suspend.** HOST_REQ as a wake IRQ, `MSG2SSP_AP_STATUS_SUSPEND/RESUME` (0xD4/0xD3), AP_SLEEP GPIO,
and batching off. Recovery path: re-download on SSP timeout.

**S6 – GPS.** First do the stock-ABI cdev mode and the vendor lhd+gpsd in a container (section 4, option 1). In stock-ABI mode the
kernel must *not* download the patch, because lhd does it. In parallel, capture a GNSS strace on Android for option 2.

**Risks [I unless noted]:**
* **USI16/PERIC1 clocking.** An unclocked access raises an SError. PERIC1 is shared with BT UART and the USI17_I2C amps.
* **The SPI mode and speed are wrong for mainline s3c64xx.** The vendor ran 25 MHz with feedback delay 0. Start slow.
  The `samsung,spi-src-clk`/ioclk divider differs from the vendor driver.
* **Full-duplex subtlety.** Data received during TX must be parsed, or bytes are lost and the TL sees garbage/lost
  seq. The TL tolerates this, but the patch download might stall.
* **The ROM is picky about autobaud or timing.** Replay TRACE's timing first, then trim it.
* **The SSP init order matters to the firmware** (for example INIT_DONE before enables, or the light coefficients being needed for sane lux).
  Copy `initialize_mcu()` order exactly at first.
* **AP_SLEEP / batching.** If the hub thinks the AP is asleep it may batch or hold data.
* **Under-display ALS.** Lux may depend on the panel brightness and COPR being sent (`91 PANEL_INFO`, `90 GET_DDI_COPR`). It may read
  wrong on mainline without panel notifications.
* **[C] Licensing.** The patch blob and the private binaries must stay local. Never commit or publish them.
* **The GNSS fallback is heavy.** binder/servicemanager and HIDL in a container, plus the missing `libwrappergps.so`. It may need the
  phone's full `/vendor`.
