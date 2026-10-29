# ABOX "calliope" firmware on Exynos 9820 (Galaxy S10e) — mainline driver spec

Target: mainline Linux 7.2, a driver that boots the ABOX Cortex-A32 cores with the stock
calliope firmware and lets the firmware (plus the VSS code the CP ships) carry cellular
call audio.

Sources (all paths relative to their tree root):

- **Stock**: `src/lineage-kernel` — `sound/soc/samsung/abox/*`,
  `include/sound/samsung/abox_ipc.h`, `arch/arm64/boot/dts/exynos/exynos9820*.dtsi/dts`,
  `arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi`.
- **User driver**: `src/linux-9820/sound/soc/samsung/exynos9820-abox.c`
  (firmware-less, 792 lines).
- **Kit**: `kit/callaudio/{fw/,mixer_paths.xml}`,
  `kit/modem/{MODEM-SPEC.md,android-dmesg-cbd-boot.txt}`.

`#ifdef`s were resolved against `arch/arm64/configs/exynos9820-beyond0lte_defconfig`:
`SND_SOC_SAMSUNG_ABOX=y`, `SND_SOC_SAMSUNG_ABOX_VERSION=0x20001` (v2.0.1),
`SND_SOC_SAMSUNG_ABOX_V2=y` (so `abox_cmpnt_v20.c` is the component file),
`SND_SOC_SAMSUNG_ABOX_DRAM_SIZE=0x1C00000`, `SOC_EXYNOS9820=y`, `MCU_IPC=y`, `SHM_IPC=y`,
`LINK_DEVICE_SHMEM=y`, `EXYNOS_IOMMU=y`. Not set: `MANUAL_SECURITY_CHANGE`, `EMULATOR`.
`GIC_IS_SECURE_FREE` is `#define`d inside `abox_gic.c` itself.

Notation: **[C]** = certain, read from the cited line(s). **[I]** = inferred (reasoning,
naming, firmware strings, or a stock log); verify before relying on it.

---

## 1. Memory, firmware loading, IOMMU

### 1.1 Physical regions (DT, `exynos9820.dts`)

| What | PA | Size | Source |
|---|---|---|---|
| ABOX SFR | 0x18c50000 | 0x10000 | dts:1235-1259 `reg` "sfr" [C] |
| SYSREG_AUD | 0x18c10000 | — | dts "sysreg" [C] |
| ABOX SRAM | 0x18d00000 | 0x59000 | dts "sram" [C] |
| ABOX timer | 0x18ce0000 | — | dts "timer" [C] |
| ABOX GIC-400 GICD | 0x18cf1000 | 0x1000 | dts:1228-1233 [C] |
| ABOX GIC-400 GICC | 0x18cf2000 | 0x1004 | dts:1228-1233 [C] |
| core0 GPR | 0x18c55000 | 0x80 | dts:1261-1280 [C] |
| core1 GPR / status | 0x18c55080 / 0x18c55100 | — | dts:1282-1301 [C] |
| SysMMU_AUD | 0x18E00000 | 0x9000 | exynos9820-sysmmu.dtsi:98-117 [C] |
| CMU_AUD | 0x18c00000 | — | cmucal (see 2.3) [C for base] |
| PMU | 0x15860000 | — | exynos9820.dtsi [C] |
| Modem shmem (modem_if) | 0xF0000000 | 0xB900000 | exynos9820-rmem_s5000ap.dtsi:16-19 [C] |
| VSS region | 0xF7800000 | 0x800000 | modem-s5000ap-sipc-pdata.dtsi:405-419 (`vss_offset 0x7800000`) [C] |
| VSS "vparam" | 0xFB700000 | 0x200000 | same, `vparam 0xB700000` [C] |
| VSS magic word | 0xF7F00000 | 4 | VSS + `magic-offset 0x700000` (dts abox_vss 1586-1590) [C] |

Correction to the brief: the magic offset on 9820 is **0x700000** (DT overrides the
`abox_vss.c:22` default of 0x500000). [C]

`shm_ipc.c:850-960` can override the shmem layout from the CP boot image's MEMn table;
with the user's `cpboot.py` the static DT numbers above hold as long as the CP image is the
same one in `kit/modem/MODEM-SPEC.md` (VSS TOC entry b_off 0x263a1c0, load 0x47800000 CP
view, size 0x5dfbc0). [I: that the CP view 0x47800000 == AP 0xF7800000; consistent with
modem_if base 0xF0000000 mapped at CP 0x40000000]

### 1.2 DRAM the AP must provide

- **Firmware DRAM**: `CONFIG_..._ABOX_DRAM_SIZE` = 0x1C00000 (28 MiB),
  `dmam_alloc_coherent` at `abox.c:2813-2820`. [C]
- **Private DRAM**: 8 MiB, `abox.c:2823-2830`. [C]
- Debug dump (`abox_debug`, memory-region `abox_rmem`, exynos9820-rmem.dtsi:158-163,
  0x3c00000 dynamic) — optional, only for crash dumps. [C]

Allocate both from a CMA/reserved-memory region or `dma_alloc_coherent`; they need
not be physically contiguous with anything, only IOVA-contiguous. Use write-combine or
coherent mapping; the firmware and AP share queues in this memory. [I]

### 1.3 IOVA map the firmware expects (`abox.h:36-52`, probe `abox.c:2722-3018`)

| ABOX VA (= IOVA) | Maps to | Size | Where |
|---|---|---|---|
| < 0x80000000 | **SRAM**, not SysMMU: REMAP_MASK 0xFFF00000 / REMAP_ADDR 0x18D00000 (reset defaults, `abox_soc.c:1290+`) | 0x59000 | [C] defaults; [I] that addr 0 = SRAM 0 |
| 0x80000000 | firmware DRAM | 28 MiB | abox.c:2813-2820 [C] |
| 0xE0000000 | private DRAM | 8 MiB | abox.c:2823-2830 [C] |
| 0xA0000000 | VSS PA 0xF7800000 | 0x800000 | abox.c:2833-2835 [C] |
| 0xA1000000 | vparam PA 0xFB700000 | 0x200000 | abox.c:2841 [C] |
| 0x10000000 | identity, 1 page (chipid block) | 4 KiB | abox.c:2844 [C] |
| 0xD0000000 | dump (abox_rmem) | optional | abox_dbg.c:505 [C] |
| 0x91000000 + id*1M | RDMA<id> PCM buffers | per stream | abox.h [C] |
| 0x92000000 + id*1M | WDMA<id> PCM buffers | per stream | abox.h [C] |
| 0x93000000 / 0x94000000 | compress / vdma | — | abox.h [C] |

Stock calls `iommu_map(..., prot=0)` — the Samsung vendor IOMMU ignored prot. **Mainline
`exynos-iommu` (v7 support present) needs `IOMMU_READ | IOMMU_WRITE`**, otherwise every
firmware access faults. [C for stock prot=0; I for mainline consequence, high confidence]

The SysMMU is **mandatory for calls**: VSS PA must appear at 0xA0000000, and the firmware
DRAM image is linked at 0x80000000. The user's driver runs the SysMMU in bypass with 32-bit
DMA — that is incompatible with firmware mode. [C/I]

SysMMU node: irqs SPI 48 (non-secure fault) and 50 (secure fault, uses `secure_base`
0x18E10000 — ignore on mainline), clock `GATE_SMMU_AUD`, `sysmmu,no-suspend`, power
domain `pd_aud` (exynos9820-sysmmu.dtsi:98-117). [C] Mainline: create an
`IOMMU_DOMAIN_UNMANAGED` domain with `iommu_paging_domain_alloc()`, attach the ABOX device,
`iommu_map()` the table above. Do not let the DMA API own the domain (default domain would
hand out IOVAs that collide). [I]

### 1.4 Firmware images and load procedure

Files in `kit/callaudio/fw` (raw binaries, no header parsing anywhere in the stock loader):

| File | Size | Destination (ABOX addr / offset) | DT |
|---|---|---|---|
| calliope_sram.bin | 50772 | SRAM +0x0 | core0 |
| calliope_dram.bin | 2253664 (starts "CLP80012") | DRAM +0x0 | core0 |
| calliope_sram_2.bin | 52300 | SRAM +0x32000 | core1 |
| calliope_dram_2.bin | 227344 | DRAM +0x280000 | core1 |
| calliope2.dt | 7620 | DRAM +0x11fe000, `samsung,mixer-control` | beyond0lte |
| txse1/txse2/txse3, rxse | — | DRAM +0x1200000/0x13b0000/0x1800000/0x1300000 | beyond0lte |
| AP_AUDIO_SLSI etc. | — | see list below | beyond0lte |

Extra-bin DRAM offsets (beyond0lte_common.dtsi:2499-2614, area 1 = DRAM) [C]:
AP_AUDIO_SLSI 0x7e0000, APBargeIn 0x7dc000, APBiBF 0x7df000, APTV 0x7b6000, APSV 0x7bc000,
SoundBoosterParam 0x4fc000, APDV 0x4d0000, dnn 0x500800, usbout 0x1100000,
usbin 0x1110000, fmsb 0x1120000, txse1 0x1200000, rxse 0x1300000, txse2 0x13b0000,
a2dpcom 0x13e2000, bidirmic 0x1414000, calliope2.dt 0x11fe000, txse3 0x1800000.

Missing from the kit vs DT: dnn, usbout, usbin, fmsb, a2dpcom, bidirmic. Stock tolerates a
missing extra bin (stock dmesg: "a2dpcom.bin file does not exist(-2)"). [C]

Order (`abox_core_download_firmware`, `abox_core.c:415-460`) [C]:
1. For core0 then core1: `memcpy_toio(sram + off, fw)`, then `memset_io` the **rest of
   the core's SRAM** to 0; `memcpy(dram + off, fw)`, then memset the rest of its DRAM
   firmware size to 0.
2. A missing core image returns `-EAGAIN` and the cores are not booted.
3. Then extra firmware (`abox.c:2194-2293`): plain copy, **no memset**. The
   `mixer-control` file (calliope2.dt) is additionally parsed and registered as a "void
   component" with descriptor id 0xAB0C (`abox_register_void_component`). [C]

Write SRAM with 32-bit accesses only (`memcpy_toio`/`iowrite32_rep`); byte writes into the
ABOX SRAM are not guaranteed on this bus. [I]

calliope_sram.bin begins with an ARM32 vector table (`ldr pc, ...`), i.e. core0 boots at
ABOX address 0 = SRAM 0. No boot-address register is written anywhere in stock. [C file
contents; I that reset vector = 0]

### 1.5 SRAM map [C unless marked]

| SRAM offset | Use |
|---|---|
| 0x0000 - 0xC654 | core0 image |
| 0x2f000 | legacy IPC tx message (764 B) — `ipc-tx-offset` |
| 0x2f2e0 (0x20) | handshake area `<0 0x2f2e0 0x20>`; overlaps legacy tx tail — harmless with IPC v2 [I] |
| 0x2f2fc | legacy tx-ack |
| 0x2f300 | legacy IPC rx message — `ipc-rx-offset` |
| 0x2f5fc | legacy rx-ack (AP writes 0 after reading) |
| 0x30040 | compat check: stock reads a word, `param3 >> 24 == 'A'` means incompatible fw |
| 0x32000 - 0x3EC4C | core1 image |
| 0x58ffc | last word; firmware or WDT path writes "PNIC" 0x504E4943 on panic |

### 1.6 DRAM map (offsets from firmware DRAM base, ABOX 0x80000000 + off)

| Offset | Use |
|---|---|
| 0x0 | core0 DRAM image |
| 0x280000 | core1 DRAM image |
| 0x850000 (0x4000) | **IPC v2 AP→ABOX queue** (`ipc-tx-area <1 0x850000 0x4000>`) [C] |
| 0x854000 (0x4000) | **IPC v2 ABOX→AP queue** (`ipc-rx-area`) [C] |
| 0x4d0000 .. 0x1800000+ | extra bins (1.4) |

The core0 DRAM memset (step 1) covers 0x850000, so the queues are all-zero at boot,
including `q_data.len`. Either the firmware re-initialises them or the AP must. **The AP
must write `len` before booting** (stock `abox_msg_init` does so at probe, but the later
memset in download erases it — so the firmware must also re-init) — and guard every `% len`
in the receive path against 0. [I; see pitfall 6.4]

---

## 2. Power, clock, reset

### 2.1 Power domain `pd_aud` (SMC involved)

- PMU block 0x15861900 (`exynos9820-pm-domains.dtsi:18-25`). CAL `aud_on`: set PMU
  0x1900 bit0, poll 0x1904 bit0 = 1. [C]
- `need_smc = 0x18c62000`: after power-on the kernel issues
  **SMC 0x82000410 (x1 = 1 restore, x2 = 0x18c62000, x3 = 2)**, and before power-off the
  same SMC with x1 = 0 (save) (`pmucal_local.c:45-52, 98-105`; `exynos-el3_mon.c:231-245`).
  This restores the TZPC settings that make ABOX SFR/SRAM non-secure accessible. [C]
- Mainline `drivers/pmdomain/samsung/exynos-pm-domains.c` already supports this TZPC SMC
  (0x82000410). The user's driver performs the same genpd power cycle with TZPC
  save/restore before touching any register. [C]
- **SMC**: this TZPC save/restore is the only SMC on the ABOX power path. [C]

### 2.2 PMU writes (no SMC)

Stock `exynos_pmu_update` on 9820 uses atomic aliases: set = `offset | 0xc000`,
clear = `offset | 0x8000` (`exynos-pmu.c:28-80`). No SMC. [C] Mainline `regmap` RMW on
the base offset works too, provided nothing else races. [I]

| PMU offset | Mask | Use | Source |
|---|---|---|---|
| 0x1900 / 0x1904 | bit0 | pd_aud config / status | [C] |
| 0x1920 | 0x800 | pad retention release (AUD) | abox.c:2404 [C] |
| 0x2d80 | 0x1 | core0 power (`pmu_power`) | dts core0 [C] |
| 0x2da0 | 0x1 | core0 enable (`pmu_enable`) | dts core0 [C] |
| 0x2da4 | 0x11 | core0 standby status (`pmu_standby`) | dts core0 [C] |
| 0x2e00 | 0x1 | core1 power | dts core1 [C] |
| 0x2e20 | 0x1 | core1 enable | dts core1 [C] |
| 0x2e24 | 0x1 | core1 standby | dts core1 [C] |

Register names for 0x2d80/0x2da0/0x2da4 are not in the source (likely
`AUD_CPU0_CONFIGURATION`, `..._OUT`, `..._IN`). [I]

`abox_core_power(1)` sets 0x2d80 bit0 and 0x2e00 bit0 (`abox_core.c:96`).
`abox_cpu_enable(true)` is a no-op; only the disable path clears `pmu_enable`. [C]

### 2.3 Clocks

DT clocks (abox node): `pll` (PLL_AUD0), `pll1`, `audif`, `dmic`, `bus`, `cnt`. [C] The
user's driver already programs PLL_AUD0 = 1179.648 MHz, DIV_BUS 6, BUSP 2, AUDIF 15,
CNT 3. [C]

The A32 CPU clock is **not** among the DT clocks — stock scales it through
`pm_qos` class `PM_QOS_AUD` (DT `pm-qos-aud 1180000 800000 590000 394000 0` kHz) whose
backend is ACPM (DVFS firmware, not in source). [C for DT/qos; I for ACPM]
CMU_AUD topology from cmucal (offsets from 0x18c00000) [I, from cmucal tables]:

- `MUX_CLK_AUD_CPU` +0x1000 bit0: 0 = `DIV_CLK_AUD_PLL` (+0x1828), 1 = `MUX_CLKCMU_AUD_CPU_USER`.
- `MUX_CLKCMU_AUD_CPU_USER` +0x100 bit4: 0 = OSC 26 MHz, 1 = `CLKCMU_AUD_CPU` from CMU_TOP
  (0x1a240000: mux +0x1008, div +0x1804, gate +0x201c).
- `MUX_CLK_AUD_CPU_HCHGEN` +0x101c; DIVs ACLK +0x1810, ATCLK +0x1814, PCLKDBG +0x1818.

Reset state may leave the CPU on OSC (26 MHz) — firmware boots but VSS processing will
miss real-time deadlines. Recommendation: dump these registers on stock during a call
and write the same values (pin the CPU at the 1180 MHz gear for the call). [I]

### 2.4 Enable sequence (`abox_enable`, `abox.c:2371-2448`) [C]

1. `abox_gic_enable_irq`: `enable_irq`, GICD_CTLR = 1, IGROUPR0-3 = 0 (line 2379).
2. `abox_enable_wdt`: IRQ_WDT (94) target AP, enabled (2380).
3. CPU gear max (PM_QOS_AUD 1180000).
4. **SYSREG MISC_CON (0x18c10504) = 1** (2385). The user's notes say "any access to
   SYSREG_AUD kills the SoC" — see pitfall 6.2.
5. `clk_enable(audif)`; pinctrl default.
6. Pad retention release: PMU 0x1920 |= 0x800 (2404).
7. regcache sync of ABOX SFRs.
8. `has_reset = (timer CTRL1(0) @ 0x18ce0604 == 0)` (2407) — i.e. power was lost.
9. If reset: `abox_gic_init_gic` + `abox_core_download_firmware`.
10. `SYSPOWER_CTRL` (SFR +0x10) = 1, poll `SYSPOWER_STATUS` (+0x14) bit0, ≤ 10 ms (2428).
11. State = ENABLING; `abox_core_power(1)` → cores leave reset and run.
12. Else (no reset, waking from WFI): `suspend_wait_flag = 'wake'` 0x656B6177, start
    timers CTRL0(0)/CTRL0(2) = 1.
13. Schedule `restore_data_work` (runs after BOOT_DONE, 4.5).

Stock timing (`android-dmesg-cbd-boot.txt:4282-4363`): `core_power(1)` at 230.987 s,
"Calliope is ready to sing (version:MUL0)" at 231.0198 s → ~32 ms boot. [C]

### 2.5 Disable sequence (`abox.c:2450-2477`) [C]

1. `suspend_wait_flag = 'host'` 0x74736F68.
2. Send `ABOX_SUSPEND` (IPC_SYSTEM, atomic, sync).
3. Poll timer `PRESET_LSB(1)` (0x18ce0628) == 0, ≤ 100 ms.
4. Poll core standby: PMU 0x2da4 & 0x11 and 0x2e24 & 0x1, ≤ 100 ms.
5. `SYSPOWER_CTRL` = 0. Cores are **not** powered off; the PD off resets them.

Runtime PM autosuspend is 500 ms → every audio session after idle is a firmware cold
boot. A PM notifier refuses system suspend while the audio mode is IN_CALL
(`abox_is_clearable`). [C]

### 2.6 Watchdog (`abox.c:183-204`) [C]

IRQ_WDT (94) handler: dump, write "PNIC" marker, clear PMU enable / power-cycle cores,
emit a failsafe uevent; the HAL then resets audio via the "CALLIOPE" path. A mainline
driver should at minimum log, power-cycle (2.5 → 2.4), and resend the call state (4.6).

---

## 3. ABOX GIC (GIC-400 inside the audio block)

- GICD 0x18cf1000, GICC 0x18cf2000; the AP sees it as a **secondary interrupt controller
  chained on SPI 46** (`INTREQ__AUD_ABOX_GIC400`), requested `IRQF_TRIGGER_RISING`
  (`abox_gic.c:276`). [C]
- `GIC_IS_SECURE_FREE` is defined in `abox_gic.c` → no SMC for GIC programming;
  `is_secure_gic()` (`abox_util.c:110`) is true only for chip id 0xE8895000 → false on
  9820. Plain MMIO. [C]
- CPU-interface numbering (targets): CORE0 = 0, CP = 1, AP = 2, CORE1 = 3. [C]

Init (`abox_gic_init_gic`, `abox_gic.c:179-215`) [C]:
```
GICC_PMR  (0x04) = 0xFF
GICD_CTLR (0x000) = 3
GICD_IPRIORITYR (0x400..) 40 words = 0x10101010
GICC_CTLR (0x00) = 3
```
Enable path additionally: GICD_CTLR = 1, GICD_IGROUPR0-3 = 0 (all group 0).

AP → firmware: write GICD_SGIR (0xF00) = `(1 << 16) | (sgi & 0xF)` — target list bit0 =
CPU interface 0 = core0. [C]

Firmware → AP handler (`abox_gic.c:133-160`) [C]: read GICC_IAR (0x0C); if irq < 16
write GICC_EOIR (0x10) **and** GICC_DIR (0x1000); else EOIR only; then dispatch
`handler[irq]`.

WDT routing: IRQ 94 → GICD_ITARGETSR (0x85C) byte 2 = 0x04 (AP), GICD_ISENABLER (0x108)
bit 30. [C]

SGI ids (`abox_soc.h` ~688) [C]: 0 RECEIVED, 1 SYSTEM, ..., 7 FLUSH, 8/9 WDMA0/1 full,
0xA config, 0xB-0xE RDMA0-3 empty, **0xF ABOX_MSG (IPC v2)**. `IRQ_CA32_0` = 0x20.
With IPC v2, SGI 15 is the only one used in both directions; SGIs 0-14 go to the legacy
`abox_irq_handler`, which reads SRAM 0x2f300 and writes 0 to 0x2f5fc. [C]

Mainline: model it as a chained irqchip (or just a single handler in the ABOX driver that
reads IAR/EOIR itself); the generic `irq-gic` driver cannot be reused because the AP is
not CPU interface 0 and the SGIs here are cross-CPU doorbells. [I]

---

## 4. IPC

### 4.1 Version

`ipc_version = 2` when `abox_msg_init` succeeds (`abox.c:2958-2960`). With the DT
`ipc-tx-area`/`ipc-rx-area` present it does. Use v2 only. [C]

### 4.2 Queue layout (`abox_msg.h`, all `__packed`, sizes verified by compiling) [C]

```c
struct abox_msg_cmd {            /* 56 bytes */
    s32 id;          /* ipcid (enum IPC_ID) */
    s32 cmd;         /* msgtype */
    s32 arg[3];
    u32 data_idx;    /* offset of the data record in q_data.elem */
    u64 send, recv;  /* union addr, unused on wire [I] */
    u64 time_put, time_get;
};
struct abox_msg_cmd_queue  { s32 idx_s, idx_e; struct abox_msg_cmd elem[16]; }; /* 904 */
struct abox_msg_data_queue { s32 idx_s, idx_e; u32 len; s8 elem[]; };          /* hdr 12 */
struct abox_msg_queue { struct abox_msg_cmd_queue q_cmd; struct abox_msg_data_queue q_data; };
/* q_data at +904, q_data.len at +912, data elem at +916; len = 0x4000 - 916 = 15468 */
```

AP tx queue = DRAM + 0x850000 (ABOX 0x80850000); AP rx queue = DRAM + 0x854000. [C]

Send (`abox_msg.c`) [C]:
1. Data record = `s32 size` + payload (the 764-byte `ABOX_IPC_MSG`) at `q_data.idx_e`;
   if the tail is too small, wrap to 0.
2. Fill `q_cmd.elem[idx_e]` (id, cmd, data_idx, time_put); `mb()`;
   `q_cmd.idx_e = (idx_e + 1) % 16`.
3. Raise SGI 15.
4. Sync send ("flush"): wait until `q_cmd.idx_s == idx_e` ≤ 20 ms, re-raising SGI;
   per-attempt send timeout 1 ms.

Receive (on SGI 15): while `rx.q_cmd.idx_s != rx.q_cmd.idx_e`, read elem, copy data at
`data_idx`, set `q_data.idx_s = (data_idx + size + 4) % len`, `q_cmd.idx_s++`. [C]

### 4.3 Message format (`include/sound/samsung/abox_ipc.h`) [C]

`ABOX_IPC_MSG`, 764 bytes: `+0 ipcid`, `+4 task_id`, `+8 union msg`.

- `IPC_SYSTEM_MSG`: `msgtype, param1, param2, param3, bundle[740]` (bundle has
  `param_s32[]`, `param_bundle[]` views).
- `IPC_PCMTASK_MSG`: `msgtype, channel_id, param` (union: hw_params {rate, bit_depth,
  channels}, setbuff {phyaddr, size, count}, trigger, ...).
- `IPC_ABOX_CONFIG_MSG`: `msgtype, param1, param2`.

IPC ids: SYSTEM 1, PCMPLAYBACK 2, PCMCAPTURE 3, OFFLOAD 4, ERAP 5, ABOX_CONFIG 0xA.

System msgtypes: SUSPEND 1, RESUME 2, BOOT_DONE 3, CHANGE_GEAR 4, REQUEST_SYSCLK 7,
CHANGED_GEAR 9, REPORT_LOG 0x10, REPORT_DUMP 0x20, SET_MODE 0x50, SET_TYPE 0x60,
START_VSS 0xA0, STOP_VSS 0xA1, RESET_VSS 0xA2, WATCHDOG_VSS 0xA3, REPORT_COMPONENT 0xC0,
UPDATE_COMPONENT_CONTROL 0xC1, REQUEST_COMPONENT_CONTROL 0xC2,
REPORT_COMPONENT_CONTROL 0xC3, REQUEST_DEBUG 0xDE, REPORT_FAULT 0xFA (param1: 1 data abort,
2 prefetch abort, 3 OS error, 4 VSS error, 5 undef), CLK_DIFF_PPB 0xC10C.
[C; START/STOP/RESET/WATCHDOG_VSS = 0xA0/0xA1/0xA2/0xA3 per abox_ipc.h:294-297]

PCM msgtypes: PLTDAI_OPEN 11, PLTDAI_CLOSE 12, PLTDAI_HW_PARAMS 14, PLTDAI_HW_FREE 15,
PLTDAI_PREPARE 16, PLTDAI_TRIGGER 17, SET_BUFFER 20.

Config msgtypes: SET_SIFS0_RATE 1 … SET_PIFS0_RATE 0xE, SET_*_FORMAT 0x10 … 0x1D,
SET_ASRC_FACTOR_CP 0x30.

### 4.4 Boot handshake [C]

1. AP boots cores (2.4 step 11).
2. Firmware sends IPC_SYSTEM `BOOT_DONE`, `param3` = version (4 ASCII chars, "MUL0" on
   this firmware). Handler (`abox.c:1681-1696`, 1774+) sets `calliope_state = ENABLED`,
   wakes waiters, prints "Calliope is ready to sing (version:%c%c%c%c)".
3. `boot_done_work`: send `ABOX_RESUME` (atomic sync) with bundle = `u64 ktime`
   (sched_clock ns) + `u64 atime` = timer CURVALUD(1) @ 0x18ce0630 × 500 / 13
   (timer runs at 26 MHz → ns). Then CPU gear to min.
4. `restore_data_work` → `abox_cmpnt_restore` (`abox_cmpnt_v20.c:5040-5060`): resend
   all SIFS/PIFS rates (config 1..0xE), formats (0x10..0x1D), `SET_ASRC_FACTOR_CP`
   = 36864, audio mode (SET_MODE), sound type (SET_TYPE, only if nonzero), then all cached
   component-control values (UPDATE_COMPONENT_CONTROL).
5. Any IPC issued while state != ENABLED waits ≤ 1 s for BOOT_DONE.

The firmware may also send: `REPORT_LOG` (an `ABOX_LOG_BUFFER {index_writer,
index_reader, size, buffer[]}` in DRAM to register as a log ring), `REPORT_COMPONENT`
(descriptors), `CHANGE_GEAR` (param1 = gear id, e.g. `ABOX_CPU_GEAR_CALL_VSS` 0xCA11,
param2 = requested kHz) — the AP must adjust the CPU clock and reply `CHANGED_GEAR`
param1 = actual Hz — and `REQUEST_SYSCLK`. [C for messages; I that ignoring CHANGE_GEAR
is safe for stage 1]

### 4.5 Component controls (calliope2.dt) [C]

calliope2.dt = an array of 48-byte `ABOX_COMPONENT_CONTROL` records (texts at +40), ids
0x64..0xf4, all ENUM (type 1). Each becomes ALSA control `"ABOX <name>"`.
Writing a control sends IPC_SYSTEM `UPDATE_COMPONENT_CONTROL` (0xC1), `param1` = 0xAB0C
(component id), `param2` = control id, `bundle.param_s32[0]` = value
(`abox.c:1281-1320`).

`*_EN` texts "Off,On". `*_A`/`*_B` (source-select) enum values:
None 0, RDMA0..11 = 1..12, WDMA0..7 = 13..20, SPKAMP 21, USBINSTACK 22, USBOUTSTACK 23,
USBIN_COM 24, USBOUT_COM 25, BTSCO 26, BTA2DP 27, PCMOUT 28, PCMIN 29, SOUNDBOOST 30,
FMRADIO 31, SOUNDFX 32, TONEGEN 33, VPCMIN_DAI0/1/2 34/35/36, CHCONV4CH 37, CHCONV2CH 38,
REFEXT4CH 39, REFEXT2CH 40, VPCMOUT_DAI0/1/2 41/42/43, MCD_TXSE1 44, TXSE2 45, RXSE 46,
DNN 47, BIBF 48, TXSE3 49.

Ids relevant to calls (id = EN; A = id+1; B = id+2):

| Block | EN id |
|---|---|
| RDMA0..11 / WDMA0..7 | 0x64 + 3n (RDMA0 0x64 … WDMA7_B 0x9f) |
| SPKAMP | 0xa0 |
| VPCMIN_DAI0 / DAI1 / DAI2 | 0xc5 / 0xc8 / 0xcb |
| CHCONV4CH / CHCONV2CH | 0xce / 0xd1 |
| REFEXT4CH / REFEXT2CH | 0xd4 / 0xd7 |
| VPCMOUT_DAI0 / DAI1 / DAI2 | 0xda / 0xdd / 0xe0 |
| MCD_TXSE1 / TXSE2 / RXSE / DNN / BIBF / TXSE3 | 0xe3 / 0xe6 / 0xe9 / 0xec / 0xef / 0xf2 |
| TONEGEN_1KHZ_EN | 0xc4 (useful stage-1 test tone) |

Kernel-defined (not .dt) controls: `"ABOX Audio Mode"` → SET_MODE param1 (NORMAL 0,
RINGTONE 1, IN_CALL 2, IN_COMM 3, IN_VIDEOCALL 4); `"ABOX Sound Type"` → SET_TYPE param1
(VOICE 0, SPEAKER 1, HEADSET 2, BTVOICE 3, USB 4, CALLFWD 5)
(`abox_cmpnt_v20.c:756-900`, controls 1452-1460). [C]

### 4.6 Call start / stop

VSS IPC from the modem notifier (`abox.c:2578-2624`) [C]:

| Modem event | IPC (IPC_SYSTEM) | Condition |
|---|---|---|
| CP ONLINE | `START_VSS` 0xA0 | only if ABOX is powered |
| CP EXIT | `STOP_VSS` | |
| CP RESET | `RESET_VSS` | |

Stock ONLINE fires at CP `INIT_END` (`link_device_shmem.c:703`). `abox_vss_notify_call`
is a no-op on 9820. VSS probe writes 0 to the magic word (VSS + 0x700000). [C]

Firmware strings [C, strings; I, meaning]: "CP Magic Code is not ready addr=0x%X MC=0x%X
cnt=%d", "CP Magic Code is ready, VHM_initialize done", "VSS_init_proc_task created ...
coreid", "Terminate VSS_init_proc", "SET AUDIO MODE", "SET CALLPATH", "register is only
available to the VSS" (vpcmin/vpcmout). Interpretation: after START_VSS the firmware polls
the CP-written magic at ABOX 0xA0700000, then runs the VSS code that the CP image placed
in the VSS region, on an ABOX core; VPCMIN/VPCMOUT are the ports where that code exchanges
PCM with the ABOX graph.

Call start, as stock does it (sequence assembled from mixer_paths + HAL behaviour) [I,
except the individual messages, which are C]:
1. ABOX enabled (BOOT_DONE seen), CP online → `START_VSS` sent.
2. HAL: `ABOX Audio Mode` = IN_CALL (SET_MODE 2) — not in mixer_paths, set by HAL.
3. mixer path `incall_nb-handset` + `incall_nb-handset-mic` (4.7).
4. HAL opens the "call" PCMs (RDMA4 playback, WDMA2 capture) — 5.2.
Call stop: close PCMs, reset paths, `Audio Mode` = NORMAL. STOP_VSS only at CP exit.

### 4.7 mixer_paths.xml controls (kit/callaudio/mixer_paths.xml) [C]

Init: `SIFS0 Width` 24, `SIFS1 Width` 32; UAIF0 24-bit 2ch 48k; UAIF1 32-bit 4ch 48k;
`SPUS OUT0-5,8,9,11` = SIFS0, `OUT6` = SIFS1, `OUT7` = SIFS2, `OUT10` = SIFS3;
`SIFS1` = SPUS OUT7; `ABOX Audio Mode` NORMAL; `ABOX VPCMOUT_DAI0_EN` On,
`ABOX RDMA0_EN` On, `ABOX RDMA0_A` VPCMOUT_DAI0; `ABOX REFEXT4CH_EN` On,
`ABOX REFEXT4CH_A` WDMA3.

`incall_nb-handset` (line 1538) = `incall-handset` (= `media-handset`) + `ABOX Sound Type`
VOICE. `media-handset` (786): `dev-handset` (amp), SIFS0 → NSRC2 → WDMA3, UAIF1 → NSRC3 →
WDMA4, `erap-amp-reference-dsm-bypass` (`RDMA7_EN` On, `CHCONV4CH_EN` On, `RDMA7_A`
CHCONV4CH, `CHCONV4CH_A` WDMA3, `REFEXT4CH_A` WDMA4), RDMA7 → SIFS1 (`SPUS OUT7` = SIFS1),
UAIF1 SPK = SIFS1.

`incall_nb-handset-mic` (2865) = `route-cp-tx-bridge` (2812) + `dev-dual-mic`:
UAIF0 → NSRC6 → WDMA7; UAIF0 → NSRC0 (bridge) → WDMA1 → VPCMIN_DAI0 (`WDMA1_EN` On,
`VPCMIN_DAI0_EN` On, `VPCMIN_DAI0_A` WDMA1); UAIF0 → NSRC1 (bridge) → WDMA2.
`route-cp-tx` (2807) is the simpler UAIF0 → NSRC1 → WDMA2.

---

## 5. Call routing: what the AP does vs what the firmware does

### 5.1 Split [C for stock code paths]

AP (Linux, `abox.c`/`abox_cmpnt_v20.c` DAPM): routing SFRs only — ROUTE_CTRL0/1/2,
SPUS/SPUM selects, NSRC selects, UAIF format/enable, SIFS width/rate, burst length — plus
the codec (CS47L93 on UAIF0) and amps (CS35L40/41 on UAIF1, 32-bit 4ch TDM 48k).

Firmware: **all RDMA/WDMA channels** — enable, buffer address, period, start/stop.
The stock AP never writes RDMA/WDMA control registers; `abox_rdma.c:1390-1730` only sends
PCM IPC: `PLTDAI_OPEN`, `SET_BUFFER` (IOVA 0x91000000+id·1M, size, count),
`PLTDAI_HW_PARAMS`, `PLTDAI_PREPARE`, `PLTDAI_TRIGGER`. Also the component graph
(VPCMIN/OUT, CHCONV, REFEXT, TXSE/RXSE effects) and VSS. [C]

### 5.2 Call data path [I — no stock call log in the kit; confirm with one capture]

```
Mic(CS47L93) → UAIF0 → NSRC0 → WDMA1 ─→ VPCMIN_DAI0 ─→ VSS (uplink, to CP)
                     → NSRC1 → WDMA2 ("call" DMA, asrc 6)
                     → NSRC6 → WDMA7 (aux/echo)
VSS downlink → VPCMOUT_DAI0 → RDMA0 (firmware-fed) ─┐
             (or RDMA4 "call", asrc 10)             ├→ SPUS → SIFS0 → ... → SIFS1 → UAIF1 → amps
RDMA7 (ERAP) → CHCONV4CH → WDMA3 / REFEXT4CH ← WDMA4 (amp feedback, DSM bypass)
```

The init path (`RDMA0_A = VPCMOUT_DAI0`) says the voice downlink enters the graph on
RDMA0 fed by the firmware. RDMA4/WDMA2 are DT type "call" (asrc ids 10 / 6) — the HAL
probably opens them as PCMs (inferred `pcmC0D4p`, `pcmC0D14c` from the dai-link order
rdma0-11, wdma0-7). Whether stage 2 needs those PCMs open, or only the routing plus
component controls, has to be settled with a capture on stock:
`dmesg` with abox debug + `tinymix` dump + `/proc/asound/pcm` + `cat
/proc/asound/card0/pcm*/sub0/status` during a call.

### 5.3 Consequence for the user's driver

The user's driver programs RDMA0 → UAIF1 and UAIF0 → NSRC0 → SIFM0 → WDMA1 itself from
the AP. In firmware mode **those same DMAs belong to the firmware** (RDMA0 = VPCMOUT
downlink, WDMA1 = VPCMIN uplink). The AP must stop writing any RDMA/WDMA register once
the firmware is booted, and keep only the routing SFR code. [I, high confidence]

---

## 6. Pitfalls

6.1 **Register access before PD + TZPC restore → SError / hang.** Any read of ABOX SFR,
GIC, SRAM, timer before `pd_aud` is on *and* the TZPC restore SMC has run. This includes
regmap debugfs, regcache sync and probe-time ID reads. [C, user's driver comment + stock
ordering]

6.2 **SYSREG_AUD MISC_CON (0x18c10504) = 1.** Stock writes it every enable
(`abox.c:2385`); the user found any SYSREG_AUD access kills the SoC on mainline. Likely
SYSREG_AUD is TZPC-protected separately or needs its gate clock (`PCLK` of SYSREG_AUD in
CMU_AUD) enabled. Unknown what MISC_CON bit0 does (possibly AXI/ACE coherency or cache
attribute for the A32). Try without it first; if the firmware's DRAM accesses fault or
return stale data, enable the SYSREG gate clock and retry. [I]

6.3 **IOMMU prot = 0** (1.3). Map with READ|WRITE. [I]

6.4 **Queue `len` zero.** DRAM memset wipes the queue header; firmware-side re-init is
unproven. Write `len` = 15468 for both queues after download, before core power; check
`% len` divisors. [I]

6.5 **SRAM byte writes / unaligned memcpy**: use 32-bit IO accessors. [I]

6.6 **DMA ownership conflict** (5.3). [I]

6.7 **Cold boot per runtime resume** (autosuspend 500 ms): every download/boot repeats;
firmware state (mode, type, component values) is lost and must be restored (4.4 step 4).
Keep it powered for the whole call. [C]

6.8 **System suspend during a call**: stock blocks it in IN_CALL. Mainline: hold a
wakeup source or `pm_stay_awake` while in-call; PD must stay on. [C/I]

6.9 **CPU clock possibly at OSC 26 MHz** (2.3). Boot works, VSS underruns. [I]

6.10 **START_VSS ordering.** Stock sends START_VSS only if ABOX is on when CP goes online,
and `abox_enable` does not resend it [C, from notifier code; I that boot_done/restore
does not resend — re-check `abox_boot_done_work`]. Either boot CP first and ABOX second
then send START_VSS after BOOT_DONE, or send START_VSS explicitly once both are up.

6.11 **VSS region and CP security.** After CP boot, CP-side protection (the CP region
lock done by the secure world) may make VSS memory inaccessible to the AP — the ABOX
accesses it through SysMMU, which is a different master; if the lock covers only the AP
CPU it is fine. The AP's own write of 0 to the magic word must happen before the CP
locks the region. [I]

6.12 **Timeouts** [C]: BOOT_DONE wait at wakeup 10 s; IPC before ENABLED 1 s; flush 20 ms;
send 1 ms; SYSPOWER 10 ms; standby polls 100 ms; component-control get 1 s.

6.13 **ITMON / bus errors**: a firmware access to an unmapped IOVA shows up as a SysMMU
fault (IRQ 48) — log the fault address, it tells you which region is missing. Install a
fault handler with `iommu_set_fault_handler`. [I]

6.14 **Missing extra bins**: fine for handset NB calls; dnn/bidirmic/a2dpcom only matter
for their features. TXSE/RXSE are present. [C/I]

6.15 **Firmware compatibility check** at SRAM 0x30040 ('A' in top byte = incompatible).
Read it after download and log it. [C]

6.16 **The handshake area 0x2f2e0** overlaps the legacy tx buffer; never use the legacy
path concurrently. [I]

---

## 7. Stage 1: boot the firmware and see BOOT_DONE

Goal: "Calliope is ready to sing (version:MUL0)" on mainline, then a clean
suspend/resume cycle. No audio yet.

1. **Power**: `pm_runtime_get_sync` on `pd_aud` (mainline genpd does the TZPC SMC
   0x82000410 restore). Do nothing before this. [C]
2. **Clocks**: as the user's driver (PLL_AUD0 1179.648 MHz, BUS/BUSP/AUDIF/CNT dividers),
   plus `GATE_SMMU_AUD`. Read `MUX_CLK_AUD_CPU` (CMU_AUD +0x1000) and
   `MUX_CLKCMU_AUD_CPU_USER` (+0x100) and log them; switch the CPU to PLL-derived clock
   if it reads OSC. [I]
3. **Memory**: allocate 28 MiB firmware DRAM + 8 MiB private (coherent). [C]
4. **SysMMU**: unmanaged domain; map 0x80000000 → fw DRAM, 0xE0000000 → private,
   0x10000000 → chipid page (identity), 0xA0000000 → 0xF7800000 (8 MiB),
   0xA1000000 → 0xFB700000 (2 MiB); all READ|WRITE. Fault handler on. [C map; I prot]
5. **Sanity**: read the ABOX IP index register, `ABOX_IP_INDEX` (SFR +0x0, abox_soc.h:27),
   expect 0x41424F58 ("ABOX"). [C offset; I expected value]
6. **GIC**: `abox_gic_init_gic` (3), GICD_CTLR = 1, IGROUPR0-3 = 0; route IRQ 94 to AP;
   request SPI 46 rising; handler does IAR/EOIR(/DIR) and dispatches SGI 15. [C]
7. **Timer check**: read 0x18ce0604; expect 0 after a PD cycle (= reset). [C]
8. **Pad retention release**: PMU 0x1920 |= 0x800. [C]
9. **Download**: zero SRAM 0x0-0x59000 with 32-bit writes; copy calliope_sram.bin @0,
   calliope_sram_2.bin @0x32000; zero DRAM firmware area; copy calliope_dram.bin @0,
   calliope_dram_2.bin @0x280000; then calliope2.dt @0x11fe000, txse1/2/3, rxse,
   AP_* bins at their offsets (1.4). [C]
10. **Queues**: write both queue headers (idx 0, `q_data.len` = 15468). [I]
11. **Compat**: log SRAM 0x30040. [C]
12. **SYSPOWER**: SFR +0x10 = 1, poll +0x14 bit0 ≤ 10 ms. [C]
13. **Core power**: PMU 0x2d80 |= 1, 0x2e00 |= 1. [C]
14. **Observe**: poll core0 PC = GPR[31] (0x18c5507C) a few times — it should move
    off 0 within microseconds. Wait ≤ 100 ms for SGI 15 carrying IPC_SYSTEM BOOT_DONE;
    log `param3` (expect "MUL0", ~32 ms). [C register; I expected timing from stock log]
15. **Resume**: send ABOX_RESUME with ktime/atime bundle; confirm flush
    (`q_cmd.idx_s` catches up within 20 ms). [C]
16. **Optional**: register the REPORT_LOG buffer and print firmware logs — the single
    most useful debugging tool for stage 2. Answer CHANGE_GEAR with CHANGED_GEAR. [C/I]
17. **Shutdown**: 2.5 sequence (flag 'host', SUSPEND, poll timer + standby, SYSPOWER 0),
    then PD off (genpd does the TZPC save SMC). Repeat 1-17 to prove a cold reboot works.

Stage 2 (out of scope here, for orientation): restore sequence (4.4 step 4), mixer
controls for `incall_nb-handset(-mic)`, START_VSS after CP online, Audio Mode IN_CALL,
Sound Type VOICE, and whichever "call" PCMs the stock capture (5.2) shows open.

Failure triage:
- Hang/SError on first access → 6.1/6.2.
- PC stays 0 → SYSPOWER not set, wrong PMU bit, or CPU clock gated (2.3).
- PC runs, no SGI → GIC not initialised / SPI 46 trigger type / SGI targeting; check
  SysMMU faults (firmware DRAM not mapped → firmware dies in early init); check SRAM
  0x58ffc for "PNIC".
- SGI arrives, queue empty/garbage → layout/len (4.2, 6.4).
