# S10e (SM-G970F, Exynos 9820) audio on mainline 7.2: hardware map and bring-up spec

Researched 2026-10-29. Sources:
- **DS** = downstream LineageOS kernel, `src/lineage-kernel`
  - board: `arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi` (written **BD:** below).
    No `eur_open_*` revision dts/dtsi overrides any audio node (grepped).
  - SoC: `arch/arm64/boot/dts/exynos/exynos9820.dts` (**SD:**), `exynos9820-pinctrl.dtsi` (**SP:**),
    `exynos9820-pm-domains.dtsi`, `exynos9820-sysmmu.dtsi`
- **ML** = mainline tree `src/linux-9820` (7.2-rc2), board DT
  `src/u-boot/dts/upstream/src/arm64/exynos/exynos9820{.dtsi,-common.dtsi,-beyond0lte.dts}`
- **PH** = the phone itself, read-only (running `7.2.0-rc2-s10e-g62fdd500d8ca-dirty #7`)

Labels: **[V]** = verified (read in source or on the phone), **[I]** = inferred or not yet tested.

---

## 0. Summary

| Path | Hardware route | What stops it today |
|---|---|---|
| Loudspeaker (bottom) | ABOX UAIF1 to CS35L40 @0x40 (right, "bot") | UAIF1 is part of ABOX: no ABOX driver, no CMU_AUD |
| Earpiece | ABOX UAIF1 to CS35L40 @0x41 (left, "rcv"), same TDM bus | same |
| 3.5 mm headphones | ABOX UAIF0 to CS47L93 AIF1 to HPOUT2L/R | ABOX for PCM data. The codec on its own (tone generator) needs only SPI, LDOs and MCLK |
| Mics (DMIC1/2, headset) | CS47L93 IN3L/IN4L (DMIC), IN1BL (headset), AIF1TX to UAIF0 | ABOX for capture |
| BT SCO | BCM4375 PCM to ABOX UAIF3 | ABOX. A2DP goes over the HCI UART instead and needs no ABOX |

**Conclusion [V for the wiring, I for the effort]:** every PCM path from the AP to the codec or the
amps goes through ABOX. The SoC has no other I2S/PCM controller: the downstream SoC dts has no
`i2s` node (grep of SD: is empty), and the UAIF pins are AUD-block pins at pinctrl@18C60000. Making
the codec the clock master does not get around this. The data still has to come from an ABOX UAIF,
and the amps hang off UAIF1, which the codec never sees. So the minimum for real sound is
**a small, firmware-less ABOX driver**: CMU_AUD clocks, the AUD power domain (with a TZPC SMC),
RDMA plus SPUS/SIFS routing plus UAIF. The codec and amp drivers already exist in mainline.

---

## 1. Hardware wiring

### 1.1 CS47L93 codec (madera family) [V]
- **Bus:** SPI, `&spi_2` = `spi@10470000`, the **USI01** SPI function in PERIC0 (SD:3504-3520,
  IRQ `INTREQ__USI01_USI` = SPI 384, clocks `GATE_USI01_USI` / `DOUT_CLK_PERIC0_USI01_USI`, 64-byte
  FIFO, `swap-mode`, DMA commented out). Chip select 0 (`cs47l93@0`, `reg = <0>`), **25 MHz**
  (BD:1894-1905). Controller-data: `samsung,spi-feedback-delay = <1>`, `chip-select-mode = <0>`.
  - USI mode: `&usi_1 { usi_v2_mode = "spi"; }` (BD:1183) = sysreg_peric0 (0x10420000) + **0x100C**
    (SD:2134 `usi@1042100c`).
  - Pins: `spi2_bus` = gpp0-6, gpp0-5, gpp0-4, function 2 (SP:1298). CS = **gpp0-7 function 2**
    (native CS, BD:1886 `spi2_cs_func`, pull-up 3).
- **Reset:** `reset-gpios = <&gpg1 0 0>` = **gpg1-0**, active high in the DT flags, output
  (BD:1878 `codec_reset`, BD:1921). Madera's driver treats /RESET as active-low internally, so on
  mainline write `GPIO_ACTIVE_LOW` [I: check the polarity convention in madera-core against
  downstream before the first boot].
- **IRQ:** **gpa2-2** (`interrupt-parent = <&gpa2>; interrupts = <2 0 0>`, BD:1910; pin cfg
  `codec_irq` BD:1868, function 0xf = EINT). gpa2 is a wake-up EINT bank in mainline pinctrl
  (`exynos9820_pin_banks0`, ML `drivers/pinctrl/samsung/pinctrl-exynos-arm64.c:1244`).
- **Supplies** (BD:1915-1919): AVDD, DBVDD1, CPVDD1 = `ldo17_reg` = **S2MPS19 LDO17M
  "vdd_pmic_codec_1p8"**, 1.8 V, always-on + boot-on (BD:1614). CPVDD2, DCVDD = **LDO18M
  "vdd_codec_1p2"**, 1.2 V, ramp 12000, enable-ramp 5000 µs (BD:1623). MICVDD is internal (a
  `micvdd` sub-regulator at 3.0 V). MICBIAS1 = 1.8 V and MICBIAS2 = 2.8 V, both with ext-cap.
  Mainline has S2MPS19 regulators over ACPM (`drivers/regulator/s2mps11.c:1830`), and the board
  dtsi already uses ldo19m/ldo27m (`exynos9820-common.dtsi:142-163`), so adding `ldo17m`/`ldo18m`
  is a DT-only change [V for the driver, I for the names].
- **MCLK1 = 26 MHz from the PMU CLKOUT0 pad (XCLKOUT)**: the machine node has
  `clocks = <&clock OSC_AUD>; clock-names = "xclkout"` (BD:203-204), and `OSC_AUD = VCLK_CLKOUT0`
  (DS `drivers/clk/samsung/clk-exynos9820.c:540`). CLKOUT0 is PMU+0xA00: SEL field [13:8] =
  1 (TCXO), bit 0 = *disable* (DS `cal-if/exynos9820/clkout_exynos9820.c`; `ra.c:603-616` writes 0
  to enable). That matches ML's `clk-exynos-clkout.c` "exynos5" layout (0xa00, mux shift 8, disable
  bit 0), which has no 9820 compatible yet.
  - PH: `busybox devmem 0x15860A00` = **0x00000000**. The pad is enabled but SEL=0, not TCXO, so the
    codec has no 26 MHz reference right now [V]. Linux has to set SEL=1.
  - MCLK2 (32 kHz) is not described downstream. Madera only warns when it is missing (ML
    `drivers/mfd/madera-core.c:490`) [V].
- **Clocking, as downstream programs it** (BD:207-214, `sound/soc/samsung/beyond_madera.c`):
  - `cirrus,sysclk = <1 4 98304000>`: SYSCLK from FLL1 at 98.304 MHz.
  - `cirrus,fll1-refclk = <1 0 26000000>`: FLL1 is referenced to MCLK1 at 26 MHz
    (`MADERA_FLL_SRC_MCLK1 = 0`, ML `sound/soc/codecs/madera.h:31`).
  - `cirrus,asyncclk = <2 5 98304000>`: ASYNCCLK from FLL2. `cirrus,fll2-refclk = <2 8 …>` is
    referenced to **AIF1BCLK** (src 8). Rate = fs × width × 2 (beyond_madera.c:168).
  - `cirrus,dspclk = <8 4 147456000>`, `cirrus,outclk = <9 1 0>`.
- **DAI format:** all links use `snd_soc_of_parse_daifmt(dai, NULL, …)` with no
  bitclock-master/frame-master in the DT (beyond_madera.c:2014; BD:492-528), so **the codec is the
  clock consumer and ABOX UAIF is the provider** on every link [V]. UAIF also accepts consumer mode
  (`SND_SOC_DAIFMT_CBM_CFM` sets `ABOX_MODE = 0`, DS `abox/abox_if.c:296-304`), and madera can be
  provider. So the codec *could* master UAIF0/UAIF2 [V that both sides support it, I that it works
  on this board]. That would remove the need for UAIF BCLK dividers in CMU_AUD for the codec links.
  It does **not** remove ABOX from the data path.
- **Codec AIF to ABOX:** [V] BD:492-519
  - `uaif0` (format i2s): ABOX UAIF0 to **cs47l93 DAI 0 = AIF1** (ML `cs47l92.c:1697` "cs47l92-aif1").
  - `uaif2` (i2s): ABOX UAIF2 to **cs47l93 DAI 2 = AIF3** (`cs47l92-aif3`).
  - Codec pinctrl muxes groups `aif1`, `aif3`, plus gpio9/gpio10 as io (BD:1940-1963). AIF2 is unused.
  - `debug-trace` link: cs47l93 DAI 6 to 7 (the DSP trace firmware `cs47l93-dsp1-trace.wmfw`,
    BD:2052). Not needed.
- **Analog routing** (BD:216-231): DMIC1 (MICBIAS1A) to IN3L = main mic. DMIC2 (MICBIAS1B) to
  IN4L. HEADSETMIC (MICBIAS2A) to IN1BL. FM IN to IN2BL/R. **HEADPHONE = HPOUT2L/R**. RECEIVER =
  "Left AMP SPK". SPEAKER = "Right AMP SPK". `cirrus,inmode` IN2 is differential,
  `cirrus,dmic-ref = <0 0 1 1>`, `cirrus,out-mono = <0 0 0 1 0 0>`.
  - Stock `/vendor/etc/mixer_paths.xml` (PH, read via a read-only mount): `dev-headset` =
    `HPOUT2L Input 1 = AIF1RX1`, `HPOUT2R Input 1 = AIF1RX2`, `HPOUT2 Digital Switch = 1`.
    `dev-main-mic` = IN3L to LHPF1 to ASRC1IN1L to ISRC1INT1 to AIF1TX1/2, `DMIC1 Switch = 1`.
- **Jack detection:** downstream `cirrus,accdet` acc@1 (JD2, hpdet, micd ranges, moisture pin 7,
  `cirrus,gpsw = <3 0>`). **Mainline has no madera accessory-detect/extcon driver** (no
  extcon-madera in ML) [V]. Headphones can still be driven with routing forced by hand.

### 1.2 CS35L40 amplifiers: two of them [V]
- **Bus:** `&hsi2c_38` = `hsi2c@109A0000` in PERIC1, IRQ `INTREQ__USI17_I2C` (= SPI 428), 400 kHz,
  clocks `DOUT_CLK_PERIC1_USI_I2C` / `GATE_USI17_I2C` (SD:3278-3294, `samsung,usi-i2c-v2`).
  Pins **gph0-6 (SCL), gph0-7 (SDA), function 3** (BD:2068). USI mode: `&usi_17 { usi_v2_mode =
  "i2c"; }` (BD:1257) = sysreg_peric1 (0x10820000) + **0x104C** (SD:2382). There is also
  `usi_17_i2c` at +0x1050 (SD:2390). Which of the two has to be written for the 0x109A0000 port is
  [I]; copy downstream (+0x104C = "i2c").
- **Amp R = "bot" = loudspeaker:** `cs35l40@40`, `cirrus,right-channel-amp`, reset **gph1-1**
  (BD:2075, BD:2119-2127), dsp part `cs35l40-bot`.
- **Amp L = "rcv" = earpiece:** `cs35l40@41`, reset **gph1-2** (BD:2083, BD:2163-2173), dsp part
  `cs35l40-rcv`.
- **IRQ:** both share **gpm24-0** (CMGP wake-up bank, `interrupt-parent = <&gpm24>; interrupts =
  <0 0 0>`, BD:2101). Shared and level-low [I], so use `IRQF_SHARED`. Mainline cs35l41 requests
  `IRQF_ONESHOT | IRQF_SHARED | IRQ_TYPE_LEVEL_LOW` [I: check cs35l41.c].
- **ASP link:** `uaif1`, format **dsp_a + bitclock-inversion**, ABOX UAIF1 to `cs35l40_l 0` and
  `cs35l40_r 0` (BD:501-510). Amp clock `cirrus,ampclk = <0 0 6144000>` = SCLK at 6.144 MHz
  (48 kHz × 32 bit × 4 slots). Stock speaker path (mixer_paths `dev-speaker`): `Right ASPTX1..4
  Slot Position = 0/2/4/6`, `ASP TX1 = DSPTX1`, `TX2 = ASPRX1`, **`Right PCM Source = DSP`**,
  `Right Boost Enable`, `Right AMP Enable Switch = 1`.
- **Supplies:** none are listed in the DT (VA/VP come straight from the battery/system rails) [V
  for "not listed"]. Mainline cs35l41 asks for VA/VP supplies; they fall back to dummies.
- **Boost:** DT has only `cirrus,boost-peak-milliamp = <4100>`. Downstream never programs the
  inductor or capacitor (no such property, DS `sound/soc/codecs/cs35l41.c:1840-1880`), so the chip
  runs on reset defaults. **Mainline requires `cirrus,boost-ind-nanohenry` and
  `cirrus,boost-cap-microfarad`** (ML `cs35l41-lib.c:994-1031`, -EINVAL otherwise). The reset
  defaults are BSTCVRT_COEFF = 0x2424 and SLOPE_LBST = 0x7500 (ML `cs35l41-lib.c:28-29`). In the
  k1/k2/slope tables those decode to **LBST = 1.0 µH (1000 nH), CBST range 0 (≤19 µF)**. So use
  `boost-ind-nanohenry = <1000>` and `boost-cap-microfarad = <15>` [I: derived from reset defaults;
  the stock spk-prot `.bin` tuning may override the boost registers].
- Noise gate, class-H and `asp-sdout-hiz = <3>` properties are downstream-only (except
  `asp-sdout-hiz`, which mainline reads, ML `cs35l41.c:1053`).

### 1.3 ABOX UAIF pins [V]
All in the AUD pin controller `pinctrl@18C60000` (SP:144), which sits **inside the AUD power
domain**:

| Port | Pins (function 2) | Peer |
|---|---|---|
| MCLK out | gpb0-0 (`aud_codec_mclk`, SP:169). Not used on this board: the codec MCLK is PMU XCLKOUT | n/a |
| UAIF0 | gpb0-1..4 (SP:181) | codec AIF1 |
| UAIF1 | gpb1-0..3 (SP:193) | both CS35L40 (TDM) |
| UAIF2 | gpb1-4..7 (SP:205), shared with DSIF fn 3 | codec AIF3 |
| UAIF3 | gpb2-0..3 (SP:229) | BCM4375 PCM (BT SCO) |

ML already has the bank layout (`exynos9820_pin_banks1` gpb0[5], gpb1[8], gpb2[4],
`pinctrl-exynos-arm64.c:1252`), and `pinctrl@18c60000` probes on the phone. Its DT node has **no
`power-domains`** [V, PH: `/sys/firmware/devicetree/base/soc@0/pinctrl@18c60000` has no such
property]. With AUD off, any access to it is a bus error. Add `power-domains = <&pd_aud>` before
touching gpb*.

---

## 2. ABOX

### 2.1 Blocks and addresses (SD:1228-1620) [V]
- `abox@18c50000`: regs `sfr` 0x18c50000 (64 K), `sysreg` 0x18c10000, `sram` 0x18d00000
  (0x59000), `timer` 0x18ce0000. Clocks `PLL_OUT_AUD0`, `PLL_OUT_AUD1`, `DOUT_CLK_AUD_AUDIF`,
  `_DMIC`, `_BUS`, `_CNT`. `iommus = <&sysmmu_aud>`, `samsung,uaif-max-div = <512>`.
- `abox-gic@18cf0000`: a GIC-400, gicd 0x18cf1000, gicc 0x18cf2000. It reaches the AP GIC as
  `INTREQ__AUD_ABOX_GIC400` = SPI 46.
- RDMA0..11 at 0x18c51000 + 0x100·n. WDMA0..7 at 0x18c53000 + 0x100·n. UAIF0..3 at 0x18c50500 +
  0x10·n (CTRL0 +0, CTRL1 +4, STATUS +0xC). DSIF at 0x18c50550. ROUTE_CTRL0/1/2 at +0x40/44/48.
  SPUS_CTRL at +0x200. SPUM at +0x300. QCHANNEL_DISABLE at +0x38 (DS `abox/abox_soc.h:36-475`).
- UAIF clocks: `DOUT_CLK_AUD_UAIFn` + `GATE_ABOX_BCLKn`, pinctrl default/sleep per port.
- Internal CPUs: `abox_core0`/`core1` "CA32", PMU power 0x2d80/0x2e00, firmware
  `calliope_sram.bin`, `calliope_dram.bin`, `calliope_sram_2.bin`, `calliope_dram_2.bin`. Extra
  binaries: `SoundBoosterParam.bin`, `APDV_AUDIO_SLSI.bin`, `AP_AUDIO_SLSI.bin`,
  `APBargeIn_AUDIO_SLSI.bin`.
- SysMMU: `sysmmu@18E00000`, SPI 48/50, `GATE_SMMU_AUD`, secure base 0x18E10000, in pd_aud
  (`exynos9820-sysmmu.dtsi:100-117`).
- CMU_AUD at **0x18C00000**: PLL_AUD0/1 (CON 0x120/0x140), DIV_CLK_AUD_{AUDIF,BUS,BUSP,CNT,CPU_*,
  DMIC,DSIF,MCLK,PLL,UAIF0..3} at 0x1800-0x1838, MUX_CLK_AUD_UAIFn at about 0x1000-0x1018 (DS
  `cal-if/exynos9820/flexpmu_cal_local_exynos9820.h:8-34`, the pd save list). **ML has no CMU_AUD
  driver at all.** Only CMU_TOP's `mout/dout/gout_cmu_aud_cpu` and BUSC's LHM/LHS AUD gates exist
  (ML `clk-exynos9820.c:2638-2651, 3330, 3454, 3584`) [V].

### 2.2 What downstream does to start ABOX [V]
`abox_enable()` (DS `abox/abox.c:2371-2448`):
1. pd_aud on: PMU 0x15861900 bit 0, wait 0x15861904 (`aud_on[]`, flexpmu_cal_local:3-5). Then the
   **TZPC restore SMC**, `exynos_smc(0x82000410, 1 /*WAKEUP*/, 0x18c62000, 2 /*TZPC group*/)`
   (`pm-domains.dtsi:22 need_smc = <0x18c62000>`; `cal-if/pmucal_local.c:45-51`;
   `exynos-el3_mon.c:239-245`; `include/linux/smc.h:42`). Then restore the CMU_AUD save list.
   Before power-off it calls `exynos_pd_tz_save` = the same SMC with arg 0.
2. ABOX GIC enable and WDT. `sysreg + 0x32C (ABOX_SYSREG_MISC_CON) = 1` (abox_soc.h:684).
3. Enable `clk_cpu` and `clk_audif`. Pinctrl "default".
4. **Pad retention release:** PMU **0x1920 |= 0x800** (abox.c:76-78 + 2105-2115).
   PH: `devmem 0x15861920` = 0x00001000, so bit 11 is clear and the pads are retained [V].
5. Download firmware into SRAM/DRAM, power and boot the CA32, wait for the boot-done IPC.
6. **All PCM work runs through IPC** to the firmware: `PCM_SET_BUFFER`, `PCM_PLTDAI_HW_PARAMS`,
   `PREPARE`, `TRIGGER` (DS `abox/abox_rdma.c:1390-1600`), and the period/pointer events come
   back by IPC. The kernel itself writes only `RDMA_CTRL0.BURST_LEN` (abox_rdma.c:1444).
   **The firmware programs the RDMA buffer registers and the enable bit.**
7. Things the kernel does program directly, i.e. hardware a firmware-less driver can reuse as-is:
   UAIF CTRL0/CTRL1 (format, provider/consumer, width; `abox_if.c:248-312`), the ROUTE/SPUS/SIFS
   muxes as DAPM/kcontrols (`abox_cmpnt_v20.c`, e.g. "SPUS ASRCn" at 1465 and the SIFS frame
   counters `sifsx_cnt_val`/`abox_cmpnt_reset_cnt_val` at 4220-4300), QCHANNEL BCLK gates
   (`abox.c:241`, `abox_if.c:314-340`), and the **pointer**, which is read straight from
   `RDMA_STATUS.RBUF_OFFSET/CNT` (`abox_rdma.c:1604-1640`).

### 2.3 Can ABOX run firmware-less ("passthrough")? [I, with good evidence]
Likely yes, for plain PCM:
- RDMA, WDMA, SPUS/SIFS, ROUTE and UAIF are memory-mapped hardware with documented registers
  (`abox_soc.h:305-480`: RDMA CTRL0/1, BUF_STR/END/OFFSET, STR_POINT, VOL_FACTOR, STATUS). The AP
  already writes most of them in the downstream code, and the CA32 only sequences them.
- Interrupts: the ABOX GIC enumerates `IRQ_RDMA0..11_BUF_EMPTY` and `IRQ_WDMA0..7_BUF_FULL`
  (`abox_soc.h:686-740`), and `abox_gic_target(dev, irq, ABOX_GIC_AP)` can route any of them to the
  AP's CPU interface (`abox_gic.c:43-62`, enum `abox_gic.h:36-42`). Or skip interrupts: an hrtimer
  per period plus `RDMA_STATUS` polling, the same pointer math as downstream.
- Buffers: RDMA addresses are 32-bit (downstream IOVAs 0x91000000 + 1 MiB·n, `abox.h:38-41`).
  Leaving SysMMU_AUD in reset (bypass) and allocating buffers with DMA32 below 4 GiB should let RDMA
  read physical memory. Risk: the secure half of this SysMMU (0x18E10000, `sysmmu,secure-irq`)
  might block bypass traffic. Fallback: ML `exynos-iommu` [I: check the version register; 9820 is
  v7-ish].
- The CA32 stays powered off (do not touch PMU 0x2d80/0x2e00). No calliope firmware is needed.
- Losses: effects (SoundBooster, Dolby), the ASRC/firmware mixer features, VTS/hotword, offload.
  Without the amp DSP (`PCM Source = ASP`) there is no speaker protection either, so keep the
  volume limited until the spk-prot firmware loads (see 3.2).

### 2.4 Minimal mainline driver: scope and effort [I]

| Piece | Work | Size |
|---|---|---|
| CMU_AUD clock driver (`samsung,exynos9820-cmu-aud`) | PLL_AUD0 (and maybe AUD1), muxes, DIV_UAIF0..3, AUDIF, BUS, BUSP, CNT, gates incl. GATE_ABOX_BCLKn, GATE_SMMU_AUD. Register offsets in DS `cal-if/exynos9820/cmucal-sfr.c`/`cmucal-node.c`, PLL tables in `cmucal-vclklut.c` | 400-700 lines, 1-2 days |
| pd_aud TZPC SMC | add `samsung,tzpc-smc` (or similar) to ML `drivers/pmdomain/samsung/exynos-pm-domains.c`: SMC 0x82000410 (1, addr, 2) after power-on, (0, addr, 2) before power-off. **Nothing on mainline has ever powered on a need_smc domain on this phone**: every one is `off-0` in PH pm_genpd_summary | ~60 lines, but the riskiest step |
| PMU bits | CLKOUT0 SEL=TCXO: add a 9820 compatible to `clk-exynos-clkout.c` (exynos5 layout). Pad retention 0x1920 bit 11 in the ABOX driver via the PMU syscon | ~20 lines |
| ABOX platform + ASoC | map SFR/sysreg, MISC_CON. UAIF DAI driver (set_fmt/hw_params copied from abox_if.c). RDMA PCM component (alloc DMA32 buffer, write BUF_STR/END/OFFSET, set ENABLE, hrtimer or ABOX-GIC IRQ, pointer from STATUS). WDMA the same for capture. Fixed routing first (RDMA0 to SPUS to SIFS0 to UAIF0, RDMA1 to SIFS1 to UAIF1, UAIF0 to NSRC to WDMA0), DAPM later | 1500-2500 lines, 1-2 weeks, most of it in getting the first sample out of the UAIF |
| Machine | `simple-audio-card`/`audio-graph-card2` with three links, or a small "beyond" machine driver that sets the FLL and sysclk (FLL1 from MCLK1 26 MHz to 98.304 MHz, ASYNCCLK/FLL2 from AIF1BCLK) | 200-400 lines |

Total realistic estimate: **2-4 weeks of focused work**, gated by the TZPC SMC and by register
bring-up without documentation. The downstream sources are the only reference.

---

## 3. Mainline status

### 3.1 CS47L93 [V]
- `cirrus,cs47l93` is in ML `drivers/mfd/madera-core.c:382`, handled as a CS47L92-silicon variant
  (`:444`, `:657` uses `cs47l92_patch`/`cs47l92_devs`) and on the SPI ID table
  (`madera-spi.c:58,122`). The codec driver is `sound/soc/codecs/cs47l92.c` with CS47L93 cases in
  `madera.c:2312,2431,2956`.
- Kconfig on the phone build (`build-k/.config`): `MFD_MADERA=y`,
  `MFD_MADERA_SPI=y`, `MFD_CS47L92=y`, `PINCTRL_CS47L92=y`, `GPIO_MADERA=y`. **But
  `SND_SOC_CS47L92` and `SND_SOC_MADERA` are not set.** They are hidden symbols (Kconfig:1130,
  1402) that a machine driver has to select, or that `SND_SOC_ALL_CODECS` turns on.
- Firmware: the codec needs none for audio. Its DSP1 only ran `cs47l93-dsp1-trace.wmfw` (debug
  trace, BD:2052), which is on the vendor partition (9312 B).
- Missing in ML: madera jack detection (accdet/micd/hpdet), `cirrus,gpsw`, and the moisture
  detection.

### 3.2 CS35L40 [V]
- ML `sound/soc/codecs/cs35l41-i2c.c:65` matches **`cirrus,cs35l40`**. The chip ID is the same
  0x35A40 (`include/sound/cs35l41.h:747`). `SND_SOC_CS35L41_I2C=m`, `SND_SOC_CS35L41=m` and
  `WM_ADSP=m` are already built.
- Errata patches exist only for revid A0/B0/B2. Any other revid fails probe with -EINVAL (ML
  `cs35l41-lib.c:904-925`). Downstream patches only A0 (DS `cs35l41.c:2177-2186`), so the parts
  are probably A0 or B0/B2 [I: read the REVID at the first probe].
- Without firmware, `PCM Source = ASP` bypasses the DSP (ML `cs35l41.c:588, 670-672`). That plays,
  but with no speaker protection.
- Firmware on the vendor partition (PH, `/dev/sda26` mounted `ro,noload` under /run, then
  unmounted):
  `cs35l40-bot-dsp1-spk-prot.wmfw` (28380), `.bin` (4296), `-calib.bin` (1040), and the same three
  for `cs35l40-rcv-*`. ML names them `cs35l41-dsp1-spk-prot[-<system_name>][-<amp-name>].wmfw/.bin`
  (`dsp->part = "cs35l41"`, cs35l41.c:1089; `cirrus,subsystem-id` at :1182). Copy or symlink them
  under `/lib/firmware/cirrus/` with mainline names, using `sound-name-prefix` "Left"/"Right" and
  the amp-name [I: the exact naming has to be checked against cs_dsp/wm_adsp at load time].
  Calibration (`-calib.bin`, the stock factory per-device calibration via efs?) is a separate
  problem.

### 3.3 ABOX [V]
- **Nothing in ML.** `sound/soc/samsung/` has only the legacy i2s/pcm/spdif and board drivers. No
  CMU_AUD and no `sysmmu_aud` DT node.
- ABOX firmware on vendor (PH): `calliope_sram.bin` 50772, `calliope_dram.bin` 2253664,
  `calliope_sram_2.bin` 52300, `calliope_dram_2.bin` 227344, `calliope2.dt`, `AP_AUDIO_SLSI.bin`,
  `APDV_…`, `APBargeIn_…`, `SoundBoosterParam.bin`, `vts.bin`, and more. Only needed if the full
  downstream design is ported, which this plan avoids.

### 3.4 Live state on the phone [V, PH]
- `/proc/asound/cards`: no soundcards. AUD power domain `off-0`. PMU AUD_CONFIGURATION/STATUS = 0.
- CLKOUT0 (0x15860A00) = 0: enabled but SEL ≠ TCXO.
- AUD pads in retention (0x15861920 = 0x1000, bit 11 clear).
- The live DT has no spi@10470000 node, no hsi2c 0x109A0000 node and no CMU_PERIC1 gate clocks:
  ML's `peric1_cmu_info` registers **no clocks at all** (ML `clk-exynos9820.c:786-799`). PERIC0
  has the USI01 gates (`CLK_GOUT_PERIC0_USI01_USI_IPCLK/PCLK`, dt-bindings 94-95).
- `SND_USB_AUDIO=y`. hci0 exists (BCM4375 on UART) but was **DOWN** at check time ("failed to
  write update baudrate (-110)", frame reassembly errors). Someone else is working on it; I left it
  alone.

---

## 4. Cheaper paths

1. **USB-C audio (UAC dongle or headset): realistic now, near-zero kernel work** [I].
   `SND_USB_AUDIO=y`, xHCI platform + dwc3 dual-role + `usb-role-switch` with the max77705 Type-C
   port are in place (beyond0lte.dts:100-160), plus the OTG VBUS regulator `usb_otg_vbus`. Host
   mode has **not** been tested here: the role was `device` with a PC partner attached. The OTG
   boost is refused while CHGIN is present (DT comment), so the dongle cannot be charge-through.
   Test: unplug the PC, plug a USB-C to 3.5 mm DAC, check `cat /sys/class/usb_role/*/role` = host,
   `aplay -l`. Losing USB networking (ssh) while doing it is the main nuisance; ssh over Wi-Fi works.
2. **Bluetooth A2DP: realistic once hci0 works** [I]. A2DP and LE audio go over HCI/UART and need
   no ABOX (BlueZ + PipeWire). HFP/SCO is wired to ABOX UAIF3 by PCM (BD routing "BLUETOOTH
   SPK/MIC" = UAIF3). It could be redirected to HCI transport with a Broadcom vendor command
   (`HCI_VS_Write_SCO_PCM_Int_Param`) [I].
3. **Codec-only test, no ABOX: realistic, small** [I]. Needed: SPI2/USI01 DT node, ldo17m/ldo18m,
   CLKOUT0 = TCXO (new clkout compatible), madera MFD + `SND_SOC_CS47L92` with a trivial machine
   (or `snd-soc-dummy` CPU). Then route `HPOUT2L/R Input 1 = Tone Generator 1` with SYSCLK on
   FLL1 from MCLK1, and the 3.5 mm jack plays a tone. Mic to headphone loopback
   (`HPOUT2L Input 1 = IN3L`) proves the DMIC and headset mic. This proves the codec, its clocks,
   supplies, reset and IRQ, but not playback from Linux.
4. **Speaker/earpiece without ABOX: not realistic.** The amps take data only on UAIF1, which is
   ABOX's. They have no tone source of their own.

---

## 5. Staged bring-up plan

**Stage A: codec alive, no sound card (low risk, 1 day)**
1. DT: `usi@104700c0` (exynos850-usi, sysreg_peric0 0x100C, USI01 clocks) with a child
   `spi@10470000` (`samsung,exynos850-spi`, IRQ 384, pins gpp0-4/5/6 fn2 + gpp0-7 fn2 as CS). The
   mainline 9820 pinctrl dtsi has **no spi groups**, so add them.
2. Regulators `ldo17m` (1.8 V, always-on) and `ldo18m` (1.2 V) under `&acpm_ipc/pmic-1`.
3. Codec node `cirrus,cs47l93` (reset gpg1-0, IRQ gpa2-2 level-low, supplies as in 1.1). MCLK1 =
   a new `pmu_clkout` clock. Add `samsung,exynos9820-pmu` to `clk-exynos-clkout.c` with exynos5
   data.
4. Expect: `madera: CS47L93 revision …` in dmesg and regmap reads over SPI.
   - Risk: reset polarity.
   - Risk: USI01 can also be hsi2c_6 (SD lists "10470000.hsi2c" for hsi2c_6), so make sure nothing
     else claims USI01.
   - Risk: the SPI feedback delay (`samsung,spi-feedback-delay = 1` at 25 MHz). Start at 5 MHz.

**Stage B: codec tone and loopback (low risk, 1-2 days)**
- Enable `SND_SOC_CS47L92`. Write a tiny machine driver or simple-audio-card with a dummy CPU DAI
  that sets FLL1 (MCLK1 26 MHz to 98.304 MHz) and SYSCLK. Tone Generator to HPOUT2 into
  headphones. IN3L (DMIC1) to HPOUT2 loopback.
- Risk: madera needs a sound card to run DAPM. The dummy-CPU link has to exist.

**Stage C: amps alive (low-medium risk, 1 day)**
- `usi_17` (sysreg_peric1 0x104C = i2c) plus `i2c@109a0000` (exynosautov9-hsi2c). **Add the
  CMU_PERIC1 USI17_I2C gate/div clocks to ML `clk-exynos9820.c`**: none exist yet. Offsets are in
  DS `cal-if/exynos9820/cmucal-sfr.c`.
- Two `cirrus,cs35l40` nodes, 0x40/0x41, resets gph1-1/gph1-2, shared IRQ gpm24-0,
  `boost-peak-milliamp = <4100>`, `boost-ind-nanohenry = <1000>`, `boost-cap-microfarad = <15>`,
  `sound-name-prefix = "Right"/"Left"`, `cirrus,asp-sdout-hiz = <3>`.
- Expect a probe with revid. Risk: an unsupported revid (needs an errata table). Risk: a wrong
  boost config, which could overstress the boost when audio first plays. Probe alone is harmless.

**Stage D: AUD power domain plus CMU_AUD (high risk, the gating step)**
1. pd_aud power-on plus the **TZPC restore SMC** (0x82000410, 1, 0x18c62000, 2). Without it the
   first AUD SFR access most likely raises an SError and the phone resets. Test with
   ramoops/pstore armed.
2. Pinctrl@18c60000 gets `power-domains = <&pd_aud>`.
3. CMU_AUD driver. Bring up PLL_AUD0 [I: take the PLL rate from DS
   `cmucal-vclklut.c` (`pm-qos-aud` 1180000 kHz suggests a ~1.18 GHz CPU clock)]. Set DIV_UAIFn
   for BCLK.
4. Pad retention release, PMU 0x1920 |= 0x800.
5. Read back ABOX SFRs (UAIF STATUS, RDMA STATUS) to prove access.

**Stage E: firmware-less ABOX PCM (medium-high, 1-2 weeks)**
1. UAIF0 provider, i2s 48 kHz/16-bit, check BCLK and LRCK on the codec with FLL2 locked on
   AIF1BCLK (the codec reports the FLL lock IRQ). The alternative is codec as provider, which needs
   no UAIF BCLK divider.
2. RDMA0: DMA32 buffer, BUF_STR/END, ENABLE. Route RDMA0 to SPUS to SIFS0 to UAIF0 SPK (stock path
   names: `route-rdma7-to-sifs1` = "ABOX SPUS OUT7 = SIFS1", "ABOX UAIF1 SPK = SIFS1"). Pointer
   from STATUS, hrtimer period. Then headphones play.
3. UAIF1 dsp_a TDM 4×32 at 48 kHz to both amps. `PCM Source = ASP`, volume limited. Then the
   speaker plays.
4. WDMA: UAIF0 to NSRC to WDMA0 for capture (mics).
- Risks:
  - SysMMU bypass is not allowed (then port the IOMMU).
  - SIFS counter, ASRC or "try to asrc off" quirk details.
  - QCHANNEL gating: the BCLK q-channels must be disabled before the clocks run (`abox_if.c:314`).
  - The `pm-qos-aud` gear: the bus/CPU clocks may need a minimum rate.

**Stage F: polish**
- Amp spk-prot firmware with mainline names, so `PCM Source = DSP` works.
- Port madera jack detection.
- UCM profile for PipeWire.
- HFP via UAIF3 or SCO-over-HCI.
- Runtime PM for the AUD domain (TZPC save before off).

### Risk register
1. **TZPC SMC for pd_aud.** Untested on mainline, and no need_smc domain has been turned on yet.
   Failure means an SError or reset.
2. **CMU_AUD written from scratch.** Wrong PLL/DIV values give silent garbage clocks.
3. **ABOX register semantics without the firmware.** Unknown init that the firmware performs
   (SPUS/SIFS enables, ASRC bypass).
4. **SysMMU_AUD bypass versus the secure half.**
5. **Amp safety.** Boost coefficients are reset-default guesses, and there is no protection
   without the DSP.
6. **CMU_PERIC1 has no clocks** in ML, which blocks the amp I2C bus.
7. **The shared phone.** Other agents reboot it and run Bluetooth bring-up concurrently. Never
   rebind or modprobe on it outside a coordinated window.
