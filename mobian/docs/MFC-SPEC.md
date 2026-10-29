# Exynos 9820 MFC (video codec) on mainline 7.2 s5p-mfc: Galaxy S10e bring-up spec

Status: research only, 2026-10-29. Nothing was built or run on the phone, and no register was read or written.
No firmware blob was copied anywhere.

Labels:
- **[V]** verified by reading source or DT. Path and line are given.
- **[I]** inferred, not proven. Each [I] that matters has a check in section 9.

Path abbreviations:
- `DS` = `src/lineage-kernel` (Samsung downstream 4.14)
- `MFC` = `DS/drivers/media/platform/exynos/mfc`
- `CAL` = `DS/drivers/soc/samsung/cal-if/exynos9820`
- `DTS` = `DS/arch/arm64/boot/dts/exynos`
- `ML` = `src/linux-9820` (our 7.2-rc2 tree)
- `S5P` = `ML/drivers/media/platform/samsung/s5p-mfc`
- `UB` = `src/u-boot/dts/upstream/src/arm64/exynos` (the DTs actually booted)

---

## 0. TL;DR

- The 9820 MFC is **IP version 0x1300 ("MFC v13.0")**, one core, one 64 KiB SFR window at **0x188D0000**,
  **GIC SPI 338**, behind **two SysMMUs** (0x18890000 SPI 343, 0x188B0000 SPI 345), in **pd_mfc** (PMU 0x15862600),
  clocked from **CMU_MFC at 0x18800000**, which mainline does not have yet.
- The **host interface is the same generation as mainline's v10/v12** (the FSD `tesla,fsd-mfc` path):
  the same boot sequence (reset → RISC_BASE → RISC_ON → FW_STATUS_RET → SYS_INIT), the same command and
  return codes, the same 0xF000 shared-register window. I mapped every register mainline's v6+ table
  resolves for v12 (146 of them) onto the stock header: **141 sit at the same offset**. The other 5 are
  3 decoder info registers that mainline reads but never uses, plus the encoder `E_FRAME_WIDTH/HEIGHT`
  pair (0xF770/0xF774), which the stock driver never writes. Even the encoder ME-buffer formulas are
  identical to mainline's `ENC_V120_*`. **A new variant in s5p-mfc is realistic**: about 40 lines of
  driver code plus DT. It is not a new command generation.
- Firmware is one raw blob, `mfc_fw.bin`, **≤ 1 MiB**, copied into an ordinary IOMMU-mapped buffer at **any IOVA**.
  The stock driver uses no reserved-memory or fixed-IOVA window for the normal (non-DRM) path. The DRM/secure path
  (SMC `SMC_DRM_PPMP_MFCFW_PROT`, IP protection SMC) is optional and can be ignored entirely.
- The **real work is outside the codec driver**: (1) a new `CMU_MFC` block in `clk-exynos9820.c`, modelled on CMU_DPU;
  (2) `samsung,tzpc-base = <0x18820204>` on `pd_mfc`, so the existing SMC 0x82000410 TZPC save/restore runs
  (it already works for pd_aud); (3) two SysMMU nodes with a DT-given v7 version (no probe reads).
- **Riskiest step: the first power-on of pd_mfc and the first read of anything at 0x188x_xxxx.** Until pd_mfc is on
  *and* the TZPC restore SMC has returned 0, nothing in 0x18800000–0x188FFFFF may be touched. Holes reset the SoC.
- First targets: **H.264 decode**, then HEVC and VP9 8-bit decode. Then H.264 encode. VP9 encode, 10-bit, 4:2:2
  and NAL-Q come later or never.

---

## 1. Hardware map

### 1.1 Register blocks

| Block | Base | Size | Power | Source |
|---|---|---|---|---|
| MFC core SFR | `0x188D0000` | `0x10000` | pd_mfc | `DTS/exynos9820-mfc.dtsi:20-23` [V] |
| SysMMU_MFCD0 (port 0) | `0x18890000` | `0x9000` | pd_mfc | `DTS/exynos9820-sysmmu.dtsi:182-199` [V] |
| SysMMU_MFCD0 secure half | `0x188A0000` | – | secure | `exynos9820-sysmmu.dtsi:194` [V]. **Never map** |
| SysMMU_MFCD1 (port 1, shared with WFD/tsmux) | `0x188B0000` | `0x9000` | pd_mfc | `exynos9820-sysmmu.dtsi:201-219` [V] |
| SysMMU_MFCD1 secure half | `0x188C0000` | – | secure | `exynos9820-sysmmu.dtsi:213` [V]. **Never map** |
| CMU_MFC | `0x18800000` | `0x8000` defined (CAL block size) | pd_mfc | `CAL/cmucal-sfr.c:33` [V] |
| SYSREG_MFC | `0x18810000` | (0x104 DRCG_EN, 0x108 MEMCLK used) | pd_mfc | `CAL/flexpmu_cal_local_exynos9820.h:950-951` [V] |
| D_TZPC_MFC | `0x18820000` | (0x204 = the `need_smc` address) | pd_mfc, secure | `DTS/exynos9820-pm-domains.dtsi:122` [V]. **Never access from NS** |
| tsmux / WFD | `0x188E0000` | `0x1000` | pd_mfc | `DTS/exynos9820.dts:4663-4668` [V]. Out of scope |
| PMU MFC_CONFIGURATION / STATUS | `0x15862600` / `0x15862604` | | always on | `CAL/flexpmu_cal_local_exynos9820.h:896-899` [V] |

Ignore the extra child nodes in the stock MFC node: `hwfc` 0x18A28000 lives in the G2D block (OTF camera
encode), `mmcache` 0x1A300000 is a system cache driven by SMC, and `cmu` 0x1A200000 + 0x1BC0..0x1BF00000 are
CMU_BUSC/MIF0-3, mapped for QoS and debug dumps (`DTS/exynos9820-mfc.dtsi:124-140`, `MFC/mfc.c:1185-1250`) [V].
None of them is needed for decode or encode.

### 1.2 IRQs (GIC SPI. The stock `<0 N 0>` numbering equals mainline `GIC_SPI N`: sysmmu_aud is 48 in both, `DTS/exynos9820-sysmmu.dtsi:101` vs `UB/exynos9820.dtsi:1450`)

| Line | SPI | Source |
|---|---|---|
| MFC | **338** (`INTREQ__MFC`) | `DS/include/dt-bindings/interrupt-controller/exynos9820.h:352`, `DTS/exynos9820-mfc.dtsi:24` [V] |
| WFD | 339 | same header `:353` [V] |
| SysMMU_MFCD0 non-secure / secure | **343** / 344 | `exynos9820-sysmmu.dtsi:185-186` [V] |
| SysMMU_MFCD1 non-secure / secure | **345** / 346 | `exynos9820-sysmmu.dtsi:204-205` [V] |

The stock DT gives trigger type 0. Use `IRQ_TYPE_LEVEL_HIGH`, like every other 9820 node in UB [I].
Do not wire up the secure SysMMU lines (344/346).

### 1.3 Cores

There is **one** MFC instance (`mfc_0`, alias `mfc0`; `DTS/exynos9820.dts:220`) [V]. `num_entities = 2` in
`MFC/mfc.c:1792` is the decoder plus encoder video nodes, not cores [V]. The two AXI ports each have their own
SysMMU (`iommus = <&sysmmu_mfc0>, <&sysmmu_mfc1>`, `exynos9820-mfc.dtsi:27`) [V], and both belong to one IOMMU
domain (`iommu-domain_mfc`, `DTS/exynos9820.dts:4654-4661`) [V].

---

## 2. Version and firmware

### 2.1 Version

- DT `ip_ver = <0x1300>` (`DTS/exynos9820-mfc.dtsi:30`; the live stock DT confirms it, `s10e-live.dts:6820`) [V].
- The driver prints major/minor from `ip_ver` (`MFC/mfc_common.h:204-205`). **After** SYS_INIT it reads
  `MFC_REG_MFC_VERSION` = **0xF028** (`MFC/mfc_regs.h:51`) and refuses the firmware unless it equals `ip_ver`
  (`MFC/mfc_run.c:109-116`) [V].
- **No hardware ID register is read before the firmware boots.** 0xF028 lives in the 0xF000 shared window,
  which the reset sequence zeroes and the firmware fills in [V: `mfc_hw_reg_api.c:24-31`]. So you cannot
  "probe the version" of a cold block. Do not invent a pre-boot ID read.
- Mainline encodes v12 as `MFC_VERSION_V12 = 0xC0` (`S5P/regs-mfc-v12.h:26`), and `IS_MFCV12()` means
  `version >= 0xC0` (`S5P/s5p_mfc_common.h:792`) [V]. A v13 variant at `0xD0` therefore takes **every** v12 code
  path automatically.

### 2.2 Firmware file

- Name: `"mfc_fw.bin"` (`MFC/mfc_common.h:70`), loaded with `request_firmware` (`MFC/mfc_buf.c:580-581`) [V].
- Size limit: `firmware_code = PAGE_ALIGN(0x100000)` = **1 MiB** (`MFC/mfc.c:1785`). A larger blob is rejected
  (`MFC/mfc_buf.c:591-596`) [V]. The firmware buffer is 1 MiB plus `dev_ctx` (0x7800, page-aligned to 32 KiB)
  (`MFC/mfc_buf.c:528-530`, `mfc.c:1774`) [V].
- The format is a **raw image**: it is `memcpy`'d to offset 0 and the rest of the 1 MiB is zeroed (`MFC/mfc_buf.c:604-608`).
  There is no header and no signature check on the normal path [V].
- The blob is **re-copied on every first open** (`MFC/mfc.c:357`, inside `__mfc_init_instance`) [V]. Mainline already
  does the same for v12 (`S5P/s5p_mfc_ctrl.c:53-60`: the "reload each run" comment) [V].
- Where it lives on the phone: not in any source tree here. `DS/firmware/mfc/*.ihex` is the **wireless-charger**
  "MFC" (S2MIW04), not video. Usually it is `/vendor/firmware/mfc_fw.bin` on the vendor partition [I]. The owner
  should put it on the phone under the name the new variant asks for (see 7.3). Do not ship it in a repo.
- The memory version stamp is `FW_VERSION` 0xF000 (yy/mm/dd + 'D'/'E'/'N' tag) (`MFC/mfc_run.c:94-105`) [V].

### 2.3 Where the firmware sits in memory

- Normal (non-DRM) firmware: `MFCBUF_NORMAL` → `"ion_system_heap"`, then `ion_iovmm_map(..., 0, ...)`, an IOVA chosen
  by the allocator (`MFC/mfc_mem.c:89-141`, `mfc_buf.c:539-545`) [V]. **There is no reserved-memory node, no fixed
  IOVA and no `iommu-addresses`.** The IOVA goes straight into `RISC_BASE_ADDRESS`
  (`MFC/mfc_hw_reg_api.c:39-49`) [V].
- DRM firmware (`vfw_heap`, `ION_FLAG_PROTECTED`) plus `SMC_DRM_PPMP_MFCFW_PROT` 0x82002112 is used only when the protect
  SMC succeeds, and only for secure contexts (`MFC/mfc.c:361-378`, `DS/include/linux/smc.h:65`). The defconfig enables
  it (`DS/arch/arm64/configs/exynos9820-beyond0lte_defconfig:4888`), but the normal path does not depend on it
  (`MFC/mfc_run.c:144-158`) [V]. **Skip it entirely.**
- Mainline (IOMMU present) preallocates **8 MiB** with `dma_alloc_coherent`. The firmware goes at the start, the base
  is 128 KiB-aligned, and IOVA 0 is avoided (`S5P/s5p_mfc.c:1218-1266`) [V]. That fits the stock constraint
  (`MFC_BASE_MASK = (1<<17)-1`, `MFC/mfc_common.h:74`) [V]. 8 MiB holds the firmware (1 MiB) plus the device
  context plus about three H.264 decode instances (2 MiB context each). Raise it with `s5p_mfc.mem=` if needed [I].

---

## 3. How the stock driver boots the firmware

`__mfc_init_hw()` (`MFC/mfc_run.c:30-140`) [V], with the pd already on (runtime PM) and the clock enabled:

| # | Access | Offset | Source |
|---|---|---|---|
| 0 | `clk_enable(aclk_mfc)` (= GATE_MFC = MFC_QCH, see 6.2) | – | `mfc_run.c:51`, `mfc_pm.c:92` |
| 1 | W `RISC2HOST_CMD`=0, `HOST2RISC_CMD`=0, `FW_VERSION`=0 | 0x1104, 0x1100, 0xF000 | `mfc_hw_reg_api.c:24-26` |
| 2 | W 0 to 1024 words from 0xF000 (`REG_CLEAR_BEGIN/COUNT`) | 0xF000–0xFFFC | `mfc_hw_reg_api.c:28-29`, `mfc_regs.h:491-492` |
| 3 | W `MFC_RESET` = 0x1FFF, then 0 | 0x1070 | `mfc_hw_reg_api.c:31-32` |
| 4 | W `RISC_BASE_ADDRESS` = firmware IOVA | 0x0054 | `mfc_hw_reg_api.c:47` |
| 5 | clear dev int flags; W `RISC_ON` = 1; W `MFC_OFF` = 0 | 0x0000, 0x7120 | `mfc_hw_reg_api.h:93-98` |
| 6 | wait for IRQ with R2H = `FW_STATUS_RET` (12) | ISR reads 0x1104/0x003C | `mfc_run.c:68-74`, `mfc_regs.h:527` |
| 7 | W `CONTEXT_MEM_ADDR`/`SIZE` = common ctx buffer; H2R `SYS_INIT` (1): W 0x1100=cmd, W 0x0044=1 | 0xF014, 0xF018, 0x1100, 0x0044 | `mfc_cmd.c:26-46`, `mfc_hw_reg_api.c:56-82` |
| 8 | wait for `SYS_INIT_RET` (1); check `ERROR_CODE` 0xF074 | | `mfc_run.c:79-93` |
| 9 | R `FW_VERSION` 0xF000, R `MFC_VERSION` 0xF028, which must equal 0x1300 | | `mfc_run.c:94-116` |

IRQ ack: W `RISC2HOST_CMD` = 0, W `RISC2HOST_INT` = 0 (`MFC/mfc_reg_api.h:22-28`) [V].
Teardown and sleep: R `MFC_STATE` 0x7124, then W `MFC_OFF` 0x7120 = 1 (`mfc_hw_reg_api.h:125-130`). Shutdown polls
`BUS_STATUS` 0x7018 → 0, then W `RISC_ON` = 0 (`:103-123`). The stock flow never calls `mfc_stop_bus`
(the 0x7110 bus reset) [V: no caller in `MFC/*.c`].

**Mainline v6+ (`S5P/s5p_mfc_ctrl.c:121-291`) does the same thing.** It zeroes 0x1104/0x1100/0xF000 and the
0xF000 window, skips the bus reset for v12 (`:139`), writes RISC_ON = 0 before reset (harmless), writes MFC_RESET
0x1FFF/0, then RISC_BASE, RISC_ON = 1, and `S5P_FIMV_MFC_CLOCK_OFF_V10` **0x7120** = 0 (`:245-246`;
`S5P/regs-mfc-v10.h:18`; this is the same register as stock `MFC_OFF`). It waits for FW_STATUS_RET, sends
SYS_INIT with `CONTEXT_MEM_ADDR`/`SIZE` (`S5P/s5p_mfc_cmd_v6.c`, `sys_init_cmd_v6`) and reads FW_VERSION [V].
Command codes match one for one: H2R 1..14, R2H 1..18 plus 20 and 32
(`MFC/mfc_regs.h:497-535` vs `S5P/regs-mfc-v6.h:37-53`, `S5P/s5p_mfc_common.h:82-98`) [V].

The only things mainline lacks are the 0xF028 version check (add it as a `dev_info` and a warning) and stock's
optional `mfc_wait_pending` poll of `FIRMWARE_STATUS_INFO` 0xF0A4 bit0 plus `RPEND` 0x7028 before re-basing
(`mfc_hw_reg_api.h:30-61`). That poll only runs when switching between normal and DRM [V].

---

## 4. Register interface vs mainline v12: concrete differences

Method: I parsed `R(field, MACRO)` in `S5P/s5p_mfc_opr_v6.c:2408-2660`, applying all V7/V8/V10 overrides,
which is the table a v12 device actually uses. That gives 146 registers. I looked each offset up in
`MFC/mfc_regs.h` (393 named registers). The scripts are in the session scratchpad and are not needed again.

**Same offset, same meaning: 141 / 146.** Nine of these have a different name but the same function:
`D_FIRST/SECOND/THIRD_PLANE_DPB(0)` 0xF160/0xF260/0xF360, `D_MV_BUFFER(0)` 0xF460, `D_(FRAME_PACK_)SEI_AVAIL` 0xF6DC,
`E_RC_RPARAM`/`E_RC_MODE` 0xF7A4, `E_H264_I_PERIOD`/`REFRESH_PERIOD` 0xFB64,
`E_(H264_)NUM_T_LAYER` 0xFBAC, `E_VP8_OPTION(S)` 0xFDB0.

**The 5 that differ:**

| Mainline field (v12 resolves to) | Mainline offset | Stock (v13) | Impact |
|---|---|---|---|
| `d_display_aspect_ratio` (`S5P_FIMV_D_DISPLAY_ASPECT_RATIO_V6`, `S5P/regs-mfc-v6.h:164`) | 0xF534 | **0xF634** (`MFC/mfc_regs.h:165`) | none. Mapped, but no mainline code reads it [V: grep] |
| `d_display_extended_ar` (`…_V6`, `:165`) | 0xF538 | **0xF638** (`mfc_regs.h:166`) | none, same reason |
| `d_h264_info` (`…_V6`, `:196`) | 0xF590 | **0xF690** (`mfc_regs.h:189`) | none, same reason |
| `e_frame_width` (`S5P_FIMV_E_FRAME_WIDTH_V6`, `:216`) | 0xF770 | **not defined**. Stock writes only `E_CROPPED_FRAME_WIDTH` 0xF778 (`MFC/mfc_enc_param.c:169`) | mainline writes it (`S5P/s5p_mfc_opr_v6.c:873,1151`). It is a write into the zeroed 0xF000 shared RAM the firmware ignores [I]. Guard it with `!IS_MFCV13` |
| `e_frame_height` (`…_V6`, `:217`) | 0xF774 | not defined | same (`opr_v6.c:875`) |

Other differences that matter but are not offsets:
- **Context buffer sizes.** Stock v13: other_dec 50 KiB, hevc_enc 50 KiB, other_enc 50 KiB (`MFC/mfc.c:1773-1782`).
  Mainline v12: 30 / 40 / 25 KiB (`S5P/regs-mfc-v12.h:15-21`). Dev ctx 30 KiB and H.264 dec 2 MiB are the same.
  **Use the stock sizes** [V for both values; I that the smaller ones would break].
- **Decoder scratch/MV sizes**: for v10+ mainline takes the firmware-reported `D_MIN_SCRATCH_BUFFER_SIZE` 0xF108
  (`S5P/s5p_mfc_opr_v6.c:167-172`, "Use min scratch buffer size"). This matches stock [V].
- **Encoder ME sizes**: stock `ENC_V100_H264/MPEG4/HEVC_ME_SIZE` (`MFC/mfc_macros.h:58-67`, constant 2304) are
  **identical** to mainline `ENC_V120_*` (`S5P/regs-mfc-v12.h:33-49`) [V]. More evidence that the interfaces match.
- **PIXEL_FORMAT 0xF020**: stock writes `pix_val | mem_type_10bit<<4`, with NV12=0, NV21=1, YVU420=2, YUV420=3
  (`MFC/mfc_reg_api.c:470-521`). Mainline's v12 values are the same, 0..3 (`opr_v6.c:929-957, 1796-1802`) [V].
  Bit 4 matters only for 10-bit output.
- **NAL-Q** (`H2R_CMD_NAL_QUEUE` 13/14, `NAL_QUEUE_*` 0xF044–0xF094) is a stock-only fast path, enabled by DT
  `nal_q = <1 0>` (`exynos9820-mfc.dtsi:45`). It is optional. The plain `NAL_START` per-frame path is still there [V].
- About 180 stock register names have no mainline counterpart: HDR10+, static info, black bar, ROI, skype/T-layer
  extras, BPG, OTF/HWFC, and so on. All of them are optional features, not part of the basic decode/encode
  contract [V: names in `mfc_regs.h`].

**Verdict: v13 is the v10/v12 command interface ("FIMV" v6+ family), not a new generation.** A
`mfc_drvdata_v13` beside `mfc_drvdata_v12` is the right shape [I, strong]. The remaining unknown is behavioural
(firmware quirks), not register layout, and only a boot can settle it.

---

## 5. SysMMUs

- Two instances, SysMMU_MFCD0 at 0x18890000 and SysMMU_MFCD1 at 0x188B0000, each `0x9000`. Non-secure IRQs SPI 343
  and 345. Both are in `pd_mfc`, and each has a CMU_MFC gate (`GATE_SYSMMU_MFCD0/1` → QCH, see 6.2)
  (`DTS/exynos9820-sysmmu.dtsi:182-219`) [V].
- Register layout as used by the stock driver: `PT_BASE_PPN` 0x0C, `INT_STATUS` 0x60, `FAULT_ADDR` 0x70,
  `FAULT_TRANS_INFO` 0x78 (`DS/drivers/iommu/exynos-iommu.h:131-158`; the MFC fault handler reads 0x60/0x78 directly,
  `MFC/mfc_regs.h:16-19`, `MFC/mfc.c:919-965`) [V]. This is **mainline's `sysmmu_v7_variant`** (non-VM)
  (`ML/drivers/iommu/exynos-iommu.c:392-405`) [V], the same layout already working for sysmmu_aud and sysmmu_dpud0.
- Version: no source states it. SysMMU_DPUD0 on the same SoC reads `0x70600000` → v7.3 at 0x34 (`kit/display/DPU-SPEC.md` §8).
  Assume **v7.x** [I]. Give it in DT as `samsung,sysmmu-version = <7 3>` so that `__sysmmu_get_version()` reads
  nothing at probe (`ML/drivers/iommu/exynos-iommu.c:516-531`). Probe runs before the PD is on, and the v7 CAPA
  probe at 0x870/0x874, or a stray read of 0x30 (a hole on v7), resets the SoC.
- No fixed IOVA windows and no secure regions on the normal path (section 2.3) [V]. The stock `tlb_property`
  (prefetch and burst hints, `sysmmu.dtsi:195-196, 214-216`) is performance tuning that mainline does not program.
  It can be left out [I].
- The secure halves (0x188A0000 / 0x188C0000, `sysmmu,secure-irq`) are reached through SMC only. Never map them.
- The tsmux/WFD block also masters through MFCD1 (`exynos9820.dts:4667`). Keep tsmux disabled.

---

## 6. Power domain and clocks

### 6.1 pd_mfc

- PMU: `MFC_CONFIGURATION` 0x15862600 bit0 = 1 (on) / 0 (off), then poll `MFC_STATUS` 0x15862604 bit0
  (`CAL/flexpmu_cal_local_exynos9820.h:896-899, 954-957, 959-960`) [V]. The on/off sequence is that single
  write plus poll, like AUD/DPU (`:3-6`, `:286-289`) and unlike G3D's 10-step sequence [V].
- **TZPC: yes, an SMC is needed.** `need_smc = <0x18820204>` (`DTS/exynos9820-pm-domains.dtsi:118-124`).
  `pmucal_local_enable` runs the restore SMC after the on sequence, and `pmucal_local_disable` runs the save SMC
  before the off sequence (`DS/drivers/soc/samsung/cal-if/pmucal_local.c:45-51, 98-104`). The call is
  `exynos_smc(0x82000410, 1 /*WAKEUP*/ or 0 /*GET_IN*/, 0x18820204, 2 /*RUNTIME_PM_TZPC_GROUP*/)`
  (`DS/drivers/soc/samsung/exynos-el3_mon.c:231-245`, `DS/include/linux/smc.h:42`,
  `DS/include/soc/samsung/exynos-el3_mon.h:23-26`) [V].
- **Mainline already implements exactly this**: `samsung,tzpc-base` in `ML/drivers/pmdomain/samsung/exynos-pm-domains.c:39-111,187`
  (save before off, restore after on, `tzpc_smc` module parameter). It works on pd_aud (memory `s10e-mainline-bringup`
  lines 85-88) [V]. **The only change is DT**: add `samsung,tzpc-base = <0x18820204>;` to `UB/exynos9820.dtsi:1301-1306`.
  Note that AUD's value has no `+0x204` (0x18c62000) while every other domain's does. Take MFC's value literally
  from stock.
- The PD's own save/restore list (`CAL/flexpmu_cal_local_exynos9820.h:901-952`) is CMU_MFC state
  (USER muxes 0x100/0x120, DIV_BUSP 0x1804, all QCH_CON 0x300C–0x305C, CONTROLLER_OPTION 0x800) plus
  SYSREG_MFC `DRCG_EN` 0x104 and `MEMCLK` 0x108. The kernel does this, not EL3. On mainline the CMU driver's runtime-PM
  save/restore of `clk_regs` covers it, plus `samsung,sysreg` with `drcg_offset`/`memclk_offset` (6.3) [V for the mechanism].
- Boot state of pd_mfc: unknown [I]. Read PMU 0x15862604 (always safe, it is the PMU). If it is ON from S-Boot,
  genpd turns it off at late boot (no consumer). With `tzpc-base` set, that runs the save SMC. Without it, the domain
  goes down with no save. Either way, put `tzpc-base` in the DT **before** the first kernel that knows pd_mfc
  powers it.
- Restore without a prior save: stock does this whenever a need_smc domain starts OFF (`exynos-pd.c:355-367` takes
  the initial state from the status register; enable always restores). So EL3 must keep its own default table [I].

### 6.2 Clocks: what the stock driver enables

- The stock MFC node has a single clock, `"aclk_mfc"` = `GATE_MFC` (`exynos9820-mfc.dtsi:25-26`, `mfc_pm.c:188`) [V].
  `GATE_MFC` is an HWACG vclk on **`MFC_QCH`** (`DS/drivers/clk/samsung/clk-exynos9820.c:296`). Toggling it writes
  `QCH_CON_MFC_QCH` **CMU_MFC+0x3030**: bit0 ENABLE, bit1 CLOCK_REQ, bit2 IGNORE_FORCE_PM_EN, bits 16-25 EXPIRE_VAL
  (`CAL/cmucal-sfr.c:2433`, `:10109-10112`; `CAL/../ra.c:463-520`) [V].
  `GATE_SYSMMU_MFCD0/1` → `SYSMMU_MFCD0/1_QCH` at +0x3050/+0x3054 (`clk-exynos9820.c:297-298`, `cmucal-sfr.c` QCH list) [V].
- Stock comment: "The MFC and TZPC SFR are in APB clock domain and it is accessible through Q-CH even if clock off"
  (`MFC/mfc_pm.c:51-53`) [V]. **Do not rely on it for the first boot.** Read only with the clock forced on.
- CMU_MFC (0x18800000). Every register the CAL defines, 76 of them (`CAL/cmucal-sfr.c` lines with `CMU_MFC)`) [V]:
  - `PLL_CON0/2_MUX_CLKCMU_MFC_MFC_USER` 0x100/0x108 (bit4 = select CLKCMU_MFC_MFC vs OSCCLK, bit7 BUSY),
    `PLL_CON0/2_MUX_CLKCMU_MFC_WFD_USER` 0x120/0x128 (`:261-264`, `:3241-3242`).
  - `MFC_CMU_MFC_CONTROLLER_OPTION` 0x800 (bits 28/29 auto-gating/PM, `:2693`, `:11089-11090`).
  - `CLK_CON_DIV_DIV_CLK_MFC_BUSP` 0x1804 (ratio bits 0-2; the boot LUT puts 3 there, i.e. ÷4, `cmucal-vclklut.c:522-524`).
  - Gates 0x2000–0x20BC, contiguous, 48 registers (`:1421-1468`). Key ones: `MFC_IPCLKPORT_ACLK` 0x2070,
    `SYSMMU_MFCD0/1` 0x20A8/0x20AC, `SYSREG_MFC_PCLK` 0x20B0, `D_TZPC_MFC_PCLK` 0x2058, `MFC_CMU_MFC_PCLK` 0x2000.
    Gate bits: 21 CG_VAL, 20 MANUAL, 28 ENABLE_AUTOMATIC_CLKGATING (`:6700-6702`).
  - QCH_CON 0x300C–0x3060 (`:2425-2446`). DBG_NFO_QCH 0x700C–0x705C (read-only, `flexpmu_cal_local…h:906-926`).
  - **Everything else in 0x0000–0x7FFF is undefined. Treat it as a hole that resets the SoC**, in particular 0x0000–0x00FC,
    0x104, 0x200–0x7FC, 0x1800, the 0x6000 range (gate+0x4000 debug, which the mainline auto-gate `is_enabled` reads;
    see 6.3), and 0x8000+ beyond the CAL block size.
- CMU_TOP side (already in mainline): `mout_cmu_mfc_mfc` 0x109C (3 bits), `gout_cmu_mfc_mfc` 0x20A8 bit21,
  `dout_cmu_mfc_mfc` 0x188C (4 bits), and the same for `_wfd` (`ML/drivers/clk/samsung/clk-exynos9820.c:3488-3491, 3592-3595, 3742-3745`) [V].
  `gout_cmu_mfc_mfc` has **flags 0**, so `clk_disable_unused` will gate it at boot if nothing claims it (unless the
  cmdline has `clk_ignore_unused`, which I have not verified) [V for the flag]. CMU_MFC must take
  `dout_cmu_mfc_mfc` as its parent and keep it enabled. Otherwise **every** CMU_MFC/MFC/SysMMU read is to an
  unclocked APB and resets the SoC [I, strong].
- BUSC side: `LHM_AXI_D0/D1_MFC`, `LHS_AXI_P_MFC` are already `CLK_IS_CRITICAL` in mainline
  (`ML/drivers/clk/samsung/clk-exynos9820.c:2701-2708, 2745-2746`) [V]. The MFC bus path stays clocked.
- DVFS: the frequency comes from ACPM (`dfs_id = ACPM_DVFS_MFC`, `DTS/exynos9820.dts:5597-5621`). Stock levels are
  134/267/400/534/666 MHz (`exynos9820-mfc.dtsi:103`) [V]. Mainline already has
  `<&acpm_ipc EXYNOS9820_CLK_ACPM_DVFS_MFC>` with a boot default of **134 MHz** (`UB/exynos9820.dtsi:532-562`) [V].
  CMU_TOP's `CLKCMU_MFC_MFC` mux/div are in the VDDI DVFS vclk (`CAL/cmucal-vclk.c:7-9`), so ACPM probably programs
  them [I]. **Never `clk_set_rate` the CMU_TOP mux/div from Linux.** Stage 1 runs at 134 MHz (fine for 1080p30 decode
  [I]). Later, set the rate only through the ACPM clock and only while pd_mfc is on (the G3D lesson,
  `kit/gpu/MALI-G76-SPEC.md` risk 4).

### 6.3 Mainline CMU_MFC: follow CMU_DPU

`dpu_cmu_info` (`ML/drivers/clk/samsung/clk-exynos9820.c:2400-2416`) is the template: `auto_clock_gate = true`,
`gate_dbg_offset = 0x4000`, `option_offset = 0x800`, `drcg_offset = 0x104`, `memclk_offset = 0x108`, sysreg via
`samsung,sysreg`, registered through `exynos_arm64_register_cmu_pm` with runtime PM
(`ML/drivers/clk/samsung/clk-exynos-arm64.c:276-329`) [V]. Things to watch:
- `samsung_is_auto_capable()` requires a DT reg size of exactly **0x10000** (`ML/drivers/clk/samsung/clk.c:246-259`).
  CMU_DPU uses 0x10000 [V]. Mapping 0x8000–0xFFFF is not an access, so it is harmless.
- In auto mode, `is_enabled` reads **gate + 0x4000** (0x6000–0x60BC for CMU_MFC) (`clk.c:262-270, 350-354`) [V].
  The CAL does not list 0x60xx for CMU_MFC [V]. On CMU_DPU, clk_summary has been read many times with no reset
  (memory: "clk_summary's N is misleading"), so the 0x6xxx debug block exists there [I]. Stage B checks one read of
  CMU_MFC+0x6070 before letting clk_summary loose.
- `exynos_arm64_init_clocks()` **writes** CONTROLLER_OPTION at probe (`clk-exynos-arm64.c:104-112`), and probe runs
  with the PD attached and on (platform `dev_pm_domain_attach` → genpd power-on → TZPC restore) [I, standard genpd].
- `clk_regs` must contain 0x100, 0x108, 0x120, 0x128, 0x800, 0x1804, 0x2000–0x20BC and 0x300C–0x3060, so runtime
  suspend/resume saves what pmucal saved (`flexpmu_cal_local…h:901-952`).
- New clock IDs are needed in **both** dt-bindings copies (`ML/include/dt-bindings/clock/exynos9820.h` and the UB
  include). Mainline has no CMU_MFC IDs today [V: grep].

---

## 7. Recommended mainline architecture

### 7.1 DT (UB/exynos9820.dtsi + both binding headers)

```dts
pd_mfc: power-domain@15862600 {           /* existing, add one line */
	...
	samsung,tzpc-base = <0x18820204>;   /* DTS/exynos9820-pm-domains.dtsi:122 */
};

cmu_mfc: clock-controller@18800000 {
	compatible = "samsung,exynos9820-cmu-mfc";
	reg = <0x18800000 0x10000>;          /* 0x10000 for auto-gating, see 6.3 */
	#clock-cells = <1>;
	clock-names = "oscclk", "bus", "wfd";
	clocks = <&oscclk>, <&cmu_top CLK_DOUT_CMU_MFC_MFC>, <&cmu_top CLK_DOUT_CMU_MFC_WFD>;
	power-domains = <&pd_mfc>;
	samsung,sysreg = <&sysreg_mfc>;
};

sysreg_mfc: syscon@18810000 {
	compatible = "samsung,exynos9820-mfc-sysreg", "syscon";
	reg = <0x18810000 0x1000>;  /* only 0x104/0x108 are known; size is a guess [I] */
	clocks = <&cmu_mfc CLK_GOUT_MFC_SYSREG_MFC_PCLK>;
};

sysmmu_mfc0: iommu@18890000 {
	compatible = "samsung,exynos-sysmmu";
	reg = <0x18890000 0x9000>;
	interrupts = <GIC_SPI 343 IRQ_TYPE_LEVEL_HIGH>;
	clock-names = "sysmmu";
	clocks = <&cmu_mfc CLK_GOUT_MFC_SYSMMU_MFCD0_CLK>;
	power-domains = <&pd_mfc>;
	samsung,sysmmu-version = <7 3>;      /* [I]: no probe read, see section 5 */
	#iommu-cells = <0>;
};
sysmmu_mfc1: iommu@188b0000 { /* same, 0x188b0000, SPI 345, MFCD1 gate */ };

mfc: video-codec@188d0000 {
	compatible = "samsung,exynos9820-mfc";
	reg = <0x188d0000 0x10000>;
	interrupts = <GIC_SPI 338 IRQ_TYPE_LEVEL_HIGH>;
	clock-names = "mfc";
	clocks = <&cmu_mfc CLK_GOUT_MFC_MFC_ACLK>;      /* CMU_MFC+0x2070 */
	iommus = <&sysmmu_mfc0>, <&sysmmu_mfc1>;
	power-domains = <&pd_mfc>;
	status = "disabled";                             /* enable in beyond0lte.dts at Stage C */
};
```

Clock names are suggestions. In auto mode the gate clocks only add a runtime-PM reference on CMU_MFC; the hardware
gates through QCH.

### 7.2 Driver (S5P)

1. `regs-mfc-v13.h` (new, or extend v12): `MFC_VERSION_V13 0xD0`, `MAX_FW_SIZE_V13 SZ_1M`, and the context sizes from
   `MFC/mfc.c:1773-1782` (dev 30K, h264_dec 2M, other_dec 50K, h264_enc 100K, hevc_enc 50K, other_enc 50K).
   `MAX_CPB_SIZE`: stock uses 3 MiB (`mfc.c:1786`). Keep v12's 7 MiB or use 3 MiB, either works [I].
2. `s5p_mfc_common.h`: `MFC_V13_BIT BIT(8)`, added to `MFC_V5PLUS/V6PLUS/V7PLUS/V10PLUS_BITS` and to the two
   `YUV420M/YVU420M` entries (`s5p_mfc_dec.c:60-71`, `s5p_mfc_enc.c:63-74`). `IS_MFCV12()` (`>= 0xC0`) already covers
   v13. Optionally add `IS_MFCV13()`.
3. `s5p_mfc.c`: `mfc_drvdata_v13` = v12 with `.version = MFC_VERSION_V13`, `.version_bit = MFC_V13_BIT`,
   `.fw_name[0] = "s5p-mfc-v13.fw"` (the owner symlinks that to the vendor `mfc_fw.bin`), `.clk_names = {"mfc"}`, and
   compatible `"samsung,exynos9820-mfc"` (`s5p_mfc.c:1698-1733` is the pattern).
4. `s5p_mfc_ctrl.c` init_hw: after SYS_INIT, read 0xF028 and `dev_info` it. Warn unless it equals 0x1300 (stock rule,
   `MFC/mfc_run.c:111`).
5. `s5p_mfc_opr_v6.c`: skip the `e_frame_width/height` writes for v13 (lines 873, 875, 1151). Optionally fix the three
   unused decoder info offsets for v13.
6. Binding: add the compatible to `ML/Documentation/devicetree/bindings/media/samsung,s5p-mfc.yaml`.

### 7.3 Userspace

The stateful V4L2 M2M decoder and encoder (`/dev/videoN`, "s5p-mfc-dec"/"-enc"): `v4l2-ctl`, GStreamer `v4l2h264dec`,
`ffmpeg -c:v h264_v4l2m2m`. Output is NV12M (2 planes). Mesa and wayland import it via dmabuf.

---

## 8. Codecs

Stock v13 (`MFC/mfc_regs.h:554-578`, `MFC/mfc_dec_internal.h:157-293`, `MFC/mfc_enc_internal.h:198-246`) [V]:
- Decode: H.264, H.264 MVC, H.263, MPEG-1/2/4, XviD, DivX (FIMV1-4), VC-1 (G/L), VP8, **HEVC**, **VP9**, BPG. HEVC and
  VP9 come in 10-bit (`support_10bit`, P010 default) and 4:2:2 (`support_422`) (`exynos9820-mfc.dtsi:55-60`).
- Encode: H.264, MPEG-4, H.263, VP8, **HEVC**, **VP9**, BPG. Spec limits: H.264 240 Mbit/s, VP8 80 Mbit/s
  (`exynos9820-mfc.dtsi:104-105`). `max_mb = 4817660` MB/s ≈ 2160p at about 148 fps aggregate (`:98`) [I, arithmetic].

Mainline v12 exposes decode H.264/MVC, H.263, MPEG-1/2/4, XviD, VC-1, VP8, HEVC, VP9 and encode H.264, MPEG-4,
H.263, VP8, HEVC (`S5P/s5p_mfc_dec.c:74-172`, `s5p_mfc_enc.c:77-109`). There is no VP9 encode format, no 10-bit
DPB format and no FIMV [V].

Realistic order:
1. **H.264 decode** to NV12M. Firmware-reported buffer sizes, the most-tested mainline path.
2. **HEVC and VP9 8-bit decode.** Same path. VP9 needs `DEC_VP9_STATIC_BUFFER_SIZE` (`opr_v6.c:262-266`).
3. **H.264 encode.** ME sizes match stock, so it is low risk after decode works.
4. HEVC and VP8 encode.
5. Later or never: 10-bit (mainline has no P010/S10B formats for s5p-mfc), VP9 encode, NAL-Q, HDR10+, DRM/secure path.

---

## 9. Staged bring-up plan and register-safety rules

**Ground rules.** A read of an unpowered, unclocked or TZPC-protected block, or of a register hole, resets the SoC
(memory: CMU_AUD+0x160, SysMMU_DPU+0x30). Always arm **INFORM3 0x1586080C = 0x12345674 and SYSIP 0x15860810 = 0xFF**
first, stream `dmesg -w` over ssh, and `msleep(500)` before each risky access.

### Safe-to-touch table by state

| State | Safe | NOT safe |
|---|---|---|
| **S0**: pd_mfc OFF (any time) | PMU `0x15862600` (RW), `0x15862604` (R). CMU_TOP `0x1A24109C`, `0x1A24188C`, `0x1A2420A8` (MFC mux/div/gate, CMU_TOP is always on; the address is CMU_TOP base `UB/exynos9820.dtsi:1571` + offset) | **anything in 0x1880_0000–0x188F_FFFF** |
| **S1**: pd_mfc ON, TZPC **not** restored (`exynos_pm_domains.tzpc_smc=0`, or the SMC failed) | PMU only | everything in 0x188x_xxxx: secure-only TZPC → external abort [I, by analogy with G3D/AUD] |
| **S2**: pd_mfc ON + TZPC restored, CMU_TOP `gout_cmu_mfc_mfc` running | CMU_MFC **only the 76 CAL-defined offsets** (6.2): 0x100/0x108/0x120/0x128, 0x800, 0x1804, 0x2000–0x20BC, 0x300C–0x3060, 0x700C–0x705C (R only). SYSREG_MFC 0x104/0x108 **only after CMU_MFC+0x20B0 shows CG_VAL** (SYSREG_AUD read without its clock = instant death) | CMU_MFC holes (0x0–0xFC, 0x104, 0x1800, 0x6xxx until Stage B checks it, ≥ 0x8000), D_TZPC_MFC 0x18820000 (secure), sysmmu secure halves |
| **S3**: S2 + MFC gate/QCH on (`QCH_CON_MFC_QCH` 0x3030 CLOCK_REQ=1, or the gate's CG_VAL=1) | MFC SFR at the offsets stock reads: 0x0000, 0x003C, 0x0044, 0x0054, 0x1060, 0x1070, 0x1100, 0x1104, 0x7018, 0x7028, 0x702C, 0x7110, 0x7120, 0x7124, 0xF000–0xFFF4. The stock **watchdog dump** reads these ranges (with `FW_CLOCK` 0x1060 = 0xFFFFFFFF first): 0x0–0x80, 0x1000–0x1CD0, 0x2000–0x2A00, 0x2F00–0x2F6C, 0x3000–0x3040, 0x3094, 0x30B4–0x30BC, 0x3110–0x3120, 0x5000–0x5100, 0x5200–0x5500, 0x5600/0x5800/0x5A00 (+0x100 each), 0x6000–0x60C4, 0x7000–0x721C, 0x8000–0x820C, 0x9000–0x910C, 0xA000–0xA20C, 0xB000–0xB444, 0xC000–0xC084 (`MFC/mfc_watchdog.c:28-83`) [V that stock reads them; I that they are hole-free] | MFC offsets outside those ranges. Both SysMMUs: only 0x00–0x24, 0x34, 0x60–0x64, 0x70–0x78 (the mainline v7 non-VM set); **never 0x30, 0x870, 0x874, 0x8000** |
| **S4**: S3 + SysMMU gate on | SysMMU_MFCD0/1 at the offsets above | – |

### Stage A: PD and TZPC only (DT change, no codec driver)

1. Pre-check, read-only: PMU 0x15862604 bit0 (boot state of pd_mfc). Check `/proc/cmdline` for `clk_ignore_unused`.
2. DT: add `samsung,tzpc-base = <0x18820204>` to pd_mfc. Nothing else.
3. Toggle the domain by hand: add a dummy consumer, or bind/unbind a test module that holds `pm_runtime_get` on a
   device in pd_mfc. Expect dmesg "Power domain MFC: TZPC restore SMC" with `res.a0 == 0`, and the status bit going 1/0.
   **No 0x188x reads yet.** Risk: an EL3 refusal (non-zero a0) means stop, because the address or group is wrong.

### Stage B: CMU_MFC driver

4. Add the CMU_MFC block (6.3) and its clock IDs. The test module, with pd on and TZPC restored, reads **only** in this
   order: `CMU_MFC+0x100` (expect bit4 = 1 after the stock-style restore, or 0 = OSCCLK from reset), `+0x800`, `+0x1804`,
   `+0x2070`, `+0x3030`. Then one read of `+0x6070` (the auto-gate debug mirror) before `cat clk_summary`.
   If 0x6070 resets the SoC, drop `gate_dbg_offset` (manual gate mode) for this CMU only.
5. Confirm `gout_cmu_mfc_mfc` stays enabled while CMU_MFC is active (clk_summary prepare/enable counts).

### Stage C: MFC SFR plus firmware (s5p-mfc v13 variant, SysMMUs in DT)

6. Make sure the firmware file is present on the phone (owner copies it from the vendor partition; do not copy it here).
7. First touch of the MFC block, from the test module: with S3 reached, read **`0x7124` (MFC_STATE)** and
   `0x003C` (RISC2HOST_INT) only. Both are stock-read registers.
8. Load s5p-mfc. Probe does no hardware access (`s5p_mfc_probe` in `S5P/s5p_mfc.c` contains no mfc_read/mfc_write/readl/writel and no
   clock or power-on call) [V]. The first `open()` runs `init_hw`. Expect `FW_STATUS_RET` within 1 s (an IRQ on SPI 338),
   then `SYS_INIT_RET`, `FW_VERSION` = a yy/mm/dd stamp, and 0xF028 = **0x1300**.
   - Hang or timeout at FW_STATUS: RISC_BASE / IOMMU problem. Check for a SysMMU fault IRQ 343/345, then translation
     (pt_base written, CTRL=enabled) and the firmware IOVA.
   - A SysMMU fault at IOVA 0 or near the firmware base: alignment (128 KiB), or one of the two ports not attached to the domain.
9. A 2-instance smoke test: open, close, open again. This proves the firmware reload and the PD off(save)/on(restore)
   cycle. Watch for "MFC disable failed" (the AUD-style 1 ms timeout seen on a second power-off).

### Stage D: codecs

10. `v4l2-ctl --stream-mmap` H.264 1080p decode, then GStreamer `v4l2h264dec ! waylandsink`.
11. HEVC and VP9 decode. Then H.264 encode (check `E_CROPPED_FRAME_*`, with no `E_FRAME_WIDTH/HEIGHT` write).
12. Later: rate via the ACPM clock (pd on only), devfreq/QoS, MIF bandwidth.

### Riskiest accesses, ranked

1. **First CMU_MFC read after the first pd_mfc power-on** (Stage B step 4). It depends on the TZPC restore working for
   0x18820204, and on CMU_TOP `gout_cmu_mfc_mfc` running.
2. **`exynos_arm64_init_clocks` writing CMU_MFC+0x800 at probe.** Same preconditions, but it runs unattended at boot.
   Make the CMU node `status = "disabled"` until Stage B has passed by hand.
3. **The auto-gate debug reads at +0x6xxx** (clk_summary / clk_disable_unused).
4. **The SysMMU probe.** Without `samsung,sysmmu-version` it reads 0x34 and the v7 CAPA registers at 0x870 with the PD
   possibly off. Always give the version in DT.
5. **The 0xF000 window zeroing** (1024 writes) and `E_FRAME_WIDTH/HEIGHT` writes. These are writes, not reads, into
   firmware-shared RAM. Low risk [I].
6. **SYSREG_MFC** 0x104/0x108 (DRCG/MEMCLK through `samsung,sysreg`). Only with its PCLK gate on (the SYSREG_AUD lesson).

---

## 10. Open questions (each needs one check on the phone)

- The boot state of pd_mfc, and whether a restore-without-save on a cold domain works (Stage A).
- The SysMMU_MFCD0/1 version register value (one read of 0x34 in S4, to confirm `<7 3>`).
- Whether CMU_MFC has the 0x6000 debug-mirror block (Stage B).
- `QCH_CON_MFC_QCH` reset value after a PD cycle (does CLOCK_REQ default to 1, as on CMU_DPU's 0x003F0002?).
- Firmware quirks the stock driver handles silently: for example, the stock `mfc_wait_pending` poll before re-basing,
  `D_DEC_OPTIONS`/`CODEC_CONTROL` bits for v13. Compare register writes in stock `mfc_reg_api.c`/`mfc_enc_param.c`
  with mainline `opr_v6.c` when the first decode misbehaves.

---

## 11. RESULT (2026-10-29 evening, live on the phone)

H.264, HEVC and VP9 1080p decode work through mainline s5p-mfc + v4l2 (GStreamer v4l2h264dec/v4l2h265dec/
v4l2vp9dec), output bit-exact against ffmpeg software decode (20-29 frames each); 1080p H.264 ~77 fps at the
134 MHz boot clock. U-Boot v31 = out/v31-mfciova.img. Encode NOT working yet (try_fmt fails from ffmpeg; GStreamer
v4l2h264enc segfaults in userspace).

What the spec missed / what it took:
- pd_mfc: tzpc-base 0x18820204 works. The SECOND power-off failed ("MFC disable failed") until CMU_MFC was
  restored after power-on: a cold CMU_MFC has 0x100/0x120 = 0 (OSCCLK), 0x800 = 0 (S-Boot f0000000), 0x1804 = 0
  (S-Boot 3). Without the CONTROLLER_OPTION bits the PMU power-down handshake never completes. Fix: CMU_MFC in
  clk-exynos9820.c with exynos_arm64_register_cmu_pm + runtime PM ops, clk_regs = 0x100,0x120,0x800,0x1804,
  all gates 0x2000-0x20bc, QCH 0x300c-0x3060; DT assigned-clock-parents/rates for the cold case.
  (Same symptom on AUD: probably CMU_AUD's option/muxes lost the same way.)
- All CMU_MFC CAL offsets, the 0x60xx debug mirror (gates +0x4000) and 0x700c-0x705c read fine with the PD on.
  SysMMU_MFCD0/1 0x34 = 0x70600000 (v7.3). MFC 0x7124 reads 0xdeadc0de before the firmware runs.
- Firmware boots (FW_STATUS 12, SYS_INIT 1, F/W 21/07/20, 0xF028 = 0x1300) with the plain v12 code path.
- Error 0x37 on SEQ_HEADER = the firmware takes buffer addresses as offsets from RISC_BASE: the base must be
  the LOWEST IOVA. iommu-dma allocates top-down (the 5433-era code assumed bottom-up). Fix: DT reserved-memory
  node `mfc-iova-reserved` with iommu-addresses <&mfc 0 0x10000000>, <&mfc 0xd0000000 0x30000000> (stock's
  window [0x10000000,0xd0000000)), and the driver allocates the firmware with dma mask 0x10000000+size-1, so it
  lands at exactly 0x10000000; later buffers fail below it and the allocator retries from the top.
- HEVC/VP9: SYNC_POINT_NOT_RECEIVED (err 0xbe) because mainline drops the header buffer for non-H.264 codecs;
  v13 stops at the end of the headers for every codec -> keep the buffer when consumed < bytesused.
- Also set (stock does, not proven necessary): CLEAR_CTX_MEM bit 16 in CODEC_TYPE; DEC_OPTIONS conceal 0x4<<8,
  realloc 1<<13, special parsing 1<<15; FORCE_PIXEL_VAL 0xf0c8 = 0x8020000; SEI recovery parsing bit 2;
  D_CPB_BUFFER_SIZE = ALIGN(real buffer, 512) instead of the max.
- Firmware on the phone: /lib/firmware/s5p-mfc-v13.fw (copied on the phone from the vendor partition; never
  copy it off the phone).
