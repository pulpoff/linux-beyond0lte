# Exynos 9820 DPU (DECON0 + DPP + DSIM0) on mainline DRM/KMS: Galaxy S10e bring-up spec

Status: research only, 2026-10-29. Nothing was built, nothing was run on the phone, and no register was read or written.

Labels:
- **[V]** verified by reading source or DT. Path and line are given.
- **[I]** inferred, not proven. Each [I] that matters gets a Stage A check.

Path abbreviations:
- `DS` = `src/lineage-kernel` (Samsung downstream 4.14)
- `DPU` = `DS/drivers/video/fbdev/exynos/dpu20`
- `CAL` = `DPU/cal_9820`
- `PNL` = `DS/drivers/video/fbdev/exynos/panel/s6e3fa7`
- `DTS` = `DS/arch/arm64/boot/dts`
- `ML` = `src/linux-9820` (our 7.2 tree)
- `UB` = `src/u-boot/dts/upstream/src/arm64/exynos` (the DTs actually booted)

---

## 0. TL;DR

| Question | Answer |
|---|---|
| Panel | **Samsung S6E3FA7** DDI, "beyond0 A3 S0", 1080x2280, 4 lanes [V `DTS/samsung/exynos9820-beyond0lte_common.dtsi:1160-1164,2397-2401`, `DTS/samsung/exynos9820-display-lcd.dtsi:268-312`]. It is NOT S6E3HA9 (that one is the S10/S10+) |
| Video or command mode | **MIPI command mode with HW TE trigger.** Panel `mode = <2>` = `DECON_MIPI_COMMAND_MODE` [V `display-lcd.dtsi:269`, `DPU/panels/decon_lcd.h:15`]. DECON `psr_mode = <2>`, `trig_mode = <0>` (HW trigger) [V `DTS/exynos/exynos9820.dts:1955-1956`] |
| DSC | **No.** `dsc_en = <0>` [V `display-lcd.dtsi:287`]. The PPS/DSC byte arrays in the panel header (`PNL/s6e3fa7_beyond0_a3_s0_panel.h:539-557`, a QHD copy-paste) are never put in any sequence table [V: grep finds no PKTINFO for them]. Uncompressed RGB888 at 1080x2280x60 is 3.55 Gbit/s on a 4x1143 Mbit/s link |
| AFBC decode in DPU | **Yes**, on 4 of 6 channels: GF0, GF1, VGF, VGRFS (attr bit 0). VG and VGS have no AFBC [V `exynos9820.dts:1710-1711,1749,1772,1786,1809,1832`]. No AFBC with rotation, block mode, or YUV420 [V `DPU/dpp_drv.c:349-352,389-404`] |
| "Flip-only" Stage B | **Feasible** [I, strong]. simpledrm works today, so DECON must be re-reading 0xca000000 on every TE: trigger unmasked, IOMMU bypassed or 1:1. A flip is then: write `IDMA_IN_BASE_ADDR_Y` of the live channel, set `SHADOW_REG_UPDATE_REQ`, and take the FRAME_START IRQ as vblank. The address register is 32-bit, so scanout buffers must be **physically contiguous and below 4 GiB** while SysMMU_DPU is bypassed |
| Zero-copy | Make the driver part of `exynos_drm` (DRM driver name `"exynos"`). Mesa's kmsro already has `exynos_dri.so` in our rootfs [V: debugfs listing of `mobian-root.img` `/usr/lib/aarch64-linux-gnu/dri/`]. With it, EGL on card0 gets panfrost, which renders straight into CMA dumb buffers that DECON scans out |
| Reusable mainline code | `exynos_drm` core (drv/gem/fb/plane/crtc helpers) [V `ML/drivers/gpu/drm/exynos/`]. Panel: extend `panel-samsung-s6e3fa7.c` with an S10e variant (the existing one is the 1080x2220 Pixel 3a panel in video mode) [V `ML/drivers/gpu/drm/panel/panel-samsung-s6e3fa7.c:96-108,204-205,239`]. CMU_DPU clocks are all present, CLK_IGNORE_UNUSED [V `ML/drivers/clk/samsung/clk-exynos9820.c:2148-2149`]. **New code needed:** the DECON/DPP CRTC and plane, because the register layout matches neither exynos5433_drm_decon nor exynos7_drm_decon. Also either a DCS-only DSI host, or a 9820 register table plus a D-PHY driver for samsung-dsim |

---

## 1. Hardware map

### 1.1 Register blocks

| Block | Base (size) | Source |
|---|---|---|
| CMU_DPU | `0x19000000` (0x10000) | [V `UB/exynos9820.dtsi:1405-1415`] |
| SYSREG_DPU | `0x19010000` (0x10000) | [V `UB/exynos9820.dtsi:1417`] |
| DISP_SS (inside SYSREG_DPU) | `0x19011000` (0x10). `+0x08` MIPI_PHY_CON (D-PHY resets), `+0x10` TE_QACTIVE_PLL_EN | [V `exynos9820.dts:1887-1890`, `CAL/regs-decon.h:17-24`] |
| DPP GF0 (ch0) | DPP `0x19021000`, DMA `0x19071000` | [V `exynos9820.dts:1700-1704`] |
| DPP VGRFS (ch1) | DPP `0x19026000`, DMA `0x19076000` | [V `:1744-1748`] |
| DPP GF1 (ch2) | DPP `0x19022000`, DMA `0x19072000` | [V `:1767-1771`] |
| DPP VGF (ch3) | DPP `0x19024000`, DMA `0x19075000` | [V `:1781-1785`] |
| DPP VG (ch4) | DPP `0x19023000`, DMA `0x19073000` | [V `:1804-1808`] |
| DPP VGS (ch5) | DPP `0x19025000`, DMA `0x19074000` | [V `:1827-1831`] |
| DPU_DMA_COMMON | `0x19070000` (0x110): QoS LUT, AFBC recovery | [V `:1704`, `CAL/regs-dpp.h:27-99`] |
| DECON0 ("decon_f") | `0x19030000` (0x10000). Shadow (active) copy readable at `+0x7000` | [V `:1936-1939`, `CAL/regs-decon.h:636`] |
| DECON2 ("decon_t", DP) | `0x19050000` | [V `:2000`] |
| DSIM0 | `0x19080000` (0x300) | [V `:1910-1912`] |
| D-PHY M4S0 (DSIM0) | `0x19161000` (0x800), PMU isolation offset `0x0710` | [V `:1903-1908,1912`] |
| D-PHY M4S4 top | `0x19160000` (0x100), PMU isolation offset `0x070c` | [V `:1896-1901`] |
| SysMMU DPUD0 / D1 / D2 | `0x190A0000` / `0x190B0000` / `0x190C0000` (0x9000 each). Secure halves at `0x190D0000` / `0x190E0000` / `0x190F0000` | [V `DTS/exynos/exynos9820-sysmmu.dtsi:20-32,44-56,68-80`] |
| MCD HDR (per DPP) | `0x191C1000..0x191C6000`, disabled | [V `exynos9820.dts:1849-1884`] |

Channel ↔ DMA mapping (WIN_CHMAP value): GF0=0, VGRFS=1, GF1=2, VGF=3, VG=4, VGS=5 [V `CAL/decon_reg.c:2198-2258`, DT aliases `exynos9820.dts:209-217`]. The SysMMU port split is DPUD0 = VGRFS+GF0, DPUD1 = VGF+GF1, DPUD2 = VGS+VG+WB [V `sysmmu.dtsi:30,54,78`].

### 1.2 IRQs (GIC SPI, the same numbering mainline uses: pinctrl_peric1 = 407 in both)

| IRQ | SPI | Source |
|---|---|---|
| DECON0 FRAME_START | **183** | [V `DS/include/dt-bindings/interrupt-controller/exynos9820.h:201`] |
| DECON0 FRAME_DONE | **184** | [V `:202`] |
| DECON0 EXTRA (resource conflict, timeout) | 189 | [V `:207`] |
| DSIM0 | 190 | [V `:208`] |
| DMA GF0/GF1/VG/VGS/VGF/VGRFS | 198/199/200/201/202/203 | [V `:216-221`] |
| DPP GF0/GF1/VG/VGF/VGS/VGRFS | 205/206/207/208/209/210 | [V `:223-228`] |
| SysMMU DPUD0/1/2 non-secure | 212/214/216 (secure: 211/213/215) | [V `:229-234`] |
| TE EINT (GPIO_PERIC1, GPD0[0]) | 407 | [V `:421`, `UB/exynos9820.dtsi:852-856`] |

Downstream sends all three DECON lines to one handler, which reads `INTERRUPT_PENDING` [V `DPU/decon_dsi.c:42-93,112-163`].

### 1.3 Clocks (mainline `ML/drivers/clk/samsung/clk-exynos9820.c`, CMU_DPU)

Every DPU gate is registered with **CLK_IGNORE_UNUSED** ("until there is a full display stack") [V `:2148-2149`], so clk_disable_unused leaves them as S-Boot set them.

| Use | Mainline clock ID (dt-bindings `ML/include/dt-bindings/clock/exynos9820.h:327-390`) |
|---|---|
| DECON AXI | `CLK_GOUT_DPU_ACLK_DECON` (43) [V `clk-exynos9820.c:2309-2312`] |
| DPP DMA AXI | `CLK_GOUT_DPU_ACLK_DMA` (44), `CLK_GOUT_DPU_ACLK_DPP` (45) [V `:2313-2318`] |
| APB | `..._AD_APB_DECON0_PCLKM/S` (5/6), `..._DPP_PCLKM/S` (11/12), `..._DPU_DMA_PCLKM/S` (13/14), `..._MIPI_DSIM0_PCLKS` (21) [V `:2157,2181,2189,2221`] |
| SysMMU | `CLK_GOUT_DPU_SYSMMU_DPUD{0,1,2}_CLK` (60-62) [V `:2373`] |
| SYSREG | `CLK_GOUT_DPU_SYSREG_DPU_PCLK` (63) [V `:2385`] |
| Parent bus | `mout_dpu_bus_user` ← CMU_TOP `dout_cmu_dpu_bus` [V `:3618`]. Downstream sets the DPU aclk rate through ACPM (`ACPM_DVFS_DISP`) [V `DPU/bts.c:82`] |

Downstream DECON/DSIM take only one clock, `GATE_DPU_DPU` ("aclk") [V `exynos9820.dts:1917-1918`]. The DSIM byte/escape clocks come from the D-PHY PLL, not from CMU [V `CAL/dsim_reg.c:1793+`].

**Rule for Stages B and C:** a driver may `clk_prepare_enable()` these clocks, but must never let them reach a disable. That means no `clk_disable` on probe-error paths and no runtime PM.

### 1.4 Power domain

- `pd_dpu`: PMU `0x15861e00`. Downstream marks it `need_smc = <0x191b0204>`, the TZPC restore SMC on power-up (same class as G3D) [V `DTS/exynos/exynos9820-pm-domains.dtsi:44-50`].
- Mainline has the node but **`status = "disabled"`** [V `UB/exynos9820.dtsi:1186-1197`]. genpd therefore never powers it off, and it stays ON as S-Boot left it.
- **Rule:** no `power-domains` property on the DPU nodes until a PD driver can make the EL3 TZPC restore (`0x82000410`). Keep `pd_dpu` disabled.

### 1.5 SysMMU_DPU and how DPP DMAs address memory

- `IDMA_IN_BASE_ADDR_Y/C/Y2/C2` are **32-bit** registers [V `CAL/regs-dpp.h:241-244`]. No high-address register exists.
- Downstream always puts the IOMMU in front of them: `iovmm_activate()` on the DSIM device, which owns the "iommu-domain_dpu" bus [V `DPU/dsim_drv.c:2489`, `exynos9820.dts:1691-1698`]. The fb is ion-allocated and IOVA-mapped [V `DPU/decon_core.c:4019-4125`].
- At boot the DPU scans out PA **0xca000000** (`s3cfb.bootloaderfb=0xca000000` in the live bootargs [V `s10e-live.dts:7092`]). Our DT gives simpledrm that address [V `UB/exynos9820-beyond0lte.dts:20-27`] and reserves `framebuffer_region@ca000000` no-map, size 1440x3040x4 [V `UB/exynos9820-common.dtsi:96-99`].
- Mainline has **no DT nodes** for SysMMU_DPUD0-2 (grep of `UB/exynos9820.dtsi` finds none), so Linux never touches them.
- **Inference [I]:** S-Boot leaves SysMMU_DPU disabled (bypass), or at most with a 1:1 map. So, until we drive the IOMMU, every scanout buffer must be physically contiguous, below 4 GiB, and inside a mapped window.
- **Stage A check:** read `REG_MMU_CTRL` at `0x190A0000 + 0x0` (`CTRL_ENABLE = 0x5`, `CTRL_BLOCK = 0x7`) [V `DS/drivers/iommu/exynos-iommu.h:100-106`]. Also read `+0x4` CFG and `+0x34` VERSION (on v7 the version is at `0x870`/`0x874` CAPA [V `:150-151`]) for D0, D1 and D2. A value of 0 (or bit 0 clear) means bypass.

### 1.6 TE pin, reset, supplies

| Item | Value | Source |
|---|---|---|
| TE | **GPD0[0]** (pinctrl_peric1 @ `0x10830000`). Function **0xF** = "hw_te_on" (TE goes to the DECON HW trigger, and EINT works too). Function 0 = "hw_te_off" | [V `DTS/exynos/exynos9820-pinctrl.dtsi:1675,1707-1715`, `exynos9820.dts:1947-1950,1987`] |
| TE pending (SW) | NWEINT_GPD0_PEND `0x10830a14` | [V `exynos9820.dts:1989-1992`] |
| Panel reset | **GPA3[4]**, active-low | [V `beyond0lte_common.dtsi:1160-1164,2344-2349`] |
| Panel detect | GPG2[2] (0 = OK); conn-det GPM23[0] | [V `:2351-2368`] |
| Supplies | S2DOS05 display PMIC on hsi2c_44 @ 0x60: LDO4 3.0 V (ddi-3p0), LDO1 1.8 V (ddi-1p8), BUCK 1.6 V, ELVSS short-detect | [V `:2263-2274,2370-2393`]. Mainline has `ML/drivers/regulator/s2dos05-regulator.c` [V]. Not in our DT yet, so Linux leaves it alone |

**Rules:**
- Nothing may reconfigure GPD0[0] away from function 0xF. Otherwise DECON never gets a trigger and the screen freezes.
- Nothing may request GPA3[4] as an output-high/asserted line. That resets the panel.

---

## 2. Panel: S6E3FA7 "beyond0 A3 S0"

| Field | Value | Source |
|---|---|---|
| Resolution | 1080 x 2280, 60 Hz | [V `display-lcd.dtsi:270,272`] |
| Size | 62 x 132 mm (comment: 67.176 x 138.084) | [V `:270-271`] |
| h-porch (hbp, hfp, hsa) | 2, 2, 2 | [V `:273`, order per `DPU/dsim_drv.c:2058-2061`] |
| v-porch (vbp, vfp, vsa) | 15, 3, 1 | [V `:274`, `dsim_drv.c:2064-2067`] |
| drm mode | htotal 1086, vtotal 2299, clock ≈ 149 800 kHz (only DSIM uses it in command mode) | [I, computed] |
| Lanes | 4 | [V `:291`] |
| HS clock | 1143 Mbit/s/lane. PMSK P=2, M=0xB0, S=2, K=0xD89E. K is signed (−10082): 26 MHz·(176−0.1538)/(2·2²) = 571.5 MHz, ×2 = 1143 Mbit/s | [V `:275-281`, [I] formula]. Alternatives 1150/1157/1176 for adaptive FFC [V `:311`] |
| Escape clock | 20 MHz | [V `:282`] |
| DSC / MIC | off / off | [V `:283,287`] |
| cmd underrun LP ref | 1407 | [V `:292`] |
| Pixel format | RGB888, VC 0 | [V `CAL/dsim_reg.c:1717-1718`] |
| DECON → panel byte order | BGR swap at the OUTFIFO when DSI and no DSC | [V `CAL/decon_reg.c:1381-1386,1446-1450`] |
| Panel ID | bootargs `lcdtype=9502997` = 0x910115 | [V `s10e-live.dts:7092`] |

### 2.1 DCS init sequence (downstream `PANEL_INIT_SEQ`, then `DISPLAY_ON_SEQ`)

From `PNL/s6e3fa7_beyond0_a3_s0_panel.h:1062-1095` (init) and `:1196-1200` (display on). Bytes come from `:528-537,599-623,691,705-708,869-874`.

| # | Packet | Bytes | Needed after S-Boot? |
|---|---|---|---|
| 0 | (reset GPIO, regulators on) | — | **skip** (already on) |
| 1 | delay 10 ms | — | skip |
| 2 | SLEEP_OUT | `11` | skip |
| 3 | delay 20 ms | — | skip |
| 4 | KEY1 enable | `9F A5 A5` | — |
| 5 | TE_ON (VBLANK) | `35 00` | **skip**, but must stay on. Verify that TE is toggling with HW_TE_CNT |
| 6 | KEY1 disable | `9F 5A 5A` | — |
| 7 | KEY2 enable | `F0 5A 5A` | — |
| 8 | ERR_FG | `ED 4C` | skip |
| 9 | TSP_HSYNC | `B9 00 00 14 00 18 00 00 00 00 11 03 02 40 02 40` | skip |
| 10 | AVC2_ON | `F4 EB 1E 19 3A 9F 0F 09 C0` | skip |
| 11 | FFC (dynamic, 1143 Mbit/s) | `C5 09 10 C8 1D 39 0F 26 E6 24 D2 7A 77 73 00 FF 78 8B 08 00` (maptbl-patched per frequency) | skip, unless the HS clock changes |
| 12 | PARTIAL_SETTING | `C4 5B 41 74 0E 00 72 5A 72 72` (maptbl per resolution) | skip |
| 13 | KEY2 disable | `F0 A5 A5` | — |
| 14 | SET_BL_SEQ: gamma `CA …`, AOR `B1 hi lo`, TSET/MPS/ELVSS `B5 …`, VGH/VINT, ACL, then `F7 03` update | `:1144-1152` | brightness only. s10e_panel_bl already does the AOR subset (`F0 5A 5A`, `B1`, `F7 03`, `F0 A5 A5`) [V `kit/s10e-panel-bl/s10e_panel_bl.c:72-92`] |
| 15 | DISPLAY_ON | `9F A5 A5`, `29`, `9F 5A 5A` | **skip** |

In stage B, skip the whole sequence. For a cold init (Stage C), use rows 1-15 in order. The resource-read sequence (`:1097-1126`: MTP, ELVSS, date, OCTA ID) is only for gamma/dimming tables, which is not needed for a fixed AOR-based brightness.

Partial update (`2A`/`2B` column/page windows) exists downstream (`DPU/win_update.c:433`). S-Boot leaves full-frame addressing, and we never send 2A/2B, so it stays full frame [I].

---

## 3. Register-level "take over the running pipeline" (command mode, HW TE trigger)

### 3.1 What downstream does each frame

Sources: `DPU/decon_core.c:2158-2256` (`__decon_update_regs`), `:2681-2700`, and `DPU/decon_dsi.c:1215-1334` (`decon_pan_display`, the closest analogue to a flip).

1. `decon_reg_wait_update_done_and_mask()` [`CAL/decon_reg.c:1987-1998`]. This polls `SHADOW_REG_UPDATE_REQ == 0`, which means the previous request has been latched, then masks the HW trigger.
2. Program the DPP/IDMA (`dpp_reg_configure_params`) [`CAL/dpp_reg.c:1085-1128`] and the DECON window (`decon_reg_set_window_control`) [`CAL/decon_reg.c:1905-1924`].
3. `decon_reg_all_win_shadow_update_req()`: `SHADOW_REG_UPDATE_REQ |= 0x3f` [`:1896-1903`].
4. `decon_reg_start()`: `GLOBAL_CONTROL |= DECON_EN|DECON_EN_F`, `SHADOW_REG_UPDATE_REQ |= GLOBAL`, wait for RUN_STATUS, then **unmask the trigger** (`HW_TRIG_EN=1`, `HW_TRIG_MASK_DECON=0`) [`:1828-1848,1943-1961`].
5. The next TE starts the frame. FRAME_START IRQ fires, `frame_cnt++` [`DPU/decon_dsi.c:59-65`]. The shadow latches and `SHADOW_REG_UPDATE_REQ` clears.
6. Wait for vsync/vstatus, then for update done, then **mask the trigger again** [`decon_core.c:2681-2700`]. The panel then self-refreshes from its GRAM (PSR).

### 3.2 What S-Boot leaves us [I, Stage A must confirm]

simpledrm writes only to 0xca000000, and the picture updates. So DECON is running with the **trigger left unmasked**, transferring a frame from 0xca000000 on every TE (continuous command mode). If Stage A finds the trigger masked instead, use the one-shot mode in 3.4.

### 3.3 Registers and bits (DECON0 base `0x19030000`)

| Purpose | Reg (offset) | Bits | Source |
|---|---|---|---|
| Run state | `GLOBAL_CONTROL` 0x0000 | `[28]` SRESET, `[20]` 10bpc, `[8]` OP_MODE (1 = command), `[5]` IDLE_STATUS, `[4]` RUN_STATUS, `[1]` DECON_EN, `[0]` DECON_EN_F (per-frame) | `CAL/regs-decon.h:89-102`; do not write in Stage B |
| IRQ enable | `INTERRUPT_ENABLE` 0x0040 | `[13]` FRAME_DONE, `[12]` FRAME_START, `[4]` EXTRA, `[0]` INT_EN (master) | `:116-122`; set by `decon_reg_set_int` `CAL/decon_reg.c:2101-2124` |
| Extra IRQ enable | `EXTRA_INTERRUPT_ENABLE` 0x0044 | `[8]` resource conflict, `[4]` timeout | `:125-127` |
| IRQ pending (W1C) | `INTERRUPT_PENDING` 0x004C | `[13]` FRAME_DONE, `[12]` FRAME_START, `[4]` EXTRA | `:131-136`; handler `CAL/decon_reg.c:2126-2165` |
| Extra pending (W1C) | `EXTRA_INTERRUPT_PENDING` 0x0050 | `[8]`, `[4]` | `:138-140` |
| **Shadow update** | `SHADOW_REG_UPDATE_REQ` 0x0060 | `[31]` GLOBAL, `[28]` DQE, `[5:0]` WIN(n). Self-clears once latched at frame start | `:142-146`; `CAL/decon_reg.c:1755-1759,1896-1903,1926-1941,1971-1985` |
| Secure windows | `SECURE_CONTROL` 0x0064 | `[n]` TZPC_FLAG_WIN(n) | `:148-149`; read in Stage A, must be 0 for our window |
| **Trigger** | `HW_SW_TRIG_CONTROL` 0x0070 | `[28]` HS status, `[24]` TRIG_SEL (0 = DDI0 TE), `[23:16]` TRIG_SKIP, `[13]` ACTIVE_VALUE, `[12]` EDGE_POL, `[8]` SW_TRIG_EN, `[5]` MASK_SLAVE0, **`[4]` HW_TRIG_MASK_DECON**, `[3]` TIMER_CLEAR, `[2]` TIMER_EN, **`[0]` HW_TRIG_EN** | `:151-166`; `CAL/decon_reg.c:533-540,1943-1961` |
| TE counters | `HW_TE_CNT` 0x0078 | `[31:16]` CNT_B, `[15:0]` CNT_A | `:170-172` |
| Secure trigger mask | `HW_SW_TRIG_CONTROL_SECURE` 0x007C | `[4]` | `:174-177` |
| PLL sleep (command mode) | `PLL_SLEEP_CONTROL` 0x0090 | `[0]` EN_OUTIF0, `[4]` MASK_OUTIF0. Downstream enables it in command mode | `:179-183`; `CAL/decon_reg.c:1724-1752,1813-1818` |
| Byte order | `OUTFIFO_DATA_ORDER_CONTROL` 0x0130 | `[6:4]` PIXEL_ORDER_SWAP | `:248-251` |
| Blender size | `BLENDER_BG_IMAGE_SIZE_0` 0x0200 | `[29:16]` H, `[13:0]` W. Expect `(2280<<16)\|1080` | `:279-285` |
| Window enable | `DATA_PATH_CONTROL_0` 0x0214 | `WIN_EN_F(w) = 1<<(4w)`, `MAPCOLOR_EN = 1<<(4w+1)` | `:309-311`; `CAL/decon_reg.c:1563-1571` |
| Window → channel | `DATA_PATH_CONTROL_1` 0x0218 | `WIN_CHMAP_F(w,ch) = (ch&7)<<(4w)` | `:313-314`; `CAL/decon_reg.c:524-531` |
| Data path | `DATA_PATH_CONTROL_2` 0x0230 | `[7:0]` COMP_OUTIF_PATH. Expect `0x001` = NOCOMP→OUTFIFO0→DSIMIF0 | `:317-326`; `CAL/decon_cal.h:60` |
| DSIM routing | `DSIM_CONNECTION_CONTROL` 0x0250 | `[2:0]` DSIM0 source (expect 0 = DSIMIF0) | `:328-334` |
| Frame counter | `FRAME_COUNT` 0x02A0 | Should advance ~60/s if continuous | `:374` |
| Window w (0..5) | `WIN_CONTROL_0(w)` 0x1000+0x30w: `[31:24]` ALPHA1, `[23:16]` ALPHA0, `[11:8]` FUNC; `WIN_CONTROL_1` +4: blend selects; `WIN_START_POSITION` +8: `Y<<16\|X`; `WIN_END_POSITION` +0xC: `(y+h-1)<<16\|(x+w-1)`; `WIN_COLORMAP_0/1` +0x10/+0x14; `WIN_START_TIME_CONTROL` +0x18 | | `:377-421`; `DPU/decon.h:1493-1501` |

DPP/IDMA (DMA base of the channel, e.g. GF0 `0x19071000`; DPP base `0x19021000`):

| Purpose | Reg (offset) | Bits | Source |
|---|---|---|---|
| Reset / status | `IDMA_ENABLE` 0x00 | `[24]` SRESET, `[9]` SRAM CG, `[4]` SFR_UPDATE_FORCE (unused on 9820), `[2]` OP_STATUS busy, `[1]` instant-off pending | `CAL/regs-dpp.h:107-117` |
| IRQ | `IDMA_IRQ` 0x04 | `[16]` FRAMEDONE, `[17]` deadlock, `[19]` read slave error, `[21]` config error, `[23]` AFBC timeout, `[25]` AFBC conflict; masks at `[10:1]`, `[0]` enable | `:119-144` |
| **Format / AFBC** | `IDMA_IN_CON` 0x08 | `[16:11]` IMG_FORMAT (0 ARGB8888, 1 ABGR, 2 RGBA, 3 BGRA, 4 XRGB, 5 XBGR, 6 RGBX, 7 BGRX, 8 RGB565…), `[10:8]` rotation/flip, `[7]` AFBC_EN, `[6]` AFBC timeout EN, `[3]` BLOCK_EN | `:146-195`; `CAL/dpp_reg.c:133-137,158-167` |
| Plane alpha | `IDMA_OUT_CON` 0x0C | `[31:24]` frame alpha (downstream 0xFF) | `:197-199`; `CAL/dpp_reg.c:70-74,1005` |
| **Stride / full size** | `IDMA_SRC_SIZE` 0x10 | `[29:16]` f_h, `[15:0]` **f_w in pixels** (the stride) | `:201-205`; `CAL/dpp_reg.c:103-111` |
| Crop offset | `IDMA_SRC_OFFSET` 0x14 | `[28:16]` y, `[12:0]` x | `:207-211` |
| Crop size | `IDMA_IMG_SIZE` 0x18 | `[28:16]` h, `[12:0]` w | `:213-217` |
| **Base address** | `IDMA_IN_BASE_ADDR_Y` 0x40 (`_C` 0x44, `_Y2` 0x48, `_C2` 0x4C). For AFBC, Y = C = header base | `:241-244`; `CAL/dpp_reg.c:712-738` |
| DPP format | `DPP_IN_CON` 0x08 (DPP base) | `[3]` ALPHA_SEL (1 = per-pixel), `[2:0]` 0 ARGB8888, 1 ARGB8101010 | `:495-516`; `CAL/dpp_reg.c:503-514` |
| DPP size | `DPP_IMG_SIZE` 0x18 | h<<16\|w | `:517`; `CAL/dpp_reg.c:492-495` |

Notes:
- Stride is in **pixels** (`src.f_w`), so pitch must equal `f_w·cpp`. The DT limits are src_f_w 16..65534, src_w ≤ 4096 [V `exynos9820.dts:1717-1720`].
- Format map: `DECON_PIXEL_FORMAT_*` → IDMA/DPP codes [V `CAL/dpp_reg.c:762-930`]. Calibrate the DRM fourcc mapping against the IDMA_IN_CON value S-Boot left, because that value displays simpledrm's `a8r8g8b8` with correct colours.
- IDMA registers latch together with DECON's shadow update. That is why downstream never uses `IDMA_SFR_UPDATE_FORCE` on 9820 [I, from `decon_pan_display`: DPP write → all-win shadow req → start].

### 3.4 Flip-only sequence (Stage B)

```
probe:
  win  = the w with DATA_PATH_CONTROL_0 & (1<<4w)            (S-Boot's window)
  ch   = (DATA_PATH_CONTROL_1 >> 4w) & 7                       (DMA = table 1.1)
  snapshot IDMA_IN_CON/SRC_SIZE/IMG_SIZE/OFFSET, DPP_IN_CON, WIN_* (read via +0x7000 shadow too)
  INTERRUPT_PENDING = 0x3010 (W1C); INTERRUPT_ENABLE |= FRAME_START|FRAME_DONE|EXTRA|INT_EN
  mode = (HW_SW_TRIG_CONTROL & (HW_TRIG_EN|HW_TRIG_MASK_DECON)) == HW_TRIG_EN ? CONTINUOUS : ONESHOT

atomic_flush(new_fb):
  wait SHADOW_REG_UPDATE_REQ == 0                 (previous flip latched; <= 1 frame)
  IDMA_IN_BASE_ADDR_Y(ch) = paddr(new_fb)         (< 4 GiB, contiguous)
  [if pitch/format changed: IDMA_SRC_SIZE, IDMA_IN_CON fmt, DPP_IN_CON]
  SHADOW_REG_UPDATE_REQ |= WIN(win) | GLOBAL      (pan_display uses 0x3f|GLOBAL)
  if ONESHOT: HW_SW_TRIG_CONTROL: HW_TRIG_EN=1, MASK_DECON=0
  drm_crtc_arm_vblank_event()

FRAME_START irq (SPI 183):
  W1C INTERRUPT_PENDING; drm_crtc_handle_vblank()
  if SHADOW_REG_UPDATE_REQ == 0: complete flip (the old fb is no longer read after this frame starts)
  if ONESHOT and nothing pending: mask trigger again (HW_TRIG_MASK_DECON=1)   [decon_core.c:2694-2700]
FRAME_DONE irq (SPI 184): optional, for "frame really sent" / PSR idle
EXTRA irq (SPI 189): log RESOURCE_OCCUPANCY_INFO_0..2 / timeout (downstream BUG()s, we must not)
```

Never write `GLOBAL_CONTROL`, `CLOCK_CONTROL_0`, `PLL_SLEEP_CONTROL`, the DSIM link or PHY registers, `DATA_PATH_CONTROL_2`, or `DSIM_CONNECTION_CONTROL` in Stage B.

### 3.5 DSIM0 (`0x19080000`): needed for DCS only

Known to work today: `PKTHDR` 0x58, `PAYLOAD` 0x5C, `FIFOCTRL` 0x68 (`[11]` FULL_PH, `[10]` EMPTY_PH, `[9]` FULL_PL, `[8]` EMPTY_PL, `[12]` EMPTY_RX). Writes are DCS short 0x05/0x15 or long 0x39 (payload words little-endian first, then header `id | len<<8`). Commands interleave safely with running frames [V `kit/s10e-panel-bl/s10e_panel_bl.c:16-71`, `kit/tools/s10e-panel.c:20-72`, `CAL/regs-dsim.h:227-256`, `CAL/dsim_reg.c:2365-2390`].

Other registers for Stages A and C:

| Register | Offset | Notes |
|---|---|---|
| `DSIM_LINK_STATUS0..3` | 0x08-0x14 | |
| `DSIM_RESOL` | 0x3C | Expect 2280<<16\|1080 |
| `DSIM_CONFIG` | 0x4C | `[18]` VIDEO_MODE (expect 0), `[6:5]` lanes−1 (expect 3), `[4:0]` lanes EN, `[29]` PLL_SLEEP |
| `DSIM_INTSRC` / `INTMSK` | 0x50 / 0x54 | `[24]` FRAME_DONE, `[17]` RX_TE, `[19]` UNDER_RUN |
| `DSIM_RXFIFO` | 0x60 | For DCS reads (BTA) |
| `DSIM_SFR_CTRL` | 0x64 | Shadow |
| `DSIM_CMD_CONFIG` / `TE_CTRL` | 0x80 / 0x84 | |

All from `CAL/regs-dsim.h:18-308`. Downstream itself skips DSIM init when the link is already up ("already enabled in bootloader") [V `CAL/dsim_reg.c:2172-2177`]. It also skips DECON init when it is running ("decon_reg_init already called by BOOTLOADER") [V `CAL/decon_reg.c:1770-1776`]. So downstream also takes over a running pipeline.

---

## 4. Recommended mainline architecture

```
exynos_drm (ML/drivers/gpu/drm/exynos/exynos_drm_drv.c, DRIVER_NAME "exynos" :36)
 ├─ NEW exynos9820_drm_decon.c   component: CRTC = DECON0, planes = DPP channels
 │     ├ primary plane = the channel S-Boot uses (expect GF0/ch0), XRGB/ARGB8888 (+ AFBC later)
 │     ├ overlay planes = GF1, VGF, VG, VGS, VGRFS (Stage D), cursor = any G* channel
 │     ├ vblank = FRAME_START, TE-paced; IRQs per table 1.2
 │     └ register list: drv list entry ML/drivers/gpu/drm/exynos/exynos_drm_drv.c:154-186 (+3 lines)
 ├─ encoder/connector:
 │     Stage B: fixed-mode "handover" encoder+connector (mode from DT, DRM_MODE_CONNECTOR_DSI)
 │     Stage C: DSI host → drm_panel_bridge → panel-samsung-s6e3fa7 (S10e variant)
 └─ GEM: exynos_drm_gem CONTIG (dma_alloc, coherent mask 32-bit: exynos_drm_drv.c:338) → CMA < 4 GiB
```

| Piece | Reuse? | Why | Est. size |
|---|---|---|---|
| exynos_drm core, gem, fb, fbdev, plane/crtc helpers | **Reuse** | Contiguous BO path exists without an IOMMU (`exynos_drm_gem.c:38,58,344-347`). Driver name "exynos" puts Mesa kmsro → panfrost on card0 | 0 |
| `exynos5433_drm_decon.c` | Pattern only | Its I80 + HW-trigger + TE IRQ logic is the right model (`:41,118-129,558,832-841`), but the registers are VIDCON/WINCON style, not 9820 | — |
| `exynos7_drm_decon.c` | No | Exynos7 DECON is FIMD-like video mode | — |
| **9820 DECON+DPP CRTC/plane** | **New** | DPU20 layout: GLOBAL_CONTROL/WIN @0x1000 + separate IDMA/DPP blocks | B: ~700-900 LOC. Full (multi-plane, AFBC, modeset): ~1500-1800 |
| `samsung-dsim.c` bridge | Possible later | Link offsets differ (9820 CONFIG 0x4C, PKTHDR 0x58, FIFOCTRL 0x68 vs 5433's 0x18/0x3C/0x4C) [V `ML/drivers/gpu/drm/bridge/samsung-dsim.c:306-328` vs `CAL/regs-dsim.h`]. The PLL is in a separate D-PHY block, not DSIM PLLCTRL. Needs a new reg table, plat data, and a D-PHY PHY driver (M4S0/M4S4) | ~250 (dsim plat) + ~500 (phy) |
| **Minimal DCS-only DSI host** | **New (recommended for C-lite)** | `mipi_dsi_host.transfer` on PKTHDR/PAYLOAD/FIFOCTRL/RXFIFO. Assumes the link is up (as downstream does). Lets the panel driver own brightness and drop `s10e_panel_bl` | ~300-400 |
| `panel-samsung-s6e3fa7.c` | **Extend** | Add `samsung,s6e3fa7-beyond0` variant: 1080x2280 mode, command-mode flags, the init table of §2.1, AOR/`B1` brightness. **Must** use `GPIOD_ASIS` and skip reset/init when already lit. Today `devm_gpiod_get(.., GPIOD_OUT_HIGH)` asserts reset at probe (`:194`) and blanks the panel | ~200 |
| S2DOS05 regulators | Reuse | `ML/drivers/regulator/s2dos05-regulator.c` exists. Mark the rails `regulator-boot-on`/`always-on` | DT only |
| SysMMU_DPU | Later (D) | Mainline `exynos-iommu.c` ("samsung,exynos-sysmmu" `:870`). v7 support must be checked against the 9820 VERSION register | DT + maybe ~100 |
| Aperture | — | Call `aperture_remove_conflicting_devices()` for 0xca000000 to evict simpledrm; `exynos_drm_drv.c` has no aperture call today | ~5 |

DT additions, all new because `UB/exynos9820.dtsi` has no DPU nodes:
- `decon@19030000` with SPIs 183/184/189, the clocks of §1.3, `samsung,disp-sysreg = <&sysreg_dpu>`, and `pinctrl-0 = <&decon_te_on>` (gpd0-0 func 0xF).
- A `dpp` (or merged reg ranges) for the 6 DMA+DPP blocks.
- `dsi@19080000` + `panel@0` (reset-gpios `<&gpa3 4 GPIO_ACTIVE_LOW>`).
- **No** `power-domains`, **no** `iommus` until Stage D.
- Keep `framebuffer_region@ca000000` reserved. Set CMA ≥ 128 MiB: the last build config has `CMA_SIZE_MBYTES=60` [V `config-s10e-before-poco-merge:7484`]; 3 scanout buffers × 9.85 MB plus cursor and overlays fits, but it is tight. Confirm with `dmesg | grep cma` that CMA sits below 4 GiB [I: arm64 places default CMA in ZONE_DMA32].

Total for Stages B+C: **~1300-1600 LOC new kernel code**, plus DT and one Mesa-free userspace change (drop `WLR_RENDER_DRM_DEVICE`: kmsro does it).

Why kmsro rather than today's `WLR_RENDER_DRM_DEVICE` [I]:
- Panfrost BOs are shmem, so non-contiguous.
- A drm_gem_dma or exynos CONTIG importer rejects non-contiguous sg tables while there is no IOMMU.
- With kmsro, the scanout buffer is a CMA dumb buffer from the KMS side, and panfrost imports it. That is zero-copy.
- After Stage D (SysMMU on), any dma-buf can be scanned out, client direct scanout included.

---

## 5. Staged plan

| Stage | Goal | Work | Risk | Recovery |
|---|---|---|---|---|
| **A** read-only dump | Prove §3.2 and §1.5 | Add `kit/tools/s10e-dpu-dump.c` (same /dev/mem mmap as `s10e-panel.c`). Dump DECON 0x19030000 0x0-0x2A4 and 0x1000-0x1120, plus the shadow at +0x7000. Dump all 6 DMA blocks 0x0-0x70 and 0x870, DPP 0x0-0x30, DMA_COMMON, DSIM 0x0-0x100, DISP_SS 0x19011000-0x1f, SysMMU D0-D2 0x0-0x40 and 0x870-0x874, PMU 0x15861e00-0x1f (pd_dpu status). Read FRAME_COUNT and HW_TE_CNT twice, 1 s apart | **Very low**. All reads hit clocked, powered blocks (gates IGNORE_UNUSED, PD on). A read of a gated block would bus-hang, so do not read DECON1 (0x19040000) or anything else outside this list | Reboot |
| A: expected values | — | GLOBAL_CONTROL: bit8=1, bit4=1, bits1:0=11. HW_SW_TRIG_CONTROL: bit0=1, bit4=0. One WIN_EN bit set. Its IDMA_IN_BASE_ADDR_Y = 0xCA000000, SRC_SIZE = 0x08E80438, IMG_SIZE = 0x08E80438. DATA_PATH_CONTROL_2[7:0] = 0x01. BLENDER = 0x08E80438. DSIM_RESOL = 0x08E80438, DSIM_CONFIG bit18=0, lanes=3. SysMMU CTRL bit0=0. FRAME_COUNT +~60/s. SECURE_CONTROL = 0 | Any mismatch changes the Stage B design: trigger masked → ONESHOT mode; SysMMU on → stop and plan the IOMMU first | — |
| **B** flip-only KMS | Atomic page flip, TE-paced vblank, zero-copy via kmsro | New exynos9820 DECON component (one CRTC, one primary plane on S-Boot's channel, fixed 1080x2280 mode from DT, handover connector). Flip per §3.4. CONTIG GEM. Evict simpledrm. First test: modetest/kmscube flipping between 0xca000000 and one CMA buffer. Then phoc without `WLR_RENDER_DRM_DEVICE` | **Low-medium**. Worst cases: wrong address/format → garbage or a DECON timeout (screen frozen, SoC alive, ssh over Wi-Fi works). An address ≥ 4 GiB → truncated read of random RAM (no write, so no corruption). MIF too low under GPU load → DSI underrun glitches (§5 D). Probe must **not** touch clocks, PD, TE pin, reset GPIO or DSIM | ssh + reboot. Boot without the module → simpledrm path is unchanged |
| **C** DSI + panel | Brightness and DPMS through DRM, then cold modeset | C1: DCS-only DSI host + S10e panel variant (handover-safe probe, `B1` AOR backlight). Removes `s10e_panel_bl`. C2: display off/on (`28`/`10` … `11`/`29` per §2.1, DSIM ULPS) **without** cutting DPU power. C3 (optional): full DSIM + D-PHY init from scratch (`CAL/dsim_reg.c:1657-1791,2165-2238`, PMS from §2) so the panel can be fully power-cycled | C1 **low**. C2 **medium**: a wrong sleep-out timing leaves the panel dark until reboot, nothing worse. C3 **high**: re-powering the D-PHY needs PMU isolation 0x0710/0x070c, and a pd_dpu power-cycle needs the TZPC SMC. Stay out of it until a PD driver can make the SMC | Reboot. S-Boot re-inits the panel |
| **D** planes, AFBC, DVFS, IOMMU | Bandwidth and power | D1: overlay and cursor planes on GF1/VGF/VG/VGS. D2: AFBC on GF0/GF1/VGF/VGRFS (header base in both Y and C, `IDMA_IN_CON[7]`, `IDMA_RECOVERY_CTRL`, `DPU_DMA_RECOVERY_NUM` = aclk in kHz [V `CAL/dpp_reg.c:158-167`, `DPU/decon_dsi.c:1308-1311`]); modifier list per §6. D3: SysMMU_DPUD0-2 via exynos-iommu (drops the < 4 GiB and contiguity limits). D4: PSR (one-shot trigger, panel GRAM self-refresh) and DVFS (ACPM DISP/INT/MIF, downstream BTS `DPU/bts.c:812-814`). D5: DSC is **not applicable** to this panel | D1/D2 **medium**: AFBC timeout/conflict IRQs; downstream has a recovery path. D3 **medium-high**: a fault means a frozen display. D4 **medium**: undervolting the bus → underruns | Per-feature module params to disable |

---

## 6. DSC and AFBC: bandwidth

| | Finding |
|---|---|
| DSC | **Not used.** `dsc_en = <0>`, `dsc_cnt`/`slice` fields are ignored [V `display-lcd.dtsi:287-290`]. Data path is NOCOMP (`DPATH_NOCOMP_OUTFIFO0_DSIMIF0`, chosen when `!dsc_enabled` [V `CAL/decon_reg.c:1402-1414`]). The panel init never sends PPS [V]. No DSC encoder work needed. The DSI link (4×1143 Mbit/s = 4.57 Gbit/s) carries 3.55 Gbit/s of RGB888 |
| Scanout read bandwidth | Linear XRGB8888: 1080·2280·4·60 = **591 MB/s** per full-screen plane [I, computed]. Trivial for LPDDR4X, but compositing it with the GPU's own traffic is what MIF DVFS must cover |
| AFBC decode | **Available** on GF0 (ch0, the likely primary), GF1, VGF, VGRFS [V attr bit 0]. Not with rotation, block mode or YUV420 [V `DPU/dpp_drv.c:389-404`]. Mali-G76 (panfrost) renders AFBC. If the modifiers match, a mostly flat UI drops to roughly 30-50 % of the linear bandwidth [I] |
| AFBC modifier | Not determinable from kernel source: the downstream fbdev driver only passes `is_comp` [V `CAL/dpp_reg.c:716-719`]. Start by advertising `DRM_FORMAT_MOD_ARM_AFBC(BLOCK_SIZE_16x16 \| YTR \| SPARSE)` (what panfrost emits for RGBA8 scanout) and test. Watch `IDMA_IRQ[23]` (AFBC timeout) and `[25]` (conflict) [I] |

---

## 7. Stage A result (2026-10-29, kit/display/stage-a-*.txt, dpu-dump.py)

**A first run that also read PMU pd_dpu 0x15861e00 / DISP_SS 0x19011000 / SysMMU D0-D2 RESET THE SoC**
(INFORM3 armed, came back via U-Boot; no output got out, so the culprit is one of those three, most
likely DISP_SS = SYSREG_DPU, the same class as SYSREG_AUD). Do not read them. DECON0 (+shadow), all
DMA/DPP blocks, DMA_COMMON and DSIM0 read safely.

- DECON0 GLOBAL_CONTROL 0x113 (command, RUN, EN|EN_F). HW_SW_TRIG_CONTROL 0x1001 = HW_TRIG_EN, not masked
  -> **CONTINUOUS**. FRAME_COUNT and HW_TE_CNT both +59/s. INTERRUPT_ENABLE 0x3011 already (FRAME_DONE|
  FRAME_START|EXTRA|INT_EN), PENDING 0x3000. SHADOW_REG_UPDATE_REQ 0x60 = 0. 0x64 = 0x3f (shadow copy 0x20):
  meaning unclear, the window is clearly not secure-blocked.
- **Window 5** (DATA_PATH_CONTROL_0 0x00100000), CHMAP 0 = **GF0**. WIN5 CONTROL_0 0x102, END 0x08e70437.
  BLENDER 0x08e80438, DATA_PATH_CONTROL_2 = 1.
- GF0 DMA: IDMA_ENABLE 0x4 (busy), IN_CON 0x02000000 (format 0 = ARGB8888 shows DRM ARGB8888 correctly;
  bit 25 unknown), OUT_CON 0xff000000, SRC_SIZE = IMG_SIZE = 0x08e80438, **BASE_Y = 0xca000000** (so DPU DMA
  sees PA: SysMMU bypass or 1:1). DPP_GF0 IN_CON 0, IMG_SIZE 0x08e80438. Other channels idle.
- DSIM0 CONFIG 0x04807cff (command, 4 lanes), RESOL 0x08e80438.

## 8. SysMMU_DPUD0 (2026-10-29, live)
- Readable in the S-Boot state: CTRL 0x00 = 4 (bypass), CFG 0x04 = 0x00020000, VERSION 0x34 = 0x70600000 (v7.3).
  A read of 0x30 (pre-v7 CAPA, a hole on v7) RESETS the SoC; avoid holes. CMU_DPU gates 0x20xx read 0x00200000,
  QCH_CON_SYSMMU_DPUD0..2 (0x3050..) = 0x003f0002, so the clock runs with nothing enabled by Linux.
- Mainline exynos-iommu, v7 non-VM layout, version from DT. S-Boot's buffer is mapped 1:1 via reserved-memory
  iommu-addresses; translation is switched on by the decon's first runtime resume. Afterwards CTRL = 5,
  CFG = 0x784, GF0 scans out IOVAs (0xfe000000...).
