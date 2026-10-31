# S10e (SM-G970F, beyond0lte, Exynos 9820) cameras on mainline 7.2.13: bring-up spec

Date: 2026-10-29. This is research and design only. Nothing was built, no source tree was changed, and the phone
was not touched.

Tags: **[C]** = read in the source, with file:line given. **[I]** = inferred. Treat every [I] as something to
verify on the device.

Path abbreviations:

| Abbrev | Path |
|---|---|
| `LK/` | `~/S10e/src/lineage-kernel` (stock Samsung 4.14) |
| `FI/` | `LK/drivers/media/platform/exynos/fimc-is2` (the built camera stack, see 0.1) |
| `MF/` | `FI/sensor/module_framework` (`cis/` = sensor drivers, `modules/` = module glue) |
| `CSI2/` | `FI/sensor/csi2` |
| `V7/` | `FI/ischain/fimc-is-v7_0_0` |
| `SDT/` | `LK/arch/arm64/boot/dts/samsung` (`CAM` = `SDT/exynos9820-beyond0lte_camera.dtsi`, `COM` = `SDT/exynos9820-beyond0lte_common.dtsi`) |
| `XDT/` | `LK/arch/arm64/boot/dts/exynos` (`SOC` = `XDT/exynos9820.dts`, `SCAM` = `XDT/exynos9820-camera.dtsi`) |
| `CAL/` | `LK/drivers/soc/samsung/cal-if/exynos9820` |
| `ML/` | `~/S10e/src/linux-9820-7.2.13` |
| `UDT/` | `~/S10e/src/u-boot/dts/upstream/src/arm64/exynos` (the live board DT) |

---

## 0. Summary

- **The plan works on this SoC, and it is simpler than the Poco's.** The 9820 has a write DMA ("CSISX4_PDP_DMA",
  the "camif-wdma") right behind each CSIS link. It writes VC0..VC3 of a CSIS straight to memory. It does not go
  through 3AA/ISP, needs no Samsung ISP firmware, and needs no secure call. Path: sensor → D-PHY (inside the DPU
  block) → CSIS v5.2 → WDMA → DRAM.
- **There is no C-PHY.** The 9820 IS config hard-codes `USE_SENSOR_IF_DPHY` (`V7/fimc-is-config.h:99`) [C]. All
  three sensors are 4-lane D-PHY RAW10 with a 26 MHz MCLK.
- **The WDMA block has its own test pattern generator** (`samsung,has_test_pattern_gen`, `SCAM:210`;
  `CSI2/fimc-is-hw-csi-v5_2.c:666-733`) [C]. "RAW frame in memory" can be proven before any sensor works. On the
  Poco the TPG was the tool that split "receiver broken" from "sensor/PHY broken".
- **Power domain: one simple domain plus the TZPC SMC that mainline already has.** `pd_isppre` (PMU 0x15862500)
  holds CSIS, WDMA, SYSREG_ISPPRE and SysMMU_ISPPRE. Its on/off is one write plus a poll (no G3D-style sequence).
  It needs the TZPC restore SMC (`need_smc = <0x17010204>`), and mainline's `samsung,tzpc-base` already
  implements that call. The D-PHY sits in the DPU block, which is always on here because the display runs.
- **What mainline lacks:**
  - drivers: S2MPB02 and S2MPB03 regulators; CMU_ISPPRE; a CSIS v5.2 + WDMA capture driver; a D-PHY config
    driver; three sensor drivers;
  - DT nodes: I2C_CAM0..3, USI10 and USI17_USI, CMGP I2C1, SysMMU_ISPPRE.
- **Recommended first sensor: S5K3P9 (ultrawide).** All its rails are on S2MPB02. It has no PDAF virtual channel,
  no AF and no OIS, and its TnP is a 280-word burst. The 2L4 is the hardest (2009-tuple TnP, PDAF VC1, a second
  PMIC, the OIS MCU) and should come last.

### 0.1 Which camera source is built

- `LK/drivers/media/platform/Makefile:99-103` builds `exynos/fimc-is2/` unless `CONFIG_VIDEO_SAMSUNG_F62=y`. The
  same switch selects the Kconfig at `LK/drivers/media/platform/Kconfig:37-49` [C].
- `exynos9820-beyond0lte_defconfig` does not set `VIDEO_SAMSUNG_F62`. Only `exynos9820-f62_defconfig:3081` does
  [C]. **beyond0lte builds `fimc-is2`.**
- The beyond0lte defconfig (lines 3092-3241) gives [C]:
  - `FIMC_IS_V7_0_0` and `EXYNOS_DEVICE_MIPI_CSIS_VER4`, so `FI/fimc-is-device-csi_v4.c` is the link driver;
  - `CSIS_V5_2`, so `CSI2/fimc-is-hw-csi-v5_2.c` is the register layer;
  - `USE_CSI_DMAOUT_FEATURE`;
  - `CAMERA_CIS_2L4/3P9/3J1_OBJ`, `CAMERA_ACT_AK737X_OBJ` (and `ZC569_OBJ`, which is unused on this board) and
    `CAMERA_FLASH_S2MPB02_OBJ`;
  - `OIS_USE`, `CAMERA_USE_MCU`, `SENSORCORE_MCU_CONTROL`, `SENSOR_RETENTION_USE`, `CAMERA_BSS_V00`,
    `SECURE_CAMERA_USE` and `USE_SENSOR_GROUP`.
- Board revision: the phone is "EUR OPEN 25", so `SDT/exynos9820-beyond0lte_eur_open_25.dts` →
  `COM` → `CAM` applies. Revs 17-24 move the 3P9 to hsi2c_2 (`SDT/…eur_open_24.dts:75-82,149-160`). That
  does not apply here [C].

---

## 1. Board wiring per camera

### 1.1 Overview

| | Rear wide **S5K2L4** (12 MP, dual pixel) | Rear ultrawide **S5K3P9** (16 MP) | Front **S5K3J1** (10 MP) |
|---|---|---|---|
| Stock DT node | `CAM:19-66`, I2C node `CAM:886-892` | `CAM:69-108`, `CAM:935-942` | `CAM:120-154`, `CAM:995-1001` |
| I2C bus | **hsi2c_0** = I2C_CAM0 @**0x10860000**, GIC 410, 1 MHz FM+ (`CAM:872-893`, `SOC:2518-2530`) | **hsi2c_1** = I2C_CAM1 @**0x10870000**, GIC 411, 1 MHz (`CAM:921-942`, `SOC:2538-2550`) | **hsi2c_2** = I2C_CAM2 @**0x10880000**, GIC 412, 1 MHz (`CAM:981-1001`, `SOC:2558-2570`) |
| I2C pins | gpc0-0/1, func 2 | gpc0-2/3, func 2 | gpc0-4/5, func 2 (`XDT/exynos9820-pinctrl.dtsi` `hsi2cN_bus`; bank gpc0 = pinctrl_peric1, `UDT/exynos9820-pinctrl.dtsi:597`) |
| 7-bit address | **0x10** | **0x2D** | **0x10** |
| Reset GPIO | **gpg2-6** (`CAM:39`) | **gpg0-3** (`CAM:90`) | **gpg2-5** (`CAM:139`) |
| Other GPIO | OIS/MCU reset gpg0-6, MCU BOOT0 gpm29-0 (`CAM:40,912-913`) | – | – |
| MCLK | ch 0: CMU_TOP CIS_CLK0, pin **gpc1-4** func 2 (`CAM:24-25,30,38`) | ch 3: CIS_CLK3, pin **gpc1-7** func 2 (`CAM:74-75,80,89`) | ch 1: CIS_CLK1, pin **gpc1-5** func 2 (`CAM:124-125,131,138`) |
| MCLK rate | 26 MHz (`V7/setup-fimc-is-sensor.c:160-162`; every setfile writes 0x0136=0x1A00) [C] | 26 MHz | 26 MHz |
| CSIS | **CSIS0** @0x17030000, GIC 309 (`CAM:1171-1182`, `SCAM:342-423`) | **CSIS1** @0x17040000, GIC 310 (`CAM:1220-1229`, `SCAM:658-722`) | **CSIS2** @0x17050000, GIC 311 (`CAM:1184-1195`, `SCAM:425-506`) |
| WDMA (no-PAF mode) | DMA0 @0x170A0000, GIC 315 | DMA1 @0x170B0000, GIC 316 | DMA2 @0x170C0000, GIC 317 |
| PHY | M4S4 "top", 0x19160B00, PMU iso 0x70C, reset bit 0. A D/C combo PHY ("DCPHY 6.5 Gbps"), used as D-PHY (`SCAM:282-292`) | M0S4S4S4 "mod" 2nd S4, 0x19162B00, iso 0x730, reset bit 2, "DPHY 2.5 Gbps" (`SCAM:306-316`) | M0S4S4S4 1st S4, 0x19162300, iso 0x730, reset bit 1, "DCPHY 6.5 Gbps" (`SCAM:294-304`) |
| Lanes | 4 | 4 | 4 |
| Link rate per lane | 2054 Mbps (mode 1: 2177, binned 5-7: 1352) | 2054 Mbps | 1807 Mbps (modes 19/20: 1872) |
| Settle (from table) | 10 @2054, 11 @2177, 67 @1352 | 10 | 9 |
| PDAF | yes, VC1 (PD_MOD3) | none | yes, VC1 (PD_MOD3) |
| AF | AK7374 @0x0C on hsi2c_3 | none | AK737x @0x0C on hsi2c_21 |
| EEPROM | @0x50 on hsi2c_3, 64 KiB | @0x51 on hsi2c_1, 16 KiB | @0x51 on hsi2c_21, 16 KiB |
| OIS | STM32 MCU @0x51 on hsi2c_37 | – | – |

Notes:
- The settle values come from `FI/include/fimc-is-hw-settle-8nm-lpp.h` (included at `V7/fimc-is-hw-chain.c:28`;
  entries 2060→10 at line 458, 2180→11 at 446, 1810→9 at 483, 1360→67 at 528). The lookup aligns the speed up to a
  multiple of 10 (`V7/fimc-is-hw-chain.c:1844-1877`). Below `PHY_REF_SPEED` = 1500 Mbps the PHY counts settle in a
  different clock, which is why 1352 Mbps gets 67 (`LK/drivers/phy/samsung/phy-exynos-mipi.c:269,312-314`) [C].
- The `sensor_i2c_ch` values in the module nodes (0/4/4) are internal framework channel ids. The DT parent bus is
  what counts [C/I].
- The 3P9 cfg says 2054 Mbps, and that matches the OP PLL. The setfile comment "1365 Mbps" is stale
  (`MF/cis/fimc-is-cis-3p9-setA.h:351-355`) [I].

### 1.2 Peripheral buses (AF, EEPROM, OIS, PMICs)

| Bus | Controller | Devices | Cite |
|---|---|---|---|
| hsi2c_3 | I2C_CAM3 @0x10890000, GIC 413, 400 kHz, gpc0-6/7 | AK7374 AF 0x0C, rear EEPROM 0x50 | `CAM:1005-1097`, `SOC:2578-2590` |
| hsi2c_21 | USI08_I2C @0x108F0000, GIC 419, 400 kHz, gpp5-2/3 func 3 | front AF 0x0C, front EEPROM 0x51 | `CAM:1100-1169`, `SOC:2938-2950` |
| hsi2c_37 | USI17_USI @0x10990000, GIC 427, 400 kHz, gph0-4/5. USI mode set by `usi_17` = sysreg_peric1+0x104C (`COM:1257-1260`, `SOC:2382-2386`) | OIS/aperture MCU 0x51 | `CAM:895-918`, `SOC:3258-3270` |
| hsi2c_24 | USI10_USI @0x10920000, GIC 422, gpp6-0/1 func 2 | **S2MPB02** 0x59 | `COM:1723-1729`, `SOC:2998-3008` |
| hsi2c_42 | I2C_CMGP1 @0x15D30000, GIC 79, gpm6-0/gpm7-0 func 3 | **S2MPB03** 0x56 | `COM:2207-2213`, `SOC:3358-3368` |

**Mainline state:** none of these I2C controllers is in `UDT/exynos9820.dtsi`. It has only `hsi2c10`,
`hsi2c_38` (USI17_I2C, the audio amps) and `hsi2c45` (`UDT/exynos9820.dtsi:840,978,1463`) [C]. Clocks:
- CMU_PERIC1 in `ML/drivers/clk/samsung/clk-exynos9820.c:783-880` has only UART_BT, USI16_USI, USI_I2C and USI17_I2C.
- It **lacks** the gates for I2C_CAM0..3, USI08_I2C, USI10_USI and USI17_USI.
- The stock offsets for them are in `CAL/cmucal-sfr.c`:
  - user muxes: I2C_CAM0..3 at 0x120/0x140/0x160/0x180 (`:325-332`), USI10 at 0x260 (`:319`), USI17_USI at 0x2C0
    (`:335`);
  - dividers at 0x1800/0x1804/0x1808/0x180C, USI10 0x1828, USI17_USI 0x1834 (`:487-496`);
  - IPCLK/PCLK gates: I2C_CAM0 0x2034/0x2038, CAM1 0x203C/0x2040, CAM2 0x2044/0x2048, CAM3 0x204C/0x2050,
    USI08_I2C 0x20C0/0x20C4, USI10 0x20E8/0x20EC, USI17_USI 0x2118/0x211C (`:1720-1783`).
- CMU_CMGP already has I2C_CMGP1 (`ML/…/clk-exynos9820.c:1814,1837`) [C].

USI wrappers: the stock `usi_i2c_cam_0..3` are sysreg_peric1 + 0x1008/0x100C/0x1010/0x1014
(`SOC:2462-2492`), `usi_8_i2c` is +0x102C, and `usi_10` is +0x1038 [C]. For the mainline `samsung,exynos850-usi` nodes,
by analogy with `usi17_i2c` (`reg = <0x109a00c0 0x20>`, `samsung,sysreg = <&sysreg_peric1 0x1050>`,
`UDT/exynos9820.dtsi:962-990`), use `<controller base + 0xc0>` with these offsets [I].

---

## 2. Power

### 2.1 Camera PMICs

**S2MPB02** @0x59 on hsi2c_24 (`COM:1723-1830`) [C]
- Registers: ID 0x00, FLED_CTRL1 0x12, FLED_CTRL2 0x13, FLED_CUR1 0x14, FLED_TIME1 0x15, L1CTRL..L18CTRL
  0x1E..0x2F (`LK/include/linux/mfd/samsung/s2mpb02-regulator.h:26-77`).
- LDO enable is bit 7. VSEL mask is 0x7F, min 600 mV, step 12.5 mV on LDO1-5 and 25 mV on LDO6 and up
  (`:109-113`, `LK/drivers/regulator/s2mpb02-regulator.c:226-266`).
- The bucks and buck-boost have their own registers: take them from the same enum. BUCK2 = `VDD_EXT_1P3_PB02`.

**S2MPB03** @0x56 on hsi2c_42 (`COM:2207-2260`) [C]
- Registers: 0x00 PMIC_ID, 0x01 STATUS, 0x02 CTRL, 0x03..0x09 = LDO1..7_CTRL
  (`LK/include/linux/regulator/s2mpb03.h:57-82`).
- Enable is bit 7 (0x80), VSEL mask 0x3F (`:89-90`).
- LDO1/2/4: min 700 mV, step 12.5 mV. LDO3: 700 mV, step 25 mV. LDO5-7: 1.8 V, step 25 mV
  (`LK/drivers/regulator/s2mpb03.c:266-288`, `s2mpb03.h:85-88`).
- Enable time is 150 µs (`s2mpb03.h:94`).

Both are trivial to write as mainline `regmap` regulator drivers: one `regulator_desc` table each, no IRQs needed
(about 150-250 lines each) [I].

| Rail (stock name) | PMIC / LDO | V | Used by | Cite |
|---|---|---|---|---|
| VDDD_RET_1.0V_CAM | S2MPB03 LDO4 | 1.025 V (0.7 V in retention) | 2L4 core retention | `COM:2236-2240` |
| VDDD_1.0V_CAM | S2MPB03 LDO1 | 1.05 V | 2L4 digital | `COM:2218-2222` |
| VDDQ_1.1V_CAM | S2MPB03 LDO2 | 1.1 V | 2L4 MIPI | `COM:2224-2228` |
| VDDIO_1.8V_CAM | S2MPB03 LDO3 | 1.8 V | 2L4 IO, rear AF IO | `COM:2230-2234` |
| VDDA_2.4V_CAM | S2MPB03 LDO5 | 2.4 V | 2L4 analog | `COM:2242-2246` |
| VDDA_2.8V_CAM | S2MPB03 LDO6 | 2.8 V | 2L4 analog | `COM:2248-2252` |
| VDD_VM_2.8V_OIS | S2MPB03 LDO7 | 2.8 V | OIS motor | `COM:2254-2258` |
| VDDAF_COMMON_CAM | S2MPB02 LDO14 | 3.3 V | rear AF | `COM:1800-1804` |
| VDDD_1.8V_OIS | S2MPB02 LDO10 | 1.8 V | OIS MCU (and probably the rear EEPROM [I]) | `COM:1788-1792` |
| VDD_EXT_1P3_PB02 | S2MPB02 BUCK2 | 1.3 V | 3P9, 3J1 (input rail of the 1.05 V LDOs [I]) | `COM:1744-1748` |
| VDDA_2.8V_WIDESUB | S2MPB02 LDO15 | 2.8 V | 3P9 analog | `COM:1806-1810` |
| VDDD_1.05V_WIDESUB | S2MPB02 LDO4 | 1.05 V | 3P9 digital | `COM:1758-1762` |
| VDDIO_1.8V_WIDESUB | S2MPB02 LDO8 | 1.8 V | 3P9 IO | `COM:1776-1780` |
| VDDAF_2.8V_VT | S2MPB02 LDO16 | 2.8 V | front AF | `COM:1812-1816` |
| VDDA_2.95V_VT | S2MPB02 LDO13 | 2.95 V | 3J1 analog | `COM:1794-1798` |
| VDDIO_1.8V_VT | S2MPB02 LDO6 | 1.8 V | 3J1 IO | `COM:1770-1774` |
| VDDD_1.05V_VT | S2MPB02 LDO5 | 1.05 V | 3J1 digital | `COM:1764-1768` |

S2MPB02 also carries rails that other drivers need: LDO17 VDD_GRIP_3P3 (always-on) and LDO9 (rear ALS), see
`kit/sensors/SENSORS-INVENTORY.md`. A mainline S2MPB02 driver must keep LDO17 on and must not turn off rails
it does not know about.

### 2.2 Power-on sequences (stock module glue)

Format: step (delay after it, µs). The delay is the last `SET_PIN` argument, applied with `usleep_range`
(`FI/ischain/setup-fimc-is-module.c:86-229`) [C].

**S5K2L4** (`MF/modules/fimc-is-device-module-2l4.c:272-307`, `power_seq_id = 1` from `CAM:31`) [C]
1. reset gpg2-6 low
2. VDDD_RET_1.0V_CAM set to 1.025 V and on (1)
3. VDDA_2.8V_CAM (0)
4. VDDA_2.4V_CAM (1)
5. VDDD_1.0V_CAM (1)
6. VDDAF_COMMON_CAM (1)
7. VDDIO_1.8V_CAM (1)
8. VDDQ_1.1V_CAM (1000)
9. VDD_VM_2.8V_OIS (100), then VDDD_1.8V_OIS (0). This is the OIS part.
10. I2C pins to function, MCLK pin to func 2, MCLK on (1500)
11. reset high (3000)
12. OIS reset gpg0-6 high (20000)

- The `HW_INIT` scenario (`:311-324`) is the same sequence without steps 9 and 12. It shows that the sensor runs
  with the OIS MCU unpowered.
- Power-off is at `:328-365`; the 1.0 V retention variant is at `:369-406`.

**S5K3P9** (`MF/modules/fimc-is-device-module-3p9.c:216-234`, `power_seq_id` absent = 0) [C]
1. VDD_EXT_1P3_PB02 (`USE_BUCK2_REGULATOR_CONTROL`, `FI/vendor/mcd/fimc-is-vendor-config_bss_v00.h:80`)
2. reset gpg0-3 low
3. VDDA_2.8V_WIDESUB
4. VDDD_1.05V_WIDESUB
5. VDDIO_1.8V_WIDESUB
6. I2C on (10)
7. reset high
8. MCLK pin func 2
9. MCLK on (1100)

Note that the reset is released **before** MCLK starts. Power-off is at `:240-257`.

**S5K3J1** (`MF/modules/fimc-is-device-module-3j1.c:266-292`, `power_seq_id` = 0) [C]
1. VDD_EXT_1P3_PB02
2. VDDAF_2.8V_VT
3. VDDA_2.95V_VT (only when `power_seq_id == 0`, which is the beyond0 case, `:278-280`)
4. VDDIO_1.8V_VT
5. VDDD_1.05V_VT (1000)
6. I2C on (10)
7. reset gpg2-5 high
8. MCLK pin func 2
9. MCLK on (9000)

Power-off is at `:300-322`.

For the mainline drivers, use the order exactly as above, with MCLK enabled through `clk_prepare_enable`. The
pinctrl "default" state should put gpc1-N in func 2 (the stock "pin0/pin1/pin2" states are `CAM:22-26`;
`sensor_mclkN_fn` = func 2, `XDT/exynos9820-pinctrl.dtsi:1938-1962`). The SMIA-style sensors tolerate generous
delays [I], so round all sub-ms delays up to 1 ms in the first version.

### 2.3 MCLK clock path

- Stock: `fimc_is_set_rate(dev, "CIS_CLKn", 26 MHz)` and then enable "MUX_CIS_CLKn"
  (`V7/setup-fimc-is-sensor.c:152-168`) [C].
  - "CIS_CLKn" is the CMU_TOP divider `CLKCMU_CIS_CLKn`.
  - "MUX_CIS_CLKn" is `GATE_DFTMUX_TOP_CIS_CLKn`, a dummy Q-channel `DMYQCH_CON_DFTMUX_TOP_QCH_CIS_CLKn` at CMU_TOP
    0x1A240000 + 0x3008 + 4n, flagged `VCLK_QCH_DIS` (`LK/drivers/clk/samsung/clk-exynos9820.c:115-119,465`;
    `CAL/cmucal-sfr.c:2044-2048`) [C].
- The CMU_TOP mux has parents {OSCCLK, PLL_SHARED2_DIV2} (`CAL/cmucal-node.c:356-359`) [C]. OSCCLK is 26 MHz.
- **Mainline has the CMU_TOP part already.** It has `mout_cmu_cis_clkN` {oscclk, dout_pll_shared2_div2},
  `gout_cmu_cis_clkN` and `dout_cmu_cis_clkN` (5-bit divider) (`ML/drivers/clk/samsung/clk-exynos9820.c:3373-3377,
  3528-3537, 3652-3661, 3784-3793`) [C].
- Recommendation: in DT, use `assigned-clock-parents = <oscclk>` on `CLK_MOUT_CMU_CIS_CLKn`, with divider 1, so
  the rate is exactly 26 MHz [I].
- Mainline does not touch the `DMYQCH` register. If the MCLK pad stays dead, look at CMU_TOP 0x3008+4n first. Do
  a read-only dump: CMU_TOP is always clocked [I].

---

## 3. Sensors: registers, tables, modes

Common facts [C]:
- **Table format.** Every table entry is a `{addr, val, width}` u32 triple (`FI/sensor/fimc-is-helper-i2c.h:27-36`).
  `sensor_cis_set_registers()` (`MF/cis/fimc-is-cis.c:58-136`) handles it like this:
  - width 1 → 8-bit write, width 2 → 16-bit write;
  - `I2C_MODE_DELAY` (0xF0000003) → `usleep(val)`;
  - `I2C_MODE_BURST_ADDR` (0xF0000001) with val 0x6F12, followed by N × `I2C_MODE_BURST_DATA` → one I2C transaction
    to 0x6F12 with N 16-bit words;
  - a fixed 3 ms sleep before every table (`:87`).
- **Indirect access.** 0x6028 = address high (0x2000/0x2001), 0x602A = address low, 0x6F12 = auto-increment data
  port. 0xFCFC = page base (0x4000 = SMIA register page).
- **Revision.** DT `rev_reg = <0x0002 2 valid…>` gives a 16-bit read at 0x0002 (`MF/cis/fimc-is-cis.c:243-296,
  459-467`). It only logs: a mismatch is fatal only with `USE_CAMERA_CHECK_SENSOR_REV` (factory builds).
- **Chip ID.** 0x0000 (16-bit) is read only for logging (`2l4.c:613`, `3p9.c:320`, `3j1.c:320`). **No expected ID
  value exists anywhere in the tree.**
  - [I] SMIA convention: 0x0000 should read 0x20C4 (2L4), 0x3109 (3P9) and 0x30E1 (3J1). These values are not
    verified.
  - Milestone 1 should log 0x0000..0x0003 and take the values as found. 0x0002 must match the DT revisions:
    0xA002/0xA102 (2L4), 0xA101 (3P9, 3J1).
- **Format.** Every mode is RAW10, 4 lanes (`0x0114=0x0300`). Every driver reports `OTF_INPUT_ORDER_BAYER_GR_BG`
  (`2l4.c:3822`, `3p9.c:1980`, `3j1.c:2088`).
  - The 2L4 stream-on also writes 0x0101 = 0x03 (H+V flip). Determine the effective order on the device with
    the soft ISP [I].
- **Group hold.** The address is 0x0104 on all three. The stock drivers compile it out (`USE_GROUP_PARAM_HOLD 0`),
  so mainline can use it or not.
- **Mainline templates.** `ML/drivers/media/i2c/s5kjn1.c` and `s5k3m5.c` are recent Samsung sensor drivers built
  on v4l2-cci. Use `cci_multi_reg_write()`, **never `regmap_multi_reg_write()`**: on a CCI regmap the latter drops
  the CCI_REG16 width, and that exact bug cost a day on the Poco (memory `poco-camera-bringup`). The 0x6F12
  bursts need a raw `i2c_transfer`: the 2L4 TnP has a 1996-word burst, so chunk it to about 256 words per message
  [I] (the controller's max transfer is not checked).

### 3.1 S5K2L4 (rear wide)

- **Setfile.** setA is the only one used: `USE_FLIP_WIDE_SENSOR` is defined only in `dns_v02.h:38`, and this board
  uses bss_v00 (`FI/vendor/mcd/fimc-is-vendor-config.h:21-22`).
  - Version string: "SAK2L4SX_EVT0.1_Ver_0.24_20190520" (`MF/cis/fimc-is-cis-2l4-setA.h:18`).
  - Rev 0xA002 is "3stack" and 0xA102 is "3stack SHBN". Both get setA (`2l4.c:383-457`).
  - Rev ≥ 0xA002 enables the low-noise mode handling (`2l4.c:904-925`).
- **Cold init, in this order:**
  1. `sensor_2l4_setfile_A_Reset_TnP` (`2l4-setA.h:21-2031`): 2009 tuples. It soft-resets (0x6010=1, 5 ms), writes
     0x6004=1, then sends two bursts at 0x2001:2F54, **1996 words in all**. This is the firmware patch.
  2. `sensor_2l4_setfile_A_Global` (`:2034-2507`): 472 tuples, no bursts. It writes 0x0136=0x1A00 (26 MHz) and
     the PLL.
  3. The mode table.
  4. The LN2 low-noise writes that stock forces after modes 0, 2-4 and 8-11 (`2l4.c:1067-1070,1122-1236`): via the
     page port 6028=2000, 602A=FF22, 6F12=0001, then 0x3012=0x0201, plus 0x0A7A/0x0A7C.
  - The applier is `2l4.c:776-802`, wrapped at `:1249-1285`.
  - Retention (`2l4-setA.h:9367-11751`, CRC check `2l4.c:1287-1365`) is a power optimisation. Ignore it.
- **Modes** (setfile ranges, enum `MF/cis/fimc-is-cis-2l4.h:33-60`). PLL table: compact
  `{ext_clk, mipi_rate, pclk, FLL, LLP}` (`2l4-setA.h:11754-11913`). pclk = 1 080 444 000 for every mode.

  | # | Mode | Lines | FLL / LLP | Mbps/lane | Binning 0x0900 | VCs |
  |---|---|---|---|---|---|---|
  | 0 | 4032×3024 @30 | 2509-2804 | 0x3160 / 0x0B20 | 2054 | 0x0111 | VC0 image + VC1 PD 4032×756 |
  | 1 | 4032×2268 @60 | 2806-3033 | 0x12E4 / 0x0E88 | 2177 | 0x0111 | + PD |
  | 2 | 4032×2268 @30 | 3035-3330 | 0x25C0 / 0x0E88 | 2054 | 0x0111 | + PD |
  | 3 | 4032×1908 @30 | 3332-3627 | 0x25C0 / 0x0E88 | 2054 | 0x0111 | + PD |
  | 4 | 3024×3024 @30 | 3629-3924 | 0x3160 / 0x0B20 | 2054 | 0x0111 | + PD |
  | 5 | 2016×1512 @30 | 3926-4141 | 0x62C0 / 0x0590 | 1352 | 0x0122 (2×2) | + PD |
  | 6 | 2016×1134 @30 | 4143-4358 | 0x62C0 / 0x0590 | 1352 | 0x0122 | + PD |
  | 7 | 1504×1504 @30 | 4360-4575 | 0x62C0 / 0x0590 | 1352 | 0x0122 | + PD |
  | 8-11 | 4032×3024 / ×2268 / ×1908 / 3024² @24 | 4577-5763 | 0x3DB8/0x0B20, 0x2F40/0x0E88 | 2054 | 0x0111 | + PD |
  | **12** | **2016×1134 @240** | 5765-5980 | 0x13F8 / 0x0370 | 2054 | 0x0242 | **VC0 only (PD_NONE)** |
  | **13** | **1008×756 @120** | 5982-6197 | 0x1C80 / 0x04D0 | 2054 | 0x0242 | **VC0 only** |
  | 14-17 | SSM/60 fps | 6200-7485 | | 2054 | 0x0242 | VC_DT + embedded lines; avoid |
  | 18-19 | DRAM test | 9197-9362 | | 2054 | | factory; avoid |

  - The cfg table is at `MF/modules/fimc-is-device-module-2l4.c:43-145`.
  - The RF-avoidance clock variants rewrite only 0x0310 (`2l4-setA.h:12018-12127`, applied at `2l4.c:102-157`).
    Skip them.
- **Stream.**
  - On: 16-bit write 0x0100 = 0x0103, i.e. mode_select = 1 and orientation = 3 (`2l4.c:1716`). Then wait until
    0x0005 ≠ 0xFF and 0x3000 == 0 (2 ms poll, 250 tries, `:1531-1612`).
  - Off: 0x010E = 0x01 (retention CRC, `:1800`), then 0x0100 = 0x00 (`:1808`). Then wait until 0x0005 == 0xFF
    (`MF/cis/fimc-is-cis.c:484-527`).
- **Exposure and gain** (all in `2l4.c`):
  - Coarse integration 0x0202 (`:2029`); WDR long 0x0226.
  - CIT = (t_us·pclk/1e6 − 0x100)/LLP, rounded to even (`:1915-1945`). CIT margin min 0x10, max 0x30 (0x18 on full
    res).
  - Frame length lines 0x0340 (`:2339`); long-frame shifters 0x0701/0x0702.
  - Analog gain 0x0204: code = gain×32, from 0x20 (1×) to 0x200 (16×) (`:2541`).
  - Digital gain 0x020E, 8.8 fixed point, 0x100..0x8000 (`:2800`).
- **PDAF.** In modes 0-11 the PD data goes out on **VC1** (RAW10, W×H/4). The SoC's PDP block normally takes it
  on the fly (`VC_OUT … VC_NOTHING`, `2l4.c` cfg).
  - With the WDMA path, either program only VC0 in the CSIS (`ISP_CONFIG_CH1` left unconfigured) or write VC1 to a
    second WDMA channel and discard it. CSIS v5.2 filters by VC per channel (`CSIS_F_VIRTUAL_CHANNEL`, below), so
    VC0-only is supported [I].
  - The readout window (0x0344..0x0348 = 0x38..0x1FC6, about 8064 columns for 4032 output) reflects the
    dual-pixel array [I].

### 3.2 S5K3P9 (rear ultrawide) — recommended first sensor

- **Setfile.** setA only. Rev 0xA101; any revision gets setA (`3p9.c:175-220`); setB is commented out (`3p9.c:41`).
  Version "S5K3P9SN_REV0.1_Ver0.H_20191230" (`3p9-setA.h:18`).
- **Global init.** `sensor_3p9_setfile_A_Global` (`3p9-setA.h:19-348`): 328 tuples with one burst of **280 words**
  at 0x2000:3F3C. The sequence is 0x6000=0x85, 0x6014=1, 3 ms, 0x6004=1. Applied at `3p9.c:418-444`.
- **Modes.** Each table has 53 tuples. Full `sensor_pll_info` at `3p9-setA.h:1170-1380`. pclk = 26/6·0xE2/7·4 =
  559.6 MHz. LLP = 0x13E0 everywhere. Enum at `MF/cis/fimc-is-cis-3p9.h:136-151`. All modes are 2054 Mbps, 4 lanes,
  VC0 only, no PD (cfg `MF/modules/fimc-is-device-module-3p9.c:43-113`).

  | # | Mode | Lines | FLL | Binning |
  |---|---|---|---|---|
  | 0 | 4608×3456 @30 | 357-411 | 0x0E52 | 0x0011 |
  | 1 | 4608×2592 @30 | 413-467 | 0x0E52 | |
  | 2 | 4608×2240 @30 | 469-523 | 0x0E52 | |
  | 3 | 4608×2184 @30 | 525-579 | 0x0E52 | |
  | 4 | 3456×3456 @30 | 581-635 | 0x0E52 | |
  | 5-9 | same @24 | 637-915 | 0x11E6 | |
  | **10** | **2304×1728 @60** | 917-971 | 0x0728 | 0x0112 + scaler 0x0404=0x2000 |
  | 11 | 2304×1296 @60 | 973-1027 | 0x0728 | |
  | 12 | 1728×1728 @60 | 1029-1083 | 0x0728 | |
  | 13 | 1152×864 @112 | 1085-1139 | 0x03D6 | 0x0114 |

  - After each mode, stock appends a 12-tuple dual-sync master/slave table (`3p9-setA.h:1141-1169`,
    `3p9.c:501-514`). Use the master variant, or skip it. It matters only for dual-camera sync [I].
- **Stream.** On: 0xFCFC=0x4000, then 0x0100=0x0100 (`3p9.c:774-777`). Off: 0xFCFC=0x4000, then 0x0100=0x0000
  (`:831-834`).
- **Exposure and gain:**
  - CIT 0x0202 = (t·pclk_MHz − 0x618)/LLP (`3p9.c:902,938`); margin min 0x07, max 0x08.
  - FLL 0x0340 (`:1188`).
  - Analog gain 0x0204, 0x20..0x200 (`:1365`).
  - Digital gain 0x020E ×4 channels, 0x100..0x1000 (`:1583`).

### 3.3 S5K3J1 (front)

- **Setfile.** setA ("default"); setB is `#if 0` (`3j1-setB.h:14`, `3j1.c:2146-2156`). Rev 0xA101. Version
  "S5K3J1S_EVT0_Ver_0.10" (`3j1-setA.h:18`).
- **Global init.** `sensor_3j1_setfile_A_Global` (`3j1-setA.h:34-265`): 228 tuples. It starts with 0x0100=0, then
  soft reset 0x6010=1 (3 ms), then one **200-word** burst at 0x2000:90C8. Applied at `3j1.c:424-497`.
- **Modes.** Setfile 267-7834; compact PLL at `3j1-setA.h:7835-8040`. PLL for mode 0: 0x0136=0x1A00, 0x0304=3,
  0x0306=0xE9, 0x030E=4, 0x0310=0x116, 0x0312=1 (`:524-531`). Enum at `MF/cis/fimc-is-cis-3j1.h:187-210`. Cfg at
  `MF/modules/fimc-is-device-module-3j1.c:58-175`. All modes are 1807 Mbps (19/20: 1872), 4 lanes.

  | # | Mode | FLL / LLP | VCs |
  |---|---|---|---|
  | 0 | 3648×2736 @30 | 0x0B30 / 0x24A8 | VC0 + PD VC1 3648×684 |
  | 2 | 3968×2232 @30 | 0x0944 / 0x2C40 | + PD |
  | 8 | 1824×1368 @30 (2×2) | 0x0597 / 0x4940 | + PD |
  | **12-14** | **non-PD versions of 0, 2, 8** | as above | **VC0 only** |
  | 19 / 20 | 3648×2736 / 3968×2232 @60 | 0x0B28 / 0x1280 | VC0 only |
  | others | 2736², 3968×1880, 2944×2208, 3216×…, 120/240 fps | | see enum |

- **Stream.**
  - On: msleep(8), dual-sync master 0x0A70=0x0001 and 0x0A72=0x0100 (`3j1.c:762-765`), then 0x0100=1 (8-bit,
    `:768`), then WDR flag 0x021E.
  - Off: 0x0100=0 (`:825`).
- **Exposure and gain:**
  - CIT 0x0202 = (t·pclk_MHz − 0x100)/LLP (`3j1.c:894,948`); margin min 2, max 4.
  - FLL 0x0340 (`:1195`).
  - Analog gain 0x0204, with min/max read from the sensor at 0x0084/0x0086 (`:1392,1519,1572`).
  - Digital gain 0x020E, min/max from 0x1084/0x1086.
- **Secure face unlock.** The 3J1's CSIS2 is one of the two channels the secure-camera feature can claim (5.6).
  The normal path does not use it.

### 3.4 Mode choice for bring-up (smallest frames, VC0 only)

| Sensor | First mode | Why |
|---|---|---|
| 3P9 | #10 2304×1728 @60, or #13 1152×864 | No PD. 2054 Mbps either way. The binned frame is small |
| 3J1 | #14 (non-PD 1824×1368 binned) | VC0 only, 1807 Mbps |
| 2L4 | #13 1008×756 @120, then #12 | PD_NONE. #0 (full res) also works if VC1 is ignored |

---

## 4. AF, EEPROM, OIS, flash (none is needed for a first picture)

### 4.1 AK737X actuators

- Rear: AK7374 at hsi2c_3 0x0C. Product id: register 0xF3 must read 0x0E (`CAM:1018-1027`; ID list
  `MF/actuator/fimc-is-actuator-ak737x.h:23-26`; check `ak737x.c:227-253`) [C].
- Front: AK737x at hsi2c_21 0x0C, also id 0xF3 = 0x0E (`CAM:1113-1121`).
- CONT1 (0x02): 0x00 active, 0x20 sleep, 0x40 standby (`ak737x.h:17-21`).
- Position is 9 bits (0..511), infinity to macro. Register 0x00 = v>>1 and register 0x01 = (v&1)<<7
  (`ak737x.c:53-61`). It looks like a 10-bit DAC driven with 9-bit values [I].
- Stock init for the rear (`vendor_use_sleep_mode`): read the id, then sleep. At stream-on: position 120, wait
  2 ms, then active (`ak737x.c:256-260,471-477`; `CAM:1024-1026`). Off: CONT1 = 0x20. There is no soft-landing
  list in DT.
- Supply: rear VDDAF_COMMON_CAM (S2MPB02 LDO14, 3.3 V), front VDDAF_2.8V_VT (LDO16).
- Mainline: there is no ak7374 driver. `ak7375.c` exists and is a close relative [I]: compare its register map
  before reusing it. Otherwise write a ~200-line VCM lens subdev. Low priority.

### 4.2 EEPROMs

Plain I2C EEPROMs with 16-bit addressing. Read path: `FI/vendor/mcd/fimc-is-sec-define.c:775-821,1094-1241` [C].

| | Rear (2L4) | Ultrawide (3P9) | Front (3J1) |
|---|---|---|---|
| Bus / address | hsi2c_3 / 0x50 | hsi2c_1 / 0x51 | hsi2c_21 / 0x51 |
| Size | 64 KiB | 16 KiB | 16 KiB |
| Header / map version | 0x50 / 0x70 | 0x40 / 0x50 | 0x30 / 0x50 |
| Cal data start | 0x200 | 0x100 | 0x200 |
| AWB master / module | 0x7450 / 0x7474 | 0x2200 / 0x2224 | 0x1A40 / 0x1A4C |
| AF pan / macro | 0x7424 / 0x7430 | – | 0x1A14 / 0x1A20 |
| DT | `CAM:1029-1096` | `CAM:944-977` | `CAM:1123-1168` |

- The rear EEPROM's power position 0 uses the 2L4 READ_ROM scenario, which turns on VDDD_1.8V_OIS and
  VDDIO_1.8V_CAM (`2l4.c:465-471`).
- A failed read does not block streaming (`sec-define.c:3222-3224`; `vender.c:1521-1523`) [C].
- Mainline: the generic `at24` driver (16-bit address, `24c512` / `24c128`) exposes them as nvmem. The LSC/AWB
  data is useful later for libcamera tuning [I].

### 4.3 OIS / aperture MCU (STM32 at hsi2c_37 0x51)

- **It is not needed for a picture.** The sensor rails and reset are all AP-driven, and the stock `HW_INIT`
  sequence powers the 2L4 without OIS (`2l4.c:309-325`) [C]. Keep gpg0-6 (MCU reset) low and leave the OIS rails
  off.
- Trap: in stock, a probed MCU that does not answer fails the rear module init
  (`MF/fimc-is-device-module-base.c:261-267`). That does not apply to new drivers.
- The MCU firmware lives in its own flash. The AP only reflashes it (STM32 system bootloader over I2C, BOOT0 =
  gpm29-0) when the bundled `fimc_is_fw_mcu.bin` is newer (`FI/vendor/mcd/fimc-is-device-ois_mcu.c:277-292,
  1099-1146`). **Never trigger that path from mainline.**
- **The MCU also drives the S10e's dual aperture** (F1.5/F2.4): register 0x63 is the target (2 = F1.5), and
  0x61 is start/busy (`ois_mcu.c:1545-1581`). With the MCU off, the blades stay wherever they are [I]. If they are
  stuck at F2.4, pictures are darker but still valid.
- Protocol for later: 16-bit register address, 8-bit data. 0x0001 status (poll for 0x01), 0x0000 start/stop,
  0x0002 mode (`ois_mcu.c:2124-2135,2405-2423`).

### 4.4 Flash / torch (S2MPB02)

- The camera driver only toggles GPIOs: torch = gpg2-4 and flash = gpg3-6 (`CAM:110-117`;
  `MF/flash/fimc-is-flash-s2mpb02.c:73-101`). The S2MPB02 LED block is set up once, in GPIO mode, by
  `LK/drivers/leds/leds-s2mpb02.c:315-327`.
- FLED_CTRL1 0x12: bit 7 enable, bit 6 torch mode; 0x80 flash, 0xC0 torch, 0x00 GPIO-controlled.
- FLED_CUR1 0x14: flash current in the high nibble (~100 mA·N + 50 mA), torch in the low nibble (20 mA·N). The DT
  sets 1250 mA flash and 240 mA torch (`LK/include/linux/leds-s2mpb02.h:25-45`; `COM:1839-1854`).
- Mainline: add an LED-flash cell to the S2MPB02 MFD later. It is out of scope for the camera milestones.

---

## 5. The SoC path (CSIS → WDMA → DRAM)

### 5.1 Blocks and addresses (stock DT; all in BLK_ISPPRE unless stated)

| Block | Address | IRQ (GIC SPI) | Needed? | Cite |
|---|---|---|---|---|
| CMU_ISPPRE | 0x17000000 (0x8000) | – | yes | `CAL/cmucal-sfr.c:31` |
| D_TZPC_ISPPRE | 0x17010000 (+0x204 = need_smc arg) | – | **never touch from NS** | `XDT/exynos9820-pm-domains.dtsi:85-92` |
| SYSREG_ISPPRE | 0x17020000. +0x40C..0x418 = SC_CON2..5 (WDMA0..3 input mux), +0x500 = MIPI PHY reset bits. **+0x44C (SC_CON17) is a hole: reading it aborts** (M3) | – | yes | `V7/fimc-is-hw-chain.c:38-58,70-100`; `V7/fimc-is-hw-chain.h:173`; `SCAM:277-280` |
| CSIS0 / CSIS1 / CSIS2 / CSIS3 | 0x17030000 / 0x17040000 / 0x17050000 / 0x17060000 (0x1000 each) | 309 / 310 / 311 / 312 | 0, 1, 2 | `SCAM:347,430,663,729`; `LK/include/dt-bindings/interrupt-controller/exynos9820.h:323-326` |
| CSIS3_1 | 0x17090000 | 330 | no | `SCAM:597` |
| CSISX4_PDP_DMA0..3 (WDMA0..3) | 0x170A0000 / 0x170B0000 / 0x170C0000 / 0x170D0000. Each has VC0..3 at +0x000/+0x100/+0x200/+0x300 and "common" at +0x400 | 315 / 316 / 317 / 318 | 0, 1, 2 | `SCAM:159-201`; `…exynos9820.h:329-332` |
| CSISX4_PDP_DMA_TOP | 0x170E0000 (0x100): arbitration, SRAM split, matrix, **test pattern** | – | yes | `SCAM:203-211`; `CSI2/fimc-is-hw-csi-v5_2.h:1031-1043` |
| PDP_TOP_DMA4..7 (+ top 0x17140000) | 0x17100000.. | 326-329 | no (PAF mode only) | `SCAM:213-264` |
| PDP cores | 0x17160000 / 0x17170000 | 313 | no | `SCAM:791-803` |
| 3AA0/3AA1, ISPLP, ISPHQ, MCSC, VRA | 0x17190000.. | | **no** | `SCAM:24-48` |
| SysMMU_ISPPRE (ports "3AA0, 3AA1, CSIS, PDP_STAT") | 0x17210000 (0x9000), NS IRQ 323, secure IRQ 324, secure half 0x17200000 | 323 | yes (or bypass, 5.5) | `XDT/exynos9820-sysmmu.dtsi:221-233,306` |
| MIPI PHY M4S4 top (CSIS0) | 0x19160B00 (0x500), **in BLK_DPU** | – | yes | `SCAM:282-292` |
| MIPI PHY M0S4S4S4 (CSIS2 / CSIS1) | 0x19162300 / 0x19162B00 (0x500) | – | yes | `SCAM:294-316` |
| PMU PHY isolation | PMU 0x15860000 + 0x70C (M4S4 top) / + 0x730 (M0S4S4S4), bit 0 = bypass | – | yes | `SCAM:286,298`; `LK/drivers/phy/samsung/phy-exynos-mipi.c:28,81-104` |

### 5.2 Path through the hardware

- **No 3AA, no ISP.** In stock, the CSIS VC output either goes OTF to the PDP/3AA chain or is written by that
  CSIS's WDMA. The DMA input mux is SYSREG_ISPPRE SC_CON2..5, one per WDMA. In no-PAF mode the value is 0, which
  means "straight from my own CSIS". WDMA n then serves CSIS n only (`V7/fimc-is-hw-chain.c:598-660`) [C].
  - Leave the CSIS→PDP mux (SC_CON0/1) and `SC_PDP_IN_EN` (0x448) alone. Setting `SC_PDP_IN_EN = 0` is safe [I].
- **Per-VC WDMA programming** (`CSI2/fimc-is-hw-csi-v5_2.c:204-318,443-515`; registers `fimc-is-hw-csi-v5_2.h:399-447`)
  [C]. The VC register block is WDMAn + 0x100·vc.

  | Register | Offset | Content |
  |---|---|---|
  | `DMA0_CTRL` | 0x00 | bit 0 ENABLE, bit 1 UPDT_PTR_EN, bits 2-6 UPDT_FRAMEPTR |
  | `DMA0_FMT` | 0x04 | bits 0-1 DATAFORMAT (0 RAW10, 1 RAW12, 2 RAW14/10_DA), bit 2 BYTESWAP, bit 3 PACK12, bit 4 STORAGE_MODE (0 legacy, 1 packet), bit 5 DIM (0 = 2D), bit 6 PACK10 (not EVT0) |
  | `DMA0_ADDR1..32` | 0x10..0x8C | 32 frame buffer slots, **32-bit addresses** |
  | `DMA0_ACT_CTRL` | 0x90 | active frame pointer |
  | `DMA0_RESOL` | 0xA4 | line width (legacy mode only) |
  | `DMA0_FCNTSEQ` | 0xB0 | bitmask of the valid slots |

  - `csi_hw_s_dma_addr` writes `ADDR[n]` and sets bit n in FCNTSEQ. Rotation over the slots is hardware-driven.
- **"Common" block per WDMA** (+0x400; `fimc-is-hw-csi-v5_2.h:624-760`, programmed at `…v5_2.c:856-967`) [C]:

  | Register | Offset | Content |
  |---|---|---|
  | `DMA_DATA_CTRL` | 0x00 | bit 0 `DMA_INPUT_PATH_CH0` (0 = OTF path, 1 = PRL) |
  | `DMA_INT_MASK` | 0x04 | – |
  | `DMA_INT_SRC` | 0x08 | per VC: FRM_START bits 4-7, FRM_END 8-11, LINE_END 0-3, OVERLAP 20-23, FRAME_DROP 24-27; bit 12 DMA_ERROR (FIFO full), bit 16 ABORT_DONE |
  | `DMA_CMN_CTRL` | 0x14 | bit 0 ABORT_REQ |
  | `OTF_FORMAT` | 0x1C | 0 RAW10 / 1 RAW12 / 2 RAW14 / 3 RAW8, USER |

  - The default mask is 0x00F11FF0 (`…v5_2.h:40`).
- **DMA top** (0x170E0000; `…v5_2.c:537-664`). The stock settings: `COMMON_DMA_ARB_PRI` (0x00) = 0x6,
  `COMMON_DMA_CTRL` (0x08) `IP_PROCESSING` bit 1 = 1, `SRAM_SPLIT` (0x0C) = 0x480/0x480, `MATRIX` (0x10) = 0.
  Test pattern registers [C]:
  - `TEST_PATTERN_ENABLE` (0x20): bit 31 enable, bit 30 PPC, bits 0-29 vblank.
  - `TEST_PATTERN_CTRL` (0x24): v-to-h blank, hblank, h-to-v blank.
  - `TEST_PATTERN_RESOL` (0x28): VSIZE 16-29, HSIZE 0-13. The width must be a multiple of 8.
  - Stock enables it with a nominal 533 MHz clock and skips `csi_hw_enable()` (`FI/fimc-is-device-csi_v4.c:1392-1424,
    1681-1682`). **[M3, measured] The pattern reaches a WDMA only through input-mux values 1 and 2 (stock's PDP0/PDP1 VC0 inputs), not 0 (own CSIS).** The old guess was CSIS0 [I]. Found by watching which WDMA's
    FRM_END counts.
- **Memory layout.** Stock sizes a RAW10 buffer as `ALIGN(width·5/4, 16) · height`
  (`FI/fimc-is-video.c:522-539`) [C].
  - In **legacy** storage mode the WDMA writes Samsung's "BAYER_PACKED" 10-bit (`hw_format
    DMA_OUTPUT_FORMAT_BAYER_PACKED`, `fimc-is-video.c:228-236`). That is probably a plain little-endian bit stream,
    **not** the MIPI RAW10 packing [I].
  - **Packet** storage mode (bit 4) stores the CSI-2 payload as received. That would be exactly
    `V4L2_PIX_FMT_SGRBG10P` (MIPI packed), which libcamera's soft ISP takes directly [I].
  - Try packet mode first. Check the result with the TPG: the pattern is known, so the byte order shows at once.
    If only legacy works, add an unpack step (an soft-ISP input format, or a 10-bit unpacked V4L2 format via a
    copy) [I].

### 5.3 CSIS v5.2 link registers

All from `CSI2/fimc-is-hw-csi-v5_2.h:102-146,248-345` [C]. The layout is close to the CSIS v3.x in
`ML/drivers/media/platform/nxp/imx-mipi-csis.c:52-185`: the same CMN_CTRL bits, the same INT_MSK/INT_SRC error bits,
and the same ISP_CONFIG/RESOL/SYNC at 0x40 + 0x10·ch. The differences: the PHY control moved to 0x704, FS/FE are
4-bit per-VC fields, and there is no built-in D-PHY.

| Register | Offset | Fields |
|---|---|---|
| `CSIS_VERSION` | 0x000 | MAJOR 31-24, MINOR 23-16. Expect 5.2 [I] |
| `CSIS_CMN_CTRL` | 0x004 | CSI_EN 0, SW_RESET 1, UPDATE_SHADOW_CTRL 2, DESCRAMBLE 3, LANE_NUMBER 8-9 (lanes−1), INTERLEAVE_MODE 10-11, DESKEW_LEVEL 13-15, UPDATE_SHADOW 16-19, QCHANNEL_EN 20, PHY_SEL 21 (0 = D-PHY) |
| `CSIS_CLK_CTRL` | 0x008 | CLKGATE_EN 4, CLKGATE_TRAIL 16-19 |
| `CSIS_INT_MSK0` / `SRC0` | 0x010 / 0x014 | FRAMESTART 24-27, FRAMEEND 20-23, ERR_SOT_HS 16-19, ERR_LOST_FS 12-15, ERR_LOST_FE 8-11, ERR_OVER 4, WRONG_CFG 3, ECC 2, CRC 1, ERR_ID 0 |
| `CSIS_INT_MSK1` / `SRC1` | 0x018 / 0x01C | V/H resolution mismatch 12-15 / 8-11, RXINVALIDCODEHS 7, ERRSOTSYNCHS 6, MAL_CRC 5, LINE_END 0-3 |
| `ISP_CONFIG_CHn` | 0x040 + 0x10·n | VIRTUAL_CHANNEL 0-1, DATAFORMAT 2-7 (MIPI DT, 0x2B = RAW10), DECOMP 8-9 [I], PIXEL_MODE 12-13 (1 = dual) |
| `ISP_RESOL_CHn` | 0x044 + 0x10·n | HRESOL 0-15, VRESOL 16-31 |
| `ISP_SYNC_CHn` | 0x048 + 0x10·n | HSYNC_LINTV |
| `FRM_CNT_CH0..3` | 0x100..0x10C | frame counters. **The milestone 3 signal** |
| `LINE_INTR_CH0` | 0x110 | – |
| `DEBUG_OPTION_SUITE` | 0x500 | stock writes 1 (pixel align, `csi_v4.c:1582`) |
| `PHY_STATUS` | 0x700 | [I] lane ULPS/stop states. **The milestone 2 signal** |
| `PHY_CMN_CTRL` | 0x704 | ENABLE_CLK 0, ENABLE_DAT 1-4 (lane mask) |

**Stream-on order** (`FI/fimc-is-device-csi_v4.c:1470-1700` with `…v5_2.c`) [C]:
1. `phy_power_on`: PMU isolation bypass, then PHY reset release (`csi_s_power`, `:1337-1356`).
2. Request the CSIS and WDMA IRQs and unmask them (`CSIS_IRQ_MASK0/1` = 0x0FFFFF1F/0x0000FFFF, DMA 0x00F11FF0).
3. Configure the PHY (5.4) with lanes, speed and settle.
4. `csi_hw_s_lane`: LANE_NUMBER, then ENABLE_DAT = lane mask.
5. INTERLEAVE_MODE: 0 = CH0 only, or VC mode.
6. DEBUG_OPTION_SUITE = 1.
7. `csi_hw_s_config` per channel: VC, DT, PIXEL_MODE dual, then RESOL.
8. Per-VC WDMA: FMT, RESOL, slot addresses, FCNTSEQ, and `DMA_N_ENABLE` with `UPDT_PTR_EN`; common OTF_FORMAT and
   INPUT_PATH.
9. DMA top common settings, once.
10. `csi_hw_enable`: UPDATE_SHADOW = 0xF, PHY_SEL = 0, PHY_CMN_CTRL ENABLE_CLK = 1, CSI_EN = 1
    (`…v5_2.c:736-766`).
11. Only after this, stream-on at the sensor.

Before all of it there is `csi_hw_reset` (SW_RESET bit, poll, `…v5_2.c:38-61`; called at `csi_v4.c:1329`).

### 5.4 MIPI D-PHY (the "DCPHY" is used in D-PHY mode)

- **Isolation and reset.**
  - Isolation: PMU 0x70C (M4S4 top) or 0x730 (M0S4S4S4), bit 0 = bypass.
  - Reset: SYSREG_ISPPRE + 0x500, bit 0 = CSIS0 PHY, 1 = CSIS2, 2 = CSIS1, 3 = CSIS3, 4 = CSIS3_1. Clear the bit,
    then set it (`SCAM:277-340`; `phy-exynos-mipi.c:106-130,159-191`).
  - Both PHY groups are flagged SHARED, so isolation is reference-counted (`phy-exynos-mipi.c:194-241,598-638`).
- **Display hazard.** `M4S4 top` (iso 0x70C) is the PHY group the DSI also hangs off (`kit/display/DPU-SPEC.md:53-54`:
  M4S4 top 0x19160000 iso 0x070c, DSIM0 on M4S0 iso 0x0710). **A camera driver must never clear PMU 0x70C**: that
  could kill the running panel [I]. Set bypass if it is clear, and never undo it. In practice S-Boot probably left
  it bypassed for the display [I]. Read PMU 0x1586070C and 0x15860730 first (PMU reads are always safe).
- **Analog configuration.** `phy_cfg[0]` = 0x0502 << 16 | minor. Minor 2 is for the 4-lane PHYs, minor 3 for the
  2-lane CSIS3_1 on non-EVT0 silicon. Type 0xDC for CSIS A/C, 0xD for the others (`CSI2/fimc-is-hw-csi-v5_2.c:976-1017`).
  - The 4-lane table is `__set_phy_cfg_0502_0002_dphy` (`phy-exynos-mipi.c:405-473`). It writes:
    - clock lane: SC_GNR_CON0/1, SC_ANA_CON0-3, SC_TIME_CON0 (base + 0x00..0x30);
    - per data lane i (base + 0x100·(i+1)): SD_GNR_CON0/1, SD_ANA_CON0-3, SD_ANA_CON7 (DC-combo lanes 0-2 only),
      SD_TIME_CON0 (settle in bits 0-7, settle_clk_sel bit 8), SD_TIME_CON1, SD_DESKEW_CON0/4.
    - Deskew is enabled above 1500 Mbps; `freq_s_xi_c` = 26 MHz.
  - About 30 writes in all. Port them as a table [C].
  - **The PHY registers are in BLK_DPU** (0x1916xxxx). They are reachable because pd_dpu and CMU_DPU stay on for
    the display. Do not read any other part of 0x19160000-0x1916FFFF. The display bring-up found holes there that
    reset the SoC (`kit/display/DPU-SPEC.md:349`).
- **Mainline vehicle.** `ML/drivers/phy/samsung/phy-exynos-mipi-video.c` already does "PMU isolation + sysreg
  resetn" per PHY id for the 5433/7870 (`:150-260`). An `exynos9820` variant there covers isolation and reset. The
  analog table goes either in a new small `phy-exynos9820-mipi-dcphy.c` with `phy_configure()`
  (`union phy_configure_opts.mipi_dphy`: lanes, hs_clk_rate, hs_settle), or straight into the CSIS driver [I].

### 5.5 Power domain, clocks, SysMMU, memory

**pd_isppre** (PMU 0x15862500) [C]
- On: `ISPPRE_CONFIGURATION` 0x15862500 bit 0 = 1, then poll `ISPPRE_STATUS` 0x15862504 bit 0
  (`CAL/flexpmu_cal_local_exynos9820.h:600-603`). Off is the mirror (`:693-701`).
- The PD's save/restore list is CMU_ISPPRE state: `PLL_CON0_MUX_CLKCMU_ISPPRE_BUS_USER` 0x100, `DIV_CLK_ISPPRE_BUSP`
  0x1804, and every `QCH_CON` 0x3020..0x30BC (`:605-692`).
- `need_smc = <0x17010204>`, stock parent `pd_dpu` (`XDT/exynos9820-pm-domains.dtsi:85-92`).
- **Mainline:** `pd_isppre` already exists in `UDT/exynos9820.dtsi:1364-1370`, without `samsung,tzpc-base`, and
  with its parent commented out because pd_dpu is disabled. The driver's TZPC save/restore SMC (0x82000410, mode
  0/1, base, group 2) is in `ML/drivers/pmdomain/samsung/exynos-pm-domains.c:39-111` and works on pd_aud and pd_mfc.
- **Required DT change:** add `samsung,tzpc-base = <0x17010204>;` to pd_isppre before any consumer powers it.
- Boot state of pd_isppre: unknown [I]. Read 0x15862504.
  - If it is on, genpd's "disable unused" switches it off at about 0.7 s. Without tzpc-base that happens with no
    TZPC save (the same situation as G3D, `kit/gpu/MALI-G76-SPEC.md` §3).
  - Stock always restores on enable even when the domain started off, so EL3 keeps a default table [I]
    (`kit/mfc/MFC-SPEC.md` §6.1).
  - Give the CSIS driver `power-domains = <&pd_isppre>` and runtime PM. Do not keep it powered by hand.
- **Children.** In stock, pd_isppre is a child of pd_dpu because the PHYs live in DPU. Mainline must not model that
  while pd_dpu is "disabled": pd_dpu is on and must stay on.

**CMU_ISPPRE** (0x17000000) — not in mainline. Add it to `clk-exynos9820.c` like CMU_MFC [C]:
- `PLL_CON0_MUX_CLKCMU_ISPPRE_BUS_USER` 0x100 (`CAL/cmucal-sfr.c:257`), parent CMU_TOP `dout_cmu_isppre_bus`. That
  one already exists in mainline (`ML/…/clk-exynos9820.c:3603-3604,3707-3708,3856-3858`).
- `DIV_CLK_ISPPRE_BUSP` 0x1804 (`:459`).
- Gates (`:1326-1380`): `ACLK_CSIS0..3` 0x2024..0x2030, `ACLK_CSIS3_1` 0x2034, `ACLK_CSISX4_PDP_DMA` 0x2038,
  `ACLK_SYSMMU_ISPPRE` 0x2060, `ACLK_XIU_D/P` 0x2068/0x206C, `SYSREG_ISPPRE_PCLK` 0x20D8, `LHS_AXI_D` 0x20C8,
  `LHM_AXI_P` 0x20B4, `ISPPRE_CMU_PCLK` 0x2004, `C2CLK_CSISX4_PDP` 0x2084, and the `QE_CSISX4_PDP` A/PCLK 0x2054/0x2094.
- Q-channels (`:2365-2387`): `QCH_CON_IS_ISPPRE_QCH_CSIS0..3` 0x304C..0x3058, `QCH_CSISX4_PDP_DMA` 0x305C,
  `QCH_SYSMMU_ISPPRE` 0x3088, `QCH_CON_ISPPRE_CMU_ISPPRE` 0x3030. The stock `GATE_IS_ISPPRE_*` names are HWACG
  Q-channel gates on these (`LK/drivers/clk/samsung/clk-exynos9820.c:263-272`).
- First version: mark everything `CLK_IGNORE_UNUSED`, like CMU_MFC, and enable the CSIS/WDMA/SysMMU gates from the
  driver. The bus side is already in mainline, critical, in CMU_BUSC (`LHM_AXI_D_ISPPRE`, `LHS_AXI_P_ISPPRE`,
  `ML/…/clk-exynos9820.c:2836-2861`).
- Bus rate: `ISPPRE_BUS` is a plain CMU_TOP clock in the always-on "vddi" list (`CAL/cmucal-vclk.c:2-22`), not an
  ACPM DVFS domain. Leave it at the bootloader rate. Stock assumes about 533 MHz for the TPG [I].

**SysMMU_ISPPRE** (0x17210000, NS IRQ 323) [C]
- The stock node is at `XDT/exynos9820-sysmmu.dtsi:221-306`: `sysmmu,secure-irq`, secure half at 0x17200000 (**never
  map**), pd_isppre, clock `GATE_IS_ISPPRE_SYSMMU_ISPPRE`.
- Mainline: add it like `sysmmu_mfc0` (`UDT/exynos9820.dtsi:1594-1603`) with `samsung,sysmmu-version = <7 3>`, so
  probe reads nothing while the PD is off (`ML/drivers/iommu/exynos-iommu.c:516-531`).
- CSIS0..3 and all WDMAs are behind this one SysMMU (`SCAM:398,481,564,709`).
- **Plan B for milestone 3.** The WDMA takes 32-bit addresses and the phone has 6 GB RAM up to 0x9_FFFFFFFF.
  Without the IOMMU, the buffer must be below 4 GiB.
  - Use `dma_set_mask(32)`, or a `memory-region` pointing at the already-reserved `camera@99000000` carve-out
    (0xE400000, no-map, `UDT/exynos9820-beyond0lte.dts:58-66`).
  - SysMMU_ISPPRE after a PD-on should be disabled, i.e. bypass [I]. Verify by checking that the WDMA writes the
    TPG to a physical address.

### 5.6 Secure-world interactions

| Path | SMC? | Notes |
|---|---|---|
| pd_isppre on/off | **yes, TZPC save/restore 0x82000410** with base 0x17010204 | Already in mainline (5.5) |
| CSIS, WDMA, PHY, SYSREG, CMU_ISPPRE access | no | Plain MMIO in stock |
| SysMMU_ISPPRE NS half | no | The secure half 0x17200000 is EL3 only |
| Secure face unlock (`SECURE_CAMERA_FACE`, channels CSIS2 and CSIS4) | yes: `SMC_SECCAM_SETENV/INIT/INIT_NSBUF` at fimc-is probe, `SECCAM_PREPARE` per secure stream | `FI/fimc-is-core.c:1513-1555`, `:224-260`; `V7/fimc-is-config.h:175-188`. Mainline never calls these. The normal stock path for CSIS2 also runs without PREPARE. **Risk [I]:** EL3 could already treat CSIS2/WDMA2 as secure at boot. Test the 3J1 (CSIS2) after the 3P9 (CSIS1) works |
| Sensor, PMIC, I2C | no | |

No Samsung ISP firmware, DDK library or setfile .bin is needed. The `setfile_*.bin` names in the module glue are
ISP tuning files (`2l4.c:548`), not sensor firmware [C].

---

## 6. Mainline: what exists, what to reuse, what to write

### 6.1 Already present in ML/UDT

| Item | Where |
|---|---|
| CMU_TOP CIS_CLK0..4, ISPPRE_BUS | `ML/drivers/clk/samsung/clk-exynos9820.c:2969-3858` |
| CMU_BUSC ISPPRE bus gates (critical) | `:2818-2861` |
| CMU_CMGP I2C_CMGP1 | `:1814,1837` |
| pd_isppre (no tzpc-base yet), TZPC SMC support | `UDT/exynos9820.dtsi:1364-1370`; `ML/drivers/pmdomain/samsung/exynos-pm-domains.c:39-111` |
| SysMMU v7 with a DT-given version | `ML/drivers/iommu/exynos-iommu.c:516-531,770-790` |
| Pin banks gpc0, gpc1, gpg0, gpg2, gpg3, gpm29, gph0, gpp5, gpp6 | `UDT/exynos9820-pinctrl.dtsi:425-654` |
| HSI2C driver (`samsung,exynosautov9-hsi2c`) and USI (`samsung,exynos850-usi`) | used for hsi2c_38 / hsi2c45 |
| Camera carve-outs reserved | `UDT/exynos9820-beyond0lte.dts:58-66`, `UDT/exynos9820-common.dtsi:86-88` |
| Sensor driver templates: `s5kjn1.c`, `s5k3m5.c` (v4l2-cci); VCM `ak7375.c`; `at24` | `ML/drivers/media/i2c/` |
| CSIS v3 reference with nearly the same register layout | `ML/drivers/media/platform/nxp/imx-mipi-csis.c` |
| PHY isolation/reset framework for Exynos MIPI | `ML/drivers/phy/samsung/phy-exynos-mipi-video.c` |

Not usable: `ML/drivers/media/platform/samsung/exynos4-is` (FIMC-IS v1 firmware ISP and FIMC-LITE, a different
generation), and s3c-camif (a parallel interface).

### 6.2 To write

| # | Item | Size [I] | Notes |
|---|---|---|---|
| 1 | DT: I2C_CAM0..3, USI08_I2C, USI10_USI, USI17_USI (with USI wrappers), I2C_CMGP1 | DT only | Plus CMU_PERIC1 gates (#2) |
| 2 | `clk-exynos9820.c`: CMU_PERIC1 I2C_CAM0-3, USI08_I2C, USI10, USI17_USI; new **CMU_ISPPRE** | ~250 lines | Offsets in 1.2 and 5.5 |
| 3 | `s2mpb02` regulator (+ an MFD core for the later LED cell) | ~250 | Keep LDO17 (grip) and LDO9 on |
| 4 | `s2mpb03` regulator | ~150 | |
| 5 | pd_isppre `samsung,tzpc-base`, SysMMU_ISPPRE node | DT | |
| 6 | **`exynos9820-csis` capture driver**: one platform device per CSIS with its WDMA, as a V4L2 subdev and video device (media controller); VB2 dma-contig; IRQs from CSIS + WDMA; TPG control | ~1200 | Model: `imx-mipi-csis.c` for the link + `imx7-media-csi.c` for the DMA/video side. Or keep two drivers (link subdev + WDMA video device) to match libcamera's simple pipeline expectations |
| 7 | D-PHY: an exynos9820 variant in `phy-exynos-mipi-video.c` (iso 0x70C/0x730, SYSREG_ISPPRE 0x500 resets) + the analog table | ~300 | Never undo isolation on 0x70C |
| 8 | Sensor drivers `s5k3p9`, `s5k3j1`, `s5k2l4` (v4l2-cci; global + mode tables generated from the stock headers; burst helper for 0x6F12) | ~900-1500 each, mostly tables | Generate the tables with a script from `*-setA.h`; do not hand-copy them |
| 9 | libcamera: add the CSIS video driver name to the simple pipeline handler's supported list, plus sensor helpers (gain = code/32, black level 64 @10-bit [I]) | small | Then soft ISP + GPU debayer as on the Poco |
| 10 | Later: ak7374 VCM, at24 EEPROM nodes, S2MPB02 flash LED, OIS MCU (do not flash it) | | Optional |

---

## 7. Order of work and milestones

Each milestone has one pass/fail signal. Do the read-only checks (R) before anything that writes.

| # | Milestone | Needs | Pass signal | Risk |
|---|---|---|---|---|
| R0 | **Read-only state dump** (devmem or a small `/dev/mem` tool, like `kit/display` stage A): PMU 0x15862504 (pd_isppre), 0x1586070C and 0x15860730 (PHY isolation); CMU_TOP CIS_CLK mux/div/gate 0x1A241010.. / 0x1A24180C.. / 0x1A242028.. and DMYQCH 0x1A243008..; MCLK pin config (gpc1). **Only if pd_isppre reads ON:** CSIS0 0x17030000 (VERSION) | nothing | values logged | very low. Never read the ISPPRE blocks with the PD off, and never read DPU 0x1916xxxx beyond the PHY windows |
| M1 | **S5K3P9 chip id over I2C**: S2MPB02 regulator, hsi2c_1 (I2C_CAM1 + clocks + USI), MCLK3 26 MHz on gpc1-7, reset gpg0-3; 3P9 power sequence (2.2); read 0x0000..0x0003 | #1-3 | 0x0002 reads 0xA101 | low. A wrong S2MPB02 write could hit LDO17/LDO9 users; write only the named LDOs |
| M1b | Same for 3J1 (hsi2c_2, MCLK1, gpg2-5) and 2L4 (S2MPB03 on CMGP I2C1 + hsi2c_0, MCLK0, gpg2-6) | #4 | 0x0002 = 0xA101 / 0xA002 or 0xA102 | low |
| M2 | **pd_isppre on with TZPC restore, CMU_ISPPRE up, CSIS1 VERSION reads 5.2** | #2, #5 | dmesg "TZPC restore SMC" returns 0; CSIS_VERSION is plausible | **medium**: an access before the restore is an external abort (the G3D lesson). Put the first ISPPRE read after the SMC, then check the PD status |
| M3 ✅ | **TPG → WDMA → RAM** (passed 2026-10-30, see §10): DMA top TPG at 640×480, WDMA n slot 0 at a <4 GiB buffer (IOMMU bypass first) | #6 (minimal) | DMA_INT_SRC FRM_END bits count; the buffer contains the pattern | low/medium: a DMA to a wrong address scribbles RAM. Use a carve-out buffer |
| M4 | **3P9 streams, CSIS1 sees frames**: PHY isolation/reset/analog config for CSIS1 at 2054 Mbps, settle 10, 4 lanes; 3P9 global + mode 10 + stream on | #7, #8 (3P9) | `FRM_CNT_CH0` (0x100) increases; INT_SRC0 FRAMESTART/FRAMEEND with no ERR_SOT_HS/CRC/ECC; PHY_STATUS shows HS activity | medium: settle and deskew are the usual suspects; also try settle ±2 |
| M5 | **RAW frame of a real scene in memory**: WDMA1 VC0 with RAW10 packet mode; dump it and debayer offline | #6 | a recognisable image | low |
| M6 | V4L2/media-controller pipeline + libcamera simple + soft ISP for the 3P9; then the 3J1 (CSIS2, DCPHY type 0xDC) | #6, #9 | `cam -c1 --capture` produces frames; Snapshot shows the camera | medium for the 3J1 (secure-CSIS2 question, 5.6) |
| M7 | 2L4: S2MPB03, the 2009-tuple TnP, mode 13 then mode 0 with VC1 dropped, LN2 writes, CSIS0 on the M4S4 top PHY | #4, #8 (2L4) | frames on CSIS0 | **medium/high**: the M4S4 top PHY is shared with the DSI group (iso 0x70C); TnP burst sizes |
| M8 | Exposure/gain controls, AF (AK7374), EEPROM nvmem, torch | #10 | | low |

Why the 3P9 first: S2MPB02 only, CSIS1 on a plain D-PHY ("DPHY 2.5 Gbps", type 0xD) away from the DSI group, no PD
VC, no AF/OIS, and the smallest init. The 3J1 is the second choice: its CSIS2 is a DC combo PHY, and it is one of
the secure-camera channels.

---

## 8. Risks

1. **Power domain plus TZPC.** Any access to 0x17000000-0x1721FFFF while pd_isppre is off, or before the TZPC
   restore SMC returns, is an external abort and probably a hang (the same class as G3D/MFC) [I].
   - Mitigation: add `samsung,tzpc-base` to the DT before the first consumer exists, do runtime PM through genpd
     only, and never flip the PD by hand.
   - Open question: does the restore work without a prior save? Stock suggests it does (MFC spec §6.1).
2. **The PHY in the DPU block and the display.** The CSIS0 PHY (M4S4 top, iso 0x70C) shares its group with the
   DSI. Clearing isolation or holding the wrong reset could blank the panel, and recovery is a reboot [I]. A stray
   read in 0x1916xxxx can reset the SoC (DPU spec).
   - Mitigation: never clear 0x70C, and touch only the documented PHY windows.
3. **IOMMU.** SysMMU_ISPPRE has a secure half and secure IRQ. Mainline's v7 variant works for MFC and AUD, but the
   CSIS/WDMA stream IDs (VGEN_LITE in ISPPRE) are untested [I].
   - Mitigation: milestone 3 in bypass with a <4 GiB buffer, then the IOMMU.
   - If the SysMMU comes up enabled with a stale page table after the PD-on, DMA faults show on IRQ 323.
4. **Secure camera channels** CSIS2 and CSIS4 (5.6). If EL3 protects WDMA2 by default, the 3J1 can't be captured
   without `SECCAM` calls [I]. Not expected: stock streams the front camera without PREPARE.
5. **Settle and deskew.** At 2054 Mbps the stock settle is 10 with deskew calibration enabled. If the sensor does
   not send a deskew sequence, the PHY may wait [I]. Try `SD_DESKEW_CON0` = 0.
6. **RAW packing.** Legacy mode probably writes Samsung's bit-packed format, not MIPI RAW10 [I]. Test packet mode
   with the TPG.
7. **TnP burst size (2L4).** The 1996-word burst may exceed what the hsi2c controller or `i2c_transfer` takes in
   one message. Split it into chunks at the same auto-incrementing address, re-sending 0x602A between chunks [I].
8. **Bayer order and flip.** The driver says GRBG, and the 2L4 additionally flips H+V. Verify with a coloured
   target [I].
9. **Aperture.** The dual aperture is MCU-driven. If it is parked at F2.4, the 2L4 image is about 1.3 EV darker.
   This is not a bring-up blocker.
10. **No chip-id constants in the source.** Use 0x0002 (revision) as the probe check. The SMIA 0x0000 values in
    §3 are [I].
11. **Agent/resource limits** (memory `subagent-fanout-limits`): build in batches. Keep the existing display/GPU
    DT untouched while adding the ISPPRE nodes.

## 9. Open questions to answer on the device (read-only first)

1. Boot state of pd_isppre (0x15862504) and of PHY isolation 0x70C and 0x730.
2. Whether the S2MPB02 camera LDOs are on after S-Boot. Read L4..L16 CTRL 0x21..0x2D over I2C once the bus exists.
3. CSIS_VERSION on CSIS0/1/2, and WDMA common register defaults after PD-on.
4. Which CSIS the DMA-top TPG feeds.
5. Whether SysMMU_ISPPRE is in bypass after PD-on (CTRL register at 0x0).
6. The real chip-id values at 0x0000 of each sensor.

## 10. Measured on the device (M3, 2026-10-30)

Test module `m3/tpg-probe.c` (the working copy is `m3/tpg-probe.c.m3-pass`).

- **SYSREG_ISPPRE as found** (pd on, S-Boot/secure state): USER_CON 0x400 = 0xFFF000, SC_CON0..14 = 0, SC_CON15 (0x440) = 1, SC_CON16 = 0, PDP_IN_EN (0x448) = 0. **0x44C aborts: never read it.**
  Stock's "reset values" for these fields (3/7) do not match what is there.
- **CMU_ISPPRE during streaming:** PLL_CON0 0x100 = 0x10 (the bus, not OSC); gates 0x2xxx = 0x200000; all QCH_CON 0x3020..0x30BC = 0x3F0002
  (Q-channel on, CLOCK_REQ 0). Forcing CLOCK_REQ made no difference: this was not the missing piece.
- **Never `clk_disable` CLK_GOUT_ISPPRE ids 3, 12 or 17** (CMU pclk, XIU_P, LHM_AXI_P). Taking and releasing them killed the P-bus path,
  and the phone reset at the next pd_isppre power-off (the secure TZPC save). Ids 4-6, 9-11, 13-15, 18 and 19 are safe to toggle.
- **TPG → WDMA works** with: DMA top ARB_PRI 6, CTRL |= IP_PROCESSING, SRAM_SPLIT 0x140/0x140, MATRIX 0; TP_CTRL 0x804640,
  TP_RESOL (480<<16)|640, TP_ENABLE BIT30|vblank(0x8000) then BIT31; WDMA VC0 FMT, RESOL = width, ADDR1, FCNTSEQ 1, CTRL 3;
  **SC_CON2 = 1 (or 2)**. INT_SRC 0x111 (LINE_END, FRM_START, FRM_END on VC0), FCNTSEQ_STAT 0x101.
  The CSIS registers do not matter for the pattern; nothing in the PDP needed clocks.
- **ACT_CTRL 0x1007C** (ABORTED, FRAMEPTR 0x1F) is the idle value before the first frame start. It is not an error.
- **WDMA output formats, legacy storage, RAW10:**
  - FMT = 0: **16-bit little-endian per pixel** (10 bits used), 1280 B/line at 640 wide = V4L2 `Sxxxx10` unpacked.
    This is what libcamera's soft ISP takes: use it.
  - FMT bit 6 (`DMA_N_PACK10`): 800 B/line, a **plain little-endian 10-bit bitstream** (bytes `00 f0 0f 00 ff` = 0, 0x3FC, 0, 0x3FC).
    This is **not** MIPI RAW10 packing.
- The pattern is a checkerboard of 0 / 0x3FC that alternates by column, with rows shifted.

## 11. M4/M5 measured (2026-10-30)

- **M4:** `m4/csis1-probe.c` plus the `stream` attribute of `s5k3p9.c` (tables in `drivers/media/i2c/s5k3p9-tables.h`).
  Results: FRM_CNT0 ~62/s at mode 10, INT_SRC0 only FS|FE, PHY_STATUS 0xF0/0x0 while streaming, a full frame in WDMA1.
  The stock D-PHY table and settle 10 worked as-is.
- **I2C:** messages longer than a few bytes time out on the CAM1 hsi2c (8-word burst = 18 bytes fails, 4-byte writes work).
  Single 0x6F12 writes work: the indirect pointer auto-increments, as the patch readback via 0x602C/0x602E shows.
- **M5:** the 3P9 outputs **GRBG**, black level 64. Sensor test pattern 0x0600=2 gives colour bars 0/511/1023.
  Exposure 0x0202 (lines, about 9.09 µs each at LLP 0x13E0), FLL 0x0340 and gain 0x0204 (0x20 = 1×) all act as expected.
