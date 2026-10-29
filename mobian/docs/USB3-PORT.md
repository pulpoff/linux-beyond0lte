# Exynos9820 (S10e) USB 3.x SuperSpeed PHY port: USB/DP gen2 combo, stock sub_phy_version 0x400

Status: code and DT written, both builds clean (kernel Image+modules with LLVM=1, no warnings,
and W=1 is clean on phy-exynos5-usbdrd.o; U-Boot/DTB build clean). NOT tested on hardware.

## Files changed

| File | Change |
|---|---|
| linux-9820/drivers/phy/samsung/phy-exynos5-usbdrd.c | new compatible `samsung,exynos9820-usbdrd-phy`, 9820 PIPE3 code, small framework hooks |
| linux-9820/include/linux/soc/samsung/exynos-regs-pmu.h | `EXYNOS9820_PHY_CTRL_USBDP (0x704)` |
| linux-9820/Documentation/devicetree/bindings/phy/samsung,usb3-drd-phy.yaml | compatible, clocks, `samsung,phy-port-gpios` (YAML parses; dt-schema not installed here, so dt_binding_check was not run) |
| u-boot/dts/upstream/src/arm64/exynos/exynos9820.dtsi | usbdrd_phy: new compatible, 3 regs + reg-names, 5 clocks; dwc3: 2 phys, `maximum-speed = "super-speed"` |
| u-boot/dts/upstream/src/arm64/exynos/exynos9820-beyond0lte.dts | `&usbdrd_phy` orientation-switch + port + phy-port gpio (gpg0-4) + pinctrl; connector `port@1` (SS) -> PHY |

Backups: `kit/usb3/*.before-usb3`. Both the dtsi and dts backups already
contain the parent's PERIC1/USI16/SPI17 edits, so copying them back removes only the USB3 changes.
The trace harness, see below, is in `kit/usb3/cal-trace/`.

Not changed: exynos990 compatible/drvdata, `exynos850_usbdrd_utmi_init`, `exynos850_usbdrd_phy_set_mode`
(our OTG host-mode fix). The 9820 UTMI path calls those same functions. The one shared-code edit
is in `exynos850_usbdrd_utmi_set_role()`. It used to return early whenever an orientation switch
was registered. It now returns early only for the generic (gs101) switch callback, because the
9820 switch handles the SS lanes only. Behaviour for every other SoC is unchanged.
`exynos5_usbdrd_apply_phy_tunes()` was split into a list helper and a wrapper; the wrapper's
behaviour is unchanged.

## How the stock code was translated (and why it is exact)

The stock CAL (lineage-kernel `drivers/phy/samsung/phy-exynos-usbdp-gen2.c` + `-gen2-reg.h`) was
compiled **unmodified** on the host with `CONFIG_SOC_EXYNOS9820_EVT0` undefined. readl/writel were
stubbed to log into a simulated register file. Each function was run twice, with unwritten
registers reading 0x00000000 and then 0xffffffff, which gives exact set and clear masks for every
read-modify-write. The tables in the driver were then preprocessed back out of the driver source
and diffed against the trace: **identical (184 entries)** (`cal-trace/ref.txt` vs `drv.txt`).
The port-specific part, PLL/CDR polls and run_lane are hand-written code and were checked
against `cal-trace/en0_p0.txt` / `en0_p1.txt`.

Sequence: stock `exynos_usbdrd_pipe3_init()` (phy-exynos-usbdrd.c:1157-1240) -> mainline
`exynos9820_usbdrd_pipe3_init()`:

1. `phy_exynos_usb_v3p1_g2_pma_ready()` (usb3p1.c:221) -> `exynos9820_usbdp_pma_ready()`.
   This includes stock's quirk of ORing PMA_REF_FREQ=1 into 0x48[9:8] without clearing the field.
2. `phy_exynos_usbdp_g2_enable()` (gen2.c:519-, non-EVT0 lines 525-576 + 1046-1235):
   - Power-optimisation and PLL block -> `exynos9820_tunes_pipe3_preinit`.
   - `phy_exynos_g2_usbdp_tune()` -> `exynos9820_tunes_ss_device` / `_host`.
   - 0x310/0x308, `_set_pcs()` (gen2.c:470-512) and FBB -> `exynos9820_tunes_pipe3_init`.
   - The `used_phy_port` branches -> `exynos9820_usbdp_lane_setup(port)`. The EVT1.0 alt-mode
     W/A (`reg == 0x33`) never triggers for USB-only (reg = 0x30).
3. `phy_exynos_usb_v3p1_g2_pma_sw_rst_release()` (usb3p1.c:304).
4. `_check_pll_lock()`: 0x350 bit 7, polled for 100 ms (stock: 100 x 1000 x 1 us).
5. `_run_lane()`: 0x31c/0x328/0x32c = 0.
6. `_rx_cdr_pll_lock()`: 0xb84 bit 0 (port 0) or 0x1b84 bit 0 (port 1), polled for 100 ms.
7. `phy_exynos_usb_v3p1_g2_link_pclk_sel()`: CLKRST bit 7, then 3 ms.

Deviations from stock, all deliberate:
- **PLL lock failure**: stock calls `utmi_exit()`, which also kills USB2. Here only the SS part
  is parked (pipe override + PMA low power, the state utmi_init leaves), so USB2 keeps working.
- **CDR lock failure**: stock treats it as fatal (utmi_exit). Here it is a warning and bring-up
  continues, as gs101 does.
- **Tune values**: stock's first g2_enable after boot applies the tune list with value 0
  (kzalloc'd params), then `phy_tune()` re-applies the host/device values after the core reset.
  Here the mode values are applied inside the enable sequence, and re-applied live on
  `phy_set_mode()` (dwc3 calls it on every role switch). The end state is the same, because
  all entries are clear-then-set except mf_eq_en, which is a constant OR.
- **Exit**: stock's `phy_exynos_usbdp_g2_disable()` is empty. Here exit clears CLKRST.LINK_PCLK_SEL
  (stock `phy_exynos_usb_v3p1_g2_disable()`, which stock never calls), re-applies stock
  `pipe_ovrd` (LINKCTRL force + 0x48 LOW_PWR), and then isolates via PMU 0x704. Exit writes
  link-side registers only; it never touches PMA or PCS.

The beyond0lte tune nodes are `status = "disabled"`, but stock **does** apply `ss_tune_param`:
`exynos_usbdrd_get_sub_phyinfo()` parses the phandle without `of_device_is_available()`. Only
`ss_tune_info`/`hs_tune_info` check availability, and beyond0 does not use those. All 20 entries
are ported. Device and host differ only in ssrx_hf_eq_ctrl_ss (0xc/0xe) and ssrx_dfe1_tap_ctrl (0/2).

## Every register offset accessed (new code)

R = read (all reads are RMW reads unless marked "poll"), W = write. Every offset below, read or
written, is one the stock CAL path accesses with the same R/W pattern. **No read of an offset
that stock does not read.** The only shortfall is that we read some registers less often than
stock (polls). The registers stock writes without reading (e.g. 0x0a34) are written without a
read here too.

**Link/ctrl 0x10b00000** (`phy`): 0x04 LINKCTRL R/W; 0x20 CLKRST R/W (bit 7 only); 0x48 SECPMACTL R/W.
The UTMI path is unchanged (0x04, 0x08, 0x20, 0x30, 0x50, 0x54, 0x58, 0x5c, as before).

**PCS 0x10af0000** (`pcs`, size 0x800): R/W 0x014c, 0x0150, 0x0304, 0x030c, 0x0318.

**PMA 0x10ae0000** (`pma`, size 0x2800):
- W only: 0x0054 0x0060 0x0064 0x0070 0x00e0 0x0104 0x0228 0x0248 0x0258 0x0288 0x028c
  0x0308 0x0310 0x031c 0x0328 0x032c 0x0830 0x085c 0x0878 0x0898 0x0994 0x0998 0x099c
  0x09a4 0x09a8 0x0a34 0x1000 0x1030 0x105c 0x1830 0x185c 0x1878 0x1898 0x1994 0x1998
  0x199c 0x19a4 0x19a8 0x1a34 0x2000 0x2030 0x205c
- R/W (RMW): 0x038c 0x041c 0x0420 0x0904 0x09b8 0x09c0 0x09e4 0x09e8 0x09ec 0x09f0
  0x09f4 0x0a98 0x0af0 (port 0 only) 0x0af4 0x0c2c 0x0c30 0x0c44 0x0c48 0x1010 0x1014
  0x1018 0x101c 0x1080 0x1904 0x19b8 0x19c0 0x19e4 0x19e8 0x19ec 0x19f0 0x19f4 0x1a98
  0x1af0 (port 1 only) 0x1af4 0x1c2c 0x1c30 0x1c44 0x1c48 0x2010 0x2014 0x2018 0x201c 0x2080
- R poll: 0x0350 (PLL), 0x0b84 (port 0) / 0x1b84 (port 1) (CDR).

**PMU 0x15860000**: 0x704 bit 0 (regmap RMW, as stock `pipe3_phy_isol`), 1 = un-isolated. 0x72c
is unchanged (UTMI). Stock pmu_mask = <0>, i.e. BIT(0).

The gs101 PMA helpers (CMN_REG01C0 0x700, REG00B8 0x2e0, 0x0020, 0x104c/0x204c, 0x0f0c/0x1f0c)
are **not** used for 9820. The layout really is different: LC-PLL lock is at 0x350, the lane mux
is at 0x288, and the CDR monitor is at 0xb84/0x1b84.

## Ordering / power / clocks

- PIPE3 `phy_init`: core clocks on (`ref` = oscclk, `pma_ref` = REF_SOC_PLL gate, held while SS
  runs) -> **PMU 0x704 released** -> register clocks (`phy` ACLK_PHYCTRL, `scl_pclk` USBDPPHY
  SCL_APB_PCLK, `pcs_pclk` USBPCS_APB_CLK) -> sequence. This matches stock's power_on, then
  clk_enable, then pipe3_init. The PIPE3 `phy_isol` hook is a no-op because mainline dwc3 calls
  power_on *after* phy_init, and isolation has to be released before the first PMA access.
- Every later PMA/PCS access (orientation re-init, set_mode re-tune) is gated on `ss_inited` or
  `ss_running` and enables the register clocks first.
- FSYS0A runs with `auto_clock_gate`, so enable/disable of these gates is a no-op; they are listed
  for correctness. Stock enables only oscclk + SLV_LINK. A one-time log prints both reference
  rates.
- **Regulators**: stock toggles S2MPS19 LDO10M (0.70-1.10 V), LDO11M (1.6-1.95 V) and LDO12M
  (1.7-3.35 V) on connect via `phy_conn()`. All three are `regulator-boot-on`, and mainline DT
  does not declare them, so the regulator core never turns them off. USB2 works today, so they
  are on. No DT change was made for them: adding nodes with ranges could make the core re-set
  voltages. If they are declared later, use always-on.
- **dwc3**: stock dwc3 sets `DWC3_FORCE_GEN1` in LLUCTL unconditionally on DWC_usb31 > 1.20a
  (lineage core.c:352-356). Stock therefore runs Gen1 5 Gbps despite `maximum-speed =
  "super-speed-plus"` in its DT. Mainline sets FORCE_GEN1 (host mode) only with
  `maximum-speed = "super-speed"`, hence that value.

## Orientation

- The PHY registers as a typec orientation switch. The connector `port@1` (SS) links to the PHY
  `port`; max77705 calls `typec_set_orientation()` (CCPINSTAT 1 -> NORMAL, 2 -> REVERSE).
- Stock does **not** use the CC IC for this. It reads **gpg0-4 ("PHY_CON")** at every PIPE3 init:
  0 = lanes 0/1 (USB RX LN0, TX LN1, DP mux 0xc), 1 = lanes 2/3 (RX LN2, TX LN3, DP mux 0x3).
  `reverse_con_dir` is absent on beyond0. The driver does the same through
  `samsung,phy-port-gpios` (input, pull-up, as stock pinctrl). If the GPIO is absent, it falls
  back to REVERSE -> port 1.
- On an orientation event (not NONE), the full SS bring-up is re-run if the lane pair changed or
  the previous bring-up failed (similar to QMP combo). NONE only records the state.
- Lane setup is `exynos9820_usbdp_lane_setup()`. DP alt mode later means a pin-assignment
  dependent LANE_MUX_SEL_DP/DP_LANE_EN (0x288) and the other lanes' settings there.

## Safe bring-up order on hardware

1. Boot with `phy_exynos5_usbdrd.exynos9820_ss=0`. The PHY probes with the new compatible, no
   PMA/PCS access happens, and USB2 gadget (ssh) plus OTG host should behave exactly as before.
   Also check that max77705 typec still probes. It now looks up the orientation switch and would
   `-EPROBE_DEFER` forever if the PHY failed to probe.
2. Boot normally with no cable change, and read dmesg:
   `SS PHY: ref 26000000 Hz, pma_ref N Hz`, `SS PHY: lanes 0/1 (orientation x, gpio y)`,
   `SS PHY: LC PLL locked`, `SS PHY: RX CDR locked`. If PLL is not locked, SS parks and USB2
   keeps working. Check the pma_ref rate; 26 MHz is expected, and anything else means
   PMA_REF_FREQ_SEL=1 is wrong.
3. Gadget to a PC with a USB3 cable: `lsusb -t` on the PC should show 5000M. Flip the plug: the
   log line should show the other lanes and gpio value. **Compare gpio vs CC orientation in both
   plug directions** to learn the mapping (see uncertainties).
4. Host mode with a USB3 hub or stick: bus 2 (SS root hub) should enumerate at 5000M. Repeat
   with the plug flipped.
5. Only then try LPM/U1/U2 properties (see the dwc3 notes below).

## dwc3 notes (stock lineage drivers/usb/dwc3 + beyond0 DT)

Added: phys, phy-names, `maximum-speed = "super-speed"`. Not added (deliberately, to keep the
first SS test minimal and USB2 unchanged):
- `snps,usb3_lpm_capable`, `snps,has-lpm-erratum`, `snps,lpm-nyet-threshold` (must be
  `/bits/ 8 <0>` in mainline), `snps,dis-u2-freeclk-exists-quirk`,
  `snps,quirk-frame-length-adjustment = <0x20>` (the default anyway). Stock sets all of these;
  they are candidates once SS works.
- Not expressible in mainline: `snps,ux_exit_in_px_quirk` (GUSB3PIPECTL bit 27; mainline
  forcibly clears it), `snps,u1u2_exitfail_quirk` (bit 25), `snps,elastic_buf_mode_quirk`
  (bit 0), and the LLUCTL PENDING_HP_TIMER/EN_US_HP_TIMER and LSKIPFREQ PM timers. If U1/U2
  exits misbehave, these would need a small dwc3 patch.
- Stock sets no `dis_u3_susphy` (gs101 does). Keep it unset unless P3 entry breaks things.

## Known uncertainties

1. **GPIO polarity vs CC orientation**: unverified. The GPIO is the stock source of truth. If it
   does not follow the plug (e.g. it is stale at the time of the typec event), drop
   `samsung,phy-port-gpios` and the driver uses CC orientation. That mapping (NORMAL = lanes 0/1)
   may itself be inverted on this board.
2. **pma_ref parent**: REF_SOC_PLL sits behind `mout_fsys0a_usbdp_debug_user`, which is a
   bootloader-set mux between oscclk and dout_usbdp_debug. Check the logged rate.
3. **Gen2 / SSP**: not attempted. Stock forces Gen1, and the CAL has SSP tune values but no
   evidence it ran 10 Gbps.
4. **Re-init while dwc3 is running** (orientation flip in host mode): stock never did this,
   because it re-initialised the whole core per attach. Watch for xHCI SS port errors after a
   flip. A replug also triggers it.
5. **PMU 0x704** is now toggled at PIPE3 init/exit (stock toggles it at power_on/off). If DP is
   added later, isolation must be shared between USB and DP users.
6. Both DTs (beyond1/beyond2) that include exynos9820.dtsi get the new compatible and 2 phys, but
   without an orientation switch: SS on lanes 0/1 only.

## Rollback

- DT only (keeps the new kernel): use `phy_exynos5_usbdrd.exynos9820_ss=0`, or restore
  `kit/usb3/exynos9820.dtsi.before-usb3` and `exynos9820-beyond0lte.dts.before-usb3` (provided
  neither file has been edited again since).
- Kernel: copy back `phy-exynos5-usbdrd.c.before-usb3`, `exynos-regs-pmu.h.before-usb3`,
  `samsung,usb3-drd-phy.yaml.before-usb3`. An old DTB with the exynos990 compatible keeps
  working with the new kernel.
