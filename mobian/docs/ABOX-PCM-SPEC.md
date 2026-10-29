# S10e ABOX without firmware: register-level PCM playback spec (RDMA -> UAIF1)

This extends `SOUND-SPEC.md` (read 2.1-2.4 there first; not repeated here).
Goal: a mainline ASoC driver plays PCM from AP memory through ABOX RDMA to UAIF1 (the CS35L40
speaker bus) with the CA32 cores off and no calliope firmware.

Abbreviations. DS = `src/lineage-kernel`. A = `DS/sound/soc/samsung/abox`.
SOC = `A/abox_soc.h`. CMP = `A/abox_cmpnt_v20.c`. CAL = `DS/drivers/soc/samsung/cal-if/exynos9820`.
[V] = read in code (cited). [I] = inferred. PH = given as verified on the live phone (mainline 7.2).

Correction to SOUND-SPEC 2.2 step 2: on 9820 `ABOX_SYSREG_MISC_CON` is **0x504**, not 0x32C
(SOC:679-681; 0x32C is the pre-9820 value). `aud-probe/s10e_aud_probe.c:62` reads sysreg+0x32C,
which is the access PH saw kill the SoC.

---

## 1. Version branch and register map

### 1.1 Which code is the reference [V]
| Fact | Source |
|---|---|
| `CONFIG_SND_SOC_SAMSUNG_ABOX_VERSION=0x20001`, `ABOX_V2=y` | `DS/arch/arm64/configs/exynos9820-beyond0lte_defconfig:3778,3780` |
| V2 builds `abox_cmpnt_v20.c` | `A/Makefile:3` |
| HW VERSION reg 0x4 = 0x02000100; bits [31:8] = 0x020001 = v2.0.1 (PH) | SOC:28, 43-44 |
| So every `#if ABOX_SOC_VERSION(2,0,1) > CONFIG` takes the `#else` (v2.0.1) branch; `(2,0,0) > CONFIG` false | SOC passim |

All field positions below are the v2.0.1 branch. Field macro: `L = (BASE + x*ITV + o) % 32` (SOC:17-24).

### 1.2 System block (SFR base 0x18c50000)
| Off | Register | Fields | Reset (9820) | Line |
|---|---|---|---|---|
| 0x000 | IP_INDEX | - | 0x41424F58 "ABOX" | SOC:27; abox_soc.c reg_defaults_9820 (1290-1812) |
| 0x004 | VERSION | [31:8] ver | 0x02000100 (PH) | SOC:28,43 |
| 0x010 | SYSPOWER_CTRL | [0] | 0 | SOC:29,47 |
| 0x014 | SYSPOWER_STATUS | [0] | 0 | SOC:30,51 |
| 0x020 | SYSTEM_CONFIG0 | none defined | 0x4444 | SOC:31 |
| 0x024/0x028 | REMAP_MASK/ADDR | - | 0xFFF00000 / 0x18D00000 | SOC:32-33 |
| 0x030 | DYN_CLOCK_OFF | [30:0] | 0x7FFFFFFF | SOC:34,55 |
| 0x034 | DYN_CLOCK_OFF1 | - | 0x1F | SOC:35 |
| 0x038 | QCHANNEL_DISABLE | bit = enum qchannel | 0 | SOC:36,59-63 |
| 0x040 | ROUTE_CTRL0 | SPUSM [27:24], DSIF [23:20], UAIF_SPK(x) [4x+3:4x] | 0 | SOC:65-77 |
| 0x044 | ROUTE_CTRL1 | NSRC(x) [4x+3:4x] | 0 | SOC:84-88 |
| 0x048 | ROUTE_CTRL2 | RSRC_CONN_TYPE bit 28+x, NSRC_CONN_TYPE bit 20+x, RSRC(x) [4x+3:4x] | 0 | SOC:89-104; CMP:1441-1458 |
| 0x050 | TICK_DIV_RATIO | - | 0x4E1 | |
| 0x054 | MO_CTRL | - | 0x0C72108C | |

enum qchannel (`A/abox.h:157-167`): 0 CCLK_CA7, 1 ACLK, 2 BCLK_UAIF0, **3 BCLK_UAIF1**, 4 UAIF2, 5 UAIF3,
6 DSIF, 7 CCLK_ATB, 8 CCLK_ASB.

Value enums [V]:
| Field | Values | Line |
|---|---|---|
| ROUTE UAIF_SPK(x) | 0 reserved, 1 SIFS0, **2 SIFS1**, 3 SIFS2, 4 SIFS3, 5 SIFS4, 12 SIFMS | CMP:3354-3358 |
| ROUTE SPUSM | 8-11 = UAIF0-3 | CMP:3290-3294 |
| ROUTE DSIF | 2-5 = SIFS1-4 | CMP:3386 |
| RSRC/NSRC | 1-5 = SIFS0-4, 8-11 = UAIF0-3 | CMP:3412, 3428 |

### 1.3 SPUS (playback mixer/router)
| Off | Register | Fields (v2.0.1) | Reset | Line |
|---|---|---|---|---|
| 0x200 | SPUS_CTRL0 | chain x=0..5, ITV 5: ASRC_EN bit 5x, OUT_SEL [5x+3:5x+1], IN_SEL bit 5x+4 | 0 | SOC:143-153 |
| 0x204 | SPUS_CTRL1 | MIXP_FORMAT [4:0], SIFSM_IN_SEL [11:8], SIFS_OUT_SEL(n) [8+4n+3:8+4n] n=1..4 | 0 | SOC:167-179 |
| 0x208 | SPUS_CTRL2 | MIXP_LD_FLUSH b1, MIXP_FLUSH b0 (v10 only) | 0 | SOC:181-185 |
| 0x20C | SPUS_CTRL3 | SIFSM_FLUSH b2, SIFS_FLUSH (v10 only) | 0 | SOC:187-191 |
| 0x210 | SPUS_CTRL4 | ASRC_ID idx0-3 at [31:16], 4 bit each, L=(16+4x)%32 | 0x32100000 | CMP:1569-1594 |
| 0x214 | SPUS_CTRL5 | ASRC_ID idx4-11 | 0xBA987654 | same |
| 0x218 | SPUS_CTRL_FC1 | chains 6..11, same layout as CTRL0 with x-6 | 0 | CMP:3172-3184, 3257-3269 |
| 0x220+4x | SBANK_RDMA(x) | SIZE [29:20], STR [13:4] (16-byte units) | 0 | SOC:114-116, 297-302 |
| 0x250+4x | SBANK_ASRC(x) | same | 0 | SOC:122,127 |
| 0x280 | SBANK_MIXP | same | 0 | SOC:123,129 |
| 0x290+4x (x<=3), 0x28C (x=4) | SIFS_CNT(x) | CNT_VAL [23:0] | 0 | SOC:124,130-131, 209-214 |

Chain values [V]: IN_SEL 0 = RDMA, 1 = SIFSM (CMP:3128). OUT_SEL **0 = SIFS1**, 1 = SIFS0, 2 = SIFS2,
4 = SIFS3, 6 = SIFS4, 3/5 reserved (CMP:3209-3212). SIFS_OUT_SEL(n) 0..11 = SPUS OUT0..11
(CMP:3301-3325). SIFS0 is a MIXER (MIXP) of every chain whose OUT selects SIFS0 (CMP:3644);
SIFS1-4 are muxes. RDMAn is hard-wired to SPUS INn (CMP:4945-4948).

**Consequence of reset values [V]:** RDMA0 -> chain 0 IN=RDMA -> OUT=0=SIFS1 -> SIFS1_OUT_SEL=0=OUT0.
The only route write needed for RDMA0 -> UAIF1 is **ROUTE_CTRL0[7:4] = 2**. ASRC off by default.

### 1.4 UAIF(x) = 0x500 + 0x10*x (UAIF1 = 0x510)
| Off | Register | Fields | Reset | Line |
|---|---|---|---|---|
| +0x0 | CTRL0 | START_FIFO_DIFF_MIC [31:28], START_FIFO_DIFF_SPK [27:24], DATA_MODE b4, IRQ_MODE b3, MODE b2 (1 = ABOX is clock provider), MIC_ENABLE b1, SPK_ENABLE b0 | 0x01000010 | SOC:305-331 |
| +0x4 | CTRL1 | FORMAT [28:24], BCLK_POLARITY b23, WS_MODE b22 (1 = DSP_A), WS_POLAR b21, SLOT_MAX [20:18], SBIT_MAX [17:12], VALID_STR [11:6], VALID_END [5:0] | 0 | SOC:333-356 |
| +0xC | STATUS | ERROR_OF_MIC b1, ERROR_OF_SPK b0 | 0 | SOC:358-363 |
DSIF CTRL 0x550: MODE b3, BCLK_POL b2, ORDER b1, ENABLE b0. TIMER(x) 0x600+0x20x (SOC:387-405).

FORMAT = `abox_get_format()` = (ch-1) | code<<3, code 1/2/3 = 16/24/32 bit (`A/abox.h:419-441`).

### 1.5 RDMA(x) = 0x1000 + 0x100*x (SOC:408-450)
| Off | Register | Fields defined | Reset |
|---|---|---|---|
| +0x00 | CTRL0 | BURST_LEN [22:19], ENABLE b0 | 0x00400000 (BURST_LEN 8) |
| +0x04 | CTRL1 | **none defined** | 0 |
| +0x08 | BUF_STR | none (address) | 0 |
| +0x0C | BUF_END | none | 0 |
| +0x10 | BUF_OFFSET | none | 0 |
| +0x14 | STR_POINT | none | 0 |
| +0x18 | VOL_FACTOR | [23:0]; max 0xFFFFFF (`A/abox_rdma.c:103`) | 0 |
| +0x1C | VOL_CHANGE | none | 0 |
| +0x20 | SBANK_LIMIT | none | 0 |
| +0x30 | STATUS | PROGRESS b31, RBUF_OFFSET [27:12] (16-byte units), RBUF_CNT [11:0] | 0 |

The AP code writes only CTRL0.BURST_LEN and VOL_FACTOR (the latter only for ION RDMA,
`abox_rdma.c:2023-2030`). Everything else in this table is written by the firmware.

### 1.6 Other blocks (address only, not needed)
SPUM 0x300-0x34C (SOC:217-237); SPUS ASRC(x) 0x2000+0x100x, fields SOC:520-612; WDMA(x)
0x3000+0x100x (SOC:469); SPUM ASRC 0x4000; CA32 0x5000/0x5080/0x5100; APF 0x7000;
MAX_REGISTERS 0x7170.

### 1.7 ABOX GIC (GIC-400 inside ABOX; DT `exynos9820.dts:1228-1232`)
gicd 0x18cf1000, gicc 0x18cf2000, output = AP SPI 46 (`INTREQ__AUD_ABOX_GIC400`,
`dt-bindings/.../exynos9820.h:64`), rising edge (`A/abox_gic.c:275`). 103 SPIs (`A/abox_gic.h:17-19`).
CPU-interface targets: CORE0=0, CP=1, **AP=2**, CORE1-3=3-5 (`abox_gic.h:36-42`).

| INTID | Source (SOC:687-805) |
|---|---|
| 0x0-0xF | SGIs; firmware-relayed RDMA0-3 BUF_EMPTY = 0xB-0xE, WDMA0/1 BUF_FULL = 8/9 |
| 0x20/0x21 | CA32_0/1 |
| 0x22-0x25 | TIMER0-3 |
| **0x26-0x31** | **RDMA0-11 BUF_EMPTY** (RDMA0 = 38) |
| 0x32-0x3D | RDMA FADE_DONE |
| 0x3E-0x45 / 0x46-0x4D | WDMA0-7 BUF_FULL / FADE |
| 0x4E-0x55 | UAIF0-3 SPK/MIC (UAIF1 SPK = 0x50) |
| 0x56 | DSIF_OVF |
| 0x57-0x5C | PCM_COUNTER0-5 |
| 0x5D / 0x5E | IRQC / WDT |
| **0x5F-0x6A** | **RDMA0-11 ERR** |
| 0x6B-0x72 | WDMA ERR |

Handler model [V] (`abox_gic.c:133-162`): AP reads GICC_IAR (0x0C), writes EOI (0x10) (plus DIR
0x1000 for SGIs). Target set per byte in ITARGETSR (`abox_gic.c:43-62`): value `1<<2`.

### 1.8 SYSREG_AUD (0x18c10000) - every downstream write
| Write | Where | Needed for firmware-less? |
|---|---|---|
| MISC_CON 0x504 = 1 | `A/abox.c:2385` (abox_enable) | **Skip** [I]. Meaning undocumented; written before the CA32 boot, most likely CA32/bus config |
| L2_CACHE_CON 0x0 | only with SHARE_VTS_SRAM quirk, not set on 9820 | No |
| DRCG_EN 0x104, MEMCLK 0x108 b0 = 0, SNPS_EMA 0x300 | pd sequences `CAL/flexpmu_cal_local_exynos9820.h:99-101`, `flexpmu_cal_system...:24,79` | **Skip** [I]; reset values are what HW powers up with |

**Design rule: the proposed driver performs zero SYSREG_AUD accesses (read or write).** Do not
map it. PH proves SFR is readable without any of these writes, so none is needed for bus access.

---

## 2. Downstream write sequence for the speaker route (RDMA7 -> SIFS1 -> UAIF1)

Stock mixer (quoted in SOUND-SPEC from the phone): `ABOX SPUS OUT7 = SIFS1`, `ABOX UAIF1 SPK = SIFS1`.
Machine links: RDMA7 FE with DPCM trigger POST_PRE/PRE_POST (`DS/sound/soc/samsung/beyond_madera.c:930-943`),
UAIF1 BE link 1172-1185, `uaif1_init` 678-711. Board DT: uaif1 dsp_a + bitclock-inversion
(`exynos9820-beyond0lte_common.dtsi:501-510`).

Order as executed at open -> hw_params -> prepare -> trigger. "AP" = kernel register write, "FW" = IPC.

| # | Step | Writes | Source | |
|---|---|---|---|---|
| 0 | abox_enable (once, runtime-resume) | GICD_CTLR=1, IGROUPR0-3=0; WDT IRQ -> AP; SYSREG 0x504=1; clk_cpu/clk_audif on; pinctrl default; PMU 0x1920 \|= 0x800; regcache_sync (incl. patch_9820 SBANK values, §5); GICC_PMR=0xFF, GICD_CTLR=3, IPRIORITYR[0..39]=0x10101010, GICC_CTLR=3; FW download; SYSPOWER_CTRL=1 + poll STATUS b0; CA32 on | `abox.c:2371-2448`, `abox_gic.c:163-215`, `abox.c:931-995` | V |
| 1 | DAPM mux puts | SPUS_CTRL0 chain7 is in FC1: FC1 OUT_SEL(1) [8:6] = 0 (SIFS1); SPUS_CTRL1 SIFS1_OUT_SEL [15:12] = 7; ROUTE_CTRL0 [7:4] = 2 | CMP:3172-3184, 3301-3325, 3354 | V |
| 2 | UAIF1 startup | clk_enable bclk + bclk_gate (HWACG gate enable is a no-op, `drivers/clk/samsung/composite.c:1375-1376`) | `abox_if.c:40-49` | V |
| 3 | UAIF1 set_fmt (DSP_A, IB_NF, CBS_CFS) | CTRL1 WS_MODE=1, BCLK_POL=0, WS_POL=0; CTRL0 MODE=1 | `abox_if.c:248-312` (269, 284, 301) | V |
| 4 | prepare (machine) -> set_tristate(0) | QCHANNEL_DISABLE b3 = 1 | `beyond_madera.c:210-211`, `abox_if.c:316-344`, `abox.c:241-246` | V |
| 5 | UAIF1 hw_params | clk_set_rate(bclk, rate*ch*width); CTRL1 SBIT_MAX=w-1, VALID_STR/END=w-1 (0 for I2S), SLOT_MAX=ch-1, FORMAT; reset_cnt_val -> SIFS_CNT(1) 0x294 = 0 | `abox_if.c:346-417` (372, 401, 407, 414); CMP:4233-4272 | V |
| 6 | SIFS1 format/rate fixup | IPC CONFIGMSG SET_SIFS1_RATE/FORMAT to FW (only SIFS0 also writes MIXP_FORMAT) | CMP:431-486; `include/sound/samsung/abox_ipc.h:218-253` | V |
| 7 | ASRC decision | chain7 ASRC_EN = 1 iff rate/width differs from SIFS1 target; then ASRC CTRL/PARA/FILTER from AP | CMP:2968-3011, 2835-2933 | V |
| 8 | RDMA7 hw_params | SBANK_RDMA(7) SIZE = 128 B if period <= 10 ms else 512 B (<<16); CTRL0 BURST_LEN = 1 if sbank < 128 else 8; IPC PCM_SET_BUFFER {IOVA 0x91000000+7 MiB, period_bytes, periods}; IPC PCM_PLTDAI_HW_PARAMS {rate, width, ch} | CMP:4160-4218; `abox_rdma.c:1390-1489` (1438-1468) | V |
| 9 | RDMA7 prepare | memset buffer; IPC PREPARE | `abox_rdma.c:1521-1555` | V |
| 10 | trigger start | FE: IPC TRIGGER 1 (FW programs and enables RDMA7); BE: UAIF1 CTRL0 SPK_ENABLE=1 | `abox_rdma.c:1557-1602`; `abox_if.c:446` | V |
| 11 | running | pointer: IPC-reported IOVA, or RDMA STATUS path (§3.3); period_elapsed from SGI 0xB-0xE relay | `abox_rdma.c:1604-1651`; `abox.c:1698-1751` | V |
| 12 | stop | IPC TRIGGER 0; hw_free: IPC then poll STATUS.PROGRESS == 0 up to 40 ms; UAIF shutdown: usleep 600-1200 us, SPK_ENABLE=0; set_tristate(1) -> QCH b3 = 0 | `abox_rdma.c:1372-1388`; `abox_if.c:480`; `beyond_madera.c:277-278` | V |

Concrete UAIF1 values for the stock speaker config 4 ch x 32 bit @ 48 kHz (config kcontrols
"UAIF1 Width/Channel/Rate", `abox_if.c:578-585, 790-870`):
- CTRL1 = FORMAT 0x1B<<24 | WS_MODE b22 | SLOT_MAX 3<<18 | SBIT_MAX 31<<12 | VALID_STR 31<<6 | VALID_END 31
  = **0x1B4DF7DF** [V arithmetic].
- CTRL0 running = 0x01000010 | MODE | SPK_ENABLE = **0x01000015**.
- BCLK = 48000*4*32 = **6.144 MHz** (matches amp clock `common.dtsi:211`).

---

## 3. RDMA: what the firmware does, and the AP-side replacement

### 3.1 What the firmware receives [V]
Only `PCMTASK_SET_BUFFER {phyaddr, size=period_bytes, count=periods}`, `HW_PARAMS {sample_rate,
bit_depth, channels}`, `PREPARE`, `TRIGGER on/off` (`abox_ipc.h:30-99`). From that it must write
RDMA CTRL0 ENABLE, CTRL1, BUF_STR/END/OFFSET, STR_POINT, probably SBANK_LIMIT/VOL; it raises
SGI 0xB+n per period to the AP. No firmware source or RDMA dump exists locally, so the
semantics of CTRL1 and BUF_* are **not in any code we have**.

### 3.2 Proposed AP sequence (RDMA0 -> UAIF1, S32_LE 4ch 48 kHz)
Preconditions: pd_aud on + TZPC SMC (SOUND-SPEC 2.2 step 1); CMU_AUD per §4; CA32 off;
SysMMU_AUD in reset (bypass). Never touch SYSREG_AUD.

| # | Write (offset from 0x18c50000 unless noted) | Value | |
|---|---|---|---|
| 1 | Read IP_INDEX / VERSION | 0x41424F58 / 0x02000100, else abort | V (PH) |
| 2 | PMU 0x15861920 \|= 0x800 (pad retention release) | | V (`abox.c:77-78, 2105-2115`) |
| 3 | SYSPOWER_CTRL 0x10 = 1; poll 0x14 b0 = 1 (timeout ~ms) | | V (`abox.c:956-995`); purpose (SRAM/DRAM path power) [I] |
| 4 | SBANK patch: SBANK_RDMA(n) 0x220+4n = (512<<16)\|(n*512) for n = 0..11; SBANK_MIXP 0x280 = (128<<16)\|0x3000; ASRC banks as patch_9820 | | V (`abox_soc.c:1818-1873`) |
| 5 | Optional: SBANK_RDMA(0) SIZE = 128 if period <= 10 ms | (128<<16)\|0 | V (CMP:4160-4218) |
| 6 | ROUTE_CTRL0 [7:4] = 2 (UAIF1 <- SIFS1) | 0x00000020 | V |
| 7 | Leave SPUS_CTRL0/1/FC1 = 0 (RDMA0 -> SIFS1, ASRC off) | | V (reset) |
| 8 | SIFS_CNT(1) 0x294 = 0 | 0 | V (CMP:4233-4272) |
| 9 | UAIF1 CTRL1 0x514 | 0x1B4DF7DF | V |
| 10 | UAIF1 CTRL0 0x510 (MODE=1, SPK off) | 0x01000014 | V |
| 11 | QCHANNEL_DISABLE 0x38 \|= BIT(3) (and BIT(1) ACLK, see risks) | | V for b3, I for b1 |
| 12 | RDMA0 CTRL0 0x1000 BURST_LEN | 8<<19 (or 1<<19 if sbank < 128 B) | V |
| 13 | RDMA0 BUF_STR 0x1008 = buffer phys (DMA32, page-aligned) | | I |
| 14 | RDMA0 BUF_END 0x100C = phys + buffer_bytes (try -16 if it underruns at wrap) | | I |
| 15 | RDMA0 BUF_OFFSET 0x1010 = period_bytes (IRQ / RBUF_CNT granularity) | | I |
| 16 | RDMA0 STR_POINT 0x1014 = phys (or 0) | | I |
| 17 | RDMA0 CTRL1 0x1004: leave 0 first; if output is silent or wrong-width, this is the format field (expect ch/width encoding like UAIF FORMAT) | | I |
| 18 | RDMA0 VOL_FACTOR 0x1018 = 0x800000 (unity if Q1.23) or 0xFFFFFF | | I (reset 0 may mute) |
| 19 | Optional IRQ: ABOX GICD ITARGETSR byte 38 (0x18cf1826) = 0x04; ISENABLER1 (0x18cf1104) = BIT(6); GICD_CTLR = 1; GICC_PMR = 0xFF; GICC_CTLR = 1; AP SPI 46 rising | | V mechanics (`abox_gic.c:43-62,163-215`), I that HW RDMA IRQ fires per BUF_OFFSET |
| 20 | RDMA0 CTRL0 \|= ENABLE; poll STATUS b31 PROGRESS = 1 | | V field, I order |
| 21 | UAIF1 CTRL0 \|= SPK_ENABLE (0x01000015) | | V |

Stop: RDMA CTRL0 ENABLE=0; poll PROGRESS=0 (<= 40 ms, `abox_rdma.c:1372-1388`); usleep 600-1200 us;
UAIF1 SPK_ENABLE=0 (`abox_if.c:480`); QCH b3 = 0. [V order]

### 3.3 Pointer [V] (`abox_rdma.c:1604-1651`)
If STATUS.PROGRESS: `offset = RBUF_OFFSET<<4` (16-byte units), `count = RBUF_CNT` (used only when
period <= 4096 B), offset rounded down to a period, `pointer = offset + count`. Offset is relative
to BUF_STR [I]. Drive period_elapsed from an hrtimer at period rate (safe first) or from INTID 38.

### 3.4 Format matching
Make the RDMA stream equal to the UAIF format (S32_LE, 4 ch, 48 kHz; or 2 ch with UAIF1
SLOT_MAX=1) so the ASRC stays off and no SIFS rate/format config (a firmware-only path, §2 step 6)
is needed [I]. Downstream forces ch >= 2, width >= 16 (CMP:4624-4823).

---

## 4. Clocks (CMU_AUD base 0x18c00000, CAL `cmucal-sfr.c` / `cmucal-node.c`)

### 4.1 Target rates [V]
| Clock | Rate | Source |
|---|---|---|
| PLL_AUD0 | 1179648040 Hz (48k family) | `A/abox.h` AUD_PLL_RATE_HZ_FOR_48000; `abox.c:643-728` |
| DOUT_CLK_AUD_AUDIF | 73.728 MHz = 384000*96*2 (uaif-max-div 512, DT 1256) = PLL/16 | `abox.c:701-711, 722` |
| DOUT_CLK_AUD_UAIF1 | 6.144 MHz = AUDIF/12 | `abox_if.c:372` |
| BUS / CNT / DMIC | never set by kernel (only fetched, `abox.c:2867-2877`) | |

### 4.2 Registers
| Reg | Off | Fields | Proposed |
|---|---|---|---|
| PLL_CON0_PLL_AUD0 | 0x120 | ENABLE b31, STABLE b29 (RO), M [25:16], P [13:8], S [2:0] (`cmucal-sfr.c:46-47, 2711-2716`) | sboot value **0xA02D0110** = M 45, P 1, S 0 (`kit/sound/cmu-aud-sboot.txt:6`) |
| PLL_CON3_PLL_AUD0 | 0x12C | K [15:0] | sboot **0x5EFF** (K=24319); 26 MHz*(45+24319/65536)/1 = 1179.648 MHz [V arithmetic]. Table alt M181 P4 K31740 (`cmucal-node.c:16`) gives the same |
| PLL_CON1/2/4 | 0x124/0x128/0x130 | | sboot 0x0000F300 / 0x10000001 / 0x8000000F (copy as-is) |
| MUX_CLK_AUD_UAIF1 | 0x1010 | SELECT b0: 0 = DIV, 1 = CLKIO pad | 0 |
| DIV_CLK_AUD_AUDIF | 0x1800 | RATIO [8:0], BUSY b16 | 15 (/16) [I: ratio-1 convention] |
| DIV_CLK_AUD_BUS | 0x1804 | [2:0] | 3 (/4, ~295 MHz) [I] |
| DIV_CLK_AUD_BUSP | 0x1808 | [1:0] | 1 [I] |
| DIV_CLK_AUD_CNT | 0x180C | [8:0] | 3 (AUDIF/4) [I] |
| DIV_CLK_AUD_UAIF1 | 0x1830 | [8:0] | 11 (/12) [I convention] |
| GATE ABOX ACLK / ACLK_IRQ / BCLK_CNT / BCLK_UAIF1 / SMMU | 0x2010 / 0x2014 / 0x2018 / 0x2024 / 0x20B4 | CG_VAL b21, MANUAL b20 | MANUAL=1, CG_VAL=1 |
| QCH_CON ABOX ACLK / BCLK1 / CNT | 0x3020 / 0x3028 / 0x3040 | ENABLE b0, CLOCK_REQ b1, IGNORE_FORCE_PM b2, EXPIRE [25:16] (`cmucal-sfr.c:1932-1941`) | ENABLE=0 or CLOCK_REQ=1 (force on) [I] |
| RSTNSYNC UAIF1 | 0x20A8 | gate | on [I] |

Parents (`cmucal-node.c:1242-1256`): AUDIF/BUS/PLL <- PLL_AUD0; UAIFn/DMIC/CNT/MCLK <- AUDIF; BUSP <- BUS.
Wait for PLL STABLE (b29) and each DIV BUSY (b16) = 0. PH: CMU_AUD at reset, PLL_AUD0 off.

---

## 5. Init: replicate or skip

| Downstream item | Source | Decision |
|---|---|---|
| pd_aud + TZPC SMC 0x82000410 | SOUND-SPEC 2.2 | **Replicate** (done, PH) |
| CMU_AUD save-list restore | `CAL/flexpmu_cal_local_exynos9820.h:8-47` | Replace by §4 writes |
| SYSREG MISC_CON 0x504 = 1 | `abox.c:2385` | **Skip** (fatal-access risk; see §1.8) |
| SYSREG DRCG/MEMCLK/EMA in pd sequence | flexpmu_cal_local:99-101 | **Skip** |
| Pad retention PMU 0x1920 b11 | `abox.c:77-78` | **Replicate** (PH: still retained) |
| Pinctrl aud_i2s1_bus | DT `exynos9820.dts:1520` | **Replicate** |
| SYSPOWER_CTRL/STATUS | `abox.c:931-995` | **Replicate** |
| patch_9820 SBANK layout | `abox_soc.c:1818-1873` | **Replicate** (reset SBANKs are 0 = no SRAM bank) |
| SPUM_CTRL0 = FUNC_CHAIN_RSRC_RECP_MASK | same | Skip (capture only) |
| ABOX GIC init | `abox_gic.c:163-215` | Replicate only if IRQ mode used |
| WDT IRQ, timer check | `abox.c:206-210, 2407-2411` | Skip (WDT is CA32) |
| FW download, CA32 power, IPC | `abox.c:2415-2432` | **Skip** |
| SysMMU_AUD | `exynos9820-sysmmu.dtsi:98-116` | Skip (bypass); fallback exynos-iommu, then BUF_STR = IOVA |
| "try to asrc off" quirk | `abox.c:219-239` | No-op downstream, skip |
| DYN_CLOCK_OFF / SYSTEM_CONFIG0 | reset 0x7FFFFFFF / 0x4444 | Leave; first suspect if RDMA PROGRESS never sets |

---

## 6. Risks

| # | Risk | Mitigation / test |
|---|---|---|
| R1 | RDMA CTRL1 / BUF_* / STR_POINT / SBANK_LIMIT semantics unknown (FW-only) | **Key experiment:** boot stock/Lineage, play a 48k tone, raw-read 0x18c51000-0x18c51BFF (RDMA0-11) and 0x18c50000-0x18c50300 via /dev/mem or a debugfs regmap dump; read-only. Second source: later Samsung ABOX trees (Exynos 990/2100 `abox_dma.c` program RDMA from the AP) [I, unverified] |
| R2 | SysMMU_AUD in reset may not be bypass, or the secure half (0x18E10000) blocks | Watch RDMA ERR INTID 0x5F + bus faults; fallback: mainline exynos-iommu |
| R3 | Skipping SYSREG MISC_CON breaks the DMA bus path | No safe test: any SYSREG access killed the SoC (PH). Only if all else fails, try reading 0x18c10504 alone (0x32C was the killer; 0x504 is untested) |
| R4 | VOL_FACTOR reset 0 = mute | Write 0x800000 then 0xFFFFFF |
| R5 | DYN_CLOCK_OFF all-ones gates blocks needed without FW | Try QCHANNEL_DISABLE b1 (ACLK) and DYN_CLOCK_OFF = 0 |
| R6 | BUS/ACLK too low or ungated (DVFS LUT columns unclear, `cmucal-vclk.c:75-89`) | Read back DIV regs; start at /4 |
| R7 | CMU QCH/HWACG keeps BCLK1 off (downstream gate enable is a no-op) | Force QCH CLOCK_REQ; scope BCLK at amp |
| R8 | ABOX GIC: AP must be interface 2; bank selection by bus master [I] | Use hrtimer polling first |
| R9 | RBUF_CNT/OFFSET pointer wrong -> glitches | Compare with hrtimer-estimated position |
| R10 | Amp safety: CS35L40 without spk-prot firmware | Low digital volume, short tones (SOUND-SPEC 3.2) |
| R11 | Stop without PROGRESS drain may wedge RDMA | Always poll PROGRESS = 0 before freeing the buffer |
| R12 | Register writes before SYSPOWER STATUS = 1 may hang the bus | Poll with timeout; abort on 0 |
