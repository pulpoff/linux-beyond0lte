# S10e USB 3 link training: stock vs ours, register by register (2026-10-29)

Symptom: host mode, USB 3.2 Gen 2 SSD. Rx.Detect succeeds, then the SS root port loops
Polling -> SS.Inactive (PORTSC 0x00e802c0: PLS=6, CEC+PLC+PRC+WRC, CCS=0) -> warm reset ->
Rx.Detect -> Polling, and dmesg repeats "usb2-port1: config error" and "Cannot enable".

Reading the symptom: SS.Inactive is where the Polling.RxEQ/Active/Configuration/Idle timeouts
lead. A Polling.LFPS timeout goes to Compliance or Rx.Detect instead. So the LFPS handshake
most likely completes (RX lane, squelch and LFPS detect are working) and the link dies on
**TSEQ/TS1/TS2**, which is the 8b/10b data path through the PCS elastic buffer into the PIPE.
That moves the suspect from the PMA analog settings to the PCS<->MAC interface.

Stock config is EVT1: `# CONFIG_SOC_EXYNOS9820_EVT0 is not set` in
`lineage-kernel/arch/arm64/configs/exynos9820-beyond0lte_defconfig:436`.
Stock DT: `lineage-kernel/arch/arm64/boot/dts/exynos/exynos9820.dts` (udc/dwc3 at 4258,
usbdrd_phy0 at 4292, phy_version 0x301, sub_phy_version 0x400) plus
`samsung/exynos9820-beyond0lte_common.dtsi` (&usbdrd_dwc3 at 1295, &usbdrd_phy0 at 1320,
ss_tune at 68).

## 1. Difference table

### PMA / PCS / CMN / TRSV (phy_exynos_usbdp_g2_enable() and its callees)

The earlier port compiled the stock CAL unmodified and traced it against a simulated
register file (cal-trace/). `diff cal-trace/ref.txt cal-trace/drv.txt` is still IDENTICAL:
184 entries, same masks, same order. I re-checked the offsets and masks below by hand against
`phy-exynos-usbdp-gen2.c` / `-gen2-reg.h`.

| Block (stock source) | Stock | Ours | Changed? |
|---|---|---|---|
| Power optimisation: TX JEQ 0x830/0x1830/0x1030/0x2030 = 7, SER VREG 0x85c/0x185c/0x105c/0x205c = 0x80 | as listed | same | no |
| ROPLL/LCPLL CD 0x228=0x38, 0x104=0x44, 0x248=0x44 | | same | no |
| 0x38c: DP_INIT/CMN_RSTN low, CDR_WATCHDOG_EN | RMW 0x0e/0x02 | same | no |
| 0x878/0x1878 = 4; RX RTERM incm 0x9a4/0x19a4 = 7, vcm 0x9a8/0x19a8 = 0x22 | | same | no |
| DFE VGA RL 0x994/0x998/0x99c (+0x1000) = 0x12/0x12/0x48; CDR mdiv 0x898/0x1898 = 0x77 | | same | no |
| LC-PLL trims: AVC force 0x54=1, CD 0xe0=0x38, CPI 0x60=0x24, CPP 0x64=0x77, LPF 0x70=0x76 | | same | no |
| TX RCAL: sstx_up_term/dn_term = 3/3 (0x1080/0x2080) | | same | no |
| SS tune (ss_tune_param, 20 entries; sqhs_th 4 at 0x9b8/0x19b8, **lfps_th 2 at 0x9c0/0x19c0**, mf_eq, EQ/DFE + CDR post 0xa34 x3, term_cal 0xb, amp 0xb, deemp 5, pre 3, idrv_up 7, idrv_dn 0 at CMN 0x41c/0x420) | device/host values | same values | no |
| Tune timing: first g2_enable after boot | value 0 (kzalloc), then phy_tune() live after core reset | mode values inside enable | no (same end state, all clear-then-set except mf_eq_en) |
| CMN_A0310 PCS_PLL_EN/LANE_RSTN/INIT_RSTN = 0; CMN_A0308 BIAS/BGR/PM_STATE = 0 | plain writes | same | no |
| _set_pcs(): EBUF_PARAM skp_remove_th=1 + num_init_buffering=0; DRAINER EN_MASK_TSEQ; BACK_END disable_data_mask=0; OUT_VEC_2 deserial on in U2; OUT_VEC_3 TX keeper off in U3 | | same | no |
| CDR FBB fine 0xa98/0x1a98 = 2 | | same | no |
| Port 1 (gpio 1): 0x258=0, 0x288 = MUX 0x3 / DP_LANE_EN 0, 0x28c = OVRD+CDR_DATA_MODE_EXIT | | same | no |
| Port 1 beacon: LN1 0x1000 = SYNC_EN + OVRD_LFPS_OUT_EN (forced off), **LN3 0x2000 = SYNC_EN only (USB TX)** | | same | no |
| Port 1 SIGVAL LPF delay = 0 on **LN2** (0x1af0[4:2]) | | same | no |
| EVT1.0 alt-mode W/A (reg == 0x33) | never true for USB-only | absent | no |
| Order: pma_ready -> preinit -> tune -> 0310/0308 -> set_pcs -> FBB -> lane -> sw_rst_release -> PLL poll -> run_lane (0x31c/0x328/0x32c = 0) -> CDR poll (0xb84 or 0x1b84) -> link_pclk_sel + 3 ms | | same | no |
| PLL lock fail | utmi_exit (USB2 dies too) | 2 tries, then park SS only | no (deliberate) |
| CDR lock fail | utmi_exit | warning, continue | no (deliberate; phone reports locked anyway) |
| RX polarity | no PHY register; PIPE RxPolarity comes from the MAC | same | no |
| Lane per port | port 0: RX LN0 / TX LN1; port 1: RX LN2 / TX LN3; port = gpg0-4, no reverse_con_dir on beyond0 | same; phone reads gpio 1 -> lanes 2/3 | no |

### USB DRD wrapper (0x10b00000), phy-exynos-usb3p1.c (version 0x301, so major 03, minor 1)

| Register | Stock | Ours (phone) | Changed? |
|---|---|---|---|
| SECPMACTL 0x48 after g2_pma_ready + sw_rst_release | REF_FREQ=1, ROPLL/LCPLL ref req clear, resets + LOW_PWR clear = 0x100 | 0x00000100 | no |
| LINKCTRL 0x04 | q_ch WA (DIS_QACT_*, FORCE_QACT), BUS_FILTER_BYPASS=0xf, PIPE3_FORCE_EN cleared, FORCE_RX_ELEC_IDLE left set (only acts with FORCE_EN) | 0x00043ff0 | no |
| CLKRST 0x20 | minor >= 1: PHY20_RST_SEL + PHY30_RST_SEL set, SW_RSTs clear, LINK_PCLK_SEL after CDR | 0x00001084 | no |
| PIPE clock | LINK_PCLK_SEL = PMA pipe clock, width/rate fixed by HW (no register in stock either) | same | no |
| ESS_CTRL 0x70 DIS_RX_DETECT | set only on xhci port power off | never set | no (Rx.Detect works) |
| PMU 0x704 bit 0 | power_on | phy_init | no |
| LDO10M/11M/12M | phy_conn() | boot-on, never off | no |

### dwc3 (lineage drivers/usb/dwc3/core.c + beyond0 DT)

DWC_usb31: stock ORs 0x80000000 into `dwc->revision`, so every `revision >= DWC3_REVISION_xxx`
test is true and every `<=` test is false.

| Setting | Stock | Ours before | Mainline mapping | Changed? |
|---|---|---|---|---|
| GUSB3PIPECTL[0] ElasticBufferMode (nominal empty) | **1** (`snps,elastic_buf_mode_quirk`, core.c:348) | **0** | none | **yes**: new `snps,elastic-buf-mode-quirk` |
| GUSB3PIPECTL[27] UX_EXIT_PX | 1 (`snps,ux_exit_in_px_quirk`) | 0 (mainline force-clears it) | none | **yes**: new `snps,ux-exit-in-px-quirk` |
| GUSB3PIPECTL[25] U1U2EXITFAIL_TO_RECOV | 1 (`snps,u1u2_exitfail_quirk`) | 0 | none | **yes**: new `snps,u1u2-exitfail-quirk` |
| GUSB3PIPECTL[28] DISRXDETINP3 | 1 (core.c:321-342, `>= 250A` is true for usb31) | 0 | `snps,dis_rxdet_inp3_quirk` | **yes** (DT only) |
| GUSB3PIPECTL[17] SUSPHY | 0 during PHY init, 1 afterwards (phy_setup, `> 194A`) | always 0 (`snps,dis_u3_susphy_quirk`) | the same | no (needed for the re-init PLL lock; see causes 2) |
| GUSB3PIPECTL[24] REQP1P2P3, [12] LFPSP0Algn, [8] RX_DETOPOLL, [2:1] TxDeemph=1 | HW default, untouched | 0x01001102, untouched | n/a | no |
| GUSB3PIPECTL[21:19] DEP1P2P3 | only if `<= 210A` (false) | default | n/a | no |
| LLUCTL | PENDING_HP_TIMER 0xb, EN_US_HP_TIMER, FORCE_GEN1 | 0x008b8480 = the same bits | `maximum-speed = "super-speed"` | no |
| LSKIPFREQ PM_ENTRY 9 / PM_LC 5 / EN_PM_TIMER | set | default | none | no (U1/U2 timers only) |
| GUCTL REFCLKPER = 0xF; GFLADJ 240MHZ_DECR 0xA, LPM_SEL, FLADJ 0x7f0 (adj-sof-accuracy) | set | default | ref-clock props | no (ITP/LPM timing, not Polling) |
| GFLADJ 30MHZ = 0x20 | set | default 0x20 | quirk-frame-length-adjustment | no |
| GUCTL USBHSTINAUTORETRYEN, SPRSCTRLTRANSEN | set | mainline sets the first | | no |
| GUCTL1 DEV_L1_EXIT_BY_HW, GUCTL3 USB20_RETRY_DISABLE | set | mainline does the same | | no |
| usb3_lpm_capable / has-lpm-erratum / lpm-nyet 0 / dis-u2-freeclk | set | not set | exist | no (LPM: after the link trains) |
| xHCI quirk-broken-port-ped | only if `<= 300A` (false for usb31) | not set | | no |

## 2. Most likely root cause, ranked

1. **PIPE ElasticityBufferMode mismatch (GUSB3PIPECTL bit 0).** Stock programs the PCS elastic
   buffer for nominal-empty operation (`skp_remove_th = 1`, `num_init_buffering = 0`,
   TSEQ masked in the drainer) and tells the link so with `snps,elastic_buf_mode_quirk`.
   Mainline has no such quirk, so the link ran the PIPE in nominal half-full mode against a
   PHY set up for empty mode. That breaks how SKP insertion/removal and the RX data stream are
   interpreted, and nothing else in the PCS/PMA comparison differs. It fits the symptom:
   LFPS, which is analog and bypasses the elastic buffer, works, and the first ordered sets
   (TS1) are never decoded, so Polling.Active times out to SS.Inactive.
2. **SUSPHY at runtime.** Stock runs with SUSPHY=1 after init (Rx.Detect in P3, done via P2
   thanks to DISRXDETINP3). Ours never lets the PHY reach P3. Rx.Detect evidently works in
   P2, so this is a weaker suspect. It is kept as is because without it the re-init on
   orientation does not lock the PLL. If (1) is not enough, try removing
   `snps,dis_u3_susphy_quirk` while keeping `snps,dis_rxdet_inp3_quirk`.
3. **Host tune applied to a live PHY.** set_mode(HOST) comes after xhci starts, so the first
   attempts trained with the device RX EQ (HF EQ 0xc, DFE1 0). The port retries for ever and
   the live retune was in place from then on, so this cannot by itself explain a permanent
   failure. It now gets a full re-init (see 3).
4. UX_EXIT_PX / U1U2EXITFAIL: they only affect U-state exits, so they do not matter for Polling.
   They are set to match stock, so that the first time the link reaches U0 it behaves like
   stock on U3 suspend and resume.

Ruled out by the audit: lane assignment, beacon/LFPS TX lane, LFPS/squelch thresholds and RX
lane, SIGVAL filter, TX amplitude, de-emphasis and termination, LC-PLL trims, PCS power
vectors, reset order, PIPE clock select (all identical to stock).

## 3. What changed

All files were backed up in `kit/usb3/` as `<path>.before-polling`.

- `linux-9820/drivers/usb/dwc3/core.h`, `core.c`: new bools `elastic_buf_mode_quirk`,
  `ux_exit_in_px_quirk`, `u1u2_exitfail_quirk`; properties `snps,elastic-buf-mode-quirk`,
  `snps,ux-exit-in-px-quirk`, `snps,u1u2-exitfail-quirk`. `dwc3_ss_phy_setup()` sets
  GUSB3PIPECTL bits 0 / 27 / 25. UX_EXIT_PX is still cleared when the quirk is absent, so no
  other platform changes. Defines `DWC3_GUSB3PIPECTL_ELASTIC_BUF_MODE` (BIT 0) and
  `DWC3_GUSB3PIPECTL_U1U2EXITFAIL_TO_RECOV` (BIT 25).
- `linux-9820/Documentation/devicetree/bindings/usb/snps,dwc3-common.yaml`: the 3 properties.
- `u-boot/dts/upstream/src/arm64/exynos/exynos9820.dtsi`, dwc3 node: `snps,dis_rxdet_inp3_quirk`
  (an existing mainline quirk) plus the 3 new ones. The kernel ignores them until it boots
  with the new U-Boot DTB.
- `linux-9820/drivers/phy/samsung/phy-exynos5-usbdrd.c`, `exynos9820_usbdrd_phy_set_mode()`:
  a role change now parks SS and re-runs `exynos9820_usbdrd_pipe3_init()` with the new
  role's tune. This happens under phy_mutex with the register clocks on, like orien_sw_set.
  Before, it only re-applied the tune list. If SS was never inited (exynos9820_ss=0 or after
  exit), it only records the role.

Builds: kernel `Image` + modules with LLVM=1, no warnings, and W=1 is clean on both touched
objects. Image sha256 `64067c9ca0708e24fb814823099fafde86d6c0979ccae29d0b97eaac540ad60a`.
U-Boot `build-ub/u-boot.bin` sha256 `72c9b1aab772...`, beyond0lte DTB `2b6743808e15...`.

**U-Boot must be reflashed.** The DTB is built into U-Boot and passed to Linux. With the old
U-Boot the new kernel behaves exactly as before, except for the set_mode re-init.

## 4. What to look for on the phone

dmesg on attaching the SSD:
- `SS PHY: lanes 2/3 (orientation 2, gpio 1), device tune`, then **a second bring-up**
  `... host tune` when the host role arrives. Each one prints `LC PLL locked (try N)` and
  `RX CDR locked`.
- Success: `usb 2-1: new SuperSpeed USB device number N using xhci-hcd` and no
  `usb2-port1: config error`. lsusb -t shows the SSD under the 10000M root hub at 5000M
  (Gen1 is forced).

Registers:
- `/sys/kernel/debug/usb/10c00000.usb/regdump` (or devmem 0x10c0c2c0): GUSB3PIPECTL(0)
  should now read **0x1b001103** (it was 0x01001102), i.e. bits 0, 25, 27 and 28 added.
  If bit 0 is still 0, the old DTB is in use.
- `/sys/kernel/debug/usb/xhci/*/ports/port02/portsc`: the good end state is
  `0x00001203`-ish: CCS=1, PED=1, PLS=0 (U0), speed 4 (SS). The bad loop was
  0x002802e0 (Polling) <-> 0x00e802c0 (Inactive, CEC).
  - Still Polling -> Inactive: try cause 2 (drop `snps,dis_u3_susphy_quirk`) next.
  - Now Polling -> Compliance (PLS=10) or Rx.Detect: the LFPS stage is failing instead. Look
    at the LFPS threshold and the beacon lane.
- LLUCTL (0x10c0d024) should stay 0x008b8480.

Rollback: copy the `.before-polling` files back, or reflash the previous U-Boot alone. The
new kernel with the old DTB differs only in the set_mode re-init.
