# S10e (Exynos 9820, S5000AP CP) modem boot — as captured on Android 2026-10-29

Sources: cbd-boot.strace (strace -p on init-started cbd; root-started cbd gets
EACCES on /dev/umts_boot0 even with SELinux permissive — start it via init),
android-dmesg-cbd-boot.txt (cbd logs every step to kmsg), stock GPL driver
lineage-kernel/drivers/misc/modem_v1 (modem_ctrl_s5000ap.c, link_device_shmem.c,
modem_prj.h ioctls). Firmware: radio partition (modem.bin, TOC format) +
/mnt/vendor/efs/nv_data.bin (1 MiB). Baseband G970FXXSGHWC2.

CP memory: modem_if 0xf0000000 size 0xb900000 (already no-map in mainline DT),
cp_ram_logging 0xfb900000, modem_spare 0xd0000000.

## TOC (radio partition offset b_off -> CP addr m_off; CP base 0x40000000 = phys 0xf0000000)
TOC  b 0x0000_0000 m 0x40008000 size 0x410
BOOT b 0x0000_0420 m 0x40000000 size 0x1ff0      crc e43c4b81
MAIN b 0x0000_2420 m 0x40010000 size 0x2637d90   crc 720316b1
VSS  b 0x0263_a1c0 m 0x47800000 size 0x5dfbc0    (cbd: "already xmit once", skipped)
NV   (nv_data.bin)  m 0x45600000 size 0x100000
OFFSET b 0x7aa00 size 0x85600 (unused)
shmem offsets used = m_off - 0x40000000.

## Sequence (ioctls on /dev/umts_boot0, 'o' type)
1. 0x45 (shmem cmd), MODEM_RESET 0x21: pmucal_cp_enable_dump_pc_no_pg, cp_reset_assert, cp_reset_release
2. SECURITY_REQ 0x53 mode=2 (non-secure) -> SMC, CP_NO_ERROR
3. XMIT_BOOT 0x40 per chunk (struct modem_firmware {u64 binary; u32 size, m_offset, b_offset, mode, len}):
   TOC, BOOT, MAIN (~630 chunks), NV
4. SECURITY_REQ mode=7 param2=0xf0008000 param3=0x410  (verify TOC)
5. SECURITY_REQ mode=0(→0xf0000000 base) param2=0x1ff0 param3=0x2637d90 (BOOT+MAIN, ~290 ms)
6. MODEM_ON 0x01: init_mailbox_regs (ds_det, hw_rev 168, dsp_type 0x910115), AP2CP_CFG(0x15020000)=1
7. MODEM_BOOT_ON 0x22: state BOOTING, cp_wdt irq disabled
8. MODEM_DL_START 0x28: shmem magic 0x424F4F54 ("BOOT"), boot_confirm
9. handshake on boot0: write 0x0000900D -> read 0x0000A00D; write 0x00009F00 -> read 0x0000AF00
10. MODEM_BOOT_OFF 0x23 (PMU CP_OUT = 0x1038), CLR_CP_BOOTLOG 0x48
11. ~1.5 s later CP -> INIT_START; driver sets up SBD ring buffers (desc rgn, RB ch 35/235/236/245...)
Then rild talks Samsung IPC on /dev/umts_ipc0/ipc1 (to capture next, with SIM).
