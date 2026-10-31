// SPDX-License-Identifier: GPL-2.0
//
// Exynos 9820 CP (Shannon S5000AP modem) boot and control.
//
// The CP is part of the SoC. It runs from a DRAM carve-out (modem_if at
// 0xf0000000) that the AP fills with the TOC/BOOT/MAIN/NV images from the
// radio partition, the EL3 monitor verifies and locks, and the PMU releases
// from reset. AP and CP then talk through an MCU_IPC mailbox (shared value
// registers + doorbells) and ring buffers in the same carve-out.
//
// Userspace sees the stock modem_v1 interface, so the boot tool and the IPC
// side can follow what Samsung's cbd/rild do on Android:
//   /dev/umts_boot0  ioctls RESET, SECURITY_REQ, XMIT_BOOT, ON, BOOT_ON,
//                    DL_START, BOOT_OFF, STATUS (same numbers as modem_v1)
//   /dev/umts_ipc0   Samsung IPC frames (rild's channel 235)
//   /dev/umts_ipc1   the second IPC channel (236)
//   /dev/umts_rfs0   the CP's remote file system requests (channel 245)
//   rmnet0           raw-IP packet data (PS channel 10..17 on rings 8/9 and the
//                    pktproc DL queue in modem_spare)
//   /dev/wwan0at0    a WWAN AT port for ModemManager. The CP does not speak AT;
//                    the port is bridged to /dev/umts_atbridge, where the
//                    userspace daemon translates AT to Samsung IPC.
// One read() returns one frame without its SIPC5 link header; one write()
// sends one frame.
//
// Sequences and register fields are from the stock GPL driver
// (drivers/misc/modem_v1/modem_ctrl_s5000ap.c, link_device_shmem.c,
// drivers/misc/mcu_ipc, cal-if/exynos9820/pmucal_cp_exynos9820.h) and from
// the stock DT; the boot order is the one cbd logs on Android.

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/if_arp.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/wwan.h>

/* PMU, CP control (pmucal_cp_exynos9820.h) */
#define PMU_CP_CONFIGURATION		0x3000
#define PMU_CP_STATUS			0x3004
#define PMU_CP_CTRL_S			0x3014
#define PMU_CP_OUT			0x3020
#define PMU_CP_IN			0x3024
#define PMU_CP_PWRON			BIT(0)
#define PMU_CP_CTRL_S_START		BIT(3)
#define PMU_CP_OUT_DUMP_PC_NO_PG	BIT(16)
#define PMU_CP_IN_PWRDN_DONE		BIT(4)

/* MCU_IPC mailbox (drivers/misc/mcu_ipc/regs-mcu_ipc.h) */
#define MBOX_MCUCTLR			0x00
#define MBOX_MCUCTLR_MSWRST		BIT(0)
#define MBOX_INTGR0			0x08
#define MBOX_INTCR0			0x0c
#define MBOX_INTMR0			0x10
#define MBOX_INTSR0			0x14
#define MBOX_INTGR1			0x1c
#define MBOX_INTCR1			0x20
#define MBOX_ISSR(n)			(0x80 + 4 * (n))
#define MBOX_NUM_ISSR			64
#define MBOX_RX_INT(n)			BIT(16 + (n))

/* mailbox slots and doorbells, stock DT mif_pdata */
#define MBX_AP2CP_MSG			0
#define MBX_CP2AP_MSG			1
#define MBX_AP2CP_STATUS		2
#define MBX_CP2AP_STATUS		3
#define INT_AP2CP_MSG			0
#define INT_AP2CP_WAKEUP		1
#define INT_AP2CP_STATUS		2
#define IRQ_CP2AP_MSG			0
#define IRQ_CP2AP_STATUS		2

/* fields of the united status words */
#define SBI_CP_STATUS			GENMASK(4, 1)
#define SBI_AP_STATUS			GENMASK(4, 1)
#define SBI_PDA_ACTIVE			BIT(5)
#define SBI_SYS_REV			GENMASK(13, 6)
#define SBI_DS_DET			GENMASK(15, 14)
#define SBI_DEVICE_TYPE			GENMASK(22, 18)

/*
 * What MODEM_ON writes on Android (ap_status 0x00446a20): one SIM detected,
 * board revision 168, display "connected, type 1", PDA active.
 */
#define S10E_DS_DET			1
#define S10E_HW_REV			168
#define S10E_DEVICE_TYPE		0x11

/*
 * Legacy 4 MB map at the start of the IPC region, used for the boot
 * handshake (link_device_memory.h struct shmem_4mb_phys_map). Head and
 * tail are byte indices; "tx" is AP->CP.
 */
#define CP_IPC_OFFSET			0x0b200000
#define IPC_MAGIC			0x00
#define IPC_ACCESS			0x04
#define IPC_RAW_TX_HEAD			0x18
#define IPC_RAW_TX_TAIL			0x1c
#define IPC_RAW_RX_HEAD			0x20
#define IPC_RAW_RX_TAIL			0x24
#define IPC_RAW_TX_BUFF			0x3000
#define IPC_RAW_TX_SIZE			0x1fd000
#define IPC_RAW_RX_BUFF			0x200000
#define IPC_RAW_RX_SIZE			0x200000
#define MEM_BOOT_MAGIC			0x424f4f54

/* SIPC5 link header, as umts_boot0 (ch 0xd7, BOOT_ALIGNED) frames it */
#define SIPC5_START			0xf8
#define SIPC5_PADDING			BIT(2)
#define SIPC5_EXT_LEN			BIT(1)
#define SIPC5_CTL			BIT(0)
#define SIPC5_CH_BOOT0			0xd7

/* cp2ap_msg words of the legacy map */
#define MSG_INT_VALID			0x80
#define MSG_CMD_VALID			0x40
#define MSG_CMD_FIELD			0x3f
#define CMD_INIT_START			0x01
#define CMD_INIT_END			0x02
#define CMD_CRASH_RESET			0x07
#define CMD_PHONE_START			0x08
#define CMD_CRASH_EXIT			0x09
#define CMD_PIF_INIT_DONE		0x0d
#define MSG_SEND_DATA			0x01
#define MEM_IPC_MAGIC			0xaa

/*
 * SBD ring layout over the IPC region (include/sbd.h, link_device_memory_sbd.c).
 * The CP reads it from the global descriptor, so only the order and sizes
 * have to match the stock DT's io_devices; offsets are what stock prints.
 */
#define SBD_DESC_RGN			0x800		/* 2 KiB ctrl + cmd rgn */
#define SBD_DESC_END			0x10000
#define SBD_BUFF_RGN			0x10000
#define SBD_MAX_LINKS			32
#define SBD_RB_CH			(SBD_DESC_RGN + 12)
#define SBD_RB_DESC			(SBD_RB_CH + SBD_MAX_LINKS * 16)
#define SBD_RBPS			(SBD_RB_DESC + SBD_MAX_LINKS * 2 * 20)
#define SBD_ETDAC			(SBD_DESC_RGN - 8)
#define IPC_CLK_TABLE			0x28
#define CP_NUM_IODS			3

/*
 * PS data (kit/modem/PS-DATA-SPEC.md). rmnet0..7 are SIPC channels 10..17; they
 * share ring 8 (hiprio) and ring 9 (multipdp), raw IP per slot with the channel
 * in size_v bits 23:16. DL also arrives through pktproc: a descriptor queue in
 * modem_spare (AP 0xd0000000 = CP 0x50000000) that the CP fills and the AP
 * returns by advancing rear_ptr.
 */
#define PS_CH_FIRST			10
#define PS_CH_LAST			17
#define PS_UL_LINK			9		/* multipdp */
#define PS_MTU				1500
#define SPARE_PHYS			0xd0000000
#define SPARE_SIZE			0x1600000
#define SPARE_CP_BASE			0x50000000
#define PP_DESC_BASE			0x100		/* gdesc: desc_base, num, data_base, fore, rear */
#define PP_NUM				8192
#define PP_DESC_SIZE			24		/* data_addr u32 @0, length u16 @16, channel u8 @18 */
#define PP_DATA_BASE			0x100000
#define PP_DATA_END			0x1100000
/*
 * Ring 9 DL is zerocopy: the AP hands the CP empty buffers in the ZMB area of
 * modem_spare, as u64 offsets from the IPC base (+ the 0x5f00000 the stock
 * LINEAR_WA layout puts before it), advancing dl_rp; the CP fills them and
 * advances dl_wp. Slot n always gets cell n.
 */
#define ZMB_OFF				0x1100000	/* in modem_spare */
#define ZMB_CELL			0xa00
#define ZMB_HEADROOM			130		/* NET_SKB_PAD + NET_IP_ALIGN */
#define ZMB_SHMEM_SIZE			0x5f00000

static unsigned int ps_ul_ch = PS_CH_FIRST;
module_param(ps_ul_ch, uint, 0644);
MODULE_PARM_DESC(ps_ul_ch, "SIPC channel rmnet0 sends on (10 = PDP cid 1 [guess])");

/* the channels userspace gets, with their ring (link id) */
static const struct {
	const char *name;
	u8 ch, link;
} cp_iod_cfg[CP_NUM_IODS] = {
	{ "umts_ipc0", 235, 1 },
	{ "umts_ipc1", 236, 2 },	/* rild's second instance uses it too */
	{ "umts_rfs0", 245, 3 },
};

enum { UL, DL };

struct cp_link_cfg {
	u8 ch;
	bool ps;		/* raw IP slots, not SIPC5 frames */
	bool zerocopy_dl;	/* DL buffers are handed out by the AP */
	u16 len[2];
	u16 size[2];
};

/* stock DT io_device_0..17 that get a ring, in order (ids 0..9) */
static const struct cp_link_cfg cp_links[] = {
	{  35, false, false, {  16,   16 }, { 2048, 2048 } },	/* umts_cass */
	{ 235, false, false, {  16,   32 }, { 4096, 4096 } },	/* umts_ipc0 */
	{ 236, false, false, {  16,   32 }, { 4096, 4096 } },	/* umts_ipc1 */
	{ 245, false, false, {  16,  512 }, { 2048, 2048 } },	/* umts_rfs0 */
	{   1, false, false, {  32,   64 }, { 2048, 2048 } },	/* umts_csd */
	{  25, false, false, {  16,   16 }, { 2048, 2048 } },	/* umts_router */
	{  28, false, false, {  16,  128 }, { 2048, 2048 } },	/* umts_dm0 */
	{ 244, false, false, {  16,   32 }, { 2048, 2048 } },	/* ipc_loopback0 */
	{  10, true,  false, { 256,  256 }, { 2048, 2048 } },	/* multipdp_hiprio */
	{  11, true,  true,  { 512, 1024 }, { 2048,    8 } },	/* multipdp */
};
#define CP_NUM_LINKS			ARRAY_SIZE(cp_links)

struct cp_ring {
	u16 len, size;
	u32 buff;		/* offsets into the IPC region */
	u32 addr_v, size_v;
	u32 rp, wp;		/* offsets of the u16 pointers */
	u16 zc_rd;		/* zerocopy DL: next slot to read */
	/* RX reassembly of a frame that spans slots */
	u8 *frm;
	unsigned int total, rcvd;
};

/* EL3 services (modem_v1.h) */
#define SMC_ID_CP			0x82000700
#define SMC_ID_SSS_CLK			0x82001011
#define SSS_CLK_ENABLE			0
#define SSS_CLK_DISABLE			1
#define CP_SEC_MODE_NORMAL		0
#define CP_SEC_MODE_CHECK_CONTINUE	8
#define CP_CHECK_SIGN_NOT_FINISH	0x100
#define CP_SEC_MAX_TRY			0x1000
#define CP_SMC_READ_CTRL		3
#define CP_SMC_WRITE_CTRL		4
#define CP_SMC_CTRL_S			0	/* enum cp_control */
#define CP_NOT_WORKING			0xb

/* modem_v1 ioctls ('o'), same numbers as the stock driver */
#define IOCTL_MODEM_ON			_IO('o', 0x19)
#define IOCTL_MODEM_RESET		_IO('o', 0x21)
#define IOCTL_MODEM_BOOT_ON		_IO('o', 0x22)
#define IOCTL_MODEM_BOOT_OFF		_IO('o', 0x23)
#define IOCTL_MODEM_STATUS		_IO('o', 0x27)
#define IOCTL_MODEM_DL_START		_IO('o', 0x28)
#define IOCTL_MODEM_XMIT_BOOT		_IO('o', 0x40)
#define IOCTL_SECURITY_REQ		_IO('o', 0x53)

struct modem_firmware {
	u64 binary;
	u32 size;
	u32 m_offset;
	u32 b_offset;
	u32 mode;
	u32 len;
} __packed;

struct modem_sec_req {
	u32 mode;
	u32 param2;
	u32 param3;
	u32 param4;
} __packed;

enum cp_state {
	CP_OFFLINE,
	CP_BOOTING,
	CP_ONLINE,
	CP_CRASHED,
};

struct exynos_cp {
	struct device *dev;
	struct regmap *pmu;
	void __iomem *mbox;
	void __iomem *ap2cp_cfg;

	phys_addr_t phys;
	size_t size;
	void *base;		/* whole modem_if carve-out, write-combined */
	bool mem_ok;		/* EL3 has opened the carve-out to the AP */

	struct miscdevice boot;
	struct mutex lock;
	wait_queue_head_t wq;
	enum cp_state state;
	u32 cp2ap_msg;		/* last value the CP posted with IRQ_CP2AP_MSG */
	bool init_start;	/* CP posted CMD_INIT_START */
	unsigned long msg_count;

	/* link, valid once the CP has asked for it */
	struct cp_ring ring[CP_NUM_LINKS][2];
	bool sbd_up;		/* rings laid out, PIF_INIT_DONE sent */
	bool ipc_up;		/* CP_START seen, IPC magic set */
	bool init_end;		/* INIT_END sent */
	struct mutex tx_lock;
	void __iomem *etdac_otp;
	unsigned long rx_frames, rx_dropped;

	/* PS data */
	void *spare;		/* modem_spare, write-combined (pktproc) */
	struct net_device *ps_dev;
	spinlock_t ps_tx_lock;	/* ring 9 UL, from ndo_start_xmit */
	bool ps_ch_seen[PS_CH_LAST - PS_CH_FIRST + 1];

	/* WWAN AT port <-> /dev/umts_atbridge */
	struct wwan_port *at_port;
	struct miscdevice at_bridge;
	struct sk_buff_head at_txq;	/* AT commands from the port */
	wait_queue_head_t at_wq;
	atomic_t at_bridge_open;

	struct cp_iod {
		struct exynos_cp *cp;
		struct miscdevice misc;
		u8 ch, link;
		atomic_t opened;
		struct sk_buff_head rxq;
		wait_queue_head_t wq;
	} iod[CP_NUM_IODS];
};

/* PMU */

static bool cp_powered(struct exynos_cp *cp)
{
	u32 val = 0;

	regmap_read(cp->pmu, PMU_CP_STATUS, &val);
	return val & PMU_CP_PWRON;
}

static int cp_wait_status(struct exynos_cp *cp, bool on)
{
	u32 val;
	int ret;

	ret = regmap_read_poll_timeout(cp->pmu, PMU_CP_STATUS, val,
				       !!(val & PMU_CP_PWRON) == on, 100, 100000);
	if (ret || !on)
		return ret;
	return regmap_read_poll_timeout(cp->pmu, PMU_CP_IN, val,
					val & PMU_CP_IN_PWRDN_DONE, 100, 100000);
}

static unsigned long cp_smc(unsigned long id, unsigned long a1,
			    unsigned long a2, unsigned long a3);

/*
 * CP_CTRL_S is a secure register: a plain write from EL1 hangs the SoC.
 * EL3 reads it back 16 bits at a time, (half << 16) | error, as stock
 * pmucal_smc_read/write do.
 */
static int cp_ctrl_s_update(struct exynos_cp *cp, u32 mask, u32 val)
{
	unsigned long lo, hi, ret;
	u32 cur;

	lo = cp_smc(SMC_ID_CP, CP_SMC_READ_CTRL, 0, CP_SMC_CTRL_S);
	hi = cp_smc(SMC_ID_CP, CP_SMC_READ_CTRL, 1, CP_SMC_CTRL_S);
	if ((lo & 0xffff) || (hi & 0xffff)) {
		dev_err(cp->dev, "CP_CTRL_S read failed %lx %lx\n", lo, hi);
		return -EIO;
	}
	cur = ((hi >> 16) << 16) | ((lo >> 16) & 0xffff);
	ret = cp_smc(SMC_ID_CP, CP_SMC_WRITE_CTRL, (cur & ~mask) | val,
		     CP_SMC_CTRL_S);
	if (ret) {
		dev_err(cp->dev, "CP_CTRL_S write failed %lx\n", ret);
		return -EIO;
	}
	dev_info(cp->dev, "CP_CTRL_S 0x%08x -> 0x%08x\n", cur, (cur & ~mask) | val);
	return 0;
}

static int cp_pmu_init(struct exynos_cp *cp)
{
	int ret;

	ret = cp_ctrl_s_update(cp, PMU_CP_CTRL_S_START, PMU_CP_CTRL_S_START);
	if (ret)
		return ret;
	regmap_update_bits(cp->pmu, PMU_CP_CONFIGURATION, PMU_CP_PWRON,
			   PMU_CP_PWRON);
	return cp_wait_status(cp, true);
}

static int cp_pmu_reset_assert(struct exynos_cp *cp)
{
	regmap_update_bits(cp->pmu, PMU_CP_CONFIGURATION, PMU_CP_PWRON, 0);
	return cp_wait_status(cp, false);
}

static int cp_pmu_reset_release(struct exynos_cp *cp)
{
	regmap_update_bits(cp->pmu, PMU_CP_CONFIGURATION, PMU_CP_PWRON,
			   PMU_CP_PWRON);
	return cp_wait_status(cp, true);
}

static void cp_pmu_dump_pc_no_pg(struct exynos_cp *cp, bool enable)
{
	regmap_update_bits(cp->pmu, PMU_CP_OUT, PMU_CP_OUT_DUMP_PC_NO_PG,
			   enable ? PMU_CP_OUT_DUMP_PC_NO_PG : 0);
}

/* mailbox */

static u32 mbox_get(struct exynos_cp *cp, unsigned int n)
{
	return readl(cp->mbox + MBOX_ISSR(n));
}

static void mbox_set(struct exynos_cp *cp, unsigned int n, u32 val)
{
	writel(val, cp->mbox + MBOX_ISSR(n));
}

static void mbox_ring(struct exynos_cp *cp, unsigned int n)
{
	writel(BIT(n), cp->mbox + MBOX_INTGR1);
}

static void mbox_sw_reset(struct exynos_cp *cp)
{
	writel(readl(cp->mbox + MBOX_MCUCTLR) | MBOX_MCUCTLR_MSWRST,
	       cp->mbox + MBOX_MCUCTLR);
	udelay(5);
}

static void mbox_unmask(struct exynos_cp *cp, unsigned int n)
{
	writel(readl(cp->mbox + MBOX_INTMR0) & ~MBOX_RX_INT(n),
	       cp->mbox + MBOX_INTMR0);
}

static unsigned int cp_status_field(struct exynos_cp *cp)
{
	return FIELD_GET(SBI_CP_STATUS, mbox_get(cp, MBX_CP2AP_STATUS));
}


/* link */

static void __iomem *cp_ipc(struct exynos_cp *cp);

static void cp_send_msg(struct exynos_cp *cp, u32 val)
{
	mbox_set(cp, MBX_AP2CP_MSG, val);
	mbox_ring(cp, INT_AP2CP_MSG);
}

static void cp_send_cmd(struct exynos_cp *cp, u32 cmd)
{
	dev_info(cp->dev, "cmd 0x%02x -> CP\n", cmd);
	cp_send_msg(cp, MSG_INT_VALID | MSG_CMD_VALID | cmd);
}

static u16 ring_get(struct exynos_cp *cp, u32 off)
{
	return readw(cp_ipc(cp) + off);
}

static void ring_set(struct exynos_cp *cp, u32 off, u16 val)
{
	writew(val, cp_ipc(cp) + off);
}

/* init_sbd_link: lay the rings out the way stock does, then PIF_INIT_DONE */
static void cp_sbd_init(struct exynos_cp *cp)
{
	void __iomem *ipc = cp_ipc(cp);
	u32 desc = SBD_RBPS + CP_NUM_LINKS * 2 * 2 * 2;
	u32 buff = SBD_BUFF_RGN;
	unsigned int i, d, j;

	memset_io(ipc + SBD_DESC_RGN, 0, SBD_DESC_END - SBD_DESC_RGN);
	writel(1, ipc + SBD_DESC_RGN);			/* version */
	writel(CP_NUM_LINKS, ipc + SBD_DESC_RGN + 4);
	writel(SBD_RBPS, ipc + SBD_DESC_RGN + 8);

	for (i = 0; i < CP_NUM_LINKS; i++) {
		for (d = UL; d <= DL; d++) {
			struct cp_ring *rb = &cp->ring[i][d];
			u32 rbd = SBD_RB_DESC + (i * 2 + d) * 20;

			rb->len = cp_links[i].len[d];
			rb->size = cp_links[i].size[d];
			rb->buff = buff;
			buff += rb->len * rb->size;
			rb->addr_v = desc;
			rb->size_v = desc + rb->len * 4;
			desc += rb->len * 8;
			/* rbps = {ul_rp[], ul_wp[], dl_rp[], dl_wp[]} */
			rb->rp = SBD_RBPS + ((d * 2) * CP_NUM_LINKS + i) * 2;
			rb->wp = SBD_RBPS + ((d * 2 + 1) * CP_NUM_LINKS + i) * 2;
			rb->total = rb->rcvd = 0;

			for (j = 0; j < rb->len; j++) {
				writel(rb->buff + j * rb->size,
				       ipc + rb->addr_v + j * 4);
				writel(0, ipc + rb->size_v + j * 4);
			}

			writew(cp_links[i].ch, ipc + rbd);
			writew(0, ipc + rbd + 2);
			writew(d, ipc + rbd + 4);		/* direction */
			writew(1, ipc + rbd + 6);		/* signaling: irq */
			writel(MSG_INT_VALID | MSG_SEND_DATA, ipc + rbd + 8);
			writew(rb->len, ipc + rbd + 12);
			writew(i, ipc + rbd + 14);		/* id */
			writew(rb->size, ipc + rbd + 16);
			writew(0, ipc + rbd + 18);		/* payload_offset */

			writel(rbd, ipc + SBD_RB_CH + i * 16 + d * 8);
			writel(rb->addr_v, ipc + SBD_RB_CH + i * 16 + d * 8 + 4);
		}
	}
	if (cp->spare) {
		void __iomem *sp = (void __iomem __force *)cp->spare;
		struct cp_ring *zc = &cp->ring[PS_UL_LINK][DL];

		/* every zerocopy DL slot but one gets its cell; dl_rp = how far we fed */
		for (j = 0; j < zc->len; j++)
			writeq(ZMB_SHMEM_SIZE + j * ZMB_CELL + ZMB_HEADROOM,
			       ipc + zc->buff + j * 8);
		zc->zc_rd = 0;
		writew(zc->len - 1, ipc + zc->rp);
		writew(0, ipc + zc->wp);

		memset_io(sp + PP_DESC_BASE, 0, PP_NUM * PP_DESC_SIZE);
		writel(SPARE_CP_BASE + PP_DESC_BASE, sp + 0x00);
		writel(PP_NUM, sp + 0x04);
		writel(SPARE_CP_BASE + PP_DATA_BASE, sp + 0x08);
		writel(0, sp + 0x0c);			/* fore_ptr, CP's */
		writel(0, sp + 0x10);			/* rear_ptr, ours */
	}
	wmb();
	dev_info(cp->dev, "SBD: %zu links, desc end %u, buffers end %u\n",
		 CP_NUM_LINKS, desc, buff);
	WRITE_ONCE(cp->sbd_up, true);
	cp_send_cmd(cp, CMD_PIF_INIT_DONE);
}

/* write_clk_table_to_shmem with no ECT tables + the ETDAC OTP word */
static void cp_write_init_end_data(struct exynos_cp *cp)
{
	void __iomem *ipc = cp_ipc(cp);
	static const char names[4][4] = { "CL0", "CL1", "MIF", "INT" };
	unsigned int i;
	u32 lo, hi;

	memcpy_toio(ipc + IPC_CLK_TABLE, "CT0", 4);
	writel(4, ipc + IPC_CLK_TABLE + 4);
	for (i = 0; i < 4; i++) {
		memcpy_toio(ipc + IPC_CLK_TABLE + 8 + i * 8, names[i], 4);
		writel(0, ipc + IPC_CLK_TABLE + 12 + i * 8);
	}

	if (cp->etdac_otp) {
		/* ETDAC0[479:464] = HPM_ASV14[31:16], ETDAC1[491:480] = HPM_ASV15[11:0] */
		lo = readl(cp->etdac_otp);
		hi = readl(cp->etdac_otp + 4);
		writew(0xedac, ipc + SBD_ETDAC);
		writew(0, ipc + SBD_ETDAC + 2);
		writel((((hi >> 8) & 0xf) << 24) | ((hi & 0xff) << 16) |
		       (((lo >> 24) & 0xff) << 8) | ((lo >> 16) & 0xff),
		       ipc + SBD_ETDAC + 4);
	}
	wmb();
}

/* rild_ready: the FMT channel and the RFS channel are both open */
static bool cp_iods_open(struct exynos_cp *cp)
{
	return atomic_read(&cp->iod[0].opened) &&
	       atomic_read(&cp->iod[2].opened);
}

/* rild_ready + INIT_END; called with cp->lock or from the irq thread */
static void cp_try_init_end(struct exynos_cp *cp)
{
	if (!READ_ONCE(cp->ipc_up) || !cp_iods_open(cp))
		return;
	if (!cp->init_end)
		cp_write_init_end_data(cp);
	cp->init_end = true;
	cp_send_cmd(cp, CMD_INIT_END);
}

/* cmd_phone_start_handler: shmem_reset_ipc_link, then online */
static void cp_phone_start(struct exynos_cp *cp)
{
	void __iomem *ipc = cp_ipc(cp);

	if (READ_ONCE(cp->ipc_up)) {
		/* the CP repeats CP_START every 100 ms until INIT_END */
		cp_try_init_end(cp);
		return;
	}
	writel(0, ipc + IPC_ACCESS);
	writel(0, ipc + IPC_MAGIC);
	writel(0, ipc + IPC_RAW_TX_HEAD);
	writel(0, ipc + IPC_RAW_TX_TAIL);
	writel(0, ipc + IPC_RAW_RX_HEAD);
	writel(0, ipc + IPC_RAW_RX_TAIL);
	writel(MEM_IPC_MAGIC, ipc + IPC_MAGIC);
	writel(1, ipc + IPC_ACCESS);
	wmb();
	if (readl(ipc + IPC_MAGIC) != MEM_IPC_MAGIC ||
	    readl(ipc + IPC_ACCESS) != 1) {
		dev_err(cp->dev, "IPC magic did not stick\n");
		return;
	}
	WRITE_ONCE(cp->ipc_up, true);
	cp->state = CP_ONLINE;
	dev_info(cp->dev, "CP_START: online\n");
	cp_try_init_end(cp);
}

static struct cp_iod *cp_ch2iod(struct exynos_cp *cp, u8 ch)
{
	unsigned int i;

	for (i = 0; i < CP_NUM_IODS; i++)
		if (cp->iod[i].ch == ch)
			return &cp->iod[i];
	return NULL;
}

static unsigned int sipc5_hdr_len(u8 cfg)
{
	if ((cfg & 3) == 3)
		return 5;		/* multi-frame control byte */
	if (cfg & SIPC5_EXT_LEN)
		return 6;		/* 32-bit length */
	return 4;
}

static unsigned int sipc5_frame_len(const u8 *h)
{
	if ((h[0] & 3) == SIPC5_EXT_LEN)
		return get_unaligned_le32(h + 2);
	return get_unaligned_le16(h + 2);
}

static void cp_deliver(struct exynos_cp *cp, u8 *frm, unsigned int len)
{
	unsigned int hdr = sipc5_hdr_len(frm[0]);
	struct cp_iod *iod = cp_ch2iod(cp, frm[1]);
	struct sk_buff *skb;

	cp->rx_frames++;
	if (!iod || !atomic_read(&iod->opened) || len < hdr ||
	    skb_queue_len(&iod->rxq) > 1024) {
		cp->rx_dropped++;
		return;
	}
	if ((frm[0] & 3) == 3)
		dev_warn_ratelimited(cp->dev, "ch %u multi-frame ctl 0x%02x\n",
				     frm[1], frm[4]);
	skb = alloc_skb(len - hdr, GFP_KERNEL);
	if (!skb) {
		cp->rx_dropped++;
		return;
	}
	skb_put_data(skb, frm + hdr, len - hdr);
	skb_queue_tail(&iod->rxq, skb);
	wake_up_interruptible(&iod->wq);
}

/* one raw IP packet of PS channel ch, from the CP's memory */
static void cp_ps_deliver(struct exynos_cp *cp, u8 ch, const void __iomem *src,
			  unsigned int len)
{
	struct net_device *ndev = cp->ps_dev;
	struct sk_buff *skb;
	u8 v;

	if (ch < PS_CH_FIRST || ch > PS_CH_LAST || !len || len > 2048) {
		dev_err_ratelimited(cp->dev, "PS DL ch %u len %u dropped\n", ch, len);
		return;
	}
	if (!cp->ps_ch_seen[ch - PS_CH_FIRST]) {
		cp->ps_ch_seen[ch - PS_CH_FIRST] = true;
		dev_info(cp->dev, "PS DL on ch %u\n", ch);
	}
	if (!ndev || !netif_running(ndev)) {
		cp->rx_dropped++;
		return;
	}
	skb = netdev_alloc_skb(ndev, len);
	if (!skb) {
		ndev->stats.rx_dropped++;
		return;
	}
	memcpy_fromio(skb_put(skb, len), src, len);
	v = skb->data[0] >> 4;
	if (v != 4 && v != 6) {
		ndev->stats.rx_errors++;
		dev_kfree_skb_any(skb);
		return;
	}
	skb->protocol = v == 6 ? htons(ETH_P_IPV6) : htons(ETH_P_IP);
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += len;
	netif_rx(skb);
}

/* ring 8 DL: raw IP per slot, channel in size_v[23:16] */
static void cp_rx_ps_ring(struct exynos_cp *cp, unsigned int id)
{
	struct cp_ring *rb = &cp->ring[id][DL];
	void __iomem *ipc = cp_ipc(cp);
	u16 rp = ring_get(cp, rb->rp), wp = ring_get(cp, rb->wp);
	u32 sz, off;

	if (rp >= rb->len || wp >= rb->len) {
		dev_err_ratelimited(cp->dev, "ring %u DL dirty rp %u wp %u\n",
				    id, rp, wp);
		return;
	}
	while (rp != wp) {
		rmb();
		sz = readl(ipc + rb->size_v + rp * 4);
		off = readl(ipc + rb->addr_v + rp * 4);
		if ((sz & 0xffff) <= rb->size && off < 0x500000 - (sz & 0xffff))
			cp_ps_deliver(cp, (sz >> 16) & 0xff, ipc + off, sz & 0xffff);
		else
			dev_err_ratelimited(cp->dev, "ring %u slot %u bad %#x @%#x\n",
					    id, rp, sz, off);
		mb();
		rp = (rp + 1) % rb->len;
		ring_set(cp, rb->rp, rp);
	}
}

/* ring 9 DL: read the cells the CP filled (up to dl_wp), give each slot back (dl_rp) */
static void cp_rx_zc_ring(struct exynos_cp *cp, unsigned int id)
{
	struct cp_ring *rb = &cp->ring[id][DL];
	void __iomem *ipc = cp_ipc(cp);
	void __iomem *zmb = (void __iomem __force *)cp->spare + ZMB_OFF;
	u16 rd = rb->zc_rd, fill = ring_get(cp, rb->wp), fed;
	u32 sz;

	if (!cp->spare || fill >= rb->len || rd >= rb->len)
		return;
	while (rd != fill) {
		rmb();
		sz = readl(ipc + rb->size_v + rd * 4);
		if ((sz & 0xffff) <= ZMB_CELL - ZMB_HEADROOM)
			cp_ps_deliver(cp, (sz >> 16) & 0xff,
				      zmb + rd * ZMB_CELL + ZMB_HEADROOM, sz & 0xffff);
		else
			dev_err_ratelimited(cp->dev, "ring %u zc slot %u bad %#x\n",
					    id, rd, sz);
		mb();
		/* the slot after the last fed one gets its cell back */
		fed = ring_get(cp, rb->rp);
		if (fed < rb->len) {
			writeq(ZMB_SHMEM_SIZE + fed * ZMB_CELL + ZMB_HEADROOM,
			       ipc + rb->buff + fed * 8);
			wmb();
			ring_set(cp, rb->rp, (fed + 1) % rb->len);
		}
		rd = (rd + 1) % rb->len;
	}
	rb->zc_rd = rd;
}

/* pktproc: the CP advances fore_ptr, we read up to it and give back via rear_ptr */
static void cp_rx_pktproc(struct exynos_cp *cp)
{
	void __iomem *sp = (void __iomem __force *)cp->spare;
	u32 fore, rear, len, addr;
	void __iomem *d;
	u8 ch;

	if (!sp)
		return;
	fore = readl(sp + 0x0c);
	rear = readl(sp + 0x10);
	if (fore >= PP_NUM || rear >= PP_NUM) {
		dev_err_ratelimited(cp->dev, "pktproc dirty fore %u rear %u\n", fore, rear);
		return;
	}
	while (rear != fore) {
		rmb();
		d = sp + PP_DESC_BASE + rear * PP_DESC_SIZE;
		addr = readl(d);
		len = readw(d + 16);
		ch = readb(d + 18);
		if (ch != 0xff) {
			if (addr >= SPARE_CP_BASE + PP_DATA_BASE &&
			    addr - SPARE_CP_BASE <= PP_DATA_END - len)
				cp_ps_deliver(cp, ch, sp + (addr - SPARE_CP_BASE), len);
			else
				dev_err_ratelimited(cp->dev, "pktproc %u bad addr %#x len %u\n",
						    rear, addr, len);
		}
		mb();
		rear = (rear + 1) % PP_NUM;
		writel(rear, sp + 0x10);
		if (rear == fore)
			fore = readl(sp + 0x0c) % PP_NUM;
	}
}

/* sbd_pio_rx + check_more: one SIPC5 frame per slot, or one spread over several */
static void cp_rx_ring(struct exynos_cp *cp, unsigned int id)
{
	struct cp_ring *rb = &cp->ring[id][DL];
	void __iomem *ipc = cp_ipc(cp);
	u16 rp = ring_get(cp, rb->rp), wp = ring_get(cp, rb->wp);
	unsigned int len;
	u8 *src;

	if (rp >= rb->len || wp >= rb->len) {
		dev_err_ratelimited(cp->dev, "ring %u DL dirty rp %u wp %u\n",
				    id, rp, wp);
		return;
	}
	while (rp != wp) {
		rmb();
		len = readl(ipc + rb->size_v + rp * 4) & 0xffff;
		src = (u8 __force *)(ipc + readl(ipc + rb->addr_v + rp * 4));
		if (len > rb->size || (u8 __force *)src < (u8 __force *)ipc ||
		    src + len > (u8 __force *)ipc + 0x500000) {
			dev_err_ratelimited(cp->dev, "ring %u slot %u bad len %u\n",
					    id, rp, len);
			goto next;
		}
		if (!rb->total) {
			u8 h[6];

			if (len < 4)
				goto next;
			memcpy_fromio(h, (void __iomem *)src, min(len, 6u));
			rb->total = sipc5_frame_len(h);
			if ((h[0] & SIPC5_START) != SIPC5_START ||
			    rb->total < 4 || rb->total > 0x40000) {
				dev_err_ratelimited(cp->dev, "ring %u bad frame %6ph\n",
						    id, h);
				rb->total = 0;
				goto next;
			}
			rb->rcvd = 0;
			kfree(rb->frm);
			rb->frm = kmalloc(rb->total, GFP_KERNEL);
		}
		if (rb->frm)
			memcpy_fromio(rb->frm + rb->rcvd, (void __iomem *)src,
				      min(len, rb->total - rb->rcvd));
		rb->rcvd += len;
		if (rb->rcvd >= rb->total) {
			if (rb->frm)
				cp_deliver(cp, rb->frm, rb->total);
			else
				cp->rx_dropped++;
			kfree(rb->frm);
			rb->frm = NULL;
			rb->total = rb->rcvd = 0;
		}
next:
		rp = (rp + 1) % rb->len;
		ring_set(cp, rb->rp, rp);
	}
}

static void cp_rx(struct exynos_cp *cp)
{
	unsigned int i;

	if (!READ_ONCE(cp->sbd_up))
		return;
	cp_rx_pktproc(cp);
	for (i = 0; i < CP_NUM_LINKS; i++) {
		if (cp_links[i].zerocopy_dl)
			cp_rx_zc_ring(cp, i);
		else if (cp_links[i].ps)
			cp_rx_ps_ring(cp, i);
		else
			cp_rx_ring(cp, i);
	}
	if (cp->ps_dev && netif_queue_stopped(cp->ps_dev))
		netif_wake_queue(cp->ps_dev);
}

/* sbd_pio_tx */
static int cp_tx(struct exynos_cp *cp, struct cp_iod *iod,
		 const char __user *data, size_t count)
{
	struct cp_ring *rb = &cp->ring[iod->link][UL];
	void __iomem *ipc = cp_ipc(cp);
	unsigned int len = 4 + count;
	u16 rp, wp;
	u8 *frm;
	int ret = count;

	if (len > rb->size)
		return -EMSGSIZE;
	frm = kmalloc(len, GFP_KERNEL);
	if (!frm)
		return -ENOMEM;
	frm[0] = SIPC5_START;			/* IPC_ALIGNED is off */
	frm[1] = iod->ch;
	put_unaligned_le16(len, frm + 2);
	if (copy_from_user(frm + 4, data, count)) {
		kfree(frm);
		return -EFAULT;
	}

	mutex_lock(&cp->tx_lock);
	rp = ring_get(cp, rb->rp);
	wp = ring_get(cp, rb->wp);
	if (rp >= rb->len || wp >= rb->len) {
		ret = -EIO;
	} else if ((wp + 1) % rb->len == rp) {
		ret = -EAGAIN;
	} else {
		memcpy_toio(ipc + readl(ipc + rb->addr_v + wp * 4), frm, len);
		writel(len, ipc + rb->size_v + wp * 4);
		wmb();
		ring_set(cp, rb->wp, (wp + 1) % rb->len);
		wmb();
		cp_send_msg(cp, MSG_INT_VALID | MSG_SEND_DATA);
	}
	mutex_unlock(&cp->tx_lock);
	kfree(frm);
	return ret;
}

/* rmnet0 */

struct cp_ps_priv {
	struct exynos_cp *cp;
};

static netdev_tx_t cp_ps_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct exynos_cp *cp = ((struct cp_ps_priv *)netdev_priv(ndev))->cp;
	struct cp_ring *rb = &cp->ring[PS_UL_LINK][UL];
	void __iomem *ipc = cp_ipc(cp);
	unsigned long flags;
	u16 rp, wp;

	if (!READ_ONCE(cp->ipc_up) || skb->len > rb->size || skb_linearize(skb)) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&cp->ps_tx_lock, flags);
	rp = ring_get(cp, rb->rp);
	wp = ring_get(cp, rb->wp);
	if (rp >= rb->len || wp >= rb->len) {
		spin_unlock_irqrestore(&cp->ps_tx_lock, flags);
		ndev->stats.tx_errors++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if ((wp + 1) % rb->len == rp) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&cp->ps_tx_lock, flags);
		return NETDEV_TX_BUSY;
	}
	memcpy_toio(ipc + readl(ipc + rb->addr_v + wp * 4), skb->data, skb->len);
	writel(skb->len | (READ_ONCE(ps_ul_ch) & 0xff) << 16, ipc + rb->size_v + wp * 4);
	wmb();
	ring_set(cp, rb->wp, (wp + 1) % rb->len);
	wmb();
	cp_send_msg(cp, MSG_INT_VALID | MSG_SEND_DATA);
	spin_unlock_irqrestore(&cp->ps_tx_lock, flags);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops cp_ps_ops = {
	.ndo_start_xmit	= cp_ps_xmit,
};

static void cp_ps_setup(struct net_device *ndev)
{
	ndev->netdev_ops = &cp_ps_ops;
	ndev->type = ARPHRD_RAWIP;
	ndev->flags = IFF_POINTOPOINT | IFF_NOARP;
	ndev->hard_header_len = 0;
	ndev->addr_len = 0;
	ndev->mtu = PS_MTU;
	ndev->min_mtu = 68;
	ndev->max_mtu = 2048;
	ndev->tx_queue_len = 1000;
	ndev->needs_free_netdev = true;
}

static void cp_handle_msg(struct exynos_cp *cp, u32 msg)
{
	if (!(msg & MSG_INT_VALID))
		return;
	if (!(msg & MSG_CMD_VALID)) {
		cp_rx(cp);
		return;
	}
	switch (msg & MSG_CMD_FIELD) {
	case CMD_INIT_START:
		WRITE_ONCE(cp->init_start, true);
		if (!cp->sbd_up)
			cp_sbd_init(cp);
		break;
	case CMD_PHONE_START:
		cp_phone_start(cp);
		break;
	case CMD_CRASH_RESET:
	case CMD_CRASH_EXIT:
		dev_err(cp->dev, "CP crashed (cmd 0x%02x), cp_status 0x%08x\n",
			msg & MSG_CMD_FIELD, mbox_get(cp, MBX_CP2AP_STATUS));
		cp->state = CP_CRASHED;
		WRITE_ONCE(cp->ipc_up, false);
		WRITE_ONCE(cp->sbd_up, false);
		break;
	default:
		dev_info(cp->dev, "unhandled CP cmd 0x%02x\n", msg & MSG_CMD_FIELD);
		break;
	}
}

static irqreturn_t cp_mbox_irq(int irq, void *data)
{
	struct exynos_cp *cp = data;
	u32 stat = readl(cp->mbox + MBOX_INTSR0) & GENMASK(31, 16);

	stat &= ~readl(cp->mbox + MBOX_INTMR0);
	if (!stat)
		return IRQ_NONE;
	writel(stat, cp->mbox + MBOX_INTCR0);

	if (stat & MBOX_RX_INT(IRQ_CP2AP_MSG)) {
		u32 msg = mbox_get(cp, MBX_CP2AP_MSG);

		WRITE_ONCE(cp->cp2ap_msg, msg);
		cp->msg_count++;
		dev_dbg(cp->dev, "CP2AP msg 0x%04x\n", msg);
		cp_handle_msg(cp, msg);
	}
	if (stat & MBOX_RX_INT(IRQ_CP2AP_STATUS))
		dev_dbg(cp->dev, "CP2AP status 0x%08x\n",
			mbox_get(cp, MBX_CP2AP_STATUS));

	wake_up(&cp->wq);
	return IRQ_HANDLED;
}

/* EL3 */

static unsigned long cp_smc(unsigned long id, unsigned long a1,
			    unsigned long a2, unsigned long a3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(id, a1, a2, a3, 0, 0, 0, 0, &res);
	return res.a0;
}

static int cp_security_request(struct exynos_cp *cp, struct modem_sec_req *req)
{
	unsigned long mode = req->mode, p2, p3 = req->param3, ret;
	int tries = 0;

	/* shm_get_security_param2/3 of the stock shm_ipc.c */
	switch (req->mode) {
	case 0:			/* normal boot: BOOT size, MAIN size */
	case 1:			/* dump */
		p2 = req->param2;
		mode |= cp->phys;
		break;
	case 2:			/* re-init: opens the carve-out to the AP */
		p2 = 0;
		p3 = 0;
		break;
	case 7:			/* manual: TOC at base + offset, its size */
		p2 = cp->phys + req->param2;
		break;
	default:
		return -EINVAL;
	}

	if (!cp_powered(cp)) {
		int err = cp_pmu_init(cp);

		if (err)
			return err;
	}
	cp_smc(SMC_ID_SSS_CLK, SSS_CLK_ENABLE, 0, 0);

	dev_info(cp->dev, "security request mode 0x%lx, 0x%lx, 0x%lx\n",
		 mode, p2, p3);
	ret = cp_smc(SMC_ID_CP, mode, p2, p3);
	while (ret == CP_CHECK_SIGN_NOT_FINISH && tries++ < CP_SEC_MAX_TRY)
		ret = cp_smc(SMC_ID_CP, CP_SEC_MODE_CHECK_CONTINUE, 0, 0);

	cp_smc(SMC_ID_SSS_CLK, SSS_CLK_DISABLE, 0, 0);
	dev_info(cp->dev, "security request -> 0x%lx\n", ret);

	/* a cold CP answers re-init with CP_NOT_WORKING; cbd carries on */
	if (ret && !(req->mode == 2 && ret == CP_NOT_WORKING))
		return -EIO;
	if (req->mode == 2)
		cp->mem_ok = true;
	return 0;
}

/* boot steps */

static int cp_reset(struct exynos_cp *cp)
{
	int ret = 0;

	cp->state = CP_OFFLINE;
	cp->mem_ok = false;
	WRITE_ONCE(cp->sbd_up, false);
	WRITE_ONCE(cp->ipc_up, false);
	cp->init_end = false;
	if (cp_powered(cp)) {
		mbox_ring(cp, INT_AP2CP_WAKEUP);
		usleep_range(5000, 10000);
		cp_pmu_dump_pc_no_pg(cp, true);
		ret = cp_pmu_reset_assert(cp);
		usleep_range(5000, 10000);
		ret = ret ?: cp_pmu_reset_release(cp);
		mbox_sw_reset(cp);
	}
	return ret;
}

static int cp_on(struct exynos_cp *cp)
{
	int i;

	for (i = 0; i < MBOX_NUM_ISSR; i++)
		mbox_set(cp, i, 0);

	mbox_set(cp, MBX_AP2CP_STATUS,
		 FIELD_PREP(SBI_DS_DET, S10E_DS_DET) |
		 FIELD_PREP(SBI_SYS_REV, S10E_HW_REV) |
		 FIELD_PREP(SBI_DEVICE_TYPE, S10E_DEVICE_TYPE) |
		 SBI_PDA_ACTIVE);
	dev_info(cp->dev, "ap_status 0x%08x\n", mbox_get(cp, MBX_AP2CP_STATUS));

	writel(1, cp->ap2cp_cfg);
	if (readl(cp->ap2cp_cfg) != 1) {
		dev_err(cp->dev, "AP2CP_CFG did not take\n");
		return -EIO;
	}
	return 0;
}

static int cp_boot_on(struct exynos_cp *cp)
{
	int cnt = 100;

	cp->state = CP_BOOTING;
	while (!cp_status_field(cp)) {
		if (!--cnt) {
			dev_err(cp->dev, "CP status never came up\n");
			return -ETIMEDOUT;
		}
		usleep_range(10000, 20000);
	}
	dev_info(cp->dev, "cp_status %u\n", cp_status_field(cp));
	return 0;
}

static void __iomem *cp_ipc(struct exynos_cp *cp)
{
	return (void __iomem *)(cp->base + CP_IPC_OFFSET);
}

/* shmem_start_download + s5000ap_modem_boot_confirm */
static int cp_dl_start(struct exynos_cp *cp)
{
	void __iomem *ipc = cp_ipc(cp);
	u32 st;

	if (cp->state != CP_BOOTING)
		return -EPERM;
	writel(0, ipc + IPC_RAW_TX_HEAD);
	writel(0, ipc + IPC_RAW_TX_TAIL);
	writel(0, ipc + IPC_RAW_RX_HEAD);
	writel(0, ipc + IPC_RAW_RX_TAIL);
	writel(0, ipc + IPC_ACCESS);
	writel(MEM_BOOT_MAGIC, ipc + IPC_MAGIC);
	wmb();
	if (readl(ipc + IPC_MAGIC) != MEM_BOOT_MAGIC) {
		dev_err(cp->dev, "BOOT magic did not stick\n");
		return -EFAULT;
	}
	WRITE_ONCE(cp->init_start, false);

	st = mbox_get(cp, MBX_AP2CP_STATUS) & ~SBI_AP_STATUS;
	mbox_set(cp, MBX_AP2CP_STATUS, st | FIELD_PREP(SBI_AP_STATUS, 1));
	dev_info(cp->dev, "dl start, ap_status 0x%08x\n",
		 mbox_get(cp, MBX_AP2CP_STATUS));
	return 0;
}

/* s5000ap_boot_off: wait until the CP is up (CP_START after INIT_START) */
static int cp_boot_off(struct exynos_cp *cp)
{
	cp_pmu_dump_pc_no_pg(cp, false);
	if (!wait_event_timeout(cp->wq, READ_ONCE(cp->ipc_up),
				msecs_to_jiffies(15000))) {
		dev_err(cp->dev, "CP not up: init_start %d sbd %d msg 0x%04x cp_status 0x%08x\n",
			READ_ONCE(cp->init_start), READ_ONCE(cp->sbd_up),
			READ_ONCE(cp->cp2ap_msg), mbox_get(cp, MBX_CP2AP_STATUS));
		return -ETIMEDOUT;
	}
	dev_info(cp->dev, "CP up, cp_status 0x%08x\n",
		 mbox_get(cp, MBX_CP2AP_STATUS));
	return 0;
}

static int cp_xmit_boot(struct exynos_cp *cp, void __user *arg)
{
	struct modem_firmware mf;

	if (copy_from_user(&mf, arg, sizeof(mf)))
		return -EFAULT;
	if (!cp->mem_ok)
		return -EPERM;
	/* the stock check, including the overflow fix */
	if (mf.size > cp->size || mf.len > cp->size ||
	    mf.m_offset > cp->size - mf.len)
		return -EINVAL;
	if (copy_from_user(cp->base + mf.m_offset,
			   u64_to_user_ptr(mf.binary), mf.len))
		return -EFAULT;
	return 0;
}

static long cp_boot_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct exynos_cp *cp = container_of(file->private_data,
					    struct exynos_cp, boot);
	struct modem_sec_req req;
	int ret;

	mutex_lock(&cp->lock);
	switch (cmd) {
	case IOCTL_MODEM_RESET:
		ret = cp_reset(cp);
		break;
	case IOCTL_SECURITY_REQ:
		ret = copy_from_user(&req, (void __user *)arg, sizeof(req)) ?
		      -EFAULT : cp_security_request(cp, &req);
		break;
	case IOCTL_MODEM_XMIT_BOOT:
		ret = cp_xmit_boot(cp, (void __user *)arg);
		break;
	case IOCTL_MODEM_ON:
		ret = cp_on(cp);
		break;
	case IOCTL_MODEM_BOOT_ON:
		ret = cp_boot_on(cp);
		break;
	case IOCTL_MODEM_DL_START:
		ret = cp_dl_start(cp);
		break;
	case IOCTL_MODEM_BOOT_OFF:
		mutex_unlock(&cp->lock);
		return cp_boot_off(cp);
	case IOCTL_MODEM_STATUS:
		ret = cp->state;
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&cp->lock);
	return ret;
}

/*
 * The boot handshake (std_udl_req_resp): cbd writes and reads 4-byte words
 * on umts_boot0. They travel SIPC5-framed through the legacy raw queues;
 * the CP bootloader polls the TX head, so no doorbell (xmit_udl rings none).
 */
static ssize_t cp_boot_write(struct file *file, const char __user *data,
			     size_t count, loff_t *ppos)
{
	struct exynos_cp *cp = container_of(file->private_data,
					    struct exynos_cp, boot);
	void __iomem *ipc = cp_ipc(cp);
	u8 frm[4 + 64];
	unsigned int len, in, out, i;

	if (cp->state != CP_BOOTING)
		return -EPERM;
	if (!count || count > sizeof(frm) - 4)
		return -EINVAL;
	len = 4 + count;
	frm[0] = SIPC5_START | SIPC5_PADDING;
	frm[1] = SIPC5_CH_BOOT0;
	frm[2] = len & 0xff;
	frm[3] = len >> 8;
	if (copy_from_user(frm + 4, data, count))
		return -EFAULT;

	in = readl(ipc + IPC_RAW_TX_HEAD);
	out = readl(ipc + IPC_RAW_TX_TAIL);
	if (in >= IPC_RAW_TX_SIZE || out >= IPC_RAW_TX_SIZE)
		return -EIO;
	if ((out - in - 1 + IPC_RAW_TX_SIZE) % IPC_RAW_TX_SIZE < ALIGN(len, 4))
		return -ENOSPC;
	for (i = 0; i < ALIGN(len, 4); i++)
		writeb(i < len ? frm[i] : 0,
		       ipc + IPC_RAW_TX_BUFF + (in + i) % IPC_RAW_TX_SIZE);
	wmb();
	writel((in + ALIGN(len, 4)) % IPC_RAW_TX_SIZE, ipc + IPC_RAW_TX_HEAD);
	dev_info(cp->dev, "boot tx %*ph\n", len, frm);
	return count;
}

static ssize_t cp_boot_read(struct file *file, char __user *data,
			    size_t count, loff_t *ppos)
{
	struct exynos_cp *cp = container_of(file->private_data,
					    struct exynos_cp, boot);
	void __iomem *ipc = cp_ipc(cp);
	unsigned int in, out, len, hdr, i, tries = 0;
	u8 frm[4 + 64];

	if (cp->state != CP_BOOTING)
		return -EPERM;
	for (;;) {
		in = readl(ipc + IPC_RAW_RX_HEAD);
		out = readl(ipc + IPC_RAW_RX_TAIL);
		if (in != out)
			break;
		if (++tries > 10000) {		/* ~5 s */
			dev_err(cp->dev, "boot rx timeout, tx %x/%x\n",
				readl(ipc + IPC_RAW_TX_HEAD),
				readl(ipc + IPC_RAW_TX_TAIL));
			return -ETIMEDOUT;
		}
		if (signal_pending(current))
			return -ERESTARTSYS;
		usleep_range(400, 600);
	}
	rmb();
	if (in >= IPC_RAW_RX_SIZE || out >= IPC_RAW_RX_SIZE)
		return -EIO;
	for (i = 0; i < 4; i++)
		frm[i] = readb(ipc + IPC_RAW_RX_BUFF + (out + i) % IPC_RAW_RX_SIZE);
	len = frm[2] | frm[3] << 8;
	hdr = 4 + ((frm[0] & SIPC5_EXT_LEN) ? 2 : (frm[0] & SIPC5_CTL) ? 1 : 0);
	if ((frm[0] & SIPC5_START) != SIPC5_START || len < hdr ||
	    len > sizeof(frm)) {
		dev_err(cp->dev, "boot rx bad frame %*ph (%x/%x)\n", 4, frm,
			in, out);
		writel(in, ipc + IPC_RAW_RX_TAIL);
		return -EIO;
	}
	for (i = 4; i < len; i++)
		frm[i] = readb(ipc + IPC_RAW_RX_BUFF + (out + i) % IPC_RAW_RX_SIZE);
	if (frm[0] & SIPC5_PADDING)
		len = ALIGN(len, 4);
	writel((out + len) % IPC_RAW_RX_SIZE, ipc + IPC_RAW_RX_TAIL);
	dev_info(cp->dev, "boot rx %*ph\n", frm[2] | frm[3] << 8, frm);

	len = (frm[2] | frm[3] << 8) - hdr;
	len = min_t(size_t, len, count);
	if (copy_to_user(data, frm + hdr, len))
		return -EFAULT;
	return len;
}


/* umts_ipc0 / umts_rfs0 */

static int cp_iod_open(struct inode *inode, struct file *file)
{
	struct cp_iod *iod = container_of(file->private_data, struct cp_iod,
					  misc);
	struct exynos_cp *cp = iod->cp;

	if (atomic_inc_return(&iod->opened) == 1)
		skb_queue_purge(&iod->rxq);
	file->private_data = iod;
	mutex_lock(&cp->lock);
	if (!cp->init_end)
		cp_try_init_end(cp);
	mutex_unlock(&cp->lock);
	return 0;
}

static int cp_iod_release(struct inode *inode, struct file *file)
{
	struct cp_iod *iod = file->private_data;

	if (atomic_dec_and_test(&iod->opened))
		skb_queue_purge(&iod->rxq);
	return 0;
}

static ssize_t cp_iod_read(struct file *file, char __user *data, size_t count,
			   loff_t *ppos)
{
	struct cp_iod *iod = file->private_data;
	struct exynos_cp *cp = iod->cp;
	struct sk_buff *skb;
	size_t len;
	int ret;

	for (;;) {
		skb = skb_dequeue(&iod->rxq);
		if (skb)
			break;
		if (cp->state == CP_CRASHED)
			return -EIO;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(iod->wq,
					       !skb_queue_empty(&iod->rxq) ||
					       cp->state == CP_CRASHED);
		if (ret)
			return ret;
	}
	len = min_t(size_t, skb->len, count);
	if (len < skb->len)
		dev_warn_ratelimited(cp->dev, "ch %u: read %zu of %u bytes\n",
				     iod->ch, count, skb->len);
	ret = copy_to_user(data, skb->data, len) ? -EFAULT : len;
	kfree_skb(skb);
	return ret;
}

static ssize_t cp_iod_write(struct file *file, const char __user *data,
			    size_t count, loff_t *ppos)
{
	struct cp_iod *iod = file->private_data;
	struct exynos_cp *cp = iod->cp;

	if (!READ_ONCE(cp->ipc_up) || cp->state != CP_ONLINE)
		return -EPERM;
	if (!count)
		return 0;
	return cp_tx(cp, iod, data, count);
}

static __poll_t cp_iod_poll(struct file *file, poll_table *wait)
{
	struct cp_iod *iod = file->private_data;
	struct exynos_cp *cp = iod->cp;
	__poll_t mask = 0;

	poll_wait(file, &iod->wq, wait);
	if (!skb_queue_empty(&iod->rxq))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (cp->state == CP_ONLINE)
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (cp->state == CP_CRASHED)
		mask |= EPOLLHUP | EPOLLERR;
	return mask;
}

static const struct file_operations cp_iod_fops = {
	.owner		= THIS_MODULE,
	.open		= cp_iod_open,
	.release	= cp_iod_release,
	.read		= cp_iod_read,
	.write		= cp_iod_write,
	.poll		= cp_iod_poll,
	.llseek		= noop_llseek,
};


/* WWAN AT port, bridged to userspace */

static int cp_at_start(struct wwan_port *port)
{
	return 0;
}

static void cp_at_stop(struct wwan_port *port)
{
}

static int cp_at_tx(struct wwan_port *port, struct sk_buff *skb)
{
	struct exynos_cp *cp = wwan_port_get_drvdata(port);

	if (!atomic_read(&cp->at_bridge_open) || skb_queue_len(&cp->at_txq) > 256) {
		/* nobody translates: answer like a dead modem would, not at all */
		consume_skb(skb);
		return 0;
	}
	skb_queue_tail(&cp->at_txq, skb);
	wake_up_interruptible(&cp->at_wq);
	return 0;
}

static const struct wwan_port_ops cp_at_ops = {
	.start	= cp_at_start,
	.stop	= cp_at_stop,
	.tx	= cp_at_tx,
};

static int cp_atb_open(struct inode *inode, struct file *file)
{
	struct exynos_cp *cp = container_of(file->private_data,
					    struct exynos_cp, at_bridge);

	struct wwan_port *port;

	if (atomic_cmpxchg(&cp->at_bridge_open, 0, 1))
		return -EBUSY;
	skb_queue_purge(&cp->at_txq);
	/*
	 * The port only exists while a translator listens: ModemManager
	 * probes a new port at once and gives up on one that stays silent.
	 */
	port = wwan_create_port(cp->dev, WWAN_PORT_AT, &cp_at_ops, NULL, cp);
	if (IS_ERR(port)) {
		atomic_set(&cp->at_bridge_open, 0);
		return PTR_ERR(port);
	}
	WRITE_ONCE(cp->at_port, port);
	file->private_data = cp;
	return 0;
}

static int cp_atb_release(struct inode *inode, struct file *file)
{
	struct exynos_cp *cp = file->private_data;

	wwan_remove_port(cp->at_port);
	WRITE_ONCE(cp->at_port, NULL);
	atomic_set(&cp->at_bridge_open, 0);
	skb_queue_purge(&cp->at_txq);
	return 0;
}

static ssize_t cp_atb_read(struct file *file, char __user *data, size_t count,
			   loff_t *ppos)
{
	struct exynos_cp *cp = file->private_data;
	struct sk_buff *skb;
	size_t len;
	int ret;

	for (;;) {
		skb = skb_dequeue(&cp->at_txq);
		if (skb)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(cp->at_wq,
					       !skb_queue_empty(&cp->at_txq));
		if (ret)
			return ret;
	}
	len = min_t(size_t, skb->len, count);
	if (copy_to_user(data, skb->data, len)) {
		skb_queue_head(&cp->at_txq, skb);
		return -EFAULT;
	}
	if (len < skb->len) {
		skb_pull(skb, len);
		skb_queue_head(&cp->at_txq, skb);
	} else {
		consume_skb(skb);
	}
	return len;
}

static ssize_t cp_atb_write(struct file *file, const char __user *data,
			    size_t count, loff_t *ppos)
{
	struct exynos_cp *cp = file->private_data;
	struct sk_buff *skb;

	if (!count)
		return 0;
	count = min_t(size_t, count, 4096);
	skb = alloc_skb(count, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	if (copy_from_user(skb_put(skb, count), data, count)) {
		kfree_skb(skb);
		return -EFAULT;
	}
	wwan_port_rx(cp->at_port, skb);
	return count;
}

static __poll_t cp_atb_poll(struct file *file, poll_table *wait)
{
	struct exynos_cp *cp = file->private_data;
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	poll_wait(file, &cp->at_wq, wait);
	if (!skb_queue_empty(&cp->at_txq))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static const struct file_operations cp_atb_fops = {
	.owner		= THIS_MODULE,
	.open		= cp_atb_open,
	.release	= cp_atb_release,
	.read		= cp_atb_read,
	.write		= cp_atb_write,
	.poll		= cp_atb_poll,
	.llseek		= noop_llseek,
};

static const struct file_operations cp_boot_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= cp_boot_ioctl,
	.read		= cp_boot_read,
	.write		= cp_boot_write,
};

/* debugfs-free status for bring-up */
static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct exynos_cp *cp = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "state %d pmu %d mem_ok %d ap_status 0x%08x cp_status 0x%08x msg 0x%04x count %lu sbd %d ipc %d init_end %d rx %lu dropped %lu\n",
			  cp->state, cp_powered(cp), cp->mem_ok,
			  mbox_get(cp, MBX_AP2CP_STATUS),
			  mbox_get(cp, MBX_CP2AP_STATUS),
			  READ_ONCE(cp->cp2ap_msg), cp->msg_count,
			  cp->sbd_up, cp->ipc_up, cp->init_end,
			  cp->rx_frames, cp->rx_dropped);
}
static DEVICE_ATTR_RO(status);

static struct attribute *cp_attrs[] = {
	&dev_attr_status.attr,
	NULL
};
ATTRIBUTE_GROUPS(cp);

static int exynos_cp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reserved_mem *rmem;
	struct device_node *np;
	struct exynos_cp *cp;
	int irq, ret, i;

	cp = devm_kzalloc(dev, sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;
	cp->dev = dev;
	mutex_init(&cp->lock);
	mutex_init(&cp->tx_lock);
	init_waitqueue_head(&cp->wq);
	platform_set_drvdata(pdev, cp);

	cp->pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						  "samsung,pmu-syscon");
	if (IS_ERR(cp->pmu))
		return dev_err_probe(dev, PTR_ERR(cp->pmu), "no PMU\n");

	cp->mbox = devm_platform_ioremap_resource_byname(pdev, "mailbox");
	if (IS_ERR(cp->mbox))
		return PTR_ERR(cp->mbox);
	cp->ap2cp_cfg = devm_platform_ioremap_resource_byname(pdev, "ap2cp-cfg");
	if (IS_ERR(cp->ap2cp_cfg))
		return PTR_ERR(cp->ap2cp_cfg);

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no memory-region\n");
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem)
		return dev_err_probe(dev, -EINVAL, "memory-region not reserved\n");
	cp->phys = rmem->base;
	cp->size = rmem->size;
	/*
	 * Mapped, never touched until the re-init SMC has opened the
	 * carve-out (the stock order); an early access can fault.
	 */
	cp->base = devm_memremap(dev, cp->phys, cp->size, MEMREMAP_WC);
	if (IS_ERR(cp->base))
		return PTR_ERR(cp->base);

	spin_lock_init(&cp->ps_tx_lock);
	np = of_find_node_by_path("/reserved-memory/modem_spare@d0000000");
	rmem = np ? of_reserved_mem_lookup(np) : NULL;
	of_node_put(np);
	if (rmem && rmem->base == SPARE_PHYS && rmem->size >= SPARE_SIZE)
		cp->spare = devm_memremap(dev, SPARE_PHYS, SPARE_SIZE, MEMREMAP_WC);
	if (IS_ERR_OR_NULL(cp->spare)) {
		dev_warn(dev, "no modem_spare: no pktproc DL\n");
		cp->spare = NULL;
	}

	/* all CP doorbells masked until someone listens */
	writel(GENMASK(31, 16), cp->mbox + MBOX_INTMR0);
	writel(GENMASK(31, 16), cp->mbox + MBOX_INTCR0);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	/* threaded: INIT_START lays out 64 KiB of descriptors, RX copies frames */
	ret = devm_request_threaded_irq(dev, irq, NULL, cp_mbox_irq,
					IRQF_ONESHOT, dev_name(dev), cp);
	if (ret)
		return ret;
	mbox_unmask(cp, IRQ_CP2AP_MSG);
	mbox_unmask(cp, IRQ_CP2AP_STATUS);

	cp->boot.minor = MISC_DYNAMIC_MINOR;
	cp->boot.name = "umts_boot0";
	cp->boot.fops = &cp_boot_fops;
	cp->boot.parent = dev;
	ret = misc_register(&cp->boot);
	if (ret)
		return ret;

	skb_queue_head_init(&cp->at_txq);
	init_waitqueue_head(&cp->at_wq);
	cp->at_bridge.minor = MISC_DYNAMIC_MINOR;
	cp->at_bridge.name = "umts_atbridge";
	cp->at_bridge.fops = &cp_atb_fops;
	cp->at_bridge.parent = dev;
	ret = misc_register(&cp->at_bridge);
	if (ret) {
		misc_deregister(&cp->boot);
		return ret;
	}

	/* the OTP words stock copies to the CP at INIT_END (chipid block) */
	cp->etdac_otp = devm_ioremap(dev, 0x1000a038, 8);

	for (i = 0; i < CP_NUM_IODS; i++) {
		struct cp_iod *iod = &cp->iod[i];

		iod->cp = cp;
		iod->ch = cp_iod_cfg[i].ch;
		iod->link = cp_iod_cfg[i].link;
		skb_queue_head_init(&iod->rxq);
		init_waitqueue_head(&iod->wq);
		iod->misc.minor = MISC_DYNAMIC_MINOR;
		iod->misc.name = cp_iod_cfg[i].name;
		iod->misc.fops = &cp_iod_fops;
		iod->misc.parent = dev;
		ret = misc_register(&iod->misc);
		if (ret) {
			while (i--)
				misc_deregister(&cp->iod[i].misc);
			misc_deregister(&cp->at_bridge);
			misc_deregister(&cp->boot);
			return ret;
		}
	}

	cp->ps_dev = alloc_netdev(sizeof(struct cp_ps_priv), "rmnet%d",
				  NET_NAME_ENUM, cp_ps_setup);
	if (cp->ps_dev) {
		((struct cp_ps_priv *)netdev_priv(cp->ps_dev))->cp = cp;
		SET_NETDEV_DEV(cp->ps_dev, dev);
		if (register_netdev(cp->ps_dev)) {
			free_netdev(cp->ps_dev);
			cp->ps_dev = NULL;
		}
	}
	if (!cp->ps_dev)
		dev_warn(dev, "no PS netdev\n");

	dev_info(dev, "CP carve-out %pa + 0x%zx, PMU says %s\n", &cp->phys,
		 cp->size, cp_powered(cp) ? "running" : "off");
	return 0;
}

static void exynos_cp_remove(struct platform_device *pdev)
{
	struct exynos_cp *cp = platform_get_drvdata(pdev);
	unsigned int i, d;

	if (cp->ps_dev)
		unregister_netdev(cp->ps_dev);
	for (i = 0; i < CP_NUM_IODS; i++) {
		misc_deregister(&cp->iod[i].misc);
		skb_queue_purge(&cp->iod[i].rxq);
	}
	misc_deregister(&cp->at_bridge);	/* no opener left: port is gone */
	skb_queue_purge(&cp->at_txq);
	misc_deregister(&cp->boot);
	for (i = 0; i < CP_NUM_LINKS; i++)
		for (d = UL; d <= DL; d++)
			kfree(cp->ring[i][d].frm);
}

static const struct of_device_id exynos_cp_of_match[] = {
	{ .compatible = "samsung,exynos9820-cp" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_cp_of_match);

static struct platform_driver exynos_cp_driver = {
	.driver = {
		.name = "exynos9820-cp",
		.of_match_table = exynos_cp_of_match,
		.dev_groups = cp_groups,
	},
	.probe = exynos_cp_probe,
	.remove = exynos_cp_remove,
};
module_platform_driver(exynos_cp_driver);

MODULE_DESCRIPTION("Exynos 9820 CP (S5000AP modem) boot and control");
MODULE_LICENSE("GPL");
