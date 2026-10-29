// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom BCM4775 sensor hub (Samsung Galaxy S10e "SSP over BBD").
 *
 * The hub boots from ROM and needs a RAM patch on every power-up. The host
 * talks to it over SPI using Broadcom's "transport layer" (TL): framed,
 * CRC-8 protected packets carrying RPCs. Samsung's SSP sensor protocol rides
 * on RPC 32 (host to hub) and RPC 33 (hub to host). Downstream splits this
 * between the kernel (bbdpl, ssp) and a userspace relay (lhd); here it is one
 * driver. Accelerometer, light and proximity are exposed as IIO devices; each
 * hub sensor is enabled on first read and switched off again when idle. A
 * compass device turns the hub's calibrated magnetometer and the accelerometer
 * into a tilt-compensated heading, buffered, for iio-sensor-proxy.
 *
 * The patch is loaded with request_firmware() in the text format lhd reads
 * (<AsicVersion=..>, <PatchAddress=..>, <Blob=hex>).
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/hex.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#define SHUB_FW_NAME		"brcm/bcm4775-shub.patch"
#define SHUB_FW_VERSION		545126

/* SSI command bytes: full duplex, 2-byte length, flow control */
#define SSI_TX			0x58
#define SSI_RX			0x50
#define SSI_BUF_LEN		4096
#define SSI_MAX_READ		2045

/* TL flag bits; each set bit carries one detail byte, walked from bit 0 up */
#define TL_ACK			BIT(0)
#define TL_RELIABLE		BIT(1)
#define TL_RELIABLE_ACK		BIT(2)
#define TL_GARBAGE		BIT(5)
#define TL_SIZE_EXT		BIT(6)
#define TL_EXTENDED		BIT(7)
#define TL_INTERNAL		BIT(8)
#define TL_IGNORE_SEQID		BIT(9)
#define TL_MAX_PACKET		2048
#define TL_WINDOW		8
#define TL_PATCH_PAYLOAD	921

#define RPC_VERSION_REQ		0
#define RPC_VERSION		3
#define RPC_MEM_WRITE		5
#define RPC_SSP_TX		32
#define RPC_SSP_RX		33
#define RPC_DEBUG		43
#define RPC_JUMP		46

/* SSP */
#define AP2HUB_READ		0
#define AP2HUB_WRITE		1
#define HUB2AP_WRITE		2
#define AP2HUB_RETURN		4
#define SSP_HDR_LEN		9
#define SSP_MAX_FRAME		4096

#define SSP_WHOAMI		0x0f
#define SSP_DEVICE_ID		0x55
#define SSP_SENSOR_ADD		0xa1
#define SSP_SENSOR_REMOVE	0xa2
#define SSP_TIMESTAMP		0xa7
#define SSP_SCANNING		0xf4
#define SSP_FW_REV		0xf0

#define HUB_DEBUG		0x03
#define HUB_INIT_DONE		0x0a
#define HUB_BIGDATA		0x0b
#define HUB_TIME_SYNC		0x06
#define HUB_BYPASS_DATA		0x37
#define HUB_VDIS_DATA		0x38
#define HUB_TS_OFFSET		0xa8

#define SENSOR_ACCEL		0
#define SENSOR_MAG		4
#define SENSOR_PROX		7
#define SENSOR_LIGHT		9
#define SENSOR_TYPES		51

#define SHUB_IDLE_OFF_MS	5000
#define SHUB_FIRST_SAMPLE_MS	500

static unsigned int spi_hz = 1000000;
module_param(spi_hz, uint, 0444);
MODULE_PARM_DESC(spi_hz, "SPI clock (Hz) for hub transfers");

static bool mcu_debug;
module_param(mcu_debug, bool, 0644);
MODULE_PARM_DESC(mcu_debug, "Log the hub firmware's debug text");

static u8 tl_crc_table[256];

/* bytes of data in a BYPASS_DATA item, per sensor type (stock sensor_list.h) */
static const u8 shub_data_len[SENSOR_TYPES] = {
	[0] = 6, [1] = 12, [2] = 13, [3] = 6, [4] = 8, [5] = 6, [7] = 7,
	[9] = 21, [10] = 2, [13] = 1, [14] = 1, [15] = 24, [16] = 17,
	[17] = 17, [18] = 4, [26] = 1, [27] = 1, [29] = 3, [30] = 2,
	[33] = 12, [34] = 3, [35] = 3, [36] = 1, [37] = 1, [38] = 1,
	[39] = 6, [40] = 21, [42] = 1, [48] = 1, [50] = 1,
};

/* initialize_mcu() as stock sends it; values are this phone's DT/EFS data */
struct shub_init_cmd {
	u8 cmd;
	u8 len;
	const u8 *data;		/* NULL: a read of len bytes */
};

static const struct shub_init_cmd shub_init_cmds[] = {
	{ 0xb6, 2, (const u8 []){ 0x00, 0x1a } },		/* AP information */
	{ 0xf1, 3, (const u8 []){ 0x07, 0x07, 0x07 } },		/* sensor formation */
	{ 0xec, 1, (const u8 []){ 0x00 } },			/* glass type */
	{ SSP_SCANNING, 8, NULL },
	{ 0x01, 3, NULL },					/* fuse ROM */
	{ 0xfd, 27, (const u8 []){				/* mag static matrix */
		0x86, 0x54, 0x5b, 0x3f, 0xed, 0xde, 0x00, 0x45, 0xd2,
		0x36, 0x98, 0x7b, 0xf0, 0xa4, 0x00, 0xf9, 0xff, 0x1e,
		0x3e, 0x29, 0xbe, 0xfc, 0x5f, 0xfd, 0x0a, 0xb8, 0xf9 } },
	{ 0x46, 13, (const u8 []){				/* mag cal param */
		0x01, 0x05, 0x26, 0xfe, 0xc0, 0x02, 0x8e, 0x06, 0x22,
		0x01, 0xa1, 0xf8, 0x9e } },
	{ 0xb3, 4, (const u8 []){ 0x1a, 0x04, 0x21, 0x04 } },	/* thermistor */
	{ SSP_FW_REV, 4, NULL },
	{ 0xe1, 1, NULL },					/* accel range */
	{ 0x91, 8, (const u8 []){				/* panel info */
		0x00, 0x00, 0x00, 0x00, 0x51, 0x26, 0x00, 0x00 } },
	{ 0x96, 1, (const u8 []){ 0x00 } },
	{ 0xcd, 12, (const u8 []){				/* gyro cal */
		0x00, 0x96, 0x00, 0x00, 0xe9, 0xe0, 0xff, 0xff,
		0x88, 0xf0, 0xff, 0xff } },
	{ 0xce, 6, (const u8 []){ 0, 0, 0, 0, 0, 0 } },		/* accel cal */
	{ 0xf2, 8, (const u8 []){				/* prox thresholds */
		0x00, 0x00, 0x00, 0x00, 0x3f, 0xf0, 0x00, 0x00 } },
	{ 0x49, 28, (const u8 []){				/* light coefficients */
		0x88, 0xfc, 0xff, 0xff, 0xd9, 0x00, 0x00, 0x00,
		0x44, 0xfe, 0xff, 0xff, 0x5b, 0x04, 0x00, 0x00,
		0x59, 0x0e, 0x00, 0x00, 0x58, 0x04, 0x00, 0x00,
		0x5a, 0x05, 0x00, 0x00 } },
	{ 0xdb, 0, (const u8 []){ 0 } },
};

struct ssp_msg {
	struct list_head list;
	u16 opts;
	u16 len;
	const u8 *wbuf;
	u8 *rbuf;
	struct completion done;
};

struct shub_sensor {
	struct iio_dev *indio;
	struct shub *hub;
	u8 type;
	u16 delay_ms;
	bool enabled;
	bool have_sample;
	unsigned long last_read;	/* jiffies */
	u8 data[24];
	wait_queue_head_t wait;
};

enum { SHUB_ACC, SHUB_LIGHT, SHUB_PROX, SHUB_MAG, SHUB_NSENS };

struct shub {
	struct spi_device *spi;
	struct gpio_desc *pwr_en, *mcu_req, *mcu_resp, *ap_sleep, *host_req;
	struct regulator *vdd;
	int irq;

	/* io_lock: SPI, TL state, both parsers, the pending list */
	struct mutex io_lock;
	u8 *txbuf, *rxbuf;	/* the SPI transfer itself */
	u8 *txframe;		/* escaped TL frame being sent */
	u8 *rpcbuf;		/* RPC being wrapped into a frame */
	u8 *sspbuf;		/* SSP record being wrapped into an RPC */
	u8 *dlbuf;		/* patch packet */
	u8 *rxq, *rxwork;	/* received bytes waiting for the parser */
	unsigned int rxq_len;
	bool delivering;

	/* TL transmit */
	u8 seq, rseq, rseq0;
	u8 chip_seq;
	bool need_ack;
	bool rack_valid;
	u8 rack;
	unsigned int racked;
	unsigned int garbage;

	/* TL receive */
	u8 *fbuf;
	unsigned int flen;
	bool in_frame, esc;
	bool got_sync;
	bool got_version;
	u32 version[2];

	struct mutex sens_lock;	/* enable/disable of hub sensors */

	/* SSP receive */
	int ssp_state;
	unsigned int ssp_need, ssp_have;
	u8 *ssp_buf;
	struct ssp_msg *ssp_cur;
	struct list_head pending;
	unsigned int hub_frames;

	struct task_struct *thread;
	wait_queue_head_t kick;
	atomic_t irq_pending;
	bool ready;
	bool need_reset;
	unsigned int timeouts;
	u64 present;
	u32 fw_rev;

	struct shub_sensor sens[SHUB_NSENS];
	struct iio_trigger *compass_trig;
	bool compass_on;		/* buffer enabled: mag and accel stay on */
	bool compass_want;		/* buffer enabled, maybe before the hub was up */
	bool compass_added;
	struct work_struct compass_work;
	s32 heading;			/* millidegrees from magnetic north */
	struct delayed_work idle_work;
	struct work_struct init_work;
	struct completion booted;
};

enum { SSP_HDR, SSP_READ, SSP_FRAME };

/* ---------------------------------------------------------------- SSI */

static int shub_xfer(struct shub *h, unsigned int len)
{
	struct spi_transfer t = {
		.tx_buf = h->txbuf,
		.rx_buf = h->rxbuf,
		.len = len,
		.speed_hz = spi_hz,
		.bits_per_word = 8,
	};

	return spi_sync_transfer(h->spi, &t, 1);
}

static void shub_rx_bytes(struct shub *h, const u8 *p, unsigned int n);

/*
 * Parsing a packet can send one (an SSP write go-ahead, a timestamp), and
 * sending reuses the SPI buffers, so received bytes are queued and parsed
 * only once the transfer that brought them is finished with its buffers.
 */
static void shub_rx_queue(struct shub *h, const u8 *p, unsigned int n)
{
	if (h->rxq_len + n > SSI_BUF_LEN) {
		dev_warn_ratelimited(&h->spi->dev, "rx backlog full, dropping %u\n", n);
		return;
	}
	memcpy(h->rxq + h->rxq_len, p, n);
	h->rxq_len += n;
}

static void shub_rx_deliver(struct shub *h)
{
	unsigned int n;

	if (h->delivering)
		return;
	h->delivering = true;
	while (h->rxq_len) {
		n = h->rxq_len;
		memcpy(h->rxwork, h->rxq, n);
		h->rxq_len = 0;
		shub_rx_bytes(h, h->rxwork, n);
	}
	h->delivering = false;
}

/* bcm_ssi_tx: write, then resend what flow control says the chip missed */
static int shub_ssi_write(struct shub *h, const u8 *data, unsigned int m)
{
	unsigned int tries, nread, bw;
	int ret;

	if (m + 5 > SSI_BUF_LEN)
		return -EMSGSIZE;
	memmove(h->txbuf + 3, data, m);
	for (tries = 0; tries < 3; tries++) {
		h->txbuf[0] = SSI_TX;
		put_unaligned_le16(m, h->txbuf + 1);
		h->txbuf[3 + m] = 0;
		h->txbuf[4 + m] = 0;
		ret = shub_xfer(h, m + 5);
		if (ret)
			return ret;
		nread = get_unaligned_le16(h->rxbuf + 1);
		if (nread)
			shub_rx_queue(h, h->rxbuf + 3, min(nread, m));
		bw = get_unaligned_le16(h->rxbuf + 3 + m);
		if (bw >= m) {
			shub_rx_deliver(h);
			return 0;
		}
		m -= bw;
		memmove(h->txbuf + 3, h->txbuf + 3 + bw, m);
	}
	shub_rx_deliver(h);
	return -EIO;
}

/* bcm_ssi_rx: returns bytes received, 0 if the chip had nothing */
static int shub_ssi_read(struct shub *h)
{
	unsigned int n;
	int ret;

	memset(h->txbuf, 0, 3);
	h->txbuf[0] = SSI_RX;
	ret = shub_xfer(h, 3);
	if (ret)
		return ret;
	n = min_t(unsigned int, get_unaligned_le16(h->rxbuf + 1), SSI_MAX_READ);
	if (!n)
		return 0;
	memset(h->txbuf, 0, n + 3);
	h->txbuf[0] = SSI_RX;
	ret = shub_xfer(h, n + 3);
	if (ret)
		return ret;
	n = min_t(unsigned int, n, get_unaligned_le16(h->rxbuf + 1));
	shub_rx_queue(h, h->rxbuf + 3, n);
	shub_rx_deliver(h);
	return n;
}

/* ---------------------------------------------------------------- TL */

/* CRC-8, polynomial 0x4d, MSB first, init 0; the result is nibble-swapped */
static void tl_crc_init(void)
{
	unsigned int i, b;
	u8 c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (b = 0; b < 8; b++)
			c = c & 0x80 ? (c << 1) ^ 0x4d : c << 1;
		tl_crc_table[i] = c;
	}
}

static u8 tl_crc_update(u8 c, const u8 *p, unsigned int n)
{
	while (n--)
		c = tl_crc_table[c ^ *p++];
	return c;
}

static u8 tl_crc(const u8 *p, unsigned int n)
{
	u8 c = tl_crc_update(0, p, n);

	return (c << 4) | (c >> 4);
}

static int tl_send(struct shub *h, const u8 *payload, unsigned int n,
		   bool reliable, u16 ext)
{
	u8 hdr[16], *out = h->txframe;
	unsigned int hl = 0, i, o = 0;
	u8 flags = 0, crc;

	if (n > TL_MAX_PACKET)
		return -EMSGSIZE;
	hdr[hl++] = h->seq;
	hdr[hl++] = n & 0xff;
	hdr[hl++] = 0;
	if (h->need_ack && !ext) {
		flags |= TL_ACK;
		hdr[hl++] = h->chip_seq;
		h->need_ack = false;
	}
	if (reliable) {
		flags |= TL_RELIABLE;
		hdr[hl++] = h->rseq++;
	}
	if (h->garbage && !ext) {
		/* how much noise preceded the chip's last reply, as lhd reports */
		flags |= TL_GARBAGE;
		hdr[hl++] = min(h->garbage, 0xffU);
		h->garbage = 0;
	}
	if (n > 0xff) {
		flags |= TL_SIZE_EXT;
		hdr[hl++] = n >> 8;
	}
	if (ext) {
		flags |= TL_EXTENDED;
		hdr[hl++] = ext >> 8;
		for (i = 0; i < hweight16(ext); i++)
			hdr[hl++] = 0;
	}
	hdr[2] = flags;
	h->seq++;

	crc = tl_crc_update(0, hdr + 1, hl - 1);
	crc = tl_crc_update(crc, payload, n);
	crc = (crc << 4) | (crc >> 4);

	out[o++] = 0xb0;
	out[o++] = 0x00;
	for (i = 0; i < hl + n + 1; i++) {
		u8 b = i < hl ? hdr[i] : i < hl + n ? payload[i - hl] : crc;

		if (b == 0xb0 || b == 0x11 || b == 0x13) {
			out[o++] = 0xb0;
			out[o++] = b == 0xb0 ? 0x03 : b == 0x11 ? 0x04 : 0x05;
		} else {
			out[o++] = b;
		}
	}
	out[o++] = 0xb0;
	out[o++] = 0x01;
	return shub_ssi_write(h, out, o);
}

static unsigned int rpc_hdr(u8 *p, u8 id, unsigned int n)
{
	p[0] = id;
	if (n < 0x80) {
		p[1] = n;
		return 2;
	}
	p[1] = 0x80 | (n >> 8);
	p[2] = n & 0xff;
	return 3;
}

static int tl_send_rpc(struct shub *h, u8 id, const u8 *data, unsigned int n,
		       bool reliable)
{
	u8 *p = h->rpcbuf;
	unsigned int hl = rpc_hdr(p, id, n);

	memcpy(p + hl, data, n);
	return tl_send(h, p, hl + n, reliable, 0);
}

static void ssp_feed(struct shub *h, const u8 *p, unsigned int n);

static void tl_rpc(struct shub *h, u8 id, const u8 *d, unsigned int n)
{
	switch (id) {
	case RPC_VERSION:
		if (n >= 8) {
			h->version[0] = get_unaligned_le32(d);
			h->version[1] = get_unaligned_le32(d + 4);
			h->got_version = true;
		}
		break;
	case RPC_SSP_RX:
		if (n >= 2)
			ssp_feed(h, d + 2, min_t(unsigned int, n - 2,
						 get_unaligned_le16(d)));
		break;
	default:
		dev_dbg(&h->spi->dev, "rpc %u (%u bytes)\n", id, n);
	}
}

static void tl_packet(struct shub *h, const u8 *b, unsigned int len)
{
	unsigned int i = 3, n, bit;
	u8 det[16] = { 0 };
	u16 flags;

	if (len < 4)
		return;
	n = b[1];
	flags = b[2];
	for (bit = 0; bit < 8; bit++)
		if (flags & BIT(bit)) {
			if (i >= len)
				return;
			det[bit] = b[i++];
		}
	if (flags & TL_SIZE_EXT)
		n |= det[6] << 8;
	if (flags & TL_EXTENDED) {
		flags |= det[7] << 8;
		for (bit = 8; bit < 16; bit++)
			if (flags & BIT(bit)) {
				if (i >= len)
					return;
				det[bit] = b[i++];
			}
	}
	if (i + n + 1 != len || tl_crc(b + 1, len - 2) != b[len - 1]) {
		dev_dbg(&h->spi->dev, "bad TL packet (len %u)\n", len);
		return;
	}
	if (flags & TL_INTERNAL) {
		h->got_sync = true;
		return;
	}
	if (flags & TL_RELIABLE_ACK) {
		u8 prev = h->rack_valid ? h->rack : h->rseq0 - 1;
		u8 d = det[2] - prev;

		if (d < 0x80) {
			h->racked += d;
			h->rack = det[2];
			h->rack_valid = true;
		}
	}
	if (!n)
		return;
	h->chip_seq = b[0];
	h->need_ack = true;

	b += i;
	while (n >= 2) {
		unsigned int rl = b[1], hl = 2;

		if (rl & 0x80) {
			rl = ((rl & 0x7f) << 8) | b[2];
			hl = 3;
		}
		if (hl + rl > n)
			break;
		tl_rpc(h, b[0], b + hl, rl);
		b += hl + rl;
		n -= hl + rl;
	}
}

static void shub_rx_bytes(struct shub *h, const u8 *p, unsigned int n)
{
	for (; n; n--, p++) {
		u8 b = *p;

		if (h->esc) {
			h->esc = false;
			if (b == 0x00) {
				h->in_frame = true;
				h->flen = 0;
			} else if (b == 0x01) {
				if (h->in_frame)
					tl_packet(h, h->fbuf, h->flen);
				h->in_frame = false;
			} else if (h->in_frame && h->flen < TL_MAX_PACKET) {
				h->fbuf[h->flen++] = b == 3 ? 0xb0 : b == 4 ? 0x11 :
						     b == 5 ? 0x13 : b;
			}
		} else if (b == 0xb0) {
			h->esc = true;
		} else if (h->in_frame) {
			if (h->flen < TL_MAX_PACKET)
				h->fbuf[h->flen++] = b;
		} else {
			h->garbage++;
		}
	}
}

/* poll the chip until cond holds or timeout; caller holds io_lock */
#define shub_wait_locked(h, cond, ms) ({				\
	unsigned long __end = jiffies + msecs_to_jiffies(ms);		\
	int __r;							\
	while (!(cond)) {						\
		__r = shub_ssi_read(h);					\
		if (__r < 0 || time_after(jiffies, __end))		\
			break;						\
		if (!__r)						\
			usleep_range(300, 600);				\
	}								\
	(cond);								\
})

/* ---------------------------------------------------------------- SSP */

static int ssp_record(struct shub *h, const u8 *p, unsigned int n)
{
	u8 *b = h->sspbuf;

	put_unaligned_le16(n, b);
	memcpy(b + 2, p, n);
	return tl_send_rpc(h, RPC_SSP_TX, b, n + 2, false);
}

static void ssp_send_timestamp(struct shub *h)
{
	u8 b[SSP_HDR_LEN + 8] = { SSP_TIMESTAMP, 8, 0, AP2HUB_WRITE, 0 };

	put_unaligned_le64(ktime_get_boottime_ns(), b + SSP_HDR_LEN);
	ssp_record(h, b, sizeof(b));
}

/* atan2 in millidegrees, 0..359999; about 0.25 degree worst case */
static s32 shub_atan2_mdeg(s64 y, s64 x)
{
	u64 ay = abs(y), ax = abs(x), z;
	s32 a;

	if (!ax && !ay)
		return 0;
	while (ax > U32_MAX || ay > U32_MAX) {
		ax >>= 1;
		ay >>= 1;
	}
	/* atan(z) ~ 45 z + 15.64 z (1 - z) degrees for 0 <= z <= 1, z in Q16 */
	z = ax >= ay ? div64_u64(ay << 16, ax) : div64_u64(ax << 16, ay);
	a = (45000 * z + div64_u64(15642 * z * (65536 - z), 65536)) >> 16;
	if (ay > ax)
		a = 90000 - a;
	if (x < 0)
		a = 180000 - a;
	if (y < 0)
		a = 360000 - a;
	return a % 360000;
}

/*
 * Android's getRotationMatrix() and azimuth: H = E x A points east, M = A x H
 * north; the heading of the device's y axis is atan2(Hy / |H|, My / |M|), and
 * |M| = |A| |H| since A and H are perpendicular. Both vectors come from the
 * hub in the same (Android) axes, so no mount matrix enters here.
 */
static void shub_compute_heading(struct shub *h)
{
	const u8 *m = h->sens[SHUB_MAG].data, *g = h->sens[SHUB_ACC].data;
	s64 ex = (s16)get_unaligned_le16(m), ey = (s16)get_unaligned_le16(m + 2);
	s64 ez = (s16)get_unaligned_le16(m + 4);
	s64 ax = (s16)get_unaligned_le16(g), ay = (s16)get_unaligned_le16(g + 2);
	s64 az = (s16)get_unaligned_le16(g + 4);
	s64 hx = ey * az - ez * ay, hy = ez * ax - ex * az, hz = ex * ay - ey * ax;
	s64 my = az * hx - ax * hz;
	u64 na = int_sqrt64(ax * ax + ay * ay + az * az);

	if (!na || (!hx && !hy && !hz))
		return;
	h->heading = shub_atan2_mdeg(hy * (s64)na, my);
}

static void shub_sample(struct shub *h, u8 type, const u8 *d, unsigned int n)
{
	int i;

	for (i = 0; i < SHUB_NSENS; i++) {
		struct shub_sensor *s = &h->sens[i];

		if (s->type != type)
			continue;
		memcpy(s->data, d, min_t(unsigned int, n, sizeof(s->data)));
		s->have_sample = true;
		wake_up(&s->wait);
	}
	if (type == SENSOR_MAG && h->compass_on && h->sens[SHUB_ACC].have_sample) {
		shub_compute_heading(h);
		iio_trigger_poll_nested(h->compass_trig);
	}
}

static void ssp_frame(struct shub *h, const u8 *f, unsigned int len)
{
	unsigned int i = 0, n;
	u8 t;

	while (i < len) {
		switch (f[i++]) {
		case HUB_DEBUG:
			if (i + 2 > len)
				return;
			n = get_unaligned_le16(f + i);
			i += 2;
			if (i + n > len)
				return;
			if (mcu_debug)
				dev_info(&h->spi->dev, "MCU: %.*s\n",
					 (int)strnlen((const char *)f + i, n),
					 (const char *)f + i);
			i += n;
			break;
		case HUB_INIT_DONE:
			dev_dbg(&h->spi->dev, "sensor init done\n");
			break;
		case HUB_TIME_SYNC:
			break;
		case HUB_TS_OFFSET:
			i += 8;
			break;
		case HUB_BYPASS_DATA:
		case HUB_VDIS_DATA:
			if (i >= len)
				return;
			t = f[i++];
			if (t >= SENSOR_TYPES || !shub_data_len[t]) {
				dev_dbg(&h->spi->dev, "data for sensor %u\n", t);
				return;
			}
			n = shub_data_len[t];
			if (i + n + 8 > len)
				return;
			shub_sample(h, t, f + i, n);
			i += n + 8;
			break;
		default:	/* BIGDATA and anything unknown end the frame */
			return;
		}
	}
}

static void ssp_feed(struct shub *h, const u8 *p, unsigned int n)
{
	struct ssp_msg *m;
	u16 opts, len;

	while (n) {
		unsigned int k = min(n, h->ssp_need - h->ssp_have);

		memcpy(h->ssp_buf + h->ssp_have, p, k);
		h->ssp_have += k;
		p += k;
		n -= k;
		if (h->ssp_have < h->ssp_need)
			continue;

		switch (h->ssp_state) {
		case SSP_HDR:
			opts = get_unaligned_le16(h->ssp_buf);
			len = get_unaligned_le16(h->ssp_buf + 2);
			h->ssp_have = 0;
			h->ssp_need = 4;
			if ((opts & 3) == HUB2AP_WRITE) {
				if (!len || len > SSP_MAX_FRAME)
					break;
				h->ssp_state = SSP_FRAME;
				h->ssp_need = len;
				if (++h->hub_frames % 3 == 0)
					ssp_send_timestamp(h);
				break;
			}
			m = NULL;
			list_for_each_entry(m, &h->pending, list)
				if (m->opts == opts)
					break;
			if (list_entry_is_head(m, &h->pending, list)) {
				dev_dbg(&h->spi->dev, "no msg for opts %#x\n", opts);
				break;
			}
			if ((opts & 3) == AP2HUB_READ) {
				h->ssp_cur = m;
				h->ssp_state = SSP_READ;
				h->ssp_need = m->len;
			} else if ((opts & 3) == AP2HUB_WRITE) {
				list_del_init(&m->list);
				ssp_record(h, m->wbuf, m->len);
				if (opts & AP2HUB_RETURN && m->rbuf) {
					m->opts = AP2HUB_READ | AP2HUB_RETURN;
					m->len = 1;
					list_add_tail(&m->list, &h->pending);
				} else {
					complete(&m->done);
				}
			}
			break;
		case SSP_READ:
			m = h->ssp_cur;
			memcpy(m->rbuf, h->ssp_buf, m->len);
			list_del_init(&m->list);
			complete(&m->done);
			h->ssp_cur = NULL;
			h->ssp_state = SSP_HDR;
			h->ssp_have = 0;
			h->ssp_need = 4;
			break;
		case SSP_FRAME:
			ssp_frame(h, h->ssp_buf, h->ssp_need);
			h->ssp_state = SSP_HDR;
			h->ssp_have = 0;
			h->ssp_need = 4;
			break;
		}
	}
}

static void ssp_reset_parser(struct shub *h)
{
	struct ssp_msg *m, *tmp;

	h->ssp_state = SSP_HDR;
	h->ssp_have = 0;
	h->ssp_need = 4;
	h->ssp_cur = NULL;
	list_for_each_entry_safe(m, tmp, &h->pending, list)
		list_del_init(&m->list);
}

/* bbd_do_transfer: header now; data (write) or reply (read) via the parser */
static int ssp_cmd(struct shub *h, u8 cmd, u16 opts, const u8 *wbuf, u8 *rbuf,
		   u16 len)
{
	u8 hdr[SSP_HDR_LEN] = { cmd };
	struct ssp_msg m = { .opts = opts, .len = len, .wbuf = wbuf, .rbuf = rbuf };
	int ret;

	init_completion(&m.done);
	INIT_LIST_HEAD(&m.list);
	put_unaligned_le16(len, hdr + 1);
	put_unaligned_le16(opts, hdr + 3);

	mutex_lock(&h->io_lock);
	if (!h->ready) {
		mutex_unlock(&h->io_lock);
		return -EAGAIN;
	}
	if (len)
		list_add_tail(&m.list, &h->pending);
	ret = ssp_record(h, hdr, sizeof(hdr));
	if (ret || !len) {
		list_del_init(&m.list);
		mutex_unlock(&h->io_lock);
		return ret;
	}
	shub_wait_locked(h, completion_done(&m.done), 1000);
	if (!completion_done(&m.done)) {
		if (h->ssp_cur == &m)
			ssp_reset_parser(h);
		list_del_init(&m.list);
		if (++h->timeouts >= 3) {
			h->need_reset = true;
			wake_up(&h->kick);
		}
		ret = -ETIMEDOUT;
	} else {
		h->timeouts = 0;
	}
	mutex_unlock(&h->io_lock);
	return ret;
}

static int ssp_read(struct shub *h, u8 cmd, u8 *buf, u16 len)
{
	return ssp_cmd(h, cmd, AP2HUB_READ, NULL, buf, len);
}

static int ssp_write(struct shub *h, u8 cmd, const u8 *buf, u16 len)
{
	return ssp_cmd(h, cmd, AP2HUB_WRITE, buf, NULL, len);
}

static int shub_sensor_set(struct shub_sensor *s, bool on)
{
	u8 b[10] = { s->type };
	int ret;

	lockdep_assert_held(&s->hub->sens_lock);
	if (on)
		put_unaligned_le32(s->delay_ms, b + 1);
	ret = ssp_write(s->hub, on ? SSP_SENSOR_ADD : SSP_SENSOR_REMOVE, b,
			sizeof(b));
	if (!ret) {
		s->enabled = on;
		s->have_sample = false;
	}
	return ret;
}

/* ---------------------------------------------------------------- boot */

struct shub_patch {
	u32 asic, entry;
	u8 *blob;
	size_t len;
};

static int shub_hdr_val(const struct firmware *fw, const char *key, u32 *val)
{
	const char *base = (const char *)fw->data;
	const char *p = strnstr(base, key, fw->size);
	char num[16] = { 0 };

	if (!p)
		return -EINVAL;
	p += strlen(key);
	memcpy(num, p, min_t(size_t, sizeof(num) - 1, base + fw->size - p));
	*strchrnul(num, '>') = 0;
	return kstrtou32(num, 0, val);
}

static int shub_parse_patch(struct device *dev, const struct firmware *fw,
			    struct shub_patch *pt)
{
	const char *p, *end = (const char *)fw->data + fw->size;
	u32 blen;
	int hi = -1, v;

	if (shub_hdr_val(fw, "<AsicVersion=", &pt->asic) ||
	    shub_hdr_val(fw, "<PatchAddress=", &pt->entry) ||
	    shub_hdr_val(fw, "<BlobLength=", &blen))
		return -EINVAL;
	p = strnstr((const char *)fw->data, "<Blob=", fw->size);
	if (!p || blen > fw->size / 2)
		return -EINVAL;
	pt->blob = vmalloc(blen);
	if (!pt->blob)
		return -ENOMEM;
	pt->len = 0;
	for (p += 6; p < end && *p != '>' && pt->len < blen; p++) {
		v = hex_to_bin(*p);
		if (v < 0)
			continue;
		if (hi < 0) {
			hi = v;
		} else {
			pt->blob[pt->len++] = hi << 4 | v;
			hi = -1;
		}
	}
	if (pt->len != blen) {
		dev_err(dev, "patch blob is %zu bytes, header says %u\n", pt->len, blen);
		vfree(pt->blob);
		return -EINVAL;
	}
	return 0;
}

static bool shub_hello(struct shub *h)
{
	int i;

	gpiod_set_value(h->mcu_req, 1);
	for (i = 0; i < 100; i++) {
		if (gpiod_get_value(h->mcu_resp))
			return true;
		usleep_range(1000, 1100);
		if (i % 20 == 19) {
			gpiod_set_value(h->mcu_req, 0);
			usleep_range(1000, 1100);
			gpiod_set_value(h->mcu_req, 1);
			usleep_range(1000, 1100);
		}
	}
	return false;
}

static void tl_reset(struct shub *h)
{
	h->rseq = h->rseq0 = 0;
	h->rack_valid = false;
	h->racked = 0;
	h->need_ack = false;
}

static int shub_sync(struct shub *h)
{
	static const u8 autobaud[36] = { [0 ... 35] = 0x80 };
	static const u8 sync[2] = { 0x00, 0x01 };
	int ret;

	h->garbage = 0;
	h->got_sync = false;
	ret = shub_ssi_write(h, autobaud, sizeof(autobaud));
	if (ret)
		return ret;
	msleep(110);
	ret = tl_send(h, sync, sizeof(sync), false, TL_INTERNAL | TL_IGNORE_SEQID);
	if (ret)
		return ret;
	if (!shub_wait_locked(h, h->got_sync, 500))
		return -ETIMEDOUT;
	tl_reset(h);
	return 0;
}

static int shub_get_version(struct shub *h)
{
	static const u8 p[2] = { RPC_VERSION_REQ, 0 };
	int ret;

	h->got_version = false;
	ret = tl_send(h, p, sizeof(p), true, 0);
	if (ret)
		return ret;
	return shub_wait_locked(h, h->got_version, 1000) ? 0 : -ETIMEDOUT;
}

static int shub_download(struct shub *h, const struct shub_patch *pt)
{
	u8 *pkt = h->dlbuf;
	unsigned int i = 0, plen = 0, npkt = 0, base = h->racked;
	int ret;

	while (i < pt->len) {
		unsigned int n, dl, rl;
		u8 *r;

		if (pt->blob[i] != 0xf1 || i + 7 > pt->len)
			return -EINVAL;
		n = get_unaligned_le16(pt->blob + i + 1);
		if (n < 4 || i + 3 + n > pt->len)
			return -EINVAL;
		dl = n - 4;
		rl = 4 + 2 + dl;
		if (plen && plen + 3 + rl > TL_PATCH_PAYLOAD) {
			if (npkt - (h->racked - base) >= TL_WINDOW &&
			    !shub_wait_locked(h, npkt - (h->racked - base) < TL_WINDOW, 1000))
				return -ETIMEDOUT;
			ret = tl_send(h, pkt, plen, true, 0);
			if (ret)
				return ret;
			npkt++;
			plen = 0;
			shub_ssi_read(h);
		}
		r = pkt + plen;
		plen += rpc_hdr(r, RPC_MEM_WRITE, rl);
		memcpy(pkt + plen, pt->blob + i + 3, 4);		/* address */
		put_unaligned_le16(dl, pkt + plen + 4);
		memcpy(pkt + plen + 6, pt->blob + i + 7, dl);
		plen += rl;
		i += 3 + n;
	}
	if (plen) {
		if (npkt - (h->racked - base) >= TL_WINDOW &&
		    !shub_wait_locked(h, npkt - (h->racked - base) < TL_WINDOW, 1000))
			return -ETIMEDOUT;
		ret = tl_send(h, pkt, plen, true, 0);
		if (ret)
			return ret;
		npkt++;
	}
	if (!shub_wait_locked(h, h->racked - base >= npkt, 2000))
		return -ETIMEDOUT;
	return npkt;
}

static int shub_boot(struct shub *h)
{
	static const u8 autobaud16[16] = { [0 ... 15] = 0x80 };
	static const u8 dbg[6] = { 0x01, 0x00, 0x21, 0x00, 0x00, 0x00 };
	struct device *dev = &h->spi->dev;
	const struct firmware *fw;
	struct shub_patch pt;
	u8 entry[4];
	int ret;

	ret = request_firmware(&fw, SHUB_FW_NAME, dev);
	if (ret)
		return ret;
	ret = shub_parse_patch(dev, fw, &pt);
	release_firmware(fw);
	if (ret)
		return ret;

	mutex_lock(&h->io_lock);
	h->ready = false;
	h->in_frame = h->esc = false;
	h->seq = 0;
	tl_reset(h);
	ssp_reset_parser(h);

	gpiod_set_value(h->ap_sleep, 1);
	gpiod_set_value(h->mcu_req, 0);
	gpiod_set_value(h->pwr_en, 0);
	msleep(20);
	gpiod_set_value(h->pwr_en, 1);
	msleep(20);
	if (!shub_hello(h))
		dev_warn(dev, "no MCU_RESP, continuing\n");

	ret = shub_ssi_write(h, autobaud16, sizeof(autobaud16));
	if (!ret)
		ret = shub_sync(h);
	if (!ret)
		ret = shub_get_version(h);
	if (ret) {
		dev_err(dev, "ROM does not answer: %d\n", ret);
		goto out;
	}
	if (h->version[0] != pt.asic) {
		dev_err(dev, "asic %#x, patch is for %#x\n", h->version[0], pt.asic);
		ret = -ENODEV;
		goto out;
	}
	dev_dbg(dev, "ROM %#x, downloading %zu byte patch\n", h->version[1], pt.len);

	ret = shub_download(h, &pt);
	if (ret < 0) {
		dev_err(dev, "patch download failed: %d\n", ret);
		goto out;
	}
	put_unaligned_le32(pt.entry, entry);
	ret = tl_send_rpc(h, RPC_JUMP, entry, sizeof(entry), false);
	if (ret)
		goto out;
	msleep(45);
	ret = shub_sync(h);
	if (!ret)
		ret = shub_get_version(h);
	if (ret) {
		dev_err(dev, "patched firmware does not answer: %d\n", ret);
		goto out;
	}
	if (h->version[1] != SHUB_FW_VERSION)
		dev_warn(dev, "firmware version %u, expected %u\n",
			 h->version[1], SHUB_FW_VERSION);
	ret = tl_send_rpc(h, RPC_DEBUG, dbg, sizeof(dbg), false);
	h->ready = !ret;
out:
	mutex_unlock(&h->io_lock);
	vfree(pt.blob);
	return ret;
}

static int shub_init_mcu(struct shub *h)
{
	struct device *dev = &h->spi->dev;
	u8 buf[8];
	int i, ret;

	msleep(900);		/* the firmware probes its sensors first */
	mutex_lock(&h->io_lock);
	ssp_send_timestamp(h);
	mutex_unlock(&h->io_lock);

	ret = ssp_read(h, SSP_WHOAMI, buf, 1);
	if (ret)
		return ret;
	if (buf[0] != SSP_DEVICE_ID) {
		dev_err(dev, "WHOAMI %#x, expected %#x\n", buf[0], SSP_DEVICE_ID);
		return -ENODEV;
	}
	for (i = 0; i < ARRAY_SIZE(shub_init_cmds); i++) {
		const struct shub_init_cmd *c = &shub_init_cmds[i];

		if (c->data)
			ret = ssp_write(h, c->cmd, c->data, c->len);
		else
			ret = ssp_read(h, c->cmd, buf, c->len);
		if (ret) {
			dev_err(dev, "init cmd %#x: %d\n", c->cmd, ret);
			return ret;
		}
		if (c->cmd == SSP_SCANNING)
			h->present = get_unaligned_le64(buf);
		else if (c->cmd == SSP_FW_REV)
			h->fw_rev = get_unaligned_le32(buf);
	}
	return 0;
}

/* ---------------------------------------------------------------- thread */

static int shub_thread(void *arg)
{
	struct shub *h = arg;
	struct device *dev = &h->spi->dev;
	int ret, i, tries = 0;

reboot:
	ret = shub_boot(h);
	if (!ret)
		ret = shub_init_mcu(h);
	if (ret) {
		mutex_lock(&h->io_lock);
		h->ready = false;
		mutex_unlock(&h->io_lock);
		if (++tries < 3 && !kthread_should_stop())
			goto reboot;
		dev_err(dev, "giving up after %d boot attempts\n", tries);
		complete_all(&h->booted);
		while (!kthread_should_stop())
			schedule_timeout_interruptible(HZ);
		return 0;
	}
	tries = 0;
	dev_info(dev, "hub firmware %u (rev %u) running, sensors %#llx\n",
		 h->version[1], h->fw_rev, h->present);
	complete_all(&h->booted);
	if (!h->compass_added && (h->present & BIT_ULL(SENSOR_MAG))) {
		h->compass_added = true;
		schedule_work(&h->compass_work);
	}

	/* sensors that were in use before a reset come back */
	mutex_lock(&h->sens_lock);
	for (i = 0; i < SHUB_NSENS; i++)
		if (h->sens[i].enabled)
			shub_sensor_set(&h->sens[i], true);
	if (h->compass_want) {
		h->sens[SHUB_ACC].last_read = h->sens[SHUB_MAG].last_read = jiffies;
		ret = h->sens[SHUB_ACC].enabled ? 0 :
		      shub_sensor_set(&h->sens[SHUB_ACC], true);
		if (!ret && !h->sens[SHUB_MAG].enabled)
			ret = shub_sensor_set(&h->sens[SHUB_MAG], true);
		h->compass_on = !ret;
		if (ret)
			dev_warn(dev, "compass did not start: %d\n", ret);
		else
			schedule_delayed_work(&h->idle_work, HZ);
	}
	mutex_unlock(&h->sens_lock);

	while (!kthread_should_stop()) {
		bool active = false;
		long to;

		for (i = 0; i < SHUB_NSENS; i++)
			active |= h->sens[i].enabled;
		/* HOST_REQ says when the hub has data; polling only backs it up */
		if (h->irq > 0)
			to = msecs_to_jiffies(active ? 100 : 1000);
		else
			to = msecs_to_jiffies(active ? 10 : 200);
		wait_event_interruptible_timeout(h->kick,
			atomic_read(&h->irq_pending) || h->need_reset ||
			kthread_should_stop(), to);
		atomic_set(&h->irq_pending, 0);
		if (h->need_reset) {
			dev_warn(dev, "hub stopped answering, rebooting it\n");
			h->need_reset = false;
			h->timeouts = 0;
			goto reboot;
		}
		mutex_lock(&h->io_lock);
		/* an edge IRQ: keep reading while the line stays up */
		for (i = 0; i < 32; i++)
			if (shub_ssi_read(h) <= 0 && !gpiod_get_value(h->host_req))
				break;
		mutex_unlock(&h->io_lock);
	}
	return 0;
}

static irqreturn_t shub_irq(int irq, void *arg)
{
	struct shub *h = arg;

	atomic_set(&h->irq_pending, 1);
	wake_up(&h->kick);
	return IRQ_HANDLED;
}

static void shub_idle_work(struct work_struct *w)
{
	struct shub *h = container_of(to_delayed_work(w), struct shub, idle_work);
	bool any = false;
	int i;

	mutex_lock(&h->sens_lock);
	for (i = 0; i < SHUB_NSENS; i++) {
		struct shub_sensor *s = &h->sens[i];

		if (!s->enabled)
			continue;
		if (h->compass_on && (i == SHUB_MAG || i == SHUB_ACC))
			s->last_read = jiffies;
		if (time_after(jiffies, s->last_read +
			       msecs_to_jiffies(SHUB_IDLE_OFF_MS)))
			shub_sensor_set(s, false);
		else
			any = true;
	}
	mutex_unlock(&h->sens_lock);
	if (any)
		schedule_delayed_work(&h->idle_work, HZ);
}

/* ---------------------------------------------------------------- IIO */

static int shub_get_sample(struct shub_sensor *s, u8 *out, unsigned int n)
{
	struct shub *h = s->hub;
	int ret;

	if (!completion_done(&h->booted) || !h->ready)
		return -EAGAIN;
	if (!(h->present & BIT_ULL(s->type)))
		return -ENODEV;
	mutex_lock(&h->sens_lock);
	s->last_read = jiffies;
	ret = s->enabled ? 0 : shub_sensor_set(s, true);
	mutex_unlock(&h->sens_lock);
	if (ret)
		return ret;
	schedule_delayed_work(&h->idle_work, HZ);
	wake_up(&h->kick);
	if (!wait_event_timeout(s->wait, s->have_sample,
				msecs_to_jiffies(SHUB_FIRST_SAMPLE_MS)))
		return -EAGAIN;
	mutex_lock(&h->io_lock);
	memcpy(out, s->data, n);
	mutex_unlock(&h->io_lock);
	return 0;
}

#define SHUB_ACCEL_CHAN(axis, idx) {					\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##axis,					\
	.address = idx,							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
}

static const struct iio_chan_spec shub_accel_channels[] = {
	SHUB_ACCEL_CHAN(X, 0),
	SHUB_ACCEL_CHAN(Y, 1),
	SHUB_ACCEL_CHAN(Z, 2),
};

#define SHUB_INTENSITY_CHAN(mod, off) {					\
	.type = IIO_INTENSITY,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_LIGHT_##mod,				\
	.address = off,							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
}

/* light data: u32 lux, s32 cct, u16 r, g, b, w, a_gain, a_time, u8 brightness */
static const struct iio_chan_spec shub_light_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
	{
		.type = IIO_COLORTEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
	SHUB_INTENSITY_CHAN(RED, 8),
	SHUB_INTENSITY_CHAN(GREEN, 10),
	SHUB_INTENSITY_CHAN(BLUE, 12),
	SHUB_INTENSITY_CHAN(CLEAR, 14),
};

/*
 * proximity data: u8 detect, u16 adc, u32 light. The hub calibrates its own
 * threshold, so the main channel is its near (1) / far (0) decision; the raw
 * count changes by only ~130 when covered and is a second channel.
 */
static const struct iio_chan_spec shub_prox_channels[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_PROXIMITY,
		.indexed = 1,
		.channel = 1,
		.extend_name = "adc",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
};

static int shub_read_raw(struct iio_dev *indio, struct iio_chan_spec const *chan,
			 int *val, int *val2, long mask)
{
	struct shub_sensor *s = *(struct shub_sensor **)iio_priv(indio);
	u8 d[24];
	int ret;

	if (chan->type == IIO_ROT) {
		struct shub *h = s->hub;

		if (mask == IIO_CHAN_INFO_SCALE) {
			*val = 0;
			*val2 = 1000;		/* millidegrees */
			return IIO_VAL_INT_PLUS_MICRO;
		}
		ret = shub_get_sample(&h->sens[SHUB_ACC], d, sizeof(d));
		if (!ret)
			ret = shub_get_sample(s, d, sizeof(d));
		if (ret)
			return ret;
		mutex_lock(&h->io_lock);
		shub_compute_heading(h);
		*val = h->heading;
		mutex_unlock(&h->io_lock);
		return IIO_VAL_INT;
	}
	if (mask == IIO_CHAN_INFO_SCALE && chan->type == IIO_ACCEL) {
		/* +-8 g at 4096 LSB/g */
		*val = 0;
		*val2 = 2394202;
		return IIO_VAL_INT_PLUS_NANO;
	}
	ret = shub_get_sample(s, d, sizeof(d));
	if (ret)
		return ret;

	switch (chan->type) {
	case IIO_ACCEL:
		*val = (s16)get_unaligned_le16(d + 2 * chan->address);
		return IIO_VAL_INT;
	case IIO_LIGHT:
		*val = get_unaligned_le32(d);
		return IIO_VAL_INT;
	case IIO_COLORTEMP:
		*val = (s32)get_unaligned_le32(d + 4);
		return IIO_VAL_INT;
	case IIO_INTENSITY:
		*val = get_unaligned_le16(d + chan->address);
		return IIO_VAL_INT;
	case IIO_PROXIMITY:
		if (chan->channel)
			*val = get_unaligned_le16(d + 1);
		else
			*val = d[0] == 1;	/* 0xff until the first event */
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info shub_iio_info = {
	.read_raw = shub_read_raw,
};

static const struct iio_chan_spec shub_compass_channels[] = {
	{
		.type = IIO_ROT,
		.modified = 1,
		.channel2 = IIO_MOD_NORTH_MAGN_TILT_COMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = 0,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(1),
};

static irqreturn_t shub_compass_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio = pf->indio_dev;
	struct shub *h = (*(struct shub_sensor **)iio_priv(indio))->hub;
	struct {
		s32 heading;
		aligned_s64 ts;
	} scan = { .heading = h->heading };

	iio_push_to_buffers_with_ts(indio, &scan, sizeof(scan), iio_get_time_ns(indio));
	iio_trigger_notify_done(indio->trig);
	return IRQ_HANDLED;
}

static int shub_compass_set(struct iio_dev *indio, bool on)
{
	struct shub *h = (*(struct shub_sensor **)iio_priv(indio))->hub;
	int ret = 0;

	mutex_lock(&h->sens_lock);
	h->compass_want = on;
	h->compass_on = false;
	/* iio-sensor-proxy starts before the hub boots: the thread picks it up */
	if (!completion_done(&h->booted) || !h->ready) {
		mutex_unlock(&h->sens_lock);
		return 0;
	}
	if (on) {
		h->sens[SHUB_ACC].last_read = h->sens[SHUB_MAG].last_read = jiffies;
		if (!h->sens[SHUB_ACC].enabled)
			ret = shub_sensor_set(&h->sens[SHUB_ACC], true);
		if (!ret && !h->sens[SHUB_MAG].enabled)
			ret = shub_sensor_set(&h->sens[SHUB_MAG], true);
		h->compass_on = !ret;
	}
	mutex_unlock(&h->sens_lock);
	if (on && !ret) {
		schedule_delayed_work(&h->idle_work, HZ);
		wake_up(&h->kick);
	}
	return ret;
}

static int shub_compass_postenable(struct iio_dev *indio)
{
	return shub_compass_set(indio, true);
}

static int shub_compass_predisable(struct iio_dev *indio)
{
	return shub_compass_set(indio, false);
}

static const struct iio_buffer_setup_ops shub_compass_buffer_ops = {
	.postenable = shub_compass_postenable,
	.predisable = shub_compass_predisable,
};

/* iio-sensor-proxy looks for a trigger named "<device>-dev<n>" */
static int shub_compass_setup(struct shub *h, struct iio_dev *indio)
{
	struct device *dev = &h->spi->dev;
	int ret;

	h->compass_trig = devm_iio_trigger_alloc(dev, "%s-dev%d", indio->name,
						 iio_device_id(indio));
	if (!h->compass_trig)
		return -ENOMEM;
	ret = devm_iio_trigger_register(dev, h->compass_trig);
	if (ret)
		return ret;
	indio->trig = iio_trigger_get(h->compass_trig);
	return devm_iio_triggered_buffer_setup(dev, indio, NULL, shub_compass_handler,
					       &shub_compass_buffer_ops);
}

static int shub_add_sensor(struct shub *h, int idx, const char *name, u8 type,
			   u16 delay_ms, const struct iio_chan_spec *ch, int nch)
{
	struct iio_dev *indio;
	struct shub_sensor *s = &h->sens[idx];

	indio = devm_iio_device_alloc(&h->spi->dev, sizeof(s));
	if (!indio)
		return -ENOMEM;
	*(struct shub_sensor **)iio_priv(indio) = s;
	s->hub = h;
	s->type = type;
	s->delay_ms = delay_ms;
	init_waitqueue_head(&s->wait);
	indio->name = name;
	indio->info = &shub_iio_info;
	indio->modes = INDIO_DIRECT_MODE;
	indio->channels = ch;
	indio->num_channels = nch;
	s->indio = indio;
	if (ch == shub_compass_channels) {
		int ret = shub_compass_setup(h, indio);

		if (ret)
			return ret;
	}
	return devm_iio_device_register(&h->spi->dev, indio);
}

/*
 * The compass appears only once the hub runs: iio-sensor-proxy gives up on a
 * buffer that has no data within 0.5 s, and the hub takes ~2 s to boot.
 */
static void shub_compass_work(struct work_struct *w)
{
	struct shub *h = container_of(w, struct shub, compass_work);
	int ret;

	ret = shub_add_sensor(h, SHUB_MAG, "bcm4775-compass", SENSOR_MAG, 100,
			      shub_compass_channels, ARRAY_SIZE(shub_compass_channels));
	if (ret)
		dev_warn(&h->spi->dev, "no compass: %d\n", ret);
}

/* ---------------------------------------------------------------- probe */

static int shub_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct shub *h;
	int ret;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->spi = spi;
	spi_set_drvdata(spi, h);
	mutex_init(&h->io_lock);
	mutex_init(&h->sens_lock);
	INIT_LIST_HEAD(&h->pending);
	init_waitqueue_head(&h->kick);
	init_completion(&h->booted);
	INIT_DELAYED_WORK(&h->idle_work, shub_idle_work);
	INIT_WORK(&h->compass_work, shub_compass_work);
	tl_crc_init();

	h->txbuf = devm_kzalloc(dev, SSI_BUF_LEN, GFP_KERNEL | GFP_DMA);
	h->rxbuf = devm_kzalloc(dev, SSI_BUF_LEN, GFP_KERNEL | GFP_DMA);
	h->txframe = devm_kzalloc(dev, 2 * TL_MAX_PACKET + 32, GFP_KERNEL);
	h->rpcbuf = devm_kzalloc(dev, TL_MAX_PACKET, GFP_KERNEL);
	h->sspbuf = devm_kzalloc(dev, TL_MAX_PACKET, GFP_KERNEL);
	h->dlbuf = devm_kzalloc(dev, TL_MAX_PACKET, GFP_KERNEL);
	h->rxq = devm_kzalloc(dev, SSI_BUF_LEN, GFP_KERNEL);
	h->rxwork = devm_kzalloc(dev, SSI_BUF_LEN, GFP_KERNEL);
	h->fbuf = devm_kzalloc(dev, TL_MAX_PACKET, GFP_KERNEL);
	h->ssp_buf = devm_kzalloc(dev, SSP_MAX_FRAME, GFP_KERNEL);
	if (!h->txbuf || !h->rxbuf || !h->txframe || !h->rpcbuf || !h->sspbuf ||
	    !h->dlbuf || !h->rxq || !h->rxwork || !h->fbuf || !h->ssp_buf)
		return -ENOMEM;

	h->pwr_en = devm_gpiod_get(dev, "pwr-en", GPIOD_OUT_LOW);
	h->mcu_req = devm_gpiod_get(dev, "mcu-req", GPIOD_OUT_LOW);
	h->mcu_resp = devm_gpiod_get(dev, "mcu-resp", GPIOD_IN);
	h->ap_sleep = devm_gpiod_get(dev, "ap-sleep", GPIOD_OUT_HIGH);
	h->host_req = devm_gpiod_get(dev, "host-req", GPIOD_IN);
	if (IS_ERR(h->pwr_en) || IS_ERR(h->mcu_req) || IS_ERR(h->mcu_resp) ||
	    IS_ERR(h->ap_sleep) || IS_ERR(h->host_req))
		return dev_err_probe(dev, -EINVAL, "missing GPIOs\n");

	h->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(h->vdd))
		return PTR_ERR(h->vdd);
	ret = regulator_enable(h->vdd);
	if (ret)
		return ret;

	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		goto err_reg;

	/* the sync frame stock sends, CRC b1 */
	if (tl_crc((const u8 []){ 0x02, 0x80, 0x03, 0x00, 0x00, 0x00, 0x01 }, 7) != 0xb1)
		dev_warn(dev, "TL CRC self-test failed\n");

	ret = shub_add_sensor(h, SHUB_ACC, "bcm4775-accel", SENSOR_ACCEL, 100,
			      shub_accel_channels, ARRAY_SIZE(shub_accel_channels));
	if (!ret)
		ret = shub_add_sensor(h, SHUB_LIGHT, "bcm4775-light", SENSOR_LIGHT, 200,
				      shub_light_channels, ARRAY_SIZE(shub_light_channels));
	if (!ret)
		ret = shub_add_sensor(h, SHUB_PROX, "bcm4775-prox", SENSOR_PROX, 200,
				      shub_prox_channels, ARRAY_SIZE(shub_prox_channels));
	if (ret)
		goto err_reg;

	h->irq = gpiod_to_irq(h->host_req);
	if (h->irq > 0) {
		ret = devm_request_irq(dev, h->irq, shub_irq, IRQF_TRIGGER_RISING,
				       "bcm4775-shub", h);
		if (ret) {
			dev_warn(dev, "no HOST_REQ irq (%d), polling\n", ret);
			h->irq = 0;
		}
	}

	h->thread = kthread_run(shub_thread, h, "bcm4775-shub");
	if (IS_ERR(h->thread)) {
		ret = PTR_ERR(h->thread);
		goto err_reg;
	}
	return 0;

err_reg:
	regulator_disable(h->vdd);
	return ret;
}

static void shub_remove(struct spi_device *spi)
{
	struct shub *h = spi_get_drvdata(spi);

	kthread_stop(h->thread);
	cancel_work_sync(&h->compass_work);
	cancel_delayed_work_sync(&h->idle_work);
	gpiod_set_value(h->mcu_req, 0);
	gpiod_set_value(h->pwr_en, 0);
	regulator_disable(h->vdd);
}

static const struct of_device_id shub_of_match[] = {
	{ .compatible = "brcm,bcm4775-shub" },
	{ }
};
MODULE_DEVICE_TABLE(of, shub_of_match);

static const struct spi_device_id shub_spi_id[] = {
	{ "bcm4775-shub" },
	{ }
};
MODULE_DEVICE_TABLE(spi, shub_spi_id);

static struct spi_driver shub_driver = {
	.driver = {
		.name = "bcm4775-shub",
		.of_match_table = shub_of_match,
	},
	.probe = shub_probe,
	.remove = shub_remove,
	.id_table = shub_spi_id,
};
module_spi_driver(shub_driver);

MODULE_DESCRIPTION("Broadcom BCM4775 sensor hub (Samsung SSP)");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(SHUB_FW_NAME);
