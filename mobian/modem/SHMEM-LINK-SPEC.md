# S10e (Exynos 9820 / S5000AP CP) shared-memory modem link: protocol spec

This spec is derived from the stock GPL driver in `lineage-kernel/drivers/misc/modem_v1`
and `drivers/misc/mcu_ipc`. It is cross-checked against the live stock DT
(`s10e-live.dts`) and the captured boot (`android-dmesg-cbd-boot.txt`, `cbd-boot.strace`).
It is meant to be enough to write a small mainline driver.
The boot ioctls, TOC and SMC calls are covered in `MODEM-SPEC.md`. This file
covers the **link**: memory map, mailbox, framing and ring buffers.

Paths are relative to `lineage-kernel/`. Unless noted, `shmem.c` means
`drivers/misc/modem_v1/link_device_shmem.c`, `sbd.c` means `link_device_memory_sbd.c`,
`ldm.h` means `link_device_memory.h`, `sbd.h` means `include/sbd.h`, and `iod.c` means
`modem_io_device.c`.

## 0. Config used

`arch/arm64/configs/exynos9820-beyond0lte_defconfig` (it is the only beyond0 defconfig).
I resolved every `#ifdef` against it:

| Option | Value | Effect on the link |
|---|---|---|
| CONFIG_LINK_DEVICE_SHMEM | y | `link_device_shmem.c` + `link_device_memory_sbd.c` are built (Makefile:15) |
| CONFIG_LINK_DEVICE_MEMORY / _WITH_SBD_ARCH / _PCIE | n | `link_device_memory_main.c`/`_command.c` are NOT built. Ignore them |
| CONFIG_MODEM_IF_LEGACY_QOS | **not defined at all** | legacy map has no HPRIO queues (ldm.h:66-76, 89-130) |
| CONFIG_MODEM_IF_QOS | y | PS channels are NOT folded onto the hiprio ring (sbd.c:417-423 skipped) |
| CONFIG_LINK_DEVICE_NAPI | y | RX irq → NAPI poll (shmem.c:3082, 2654) |
| CONFIG_CP_ZEROCOPY / CP_DIT / CP_PKTPROC / CP_LINEAR_WA | y | used for the PS data path only (see §7) |
| CONFIG_CP_SECURE_BOOT | y | boot via SMC (MODEM-SPEC) |
| CONFIG_CP_ETDAC_OTP_WA | y | ETDAC OTP word is written to shmem at INIT_END time (shmem.c:497-543) |
| CONFIG_LINK_POWER_MANAGEMENT(_WITH_FSM) | n | no GPIO/PM handshakes. All `#ifdef CONFIG_LINK_POWER_MANAGEMENT` blocks are dead |
| CONFIG_LINK_CONTROL_MSG_IOSM | n | `cmd_handler = shmem_cmd_handler` (shmem.c:3910). CMD_RGN unused |
| CONFIG_PCI_EXYNOS | y | only registers the RAT-mode mbox irq (shmem.c:4102) |
| CONFIG_SEC_MODEM_S5000AP / UMTS_MODEM_S5000AP | y | `modem_ctrl_s5000ap.c` |
| DT `mif,ipc_version` | 0x32 = 50 = SIPC_VER_50 | SIPC5 framing |
| DT `mif,link_attrs` | 0x7c9 | bits 0 SBD_IPC, 3 DPRAM_MAGIC, 6 MEM_BOOT, 7 MEM_DUMP, 8 BOOT_ALIGNED, 9 DUMP_ALIGNED, 10 XMIT_BTDLR. **Bit 1 IPC_ALIGNED is clear** (modem_v1.h:247-261) |
| `DEBUG_MODEM_IF`, `_LINK_TX`, `_LINK_RX` | defined in modem_prj.h:41-44 | this is why `print_sbd_config` / `LNK-RX` lines show up in dmesg |

## 1. Memory map

### 1.1 Physical regions

AP physical address = CP address + 0xB000_0000 for the modem_if block. CP sees
0x4000_0000 = AP 0xF000_0000 (MODEM-SPEC). The modem_spare block maps as CP
0x5000_0000 = AP 0xD000_0000 (shm_ipc.c:262-268, sbd.c:478-485).

| Region | AP phys | Size | CP addr | Source |
|---|---|---|---|---|
| modem_if reserved-mem (all of the below) | 0xF000_0000 | 0x0B90_0000 | 0x4000_0000 | dts:4223 |
| **boot region** (`mld->boot_base`) = cp_size+vss_size | 0xF000_0000 | 0x0800_0000 | 0x4000_0000 | shm_ipc.c:241-244, 497-503. shmem.c:3952 |
| VSS | 0xF780_0000 | 0x0080_0000 | 0x4780_0000 | dts shmem,vss_* |
| L2B | 0xF800_0000 | 0x0320_0000 | 0x4800_0000 | dts shmem,l2b_* |
| **IPC region** (`mld->base`, also `sl->shmem`) | **0xFB20_0000** | **0x0050_0000** | 0x4B20_0000 | dts `ipc_offset` bytes 0b 20 00 00 = 0x0B200000. shm_ipc.c:506-512. shmem.c:3963-3964 |
| vparam | 0xFB70_0000 | 0x0020_0000 | 0x4B70_0000 | dts `vparam_offset` 0x0B700000 |
| cp_ram_logging | 0xFB90_0000 | 0x0200_0000 | n/a | dts:4228 |
| modem_spare: pktproc global desc | 0xD000_0000 | 0x100 | 0x5000_0000 | link_rx_pktproc.c:131-147. dmesg 3735 |
| modem_spare: pktproc desc array | 0xD000_0100 | ..0x0010_0000 | 0x5000_0100 | link_rx_pktproc.h `PKTPROC_DESC_BASE_OFFSET` |
| modem_spare: pktproc data | 0xD010_0000 | 0x0100_0000 | 0x5010_0000 | dmesg "addr data:d0100000" |
| modem_spare: ZMB ("legacy") | 0xD110_0000 | 0x0050_0000 | 0x5110_0000 | dmesg "legacy:d1100000", shm_ipc.c:262-275 |

Mapping attribute: the stock driver `vmap`s the boot, IPC, VSS and vparam regions
**write-combine** (`pgprot_writecombine`, shm_ipc.c:466). Mainline equivalent:
`memremap(..., MEMREMAP_WC)` or `devm_ioremap_wc`. Ordering is then explicit (§5.5).

`sl->shmem_size` = 0x500000 + 0x4900000 + 0x100000 + 0x1000000 = 0x5F00000 = 99614720.
The dmesg "SHMEM {size:99614720}" line shows exactly this value (sbd.c:473). It is used ONLY to
build zerocopy offsets: CP addr of a ZMB buffer = 0x4B200000 + 0x5F00000 + (buf − ZMB base).

### 1.2 IPC region layout (offsets from 0xFB20_0000)

Two layouts share this region. The **legacy 4 MB map** is used during boot (the
handshake). The **SBD layout** is built over it at INIT_START. Only magic/access stay
live in both.

**Legacy map**: `struct shmem_4mb_phys_map`, packed, with LEGACY_QOS off (ldm.h:66-130):

| Off | Size | Field | Notes |
|---|---|---|---|
| 0x0000 | u32 | `magic` | 0x424F4F54 "BOOT" at DL_START. 0xAA (`MEM_IPC_MAGIC`) at first CP_START (ldm.h:586-588) |
| 0x0004 | u32 | `access` | 0 → 1 at first CP_START |
| 0x0008 | u32 | fmt_tx_head | AP writes (AP→CP FMT queue, unused on this config) |
| 0x000C | u32 | fmt_tx_tail | CP writes |
| 0x0010 | u32 | fmt_rx_head | CP writes |
| 0x0014 | u32 | fmt_rx_tail | AP writes |
| 0x0018 | u32 | **raw_tx_head** | AP writes. Boot TX queue |
| 0x001C | u32 | **raw_tx_tail** | CP writes |
| 0x0020 | u32 | **raw_rx_head** | CP writes. Boot RX queue |
| 0x0024 | u32 | **raw_rx_tail** | AP writes |
| 0x0028 | 4056 | reserved | **clock table** lives at 0x28 (`mld->clk_table = map->reserved`, shmem.c:3356) |
| 0x1000 | 4096 | fmt_tx_buff | |
| 0x2000 | 4096 | fmt_rx_buff | |
| 0x3000 | 0x1FD000 (2084864) | **raw_tx_buff** | boot AP→CP bytes |
| 0x20_0000 | 0x200000 | **raw_rx_buff** | boot CP→AP bytes |
| 0x40_0000 | | end | |

Head and tail are **byte** indices into the buffer: `in` = head, `out` = tail. The queue is empty
when head == tail and holds at most size−1 bytes (`circ_queue.h:153-233`).

**SBD layout** (sbd.h:60-72 + the allocation order in sbd.c). Absolute values come from the dmesg:

| Off | Size | Content |
|---|---|---|
| 0x0000-0x03FF | 1 KiB | CTRL_RGN: magic@0, access@4, stale legacy heads/tails, clock table@0x28 