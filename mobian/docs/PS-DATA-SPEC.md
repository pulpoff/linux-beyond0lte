# S10e CP link, §7: the PS (packet data) path

This is the missing §7 of `SHMEM-LINK-SPEC.md`. It uses the same sources and the same
abbreviations. Paths are relative to `lineage-kernel/drivers/misc/modem_v1/` unless noted:

- `shmem.c` = link_device_shmem.c, `sbd.c` = link_device_memory_sbd.c, `sbd.h` = include/sbd.h
- `ldm.h` = link_device_memory.h, `iod.c` = modem_io_device.c
- `pp.c`/`pp.h` = link_rx_pktproc.c/.h, `zc.c` = link_rx_zerocopy.c, `dit.c`/`dit.h` = link_rx_dit.c/.h
- `fc.c` = link_device_memory_flow_control.c
- `shm_ipc.c` = drivers/misc/mcu_ipc/shm_ipc.c, `mcu_ipc.c` = drivers/misc/mcu_ipc/mcu_ipc.c
- `dts` = s10e-live.dts (the live stock DT)
- `dmesg` = kit/modem/android-dmesg-cbd-boot.txt
- `cp.c` = our mainline linux-9820-7.2.13/drivers/misc/exynos9820-cp.c

Config: `exynos9820-beyond0lte_defconfig` lines 921 and 1518-1521 set LINK_FORWARD=y,
CP_ZEROCOPY=y, CP_DIT=y, CP_LINEAR_WA=y and CP_PKTPROC=y. The driver used is modem_v1, not
modem_v1_dual (drivers/misc/Makefile:62). The DT has **no pktproc properties**. All pktproc
parameters are compile-time constants (pp.h:20-25, shm_ipc.c:260-299). There is no
"ringbuf/sktbuf mode": that only exists in modem_v1_dual's pktproc v2, which this phone does not use.

Tags: [guess] marks an inference the source does not prove. **CP-side** means the AP code
cannot show it.

---

## 7.1 Channels, iodevs and rings

### PS iodevs (dts:17475-17609)

| iodev | SIPC ch | format / io_type | attrs | ring? |
|---|---|---|---|---|
| rmnet0..rmnet7 | 10..17 (0x0a..0x11) | 1 IPC_RAW / 1 IODEV_NET | 0x180 = SBD_IPC(7) + NO_LINK_HEADER(8) | none (ul/dl_num_buffers = 0) |
| multipdp_hiprio | 0 | 3 IPC_MULTI_RAW / 2 DUMMY | 0x180 | ring id 8, **pseudo-ch 10** |
| multipdp | 0 | 3 IPC_MULTI_RAW / 2 DUMMY | 0x1180 = 0x180 + ZEROCOPY(12) | ring id 9, **pseudo-ch 11** |

- Attr bits: modem_v1.h:264-279. SIPC_CH_ID_PDP_0 = 10 (modem_v1.h:139).
- `sipc_ps_ch()` is true for ch 10..24 (modem_prj.h:225-229).
- Only 8 rmnet devices exist in the DT (rmnet0..7 = ch 10..17).

How the rings are assigned (sbd.c:378-429, `init_ctrl_tables`):

- An iodev gets a ring when its channel is an IPC channel that is not PS, or when it is
  IPC_MULTI_RAW.
- The IPC_MULTI_RAW iodevs get channel numbers from `qos_prio++`, starting at QOS_HIPRIO = 10.
  QOS_NORMAL = 11 (sbd.h:77-81).
- So **ring 8 = multipdp_hiprio, ch 10** and **ring 9 = multipdp, ch 11**.
- With MODEM_IF_QOS=y the fold-onto-hiprio block (sbd.c:419-425) is compiled out.
- rmnetN never gets a ring of its own. Its packets ride rings 8/9, and the real channel is
  carried per slot (see §7.2).

Note: the ring's `ch` field (10/11) is a QoS class, not rmnet0/rmnet1. `sipc_ps_ch(10/11)` is
true, so the per-slot channel encoding applies.

### The two PS rings in the 10-ring layout (dmesg 3760-3763, 3786-3789)

All offsets are from the IPC region (AP 0xFB20_0000, CP 0x4B20_0000).

| ring | dir | rbd off | len × buff_size | addr_v (sbdv) | size_v | buffers |
|---|---|---|---|---|---|---|
| 8 (ch 10) | UL | 2892 | 256 × 2048 | 11740 | 12764 | 0x228000 (2260992), 512 KiB |
| 8 (ch 10) | DL | 2912 | 256 × 2048 | 13788 | 14812 | 0x2A8000 (2785280), 512 KiB |
| 9 (ch 11) | UL | 2932 | 512 × 2048 | 15836 | 17884 | 0x328000 (3309568), 1 MiB |
| 9 (ch 11) | DL zerocopy | 2952 | 1024 × **8** | 19932 | 24028 | 0x428000 (4358144), 8 KiB |

- `size_v` = addr_v + len × 4 (sbd.c:185-193).
- rb_ch[8] is at 2060 + 8 × 16 = 2188, rb_ch[9] at 2204.
- The RBPS block is at 3852: `{ul_rp[10], ul_wp[10], dl_rp[10], dl_wp[10]}`, all u16
  (sbd.c:238-258). So:
  - ring 8: ul_rp 3868, ul_wp 3888, dl_rp 3908, dl_wp 3928
  - ring 9: ul_rp 3870, ul_wp 3890, dl_rp 3910, dl_wp 3930
- cp.c's `cp_links[]` reproduces exactly this. Ring 9 DL is flagged `zerocopy_dl`.

---

## 7.2 Uplink (AP→CP): an SBD ring slot, raw IP, no header

The path is `vnet_xmit` (iod.c:1253) → `ld->send` = `shmem_send` (shmem.c:2369, the IPC_RAW
case) → `xmit_ipc` (shmem.c:1445) → `xmit_ipc_to_rb` (shmem.c:1327) → `sbd_tx_func`
(shmem.c:1231) → `sbd_pio_tx` (sbd.c:541).

1. **No SIPC5 header.** rmnet has NO_LINK_HEADER, so `iod->link_header = false` (iod.c:1532-1535).
   In vnet_xmit, cfg = 0 and headroom/tailroom = 0 (iod.c:1294-1307). The slot holds the bare
   IPv4/IPv6 packet.
2. **Ring choice**: `sbd_ch2rb_with_skb` (sbd.h:486-497) uses `skb->queue_mapping == 1` → ch 10
   (ring 8), otherwise ch 11 (ring 9). `vnet_select_queue` returns 1 only when
   `skb->priomark == RAW_HPRIO` (0x514F5348, a Samsung-only skb field; iod.c:1411-1415,
   modem_prj.h:245). **Mainline: always use ring 9 UL.**
3. **The slot write** (sbd.c:541-593):
   - `in = ul_wp`, `out = ul_rp`. The ring is full when `circ_get_space(len, in, out) < 1`.
     It holds at most len−1 entries (circ_queue.h:189-194).
   - `count = skb->len`, which must be ≤ buff_size − payload_offset = 2048 (sbd.c:554).
     `xmit_ipc_to_rb` also clamps `skb->len` to buff_size (shmem.c:1359).
   - Copy the packet to `IPC + addr_v[in]`. addr_v[in] is the buffer's offset in the IPC
     region, `buffers + in × 2048`.
   - `size_v[in] = (len & 0xFFFF) | (iod->id << 16)` (sbd.c:580-581). **Bits 31..16 = the rmnet's
     real SIPC channel (10..17)**, bits 15..0 = length. Non-PS rings write the plain length (sbd.c:584).
   - `barrier()`, then `ul_wp = (in + 1) % len` (sbd.c:589), then `smp_mb()` (sbd.c:592).
4. **Doorbell**: `send_ipc_irq(mld, mask2int(MASK_SEND_DATA))` = 0x0081 (ldm.h:147, 160, 622-625;
   shmem.c:1284).
   - `send_ap2cp_irq` (shmem.c:2903-2907): `ISSR[mbx_ap2cp_msg=0]` (mailbox +0x80) = 0x81, then
     `INTGR1` (+0x1C) = `BIT(int_ap2cp_msg=0)` (mcu_ipc.c:238-243; dts:17309, 17321).
   - This is the same doorbell the control channels already use in cp.c (`cp_send_msg`).
5. **Ring full**: the skb is queued, `MASK_SEND_DATA` is still raised to kick the CP, and an
   hrtimer retries every TX_PERIOD_MS = 1 ms (shmem.c:1262-1266, 1295-1299; ldm.h:207). The PS
   netdevs are stopped through `sbd_txq_stop` (fc.c; shmem.c:1190-1194).
   Software queue limit: MAX_SKB_TXQ_DEPTH 1024 (ldm.h:206).
6. **Zerocopy / LINEAR_WA have no effect on UL.** `setup_zerocopy_adaptor` only touches
   `rb[DL]` (zc.c:180-238). LINEAR_WA only changes `sl->shmem_size`, which is used to build ZMB
   offsets (sbd.c:471-489). DIT is never used for TX.

---

## 7.3 Downlink (CP→AP)

### 7.3.1 Three possible DL paths

On every CP→AP message interrupt, while SBD is active, `ipc_rx_func` (shmem.c:2208-2261) and the
NAPI poll (shmem.c:2654-2732) do three things:

1. `rx_net_frames_from_pktproc` (shmem.c:1905-1957) drains the **pktproc** ring in modem_spare.
2. `recv_sbd_ipc_frames` (shmem.c:2148-2188) drains every DL ring. For the PS rings:
   - ring 8 DL goes through `rx_net_frames_from_rb` (a normal ring, raw IP);
   - ring 9 DL goes through `rx_net_frames_from_zerocopy_adaptor` (ZMB buffers).
3. Command messages are handled first (`cmd_valid` → `cmd_handler`). Pktproc and the rings are
   polled on *every* message, commands included (shmem.c:2224-2244).

The AP code cannot show which of the three the CP actually uses (CP-side).

- **[guess]** pktproc is the primary PS DL path on this build.
  - The ZMB region is printed as "legacy" by setup_pktproc (pp.c:152-155, dmesg 3736-3737).
  - shm_ipc.c:262-267 lays out "PktProc desc / PktProc data / ZMB data".
  - DIT counts `pkt_on_pktproc_cnt` and `pkt_on_zmb_cnt` separately (dit.h:63-64).
- So a mainline driver must handle **pktproc** and **ring 8 DL**, and either leave ring 9 DL
  unfed (see 7.3.4) or implement it.

### 7.3.2 pktproc: memory layout

Region: modem_spare, AP 0xD000_0000, size 0x0160_0000 (dts:4233-4236). CP address = AP − 0x8000_0000
(PKTPROC_CP_BASE_ADDR_MASK 0x5000_0000, pp.h:22).

| AP phys | CP addr | size | content | source |
|---|---|---|---|---|
| 0xD000_0000 | 0x5000_0000 | 0x14 (in a 0x100 hole) | `struct pktproc_global_desc` | pp.h:27-34, pp.c:133-140 |
| 0xD000_0100 | 0x5000_0100 | 8192 × 24 = 0x30000 (region up to 0x10_0000) | `struct pktproc_desc[8192]` | pp.h:20, 23, 37-47, pp.c:147 |
| 0xD010_0000 | 0x5010_0000 | 0x0100_0000 (16 MiB) | packet data, **CP-allocated** | pp.h:24, shm_ipc.c:282-289 |
| 0xD110_0000 | 0x5110_0000 | 0x0050_0000 | ZMB (zerocopy cells, §7.3.4) | shm_ipc.c:269-275, 525-533 |

Sizes come from shm_ipc.c:269-289: data = spare_size 0x160_0000 − ZMB 0x50_0000 − desc 0x10_0000 =
0x100_0000. This matches dmesg 3737: "size desc:0x00100000 data:0x01000000 legacy:0x00500000".

**Global descriptor**, `__packed`, little-endian:

| off | type | field | written by | value |
|---|---|---|---|---|
| 0x00 | u32 | desc_base_addr | AP at init | 0x5000_0100 (CP address) |
| 0x04 | u32 | num_of_desc | AP at init | 8192 (MAX_PKTPROC_DESC_NUM) |
| 0x08 | u32 | data_base_addr | AP at init | 0x5010_0000 (CP address) |
| 0x0C | u32 | **fore_ptr** | AP sets 0 at init, then **CP only** | producer index (next descriptor the CP fills) |
| 0x10 | u32 | **rear_ptr** | AP sets 0 at init, then **AP only** | consumer index (next descriptor the AP reads) |

- Pending count = `circ_get_usage(num, fore, rear)` = `(fore >= rear) ? fore − rear : num − rear + fore`
  (shmem.c:1912-1917).
- The ring holds at most num−1 entries [guess, the usual circ convention; the CP enforces it].

**Descriptor**, 24 bytes, `__packed` (pp.h:37-47):

| off | type | field | AP use |
|---|---|---|---|
| 0x00 | u32 | data_addr | CP address of the packet's first byte (in 0x5010_0000..0x510F_FFFF) |
| 0x04 | u32 | information | unused by the AP (CP-side flags?) |
| 0x08 | u32 | reserve1 | – |
| 0x0C | u16 | reserve2 | – |
| 0x0E | u16 | filter_result | unused by the AP |
| 0x10 | u16 | length | packet length in bytes. The AP rejects > 2048 (MAX_PKTPROC_SKB_SIZE, pp.c:41-45) |
| 0x12 | u8 | **channel_id** | SIPC channel = rmnet: 10 → rmnet0 ... 17 → rmnet7. **0xFF = invalid, skip** (pp.c:46-51) |
| 0x13 | u8 | reserve3 | – |
| 0x14 | u32 | reserve4 | – |

**Data buffer**: the AP never writes descriptors or data. The CP picks the buffer and fills
`data_addr` itself.

- AP address = `data_base_va + (data_addr & ~0x5000_0000) − 0x10_0000` (pp.c:53-54).
  That equals spare_va + (data_addr − 0x5000_0000).
- **[guess]** The CP uses 2048-byte buffers: 16 MiB / 8192 = 2048.

**Payload**: raw IP, no SIPC5 header. pktproc sets `lnk_hdr = 0` and `sipc_ch = channel_id`
(pp.c:81-84).

### 7.3.3 pktproc: init, interrupt and return

**Init**: `setup_pktproc` (pp.c:131-157), called from `cmd_init_start_handler` (shmem.c:446-494)
on **every INIT_START**.

1. It runs after `dit_init` (which does nothing to CP-visible memory, §7.5) and **before**
   `init_sbd_link` and PIF_INIT_DONE (shmem.c:464-491).
2. It writes the five gdesc words above: desc_base 0x50000100, num 8192, data_base 0x50100000,
   fore 0, rear 0.
3. It does not clear the descriptor array.
4. It invalidates the AP's cache for the data area (pp.c:150).

So the gdesc is in place before the CP gets PIF_INIT_DONE, well before CP_START.

**Mapping in stock**:
- gdesc + descriptors: `shm_request_region` = vmap **write-combine** (shm_ipc.c:535-545, 464-490).
- data: `phys_to_virt`, i.e. the **cacheable** linear map (shm_ipc.c:548-553). Before every read
  the AP calls `__inval_dcache_area(src, len)` (pp.c:61). The CP is not coherent with the AP caches.

**Interrupt**: there is no dedicated one.

- DL is signalled with the ordinary CP→AP message doorbell: `irq_cp2ap_msg = 0` (dts:17313),
  i.e. mailbox INTSR0 bit 16, cleared through INTCR0 (mcu_ipc.c:49-73), GIC SPI 41 (dts:17661-).
- The message word is read from ISSR1 (`mbx_cp2ap_msg = 1`).
- **[guess]** The word is 0x0081 (INT_VALID | SEND_DATA) for data, as for the SBD rings. The AP
  does not check it beyond `int_valid` (shmem.c:3062-3067). Any valid message triggers a poll of
  pktproc and of all rings.
- With NAPI the irq is masked, and the poll re-checks the pending bit with `mbox_check_irq`
  (shmem.c:3082-3091, 2667-2732).

**Consuming and returning descriptors** (`pktproc_pio_rx`, pp.c:26-98, one call per descriptor):

```
out = rear_ptr; d = desc[out]
if d.length > 2048 or d.channel_id == 0xFF or the address is out of range:
    drop it, rear_ptr = (out + 1) % num
else:
    copy d.length bytes into a new skb; deliver it to iod(channel_id)
    rear_ptr = (out + 1) % num
```

- **Returning = advancing rear_ptr.** Nothing is written back into the descriptor or the data.
  There is no doorbell to the CP for that.
- If skb allocation fails (ENOMEM), rear_ptr is not advanced and the poll retries (pp.c:71-74).
- The NAPI budget limits how many descriptors are read per poll (shmem.c:1911-1914).

### 7.3.4 Ring 8 DL and the zerocopy ring 9 DL

**Ring 8 DL** (ch 10, 256 × 2048) is an ordinary ring with the CP as producer (`sbd_pio_rx`,
sbd.c:599-631).

- Length = `size_v[rp] & 0xFFFF`. Channel = `(size_v[rp] >> 16) & 0xFF` (set_skb_priv, sbd.c:635-649).
- The payload is raw IP, because lnk_hdr is false: NO_LINK_HEADER (sbd.c:137, 271-274).
- The AP advances dl_rp.
- **cp.c today parses every non-zerocopy DL ring as SIPC5.** A PS packet on ring 8 would fail
  the 0xF8 check ("ring 8 bad frame") and be dropped. Ring 8 (and any ring with ch 10..24) needs
  the PS decoder.

**Ring 9 DL**: in a zerocopy ring **the roles of rp and wp are swapped**: `zdptr->rp = &dl_wp`,
`zdptr->wp = &dl_rp` (zc.c:210-211).

- **The AP feeds empty buffers** (`allocate_data_in_advance`, zc.c:366-410), at INIT_START and
  after every receive:
  - For each free slot at index `dl_rp`, it writes a **u64 offset** into the 8-byte slot
    (`IPC + 0x428000 + 8*dl_rp`) and advances dl_rp.
  - It keeps going while `circ_get_space(1024, dl_rp, pre_rp) > 0`.
  - Offset = `(cell + NET_HEADROOM) − ZMB_va + sl->shmem_size`, where shmem_size = 0x5F0_0000
    (zc.c:340-364).
  - So CP address = 0x4B20_0000 + offset = 0x5110_0000 + cell_off + 130.
- NET_HEADROOM = NET_SKB_PAD (max(32, L1_CACHE_BYTES=128)) + NET_IP_ALIGN 2 = **130**
  (ldm.h:994; arm64 cache.h:37).
- ZMB cells are 2560 (0xA00) bytes, 0x500000 / 0xA00 = 2048 cells (modem_utils.h:532,
  modem_utils.c:1253-1290).
- **The CP fills buffers**: it writes the packet at the offset, sets `size_v[dl_wp] = len | ch << 16`,
  and advances dl_wp. The AP reads from its private `pre_rp` up to dl_wp (zc.c:301-338, sbd.h:507-545).
  The payload is at the offset itself: the skb reserves 130 B before it (zc.c:265-281).
- **Left empty** (dl_rp = dl_wp = 0, the zeroed desc region), the CP sees no free buffers on
  ring 9. This is the current cp.c behaviour and the memory note's "safe".
  **[guess]** The CP then uses pktproc and/or ring 8 for DL. If DL never arrives on either of them
  after a PDP comes up, feeding ring 9 is the next thing to try.

### 7.3.5 DIT is optional and AP-only

DIT (reg 0x1A2A_0000, dts:17292-17293; irqs SPI 0x44/0x43/0x42 = DIT_BUS_ERR/RX_DONE/TX_DONE,
dts:17298-17299) is an **AP-side DMA copy/NAT engine**.

- It copies pktproc or ZMB data into skbs and does NAT and CLAT.
- It is used only when `linkforward_get_tether_mode() || dit_dev.forced_enable`
  (shmem.c:1838-1839, 1928-1929). Otherwise the PIO path runs. forced_enable = 0 (dit.c:791).
- `dit_init` (dit.c:702-795) maps registers, allocates its own descriptor ring in AP DMA memory
  and requests irqs. **It writes nothing the CP sees.**
- The CP never requires DIT. Skip it entirely.

---

## 7.4 What else the CP needs or sends

- **No "pktproc ready" flag or capability bit exists.**
  - `sbd_activate` only sets a host-side atomic (sbd.h:436-452).
  - The SBD g_desc has no PS field (sbd.h:143-168).
  - `vnet_open` calls `ld->init_comm`, and `shmem_init_comm` does nothing for PS channels (only
    the FMT/RFS channels send INIT_END; shmem.c:2290-2357).
  - What the CP needs is the gdesc written before PIF_INIT_DONE (§7.3.3), the rings, and an
    active PDP context (§7.6).
- **Flow control CP→AP**: `shmem_tx_state_handler` (shmem.c:3019-3051) is registered on
  `irq_cp2ap_status = 2` (shmem.c:4136-4148, dts:17314). That is mailbox INTSR0 bit 18.
  - It reads `status = (ISSR[mbx_cp2ap_status=3] >> 1) & 0xF` (sbi_cp_status pos 1, mask 0xF;
    dts:17351-17352; shmem.c:2897-2901).
  - It takes `(status & BIT(2)) << 2` (SHM_FLOWCTL_BIT, ldm.h:215). So **ISSR3 bit 3 set =
    MASK_TX_FLOWCTL_SUSPEND (0x10)**: stop every PS netdev. **Clear = RESUME**: wake them.
    Only edges count (chk_same_cmd, ldm.h:635-643).
  - In `shmem_send`, a suspended PS TX blocks for up to 3 s waiting for resume, or returns
    -EBUSY in irq context (shmem.c:2379-2396).
  - Mainline: honour the bit with `netif_stop_queue`/`netif_wake_queue`. Ignoring it risks UL
    being dropped by the CP, not a crash [guess].
- **REQ_ACK/RES_ACK** (fc.c: send_req_ack and the rest) apply to the legacy non-SBD queues only.
  For SBD the code itself says "CP is not doing anything when CP receive req_ack" (fc.c, in
  sbd_txq_stop and sbd_check_tx_flow_ctrl). Nothing to implement.
- **AP-side extras**: the cp2ap wakelock irq 8, the perf/DVFS requests irq 5/6/7 and RAT mode
  irq 9 (shmem.c:4063-4111). None of them is needed for data.

---

## 7.5 Smallest viable mainline implementation

### DT / mapping

1. Give modem_spare a label and add it to the modem node:
   `memory-region = <&modem_if>, <&modem_spare>;`.
   - The mainline DT has `modem_spare@d0000000 { reg = <0 0xd0000000 0x1600000>; no-map; }`
     (kit/modem/exynos9820-common.dtsi.before-cp:101-104).
   - Look it up with `of_reserved_mem_lookup(of_parse_phandle(np, "memory-region", 1))`.
2. Map all 0x160_0000 bytes with `devm_memremap(..., MEMREMAP_WC)`, like modem_if in cp.c:1431.
   - WC needs no cache maintenance.
   - The cost: reads of packet data are uncached. That is fine for LTE rates.
   - Faster alternative: map WB, invalidate before reading (stock pp.c:61), and clean before
     handing out ZMB buffers.
   - Do **not** make this region cacheable without that maintenance. The CP writes DRAM behind
     the AP's caches.
   - EL3 must allow the AP to access it. Stock touches it from EL1, so it is open [guess: same
     as modem_if after the boot SMC].

### At INIT_START (in `cp_sbd_init`, before `cp_send_cmd(CMD_PIF_INIT_DONE)`)

3. `writel(0x50000100, spare+0x00); writel(8192, spare+0x04); writel(0x50100000, spare+0x08);
   writel(0, spare+0x0C); writel(0, spare+0x10);`
   Optionally `memset_io(spare+0x100, 0, 8192*24)`. Then `wmb()`.
   Do this on every INIT_START: CP reset, crash and reboot all start again from 0/0.
4. Keep rings 8 and 9 exactly as now. Ring 9 DL stays unfed (dl_rp = dl_wp = 0) unless 7.3.4
   turns out to be required.

### netdev

5. Create rmnet0..7 (or only rmnet0 at first) with `alloc_netdev(..., NET_NAME_ENUM, setup)`.
   Model `setup` on stock vnet_setup (iod.c:1436-1449):
   - `type = ARPHRD_RAWIP`, `flags = IFF_POINTOPOINT | IFF_NOARP` (stock adds IFF_MULTICAST)
   - `addr_len = 0`, `hard_header_len = 0`, `mtu = 1500` (or the MTU from 0d:09 @152)
   - `tx_queue_len = 1000`
   Map rmnet index i ↔ SIPC ch 10+i.
   **[guess]** PDP cid N ↔ rmnet(N−1) ↔ ch 9+N. Confirm it from the DL `channel_id` the CP
   reports after activating a cid.
   The WWAN framework (`wwan_register_ops`, link_id = i) is an option, but not needed.

### TX (`ndo_start_xmit`)

6. Under `tx_lock`:
   - If `(ul_wp+1) % 512 == ul_rp` on ring 9 → `netif_stop_queue`, return NETDEV_TX_BUSY. Restart
     the queue from the RX/irq path when there is space.
   - Otherwise `memcpy_toio(ipc + readl(ipc + 15836 + 4*wp), skb->data, len)` (len ≤ 2048),
     `writel(len | (ch << 16), ipc + 17884 + 4*wp)`, `wmb()`, `writew((wp+1) % 512, ipc + 3890)`,
     `wmb()`, then `cp_send_msg(cp, 0x81)`.
   - Free the skb and count the stats.
   - No header and no padding.

### RX

7. In `cp_handle_msg` call `cp_rx_ps(cp)` on **every** valid message, after the command handling
   if it was a command (stock order, shmem.c:2222-2244). Better: NAPI. Mask the irq, poll, unmask.
8. `cp_rx_ps`:
   - (a) pktproc:
     ```
     fore = readl(spare+0x0C)
     rear = readl(spare+0x10)
     while rear != fore:
         rmb()
         read desc[rear] (24 B at spare + 0x100 + 24*rear)
         validate:
             ch in 10..17 and ch != 0xFF
             len in 1..2048
             0x50100000 <= data_addr
             data_addr + len <= 0x51100000
         skb = netdev_alloc_skb(len)   (napi_alloc_skb under NAPI)
         memcpy_fromio(skb, spare + (data_addr − 0x50000000), len)
         deliver
         mb()
         writel((rear+1) % 8192, spare+0x10)
     ```
     Recheck fore after the loop.
   - (b) Ring 8 DL (and ring 9 DL only if fed): for each slot, len = size_v & 0xFFFF,
     ch = (size_v >> 16) & 0xFF. Data at `ipc + addr_v[rp]`. Raw IP. Advance dl_rp.
     Exclude ch 10/11 rings from the SIPC5 parser.
   - (c) Deliver:
     - `skb->protocol = (data[0] >> 4) == 6 ? htons(ETH_P_IPV6) : htons(ETH_P_IP)` (iod.c:344-349)
     - `skb->dev = rmnet[ch-10]`; `skb_reset_mac_header(skb)`, `skb_reset_network_header(skb)`
     - `netif_receive_skb` (under NAPI) or `netif_rx`
     - drop if the netdev is down
9. Flow control: request mailbox irq 2. On an ISSR3 bit 3 edge, stop or wake all rmnet queues (§7.4).

### Traps

- **Address spaces**:
  - pktproc and ZMB: CP = AP − 0x8000_0000 (0xD000_0000 ↔ 0x5000_0000).
  - modem_if: CP = AP − 0xB000_0000.
  - SBD addr_v and the ZMB u64: **offsets from the IPC base** (0x4B20_0000 CP). A ZMB offset is
    ≥ 0x5F0_0000 and runs past the 5 MiB IPC region into modem_spare, because of LINEAR_WA
    (sbd.c:478-485).
  - `data_addr & ~0x50000000` in stock is a bit-clear, equal to subtraction only for 0x5xxx_xxxx.
- **Ordering**:
  - Read fore_ptr/dl_wp, then `rmb()`/`dma_rmb()` before reading the descriptor and data.
  - Finish reading the data (`mb()`) before writing rear_ptr/dl_rp. The CP may reuse the buffer
    immediately.
  - On TX: data and size_v, then `wmb()`, then wp, then `wmb()`, then the doorbell.
  - WC allows write merging and reordering of stores. Only barriers order them.
- **Sizes**:
  - All rbps are u16. `readw`/`writew`, never 32-bit accesses on rp/wp (their neighbours are
    other rings' pointers).
  - pktproc fore/rear are u32.
  - Ring lengths are 256/512/1024, not powers of two in general. Use `% len`.
- **Range-check everything** the CP writes: len, channel_id, data_addr, rp/wp < len. Stock skips
  bad entries but still advances rear_ptr (pp.c:92-97).
- **Stale data across CP restarts**: reinit the gdesc at each INIT_START. Never trust fore/rear
  from a previous CP life.
- The control IPC must keep working: the PS polling must not starve the umts_ipc rings. Stock
  limits PS by the NAPI budget (shmem.c:2155-2188).

---

## 7.6 GPRS IPC (group 0x0d)

**There is no struct definition for these messages anywhere on disk outside our private captures.**

I searched the whole bring-up tree, excluding the private captures:
- `find -iname '*gprs*'` found only Linux phonet `net/phonet/pep-gprs.c` and
  `include/net/phonet/gprs.h`, which are unrelated.
- There are no libsamsung-ipc or samsung-ipc headers.
- grep for `ipc_gprs|IPC_GPRS_|sipc_gprs` in the kernel drivers/include and kit found nothing.

The stock kernel does not parse IPC payloads. The only layouts we have are the ones inferred in
**kit/modem/cpd.py**, our own code, built from captures. Treat them as observation, not source:

| msg | type | len | layout (cpd.py) |
|---|---|---|---|
| 0d:01 define PDP | SET | 142 | u8 cid, u8 1, u8 pdp_type (6 = IPv4v6), char apn[100], 39 × 0 (cpd.py:824) |
| 0d:04 activate | SET | 105 | u8 cid, u8 on (1/0), u8 0x10, 99 × 0, 01 00 00 (cpd.py:825-827). Success response code 0x8000 (cpd.py:829-831) |
| 0d:09 IP config | CP→AP | 313 | u8 cid @0, u8 flags @1, ? @2, IPv4 @3, DNS1 @7, DNS2 @11, IPv6 @35, IPv6 DNS @99/@115, MTU le16 @152 (cpd.py:806-814) |
| 0d:10 PDP state | CP→AP | ≥3 | u8 cid, u8 state (1 = active), u8 cause. A deactivation is reported as cid 0, state 3 (cpd.py:837-850) |
| 0d:14 initial-attach APN | SET | 194 | 01 01 06, char apn[100], 88 × 0 (auth), 01 06 00 (cpd.py:996-1002) |

Data-path link: once 0d:10 reports state 1 for a cid, configure the matching rmnet with the
0d:09 IPv4 address /32 (point-to-point, default route via the device) and the MTU.
**[guess]** Use rmnet(cid−1). A DL packet's pktproc `channel_id` (or ring-8 `size_v >> 16`) is
the authoritative ch → rmnet mapping. Log it on the first packet.
