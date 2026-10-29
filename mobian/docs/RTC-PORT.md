# S2MPS19 RTC + 32 kHz clock port (mainline 7.2, S10e)

Built 2026-10-29. Image: `build-k/arch/arm64/boot/Image`
(sha256 `d4a8448f6a51...`). No warnings, W=1 included, in the four touched C files.

**DT not changed. No U-Boot reflash needed.**

## Files changed (each has a `<file>.before-rtc` backup next to it)

Under `src/linux-9820`:

- `include/linux/mfd/samsung/rtc.h`: new `enum s2mps19_rtc_reg`, 0x00..0x1A.
- `drivers/mfd/sec-acpm.c`: `s2mps19_regmap_config_rtc` ("rtc", ACPM type 0x02),
  hooked into `s2mps19_data.regmap_cfg_rtc`.
- `drivers/mfd/sec-common.c`: cells `s2mps19-rtc` (IRQ "alarm" = `S2MPS19_IRQ_RTCA0`)
  and `s2mps19-clk` (of_compatible `samsung,s2mps19-clk`).
- `drivers/rtc/rtc-s5m.c`: S2MPS19 support (details below).
- `drivers/clk/clk-s2mps11.c`: `s2mps19-clk`, which uses `S2MPS19_PMIC_RTCBUF` (0x13).
- `build-k/.config`: `CONFIG_RTC_DRV_S5M=y`,
  `CONFIG_COMMON_CLK_S2MPS11=y` (backup `.config.before-rtc`). olddefconfig changed nothing else.

`include/linux/mfd/samsung/s2mps19.h` already had `S2MPS19_PMIC_STATUS2` (0x0F) and
`S2MPS19_PMIC_RTCBUF` (0x13), so it was left alone.

## Which stock header layout applies

`include/linux/mfd/samsung/rtc-s2mp.h` (Lineage tree) switches on
`#if defined(CONFIG_RTC_DRV_S2MP)` (line 19). That symbol does not exist anywhere
in the tree: no Kconfig defines it, and no defconfig sets it. The beyond0lte
defconfig sets `CONFIG_RTC_DRV_S2MPS19=y`, which is a different symbol. So the
**`#else` layout (lines 46-72)** applies: it has the MSEC register at 0x04, and
everything from SEC onward is shifted by one. The counter enum at line 124 uses
the same `#ifdef`, and its `#else` arm starts at RTC_SEC=0 relative to
`S2MP_RTC_REG_SEC`.

## Register map (RTC bank = ACPM type 0x02, speedy channel 0)

The bank: `drivers/mfd/s2mps19_core.c:42` sets `#define I2C_ADDR_RTC 0x02`. Stock calls
`exynos_acpm_*(channel=0, i2c->addr, ...)` (core.c:55-127), so ACPM type is 0x02, which
matches mainline `SEC_PMIC_ACPM_ACCESSTYPE_RTC`.

| Reg | Name | Stock (rtc-s2mp.h) | regmap |
|---|---|---|---|
| 0x00 | CTRL | :46 | rw, cached |
| 0x01 | WTSR_SMPL | :47 | read-only to Linux |
| 0x02 | UPDATE | :48 | rw, volatile |
| 0x03 | CAPSEL | :49 | read-only to Linux |
| 0x04 | MSEC | :50 | ro, volatile |
| 0x05-0x0B | SEC..YEAR | :51-57 | rw, volatile |
| 0x0C-0x12 | A0SEC..A0YEAR | :58-64 | rw |
| 0x13-0x19 | A1SEC..A1YEAR | :65-71 | rw |
| 0x1A | OSCCTRL | :72 | read-only to Linux |

The regmap `wr_table` lets writes through only to 0x00, 0x02 and 0x05-0x19. A write
to WTSR_SMPL, CAPSEL, MSEC or OSCCTRL fails inside regmap and never reaches the PMIC.

Bits:
- UPDATE: RUDR bit0 (:93), WUDR bit1 (`RTC_WUDR_MASK_REV`, :99), AUDR bit4
  (`RTC_AUDR_MASK_REV`, :95), FREEZE bit2 (:97). Stock uses the _REV masks in
  `s2m_rtc_update()` (rtc-s2mps19.c:135-142). These are the same bit positions as
  mainline's S2MPS15/S2MPG10 masks.
- CTRL: BCD_EN bit0 (:77), MODEL24 bit1 (:79). Stock writes exactly
  `MODEL24_MASK` = binary, 24 h mode (rtc-s2mps19.c:724).
- Hour: PM flag is bit6 (:102). In 24 h mode it is set for hour >= 12 (rtc-s2mps19.c:106)
  and masked off on read (`& 0x1f`). Mainline `s5m8767_tm_to_data` does the same.
- Year: binary, 2000 base (`+100` at rtc-s2mps19.c:94, `-100` at :113). Range
  2000-2099.
- Alarm enable: bit7 of each alarm byte (:105).
- Alarm pending: PMIC STATUS2 0x0F (s2mps19-regulator.h:50) bit2 `RTCA0E`
  (rtc-s2mp.h:107). Mainline reads it via `regmap_pmic`.
- Time/alarm format: plain binary, no BCD.

UDR semantics: write time = WUDR, read time = RUDR, write alarm = AUDR (S2MPS15
style). Stock does no polling: it waits 1 ms after every UDR write
(rtc-s2mps19.c:156). The new `udr_settle_us = 1000` reproduces that wait in two
places: after RUDR, before the bulk read, and before polling for WUDR/AUDR to clear.

## 32 kHz clock (RTCBUF, PMIC bank 0x13)

The address comes from stock `s2mps19-regulator.h:55`. **Stock never touches
RTCBUF**, so it gives no bit layout. The driver uses the layout shared by the whole
family in mainline: bit0 AP, bit1 CP, bit2 BT. That is the layout for S2MPS11
RTC_CTRL, S2MPS13/14 RTCCTRL, S2MPS15 RTC_BUF (the same slot after BU_CHG) and
S2MPG10 RTCBUF. On S2MPS19 these clocks are flagged `CLK_IGNORE_UNUSED`, so
`clk_disable_unused` can never switch off the CP or Wi-Fi/BT sleep clock, which
have no consumer in the DT.

Because the DT is unchanged, there is no `clocks` child under `pmic-1`. The clock
cell therefore fails probe without touching anything, and logs
`s2mps11-clk s2mps19-clk: could not find clock sub-node`. That is expected.

### Why the madera MCLK2 was not wired

The stock beyond0lte DT (`samsung/exynos9820-beyond0lte_common.dtsi:1901`, the
cs47l93 node) has **no `clocks` property**. Stock madera-core just sets
`MADERA_CLK_32K_ENA | MADERA_32KZ_MCLK2` and assumes something always-on drives the
MCLK2 pin. Nothing in stock says that source is the PMIC AP 32k, so per the brief
the DT was left untouched. **"Missing MCLK2, requires 32kHz clock" will still
appear.**

If the owner later wants the clock (after confirming on the schematic which buffer
feeds MCLK2), this is the shape it takes. It was **not applied**:

```dts
/* exynos9820-common.dtsi, inside &acpm_ipc { pmic-1 { ... } } */
s2mps19_clk: clocks {
	compatible = "samsung,s2mps19-clk";
	#clock-cells = <1>;
	clock-output-names = "s2mps19_ap", "s2mps19_cp", "s2mps19_bt";
};
/* codec */
clocks = <&pmu_system_controller 0>, <&s2mps19_clk 0>;
clock-names = "mclk1", "mclk2";
```

## What the RTC probe writes

1. Regmap init: reads 0x00-0x1A once to fill the cache. No writes.
2. `CTRL`: `regmap_update_bits_check(CTRL, BCD_EN|MODEL24, MODEL24)`. This writes
   **only if** the two mode bits are not already binary/24 h, which means the RTC lost
   power. Other CTRL bits are kept. Only in that case does it set AUDR (UPDATE bit4),
   as both stock and mainline do after a CTRL write. You would see
   `RTC control was reset, set 24 hour binary mode` in dmesg.
   This is a subset of what mainline writes for S2MPG10 (an unconditional CTRL=0x02
   followed by AUDR).
3. Requesting the alarm IRQ unmasks `RTCA0` in PMIC INT2M (0x08, bit2) through the
   existing regmap-irq chip.
4. `hctosys` reads the time: RUDR is set (UPDATE bit0, read-modify-write through
   ACPM `update_reg`), then a 1 ms wait, then a bulk read of 0x05-0x0B.

Not ported, and never written: WTSR/SMPL (0x01), CAPSEL/OSC bias (0x03), OSCCTRL
(0x1A), init_time, the FREEZE clear, and the restart or power-off hooks. The
S2MPG10 WTSR restart handler is gated on S2MPG10 and does not run.

Later writes, at runtime only: setting the time writes 0x05-0x0B and then WUDR. This
happens through `hwclock -w`, or every 11 minutes once NTP has synced, because
`CONFIG_RTC_SYSTOHC=y`. Setting an alarm writes 0x0C-0x12 and then AUDR.

## What to check on the phone

```sh
dmesg | grep -iE 's2mps19|rtc|s5m|madera'
#  expect: "s5m-rtc s2mps19-rtc: registered as rtc0"
#          "rtc rtc0: setting system clock to 20xx-..."  (hctosys)
#  must NOT see "Failed to ..." / "waiting for UDR update, reached max number of retries"
#  expect (harmless): "s2mps11-clk s2mps19-clk: could not find clock sub-node"
#  still expected:    madera "Missing MCLK2, requires 32kHz clock" (DT not wired)
cat /sys/class/rtc/rtc0/name     # s5m-rtc s2mps19-rtc
cat /sys/class/rtc/rtc0/date /sys/class/rtc/rtc0/time
hwclock -r                       # should match what Android/stock last left, in UTC
sleep 5; hwclock -r              # must advance by ~5 s (it is not a frozen read buffer)
hwclock -w; hwclock -r           # write path (WUDR); then reboot and read again
echo +60 > /sys/class/rtc/rtc0/wakealarm; cat /proc/driver/rtc   # alarm path, optional
cat /sys/kernel/debug/clk/clk_summary | grep -i 32k   # nothing from s2mps19 until a clocks node exists
grep -H . /sys/kernel/debug/regmap/*pmic*/registers 2>/dev/null | grep ' 13:'   # read RTCBUF (bits 0..2 = AP/CP/BT), read-only
```

If `hwclock -r` gives a sensible date that advances, the register map is
confirmed. If it fails with "Invalid argument" or shows a garbage date, the
offsets are wrong. The only writes involved there are the UPDATE register and,
at worst, CTRL bits 0/1. Roll back with the `.before-rtc` files.
