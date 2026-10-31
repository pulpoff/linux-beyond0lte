// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos 9820 DisplayPort link controller (USB-C DP alt mode, Galaxy S10e)
 *
 * Bring-up stage: the link without a display controller. The PHY is the DP
 * instance of the USB/DP combo PHY (phy-exynos5-usbdrd), which owns the lane
 * mux. Everything is driven from debugfs (exynos9820-dp/):
 *
 *   power  write 1/0: PHY + controller on (AUX usable) / off
 *   dpcd   read/write at file offset = DPCD address
 *   edid   read: the sink's EDID over I2C-over-AUX
 *   train  write: full link training on 2 or 4 lanes, HBR2 down to HBR
 *   bist   write 1/0: 1920x1080p60 colour bars from the controller itself
 *   video  write 1/0: 1920x1080p60 stream from DECON2 (no BIST)
 *   lh     write 1/0: the LH P-channel from DECON2, before DECON2 starts
 *   regs   read: controller state
 *
 * Register sequence from the downstream CAL (dpu20/cal_9820/displayport_reg.c).
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/usb/max77705-dp.h>
#include <linux/workqueue.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>

#include "exynos9820-dp.h"

#define DP_SW_RESET			0x0004
#define  DP_SW_RESET_EN			BIT(0)
#define DP_LANE_COUNT			0x0010
#define DP_SW_FUNCTION_EN		0x0014
#define  DP_SW_FUNC_EN			BIT(0)
#define DP_COMMON_FUNCTION_EN		0x0018
#define  DP_PCS_FUNC_EN			BIT(1)
#define  DP_AUX_FUNC_EN			BIT(0)
#define DP_SST1_FUNCTION_EN		0x001c
#define  DP_SST1_LH_PWR_ON_STATUS	BIT(5)
#define  DP_SST1_LH_PWR_ON		BIT(4)
#define  DP_SST1_VIDEO_FUNC_EN		BIT(0)
#define DP_PLL_LOCK_CONTROL		0x002c
#define  DP_PLL_LOCK_STATUS		BIT(4)
#define DP_IRQ_COMMON_STATUS		0x0108
#define DP_IRQ_COMMON_MASK		0x010c

#define DP_AUX_CONTROL			0x1000
#define  DP_AUX_REPLY_TIMER_MODE	GENMASK(13, 12)
#define DP_AUX_TRANSACTION_START	0x1004
#define  DP_AUX_TRAN_START		BIT(0)
#define DP_AUX_BUFFER_CLEAR		0x1008
#define DP_AUX_ADDR_ONLY		0x100c
#define DP_AUX_REQUEST_CONTROL		0x1010
#define  DP_AUX_REQ_COMM		GENMASK(31, 28)
#define  DP_AUX_REQ_ADDR		GENMASK(27, 8)
#define  DP_AUX_REQ_LENGTH		GENMASK(5, 0)
#define DP_AUX_COMMAND_CONTROL		0x1014
#define  DP_AUX_DEFER_CTRL_EN		BIT(8)
#define DP_AUX_MONITOR_1		0x1018
#define  DP_AUX_CMD_STATUS		GENMASK(11, 8)
#define DP_AUX_MONITOR_2		0x101c
#define DP_AUX_TX_DATA(n)		(0x1030 + 4 * (n))
#define DP_AUX_RX_DATA(n)		(0x1040 + 4 * (n))

#define DP_PCS_CONTROL			0x3000
#define  DP_PCS_TRAINING_PATTERN	GENMASK(5, 4)
#define  DP_PCS_SCRAMBLE_BYPASS		BIT(0)
#define DP_PCS_LANE_CONTROL		0x3004
#define DP_PCS_TEST_PATTERN_CONTROL	0x3008
#define  DP_PCS_QUALITY_PATTERN		GENMASK(2, 0)

#define DP_SST1_MAIN_CONTROL		0x5000
#define  DP_SST1_MVID_MODE		BIT(11)
#define  DP_SST1_VIDEO_MODE		BIT(6)
#define  DP_SST1_ENHANCED_MODE		BIT(5)
#define DP_SST1_INT_STATUS0		0x5024
#define DP_SST1_INT_STATUS1		0x5028
#define DP_SST1_INT_MASK0		0x502c
#define DP_SST1_MVID_MASTER		0x5044
#define DP_SST1_NVID_MASTER		0x5048
#define DP_SST1_MVID_SFR		0x504c
#define DP_SST1_NVID_SFR		0x5050
#define DP_SST1_AS_INT_FEC_OFF		0x5080
#define DP_SST1_AS_FRAC_FEC_OFF		0x5084
#define DP_SST1_AS_THR_FEC_OFF		0x5088
#define DP_SST1_AS_THR_SEL_FEC_OFF	0x508c
#define DP_SST1_AS_INT_FEC_ON		0x5090
#define DP_SST1_AS_FRAC_FEC_ON		0x5094
#define DP_SST1_AS_THR_FEC_ON		0x5098
#define DP_SST1_AS_THR_SEL_FEC_ON	0x509c
#define DP_SST1_AS_MODE_CONTROL		0x5104
#define DP_SST1_VIDEO_CONTROL		0x5400
#define  DP_VIDEO_STRM_VALID		GENMASK(9, 8)
#define  DP_VIDEO_CEA_RANGE		BIT(7)
#define  DP_VIDEO_BPC			GENMASK(6, 4)
#define  DP_VIDEO_COLOR_FORMAT		GENMASK(3, 2)
#define  DP_VIDEO_VSYNC_POS		BIT(1)
#define  DP_VIDEO_HSYNC_POS		BIT(0)
#define DP_SST1_VIDEO_ENABLE		0x5404
#define DP_SST1_VIDEO_TIMING_GEN	0x5408
#define DP_SST1_HTOTAL			0x5414
#define DP_SST1_VTOTAL			0x5418
#define DP_SST1_HFP			0x541c
#define DP_SST1_HBP			0x5420
#define DP_SST1_HACTIVE			0x5424
#define DP_SST1_VFP			0x5428
#define DP_SST1_VBP			0x542c
#define DP_SST1_VACTIVE			0x5430
#define DP_SST1_BIST_CONTROL		0x5450
#define  DP_BIST_CTS_EN			BIT(17)
#define  DP_BIST_EN			BIT(3)
#define  DP_BIST_TYPE			GENMASK(1, 0)
#define DP_SST1_VIDEO_FSM		0x5460
#define DP_SST2_INT_STATUS0		0x6024
#define DP_SST2_INT_STATUS1		0x6028

#define DP_MAX_LANES			4

struct e9820_dp {
	struct drm_bridge bridge;
	struct device *dev;
	void __iomem *regs;
	struct clk *gtc;
	struct phy *phy;
	struct gpio_desc *aux_en;
	struct gpio_desc *sbu_sel;
	struct gpio_desc *port;
	struct drm_dp_aux aux;
	struct mutex lock;	/* power, training and video state */
	struct dentry *debugfs;
	bool powered;
	bool trained;
	unsigned int lanes;		/* of the trained link: 2 (pins D/F) or 4 (C/E) */
	bool bist;
	bool video;
	bool lh;
	int port_val;
	int link_rate;		/* symbol clock, kHz (drm_dp_helper units) */
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
	bool dpcd_valid;

	struct notifier_block nb;
	struct work_struct hpd_work;
	struct delayed_work cec_work;	/* CEC-Tunneling-over-AUX IRQs */
	bool cec_adapter;	/* drm_dp_cec_register_connector() done */
	bool cec_registered;	/* ... and not detached: cec_work may run */
	unsigned long port_state;	/* MAX77705_DP_* */
	bool hpd_enabled;
};

static inline struct e9820_dp *bridge_to_dp(struct drm_bridge *b)
{
	return container_of(b, struct e9820_dp, bridge);
}

/* CEA VIC 16, for the colour bars */
static const struct drm_display_mode e9820_dp_bist_mode = {
	DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500,
		 1920, 2008, 2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC)
};

static inline u32 dp_read(struct e9820_dp *dp, u32 reg)
{
	return readl(dp->regs + reg);
}

static inline void dp_write(struct e9820_dp *dp, u32 reg, u32 val)
{
	writel(val, dp->regs + reg);
}

static void dp_update(struct e9820_dp *dp, u32 reg, u32 mask, u32 val)
{
	u32 v = dp_read(dp, reg);

	dp_write(dp, reg, (v & ~mask) | (val & mask));
}

static ssize_t e9820_dp_aux_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	struct e9820_dp *dp = container_of(aux, struct e9820_dp, aux);
	bool read = msg->request & DP_AUX_I2C_READ;
	u8 *buf = msg->buffer;
	u32 req, m1, m2, w;
	size_t i;
	int ret;

	if (msg->size > DP_AUX_MAX_PAYLOAD_BYTES)
		return -E2BIG;
	if (!READ_ONCE(dp->powered))
		return -ENODEV;

	dp_write(dp, DP_AUX_BUFFER_CLEAR, 1);
	dp_update(dp, DP_AUX_COMMAND_CONTROL, DP_AUX_DEFER_CTRL_EN, DP_AUX_DEFER_CTRL_EN);
	dp_update(dp, DP_AUX_CONTROL, DP_AUX_REPLY_TIMER_MODE,
		  FIELD_PREP(DP_AUX_REPLY_TIMER_MODE, 3));
	req = FIELD_PREP(DP_AUX_REQ_COMM, msg->request & 0xf) |
	      FIELD_PREP(DP_AUX_REQ_ADDR, msg->address) |
	      FIELD_PREP(DP_AUX_REQ_LENGTH, msg->size ? msg->size - 1 : 0);
	dp_write(dp, DP_AUX_REQUEST_CONTROL, req);
	dp_write(dp, DP_AUX_ADDR_ONLY, msg->size ? 0 : 1);

	if (!read) {
		for (i = 0, w = 0; i < msg->size; i++) {
			w |= buf[i] << (8 * (i % 4));
			if (i % 4 == 3 || i == msg->size - 1) {
				dp_write(dp, DP_AUX_TX_DATA(i / 4), w);
				w = 0;
			}
		}
	}

	dp_write(dp, DP_AUX_TRANSACTION_START, DP_AUX_TRAN_START);
	ret = readl_poll_timeout(dp->regs + DP_AUX_TRANSACTION_START, w,
				 !(w & DP_AUX_TRAN_START), 10, 50000);
	m1 = dp_read(dp, DP_AUX_MONITOR_1);
	m2 = dp_read(dp, DP_AUX_MONITOR_2);
	dp_write(dp, DP_AUX_ADDR_ONLY, 0);
	if (ret) {
		dev_dbg(dp->dev, "AUX %x@%05x timed out\n", msg->request, msg->address);
		return -ETIMEDOUT;
	}
	if (FIELD_GET(DP_AUX_CMD_STATUS, m1) || m2) {
		dev_dbg(dp->dev, "AUX %x@%05x len %zu failed: mon1 %08x mon2 %08x\n",
			msg->request, msg->address, msg->size, m1, m2);
		udelay(400);
		return -EIO;
	}

	if (read) {
		for (i = 0; i < msg->size; i++) {
			if (i % 4 == 0)
				w = dp_read(dp, DP_AUX_RX_DATA(i / 4));
			buf[i] = w >> (8 * (i % 4));
		}
	}
	msg->reply = DP_AUX_NATIVE_REPLY_ACK | DP_AUX_I2C_REPLY_ACK;
	return msg->size;
}

static int e9820_dp_wait_pll(struct e9820_dp *dp)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(dp->regs + DP_PLL_LOCK_CONTROL, v,
				 v & DP_PLL_LOCK_STATUS, 1, 3000);
	if (ret)
		dev_err(dp->dev, "PHY PLL did not lock (%08x)\n", v);
	return ret;
}

static int e9820_dp_lh(struct e9820_dp *dp, bool on);

static void e9820_dp_power_off(struct e9820_dp *dp)
{
	if (!dp->powered)
		return;

	dp_write(dp, DP_SST1_VIDEO_ENABLE, 0);
	dp_write(dp, DP_SST1_BIST_CONTROL, 0);
	if (dp->lh)
		e9820_dp_lh(dp, false);
	dp_update(dp, DP_SW_FUNCTION_EN, DP_SW_FUNC_EN, 0);
	/* CEC transmits run outside dp->lock; let one in flight finish */
	mutex_lock(&dp->aux.hw_mutex);
	WRITE_ONCE(dp->powered, false);
	mutex_unlock(&dp->aux.hw_mutex);
	dp->trained = false;
	dp->bist = false;
	dp->video = false;
	phy_power_off(dp->phy);
	phy_exit(dp->phy);
	gpiod_set_value_cansleep(dp->aux_en, 0);
	clk_disable_unprepare(dp->gtc);
	dev_info(dp->dev, "off\n");
}

/* displayport_hpd_changed(1) up to the first AUX access */
static int e9820_dp_power_on(struct e9820_dp *dp)
{
	u32 v;
	int ret;

	if (dp->powered)
		return 0;

	ret = clk_prepare_enable(dp->gtc);
	if (ret)
		return ret;

	/* the SBU switch: polarity from the plug orientation, then on */
	dp->port_val = gpiod_get_value_cansleep(dp->port);
	if (dp->port_val < 0)
		dp->port_val = 1;
	gpiod_direction_output(dp->sbu_sel, dp->port_val);
	gpiod_set_value_cansleep(dp->aux_en, 1);
	msleep(100);

	dp_write(dp, DP_SW_RESET, DP_SW_RESET_EN);
	ret = readl_poll_timeout(dp->regs + DP_SW_RESET, v, !(v & DP_SW_RESET_EN), 1, 100);
	if (ret)
		dev_warn(dp->dev, "sw reset did not self-clear\n");

	ret = phy_init(dp->phy);
	if (ret)
		goto err_gpio;
	ret = phy_power_on(dp->phy);
	if (ret)
		goto err_phy;
	ret = e9820_dp_wait_pll(dp);
	if (ret)
		goto err_power;

	dp_update(dp, DP_COMMON_FUNCTION_EN, DP_PCS_FUNC_EN | DP_AUX_FUNC_EN,
		  DP_PCS_FUNC_EN | DP_AUX_FUNC_EN);
	dp_update(dp, DP_SST1_FUNCTION_EN, DP_SST1_VIDEO_FUNC_EN, DP_SST1_VIDEO_FUNC_EN);
	dp_update(dp, DP_SW_FUNCTION_EN, DP_SW_FUNC_EN, DP_SW_FUNC_EN);

	/* no interrupt handler yet: clear everything, unmask nothing */
	dp_write(dp, DP_IRQ_COMMON_MASK, 0);
	dp_write(dp, DP_SST1_INT_MASK0, 0);
	dp_write(dp, DP_IRQ_COMMON_STATUS, ~0);
	dp_write(dp, DP_SST1_INT_STATUS0, ~0);
	dp_write(dp, DP_SST1_INT_STATUS1, ~0);
	dp_write(dp, DP_SST2_INT_STATUS0, ~0);
	dp_write(dp, DP_SST2_INT_STATUS1, ~0);

	/* pins C/D/E/F: DP lanes 0/1 on the pair USB is not using */
	dp_write(dp, DP_PCS_LANE_CONTROL, dp->port_val ?
		 (0 << 0) | (1 << 4) | (3 << 8) | (2 << 12) :
		 (3 << 0) | (2 << 4) | (0 << 8) | (1 << 12));

	WRITE_ONCE(dp->powered, true);
	usleep_range(10000, 11000);
	dev_info(dp->dev, "on, port %d\n", dp->port_val);
	return 0;

err_power:
	phy_power_off(dp->phy);
err_phy:
	phy_exit(dp->phy);
err_gpio:
	gpiod_set_value_cansleep(dp->aux_en, 0);
	clk_disable_unprepare(dp->gtc);
	return ret;
}

static void e9820_dp_set_pattern(struct e9820_dp *dp, int pattern)
{
	dp_update(dp, DP_PCS_TEST_PATTERN_CONTROL, DP_PCS_QUALITY_PATTERN, 0);
	dp_update(dp, DP_PCS_CONTROL, DP_PCS_TRAINING_PATTERN | DP_PCS_SCRAMBLE_BYPASS,
		  FIELD_PREP(DP_PCS_TRAINING_PATTERN, pattern) |
		  (pattern ? DP_PCS_SCRAMBLE_BYPASS : 0));
}

static int e9820_dp_set_drive(struct e9820_dp *dp, const u8 vs[DP_MAX_LANES],
			      const u8 pe[DP_MAX_LANES])
{
	union phy_configure_opts opts = { };
	u8 set[DP_MAX_LANES];
	int i;

	for (i = 0; i < dp->lanes; i++) {
		opts.dp.voltage[i] = vs[i];
		opts.dp.pre[i] = pe[i];
		set[i] = (vs[i] << DP_TRAIN_VOLTAGE_SWING_SHIFT) |
			 (pe[i] << DP_TRAIN_PRE_EMPHASIS_SHIFT);
		if (vs[i] == 3)
			set[i] |= DP_TRAIN_MAX_SWING_REACHED;
		if (vs[i] + pe[i] >= 3)
			set[i] |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
	}
	opts.dp.set_voltages = 1;
	phy_configure(dp->phy, &opts);

	return drm_dp_dpcd_write_data(&dp->aux, DP_TRAINING_LANE0_SET, set, dp->lanes);
}

static void e9820_dp_adjust(const u8 status[DP_LINK_STATUS_SIZE], unsigned int lanes,
			    u8 vs[DP_MAX_LANES], u8 pe[DP_MAX_LANES])
{
	int i;

	for (i = 0; i < lanes; i++) {
		vs[i] = drm_dp_get_adjust_request_voltage(status, i) >> DP_TRAIN_VOLTAGE_SWING_SHIFT;
		pe[i] = drm_dp_get_adjust_request_pre_emphasis(status, i) >> DP_TRAIN_PRE_EMPHASIS_SHIFT;
		pe[i] = min_t(u8, pe[i], 3 - vs[i]);
	}
}

static int e9820_dp_train_at(struct e9820_dp *dp, int rate)
{
	union phy_configure_opts opts = { };
	u8 status[DP_LINK_STATUS_SIZE];
	u8 vs[DP_MAX_LANES] = { }, pe[DP_MAX_LANES] = { }, last_vs = 0;
	bool enhanced = drm_dp_enhanced_frame_cap(dp->dpcd);
	u8 link[2];
	int tps, tries, same, ret;

	opts.dp.link_rate = rate / 100;
	opts.dp.lanes = dp->lanes;
	opts.dp.set_rate = 1;
	opts.dp.set_lanes = 1;
	ret = phy_configure(dp->phy, &opts);
	if (ret)
		return ret;
	dp_write(dp, DP_LANE_COUNT, dp->lanes);
	dp_update(dp, DP_SST1_MAIN_CONTROL, DP_SST1_ENHANCED_MODE,
		  enhanced ? DP_SST1_ENHANCED_MODE : 0);
	ret = e9820_dp_wait_pll(dp);
	if (ret)
		return ret;

	link[0] = drm_dp_link_rate_to_bw_code(rate);
	link[1] = dp->lanes | (enhanced ? DP_LANE_COUNT_ENHANCED_FRAME_EN : 0);
	ret = drm_dp_dpcd_write_data(&dp->aux, DP_LINK_BW_SET, link, 2);
	if (ret)
		return ret;

	/* clock recovery */
	e9820_dp_set_pattern(dp, 1);
	ret = drm_dp_dpcd_write_byte(&dp->aux, DP_TRAINING_PATTERN_SET,
				     DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE);
	if (ret < 0)
		return ret;
	for (tries = 0, same = 0; ; tries++) {
		ret = e9820_dp_set_drive(dp, vs, pe);
		if (ret)
			return ret;
		drm_dp_link_train_clock_recovery_delay(&dp->aux, dp->dpcd);
		ret = drm_dp_dpcd_read_link_status(&dp->aux, status);
		if (ret)
			return ret;
		if (drm_dp_clock_recovery_ok(status, dp->lanes))
			break;
		last_vs = vs[0];
		e9820_dp_adjust(status, dp->lanes, vs, pe);
		same = vs[0] == last_vs ? same + 1 : 0;
		if (tries >= 15 || same >= 4) {
			dev_info(dp->dev, "CR failed at %d: status %*ph\n", rate,
				 DP_LINK_STATUS_SIZE, status);
			return -EAGAIN;
		}
	}
	dev_info(dp->dev, "CR done at %d, swing %u/%u pre %u/%u\n", rate, vs[0], vs[1], pe[0], pe[1]);

	/* channel equalisation */
	tps = drm_dp_tps3_supported(dp->dpcd) ? 3 : 2;
	e9820_dp_set_pattern(dp, tps);
	ret = drm_dp_dpcd_write_byte(&dp->aux, DP_TRAINING_PATTERN_SET,
				     (tps == 3 ? DP_TRAINING_PATTERN_3 : DP_TRAINING_PATTERN_2) |
				     DP_LINK_SCRAMBLING_DISABLE);
	if (ret < 0)
		return ret;
	for (tries = 0; tries < 6; tries++) {
		drm_dp_link_train_channel_eq_delay(&dp->aux, dp->dpcd);
		ret = drm_dp_dpcd_read_link_status(&dp->aux, status);
		if (ret)
			return ret;
		if (!drm_dp_clock_recovery_ok(status, dp->lanes))
			break;
		if (drm_dp_channel_eq_ok(status, dp->lanes)) {
			e9820_dp_set_pattern(dp, 0);
			drm_dp_dpcd_write_byte(&dp->aux, DP_TRAINING_PATTERN_SET,
					       DP_TRAINING_PATTERN_DISABLE);
			dev_info(dp->dev, "link trained: %d x%d, TPS%d, swing %u/%u pre %u/%u\n",
				 rate, dp->lanes, tps, vs[0], vs[1], pe[0], pe[1]);
			return 0;
		}
		e9820_dp_adjust(status, dp->lanes, vs, pe);
		ret = e9820_dp_set_drive(dp, vs, pe);
		if (ret)
			return ret;
	}
	dev_info(dp->dev, "EQ failed at %d: status %*ph\n", rate, DP_LINK_STATUS_SIZE, status);
	return -EAGAIN;
}

/* the lane count the port's pin assignment gives DP */
static unsigned int e9820_dp_port_lanes(struct e9820_dp *dp)
{
	return READ_ONCE(dp->port_state) & MAX77705_DP_FOUR_LANES ? 4 : 2;
}

static int e9820_dp_train(struct e9820_dp *dp)
{
	int rate, ret;

	dp->lanes = e9820_dp_port_lanes(dp);

	ret = drm_dp_read_dpcd_caps(&dp->aux, dp->dpcd);
	if (ret)
		return ret;
	dp->dpcd_valid = true;
	dev_info(dp->dev, "sink DPCD %*ph\n", DP_RECEIVER_CAP_SIZE, dp->dpcd);
	/* a dock's DP-to-HDMI converter may take two lanes even on pin C */
	dp->lanes = min_t(unsigned int, dp->lanes, drm_dp_max_lane_count(dp->dpcd));
	if (dp->lanes != 2 && dp->lanes != 4)
		dp->lanes = 2;

	/* 1080p60 needs HBR on two lanes, 4K60 HBR2 on four; RBR is not tried */
	rate = min(drm_dp_max_link_rate(dp->dpcd), 540000);
	for (; rate >= 270000; rate = rate == 540000 ? 270000 : 0) {
		ret = e9820_dp_train_at(dp, rate);
		if (!ret) {
			dp->link_rate = rate;
			dp->trained = true;
			return 0;
		}
		e9820_dp_set_pattern(dp, 0);
		drm_dp_dpcd_write_byte(&dp->aux, DP_TRAINING_PATTERN_SET,
				       DP_TRAINING_PATTERN_DISABLE);
	}
	return ret ?: -EAGAIN;
}

/* SST1_LH_PWR_ON: the stream interface DECON2 feeds (decon_reg_start/stop) */
static int e9820_dp_lh(struct e9820_dp *dp, bool on)
{
	u32 v;
	int ret;

	dp_update(dp, DP_SST1_FUNCTION_EN, DP_SST1_LH_PWR_ON, on ? DP_SST1_LH_PWR_ON : 0);
	ret = readl_poll_timeout(dp->regs + DP_SST1_FUNCTION_EN, v,
				 !!(v & DP_SST1_LH_PWR_ON_STATUS) == on, 1, 20000);
	if (ret)
		dev_err(dp->dev, "LH P-channel power %s timed out (%08x)\n", on ? "on" : "off", v);
	else
		dp->lh = on;
	return ret;
}

/*
 * set_video_configuration() for CEA 1920x1080p60, then either the BIST
 * colour bars or the stream DECON2 sends, and VIDEO_EN.
 */
static int e9820_dp_video(struct e9820_dp *dp, bool on, bool bist,
			  const struct drm_display_mode *m)
{
	const u32 pclk = m->clock, bpp = 24;
	const u32 hact = m->hdisplay, hfp = m->hsync_start - m->hdisplay;
	const u32 hbp = m->htotal - m->hsync_end;
	const u32 vact = m->vdisplay, vfp = m->vsync_start - m->vdisplay;
	const u32 vbp = m->vtotal - m->vsync_end;
	u32 ls = dp->link_rate;		/* drm's link_rate is the symbol clock in kHz */
	u64 tu, tu_on, unit = 10000000000ULL;
	u32 i_off, f_off, i_on, f_on;

	if (!on) {
		dp_write(dp, DP_SST1_VIDEO_ENABLE, 0);
		dp_update(dp, DP_SST1_VIDEO_CONTROL, DP_VIDEO_STRM_VALID, 0);
		dp_write(dp, DP_SST1_BIST_CONTROL, 0);
		dp->bist = false;
		dp->video = false;
		return 0;
	}
	if (!dp->trained)
		return -ENOLINK;
	/* 24 bpp over the trained lanes: lanes x symbol clock x 8 bits */
	if ((u64)pclk * bpp > (u64)dp->lanes * ls * 8)
		return -ENOSPC;

	dp_update(dp, DP_SST1_VIDEO_CONTROL,
		  DP_VIDEO_CEA_RANGE | DP_VIDEO_BPC | DP_VIDEO_COLOR_FORMAT |
		  DP_VIDEO_VSYNC_POS | DP_VIDEO_HSYNC_POS,
		  (drm_match_cea_mode(m) ? DP_VIDEO_CEA_RANGE : 0) |
		  FIELD_PREP(DP_VIDEO_BPC, 1) |
		  (m->flags & DRM_MODE_FLAG_PVSYNC ? DP_VIDEO_VSYNC_POS : 0) |
		  (m->flags & DRM_MODE_FLAG_PHSYNC ? DP_VIDEO_HSYNC_POS : 0));
	dp_write(dp, DP_SST1_VTOTAL, m->vtotal);
	dp_write(dp, DP_SST1_HTOTAL, m->htotal);
	dp_write(dp, DP_SST1_VACTIVE, vact);
	dp_write(dp, DP_SST1_VFP, vfp);
	dp_write(dp, DP_SST1_VBP, vbp);
	dp_write(dp, DP_SST1_HACTIVE, hact);
	dp_write(dp, DP_SST1_HFP, hfp);
	dp_write(dp, DP_SST1_HBP, hbp);

	dp_write(dp, DP_SST1_MVID_MASTER, pclk >> 1);
	dp_write(dp, DP_SST1_NVID_MASTER, ls);
	dp_update(dp, DP_SST1_MAIN_CONTROL, DP_SST1_MVID_MODE, DP_SST1_MVID_MODE);
	dp_write(dp, DP_SST1_MVID_SFR, pclk);
	dp_write(dp, DP_SST1_NVID_SFR, ls);

	/* transfer unit fill, in 1e-10 steps as the CAL computes it */
	tu = div64_u64((u64)pclk * bpp * 32 * unit, (u64)dp->lanes * ls * 8);
	tu_on = div64_u64(tu * 1000, 976);
	i_off = div64_u64(tu, unit);
	f_off = div64_u64(tu - i_off * unit, 10);
	i_on = div64_u64(tu_on, unit);
	f_on = div64_u64(tu_on - i_on * unit, 10);
	dp_write(dp, DP_SST1_AS_MODE_CONTROL, 1);
	dp_write(dp, DP_SST1_AS_THR_SEL_FEC_OFF, 1);
	dp_write(dp, DP_SST1_AS_THR_SEL_FEC_ON, 1);
	dp_write(dp, DP_SST1_AS_INT_FEC_OFF, i_off);
	dp_write(dp, DP_SST1_AS_FRAC_FEC_OFF, f_off);
	dp_write(dp, DP_SST1_AS_THR_FEC_OFF, i_off <= 2 ? 7 : i_off <= 5 ? 8 : 9);
	dp_write(dp, DP_SST1_AS_INT_FEC_ON, i_on);
	dp_write(dp, DP_SST1_AS_FRAC_FEC_ON, f_on);
	dp_write(dp, DP_SST1_AS_THR_FEC_ON, i_on <= 2 ? 7 : i_on <= 5 ? 8 : 9);

	dp_write(dp, DP_SST1_VIDEO_TIMING_GEN, 1);
	dp_update(dp, DP_SST1_MAIN_CONTROL, DP_SST1_VIDEO_MODE, 0);

	if (bist) {
		dp_write(dp, DP_SST1_BIST_CONTROL, FIELD_PREP(DP_BIST_TYPE, 0));
		dp_update(dp, DP_SST1_VIDEO_CONTROL, DP_VIDEO_STRM_VALID, DP_VIDEO_STRM_VALID);
		dp_update(dp, DP_SST1_BIST_CONTROL, DP_BIST_EN, DP_BIST_EN);
	} else {
		dp_write(dp, DP_SST1_BIST_CONTROL, 0);
		dp_update(dp, DP_SST1_VIDEO_CONTROL, DP_VIDEO_STRM_VALID, 0);
	}
	dp_write(dp, DP_SST1_VIDEO_ENABLE, 1);
	dp->bist = bist;
	dp->video = !bist;
	dev_info(dp->dev, "%s %ux%u@%d (%u kHz) on, ls %u kHz, TU %u.%09u\n",
		 bist ? "BIST" : "video", hact, vact, drm_mode_vrefresh(m), pclk,
		 ls, i_off, f_off);
	return 0;
}

/* the stream, for the display controller (exynos9820-dp.h) */

int exynos9820_dp_stream_on(struct drm_bridge *bridge, const struct drm_display_mode *mode)
{
	struct e9820_dp *dp = bridge_to_dp(bridge);
	int ret;

	guard(mutex)(&dp->lock);
	ret = e9820_dp_power_on(dp);
	if (ret)
		return ret;
	if (!dp->trained) {
		ret = e9820_dp_train(dp);
		if (ret)
			goto err;
	}
	ret = e9820_dp_video(dp, true, false, mode);
	if (ret)
		goto err;
	ret = e9820_dp_lh(dp, true);
	if (ret)
		goto err;
	return 0;
err:
	dev_err(dp->dev, "stream %s failed: %d\n", mode->name, ret);
	e9820_dp_power_off(dp);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos9820_dp_stream_on);

/* after DECON2 has stopped */
void exynos9820_dp_stream_off(struct drm_bridge *bridge)
{
	struct e9820_dp *dp = bridge_to_dp(bridge);

	guard(mutex)(&dp->lock);
	e9820_dp_power_off(dp);
}
EXPORT_SYMBOL_GPL(exynos9820_dp_stream_off);

static int e9820_dp_bridge_attach(struct drm_bridge *bridge, struct drm_encoder *encoder,
				  enum drm_bridge_attach_flags flags)
{
	return flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR ? 0 : -EINVAL;
}

static void e9820_dp_bridge_detach(struct drm_bridge *bridge)
{
	struct e9820_dp *dp = bridge_to_dp(bridge);

	WRITE_ONCE(dp->cec_registered, false);
	cancel_delayed_work_sync(&dp->cec_work);
	guard(mutex)(&dp->lock);
	if (dp->cec_adapter)
		drm_dp_cec_unregister_connector(&dp->aux);
	dp->cec_adapter = false;
}

/*
 * Connected while the alt mode is configured. The dock drops HPD when its
 * AUX has been idle for a while and raises it again on the first AUX
 * access, so HPD low alone does not mean the sink is gone.
 */
static enum drm_connector_status e9820_dp_bridge_detect(struct drm_bridge *bridge,
							struct drm_connector *connector)
{
	struct e9820_dp *dp = bridge_to_dp(bridge);

	return READ_ONCE(dp->port_state) & MAX77705_DP_CONFIGURED ?
	       connector_status_connected : connector_status_disconnected;
}

static const struct drm_edid *e9820_dp_read_edid(struct e9820_dp *dp,
						 struct drm_connector *connector)
{
	const struct drm_edid *edid;
	bool was_on;

	guard(mutex)(&dp->lock);
	was_on = dp->powered;
	if (!was_on && e9820_dp_power_on(dp))
		return NULL;
	if (!drm_dp_read_dpcd_caps(&dp->aux, dp->dpcd))
		dp->dpcd_valid = true;
	edid = drm_edid_read_ddc(connector, &dp->aux.ddc);
	/*
	 * HDMI CEC, when the dock's DP-to-HDMI converter tunnels it over AUX
	 * (DPCD 0x3000); the EDID gives the CEC physical address.
	 */
	if (!dp->cec_adapter) {
		drm_dp_cec_register_connector(&dp->aux, connector);
		dp->cec_adapter = true;
		WRITE_ONCE(dp->cec_registered, true);
	}
	drm_dp_cec_set_edid(&dp->aux, edid ? drm_edid_raw(edid) : NULL);
	mod_delayed_work(system_dfl_wq, &dp->cec_work, 0);
	if (!was_on)
		e9820_dp_power_off(dp);
	return edid;
}

/*
 * Every EDID mode, but 1920x1080@60 is the one a compositor starts with
 * (TVs tend to prefer 4K, which two lanes cannot carry at 60 Hz).
 */
static int e9820_dp_bridge_get_modes(struct drm_bridge *bridge, struct drm_connector *connector)
{
	struct e9820_dp *dp = bridge_to_dp(bridge);
	const struct drm_edid *edid = e9820_dp_read_edid(dp, connector);
	struct drm_display_mode *m, *pick = NULL;
	int n;

	drm_edid_connector_update(connector, edid);
	n = drm_edid_connector_add_modes(connector);
	drm_edid_free(edid);
	if (!n) {
		n = drm_add_modes_noedid(connector, 1920, 1080);
		drm_set_preferred_mode(connector, 1920, 1080);
		return n;
	}

	list_for_each_entry(m, &connector->probed_modes, head) {
		if (m->hdisplay == 1920 && m->vdisplay == 1080 &&
		    drm_mode_vrefresh(m) == 60 && !(m->flags & DRM_MODE_FLAG_INTERLACE) &&
		    (!pick || m->clock > pick->clock))
			pick = m;
	}
	if (pick) {
		list_for_each_entry(m, &connector->probed_modes, head)
			m->type &= ~DRM_MODE_TYPE_PREFERRED;
		pick->type |= DRM_MODE_TYPE_PREFERRED;
	}
	return n;
}

static enum drm_mode_status e9820_dp_bridge_mode_valid(struct drm_bridge *bridge,
						       const struct drm_display_info *info,
						       const struct drm_display_mode *mode)
{
	struct e9820_dp *dp = bridge_to_dp(bridge);
	unsigned int lanes;
	int rate = 540000;

	if (dp->dpcd_valid)
		rate = min(drm_dp_max_link_rate(dp->dpcd), rate);
	lanes = e9820_dp_port_lanes(dp);
	if (dp->dpcd_valid)
		lanes = min_t(unsigned int, lanes, drm_dp_max_lane_count(dp->dpcd));
	if ((u64)mode->clock * 24 > (u64)lanes * rate * 8)
		return MODE_CLOCK_HIGH;
	if (mode->hdisplay > 4096 || mode->vdisplay > 2160)
		return MODE_BAD;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;
	return MODE_OK;
}

static void e9820_dp_bridge_hpd_enable(struct drm_bridge *bridge)
{
	struct e9820_dp *dp = bridge_to_dp(bridge);

	WRITE_ONCE(dp->hpd_enabled, true);
	schedule_work(&dp->hpd_work);
}

static void e9820_dp_bridge_hpd_disable(struct drm_bridge *bridge)
{
	struct e9820_dp *dp = bridge_to_dp(bridge);

	WRITE_ONCE(dp->hpd_enabled, false);
}

static const struct drm_bridge_funcs e9820_dp_bridge_funcs = {
	.attach = e9820_dp_bridge_attach,
	.detach = e9820_dp_bridge_detach,
	.detect = e9820_dp_bridge_detect,
	.get_modes = e9820_dp_bridge_get_modes,
	.mode_valid = e9820_dp_bridge_mode_valid,
	.hpd_enable = e9820_dp_bridge_hpd_enable,
	.hpd_disable = e9820_dp_bridge_hpd_disable,
};

static void e9820_dp_hpd_work(struct work_struct *work)
{
	struct e9820_dp *dp = container_of(work, struct e9820_dp, hpd_work);
	bool connected = READ_ONCE(dp->port_state) & MAX77705_DP_CONFIGURED;

	if (!connected) {
		dp->dpcd_valid = false;
		if (READ_ONCE(dp->cec_registered))
			drm_dp_cec_unset_edid(&dp->aux);
	}
	if (READ_ONCE(dp->hpd_enabled))
		drm_bridge_hpd_notify(&dp->bridge, connected ? connector_status_connected :
						     connector_status_disconnected);
}

static int e9820_dp_port_event(struct notifier_block *nb, unsigned long state, void *data)
{
	struct e9820_dp *dp = container_of(nb, struct e9820_dp, nb);

	dev_dbg(dp->dev, "port state %lx\n", state);
	WRITE_ONCE(dp->port_state, state & ~MAX77705_DP_HPD_IRQ);
	schedule_work(&dp->hpd_work);
	if (state & MAX77705_DP_HPD_IRQ)
		mod_delayed_work(system_dfl_wq, &dp->cec_work, 0);
	return NOTIFY_OK;
}

static unsigned int cec_poll_ms = 50;
module_param(cec_poll_ms, uint, 0644);
MODULE_PARM_DESC(cec_poll_ms, "CEC: poll the sink's CEC IRQ every N ms (0: IRQ_HPD only)");

/*
 * A received CEC message, and the end of a transmit, set DP_CEC_IRQ in ESI1
 * and should raise IRQ_HPD. Docks deliver that Attention only now and then
 * (the PS176 one: none for minutes, then a burst), so poll ESI1 while the
 * sink is there; an IRQ_HPD just runs the poll at once.
 */
static void e9820_dp_cec_work(struct work_struct *work)
{
	struct e9820_dp *dp = container_of(to_delayed_work(work), struct e9820_dp,
					   cec_work);

	if (!READ_ONCE(dp->cec_registered) ||
	    !(READ_ONCE(dp->port_state) & MAX77705_DP_CONFIGURED))
		return;
	if (READ_ONCE(dp->powered))
		drm_dp_cec_irq(&dp->aux);
	if (READ_ONCE(cec_poll_ms))
		queue_delayed_work(system_dfl_wq, &dp->cec_work,
				   msecs_to_jiffies(READ_ONCE(cec_poll_ms)));
}

/* debugfs */

static ssize_t power_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct e9820_dp *dp = file_inode(f)->i_private;
	bool on;
	int ret;

	ret = kstrtobool_from_user(ubuf, len, &on);
	if (ret)
		return ret;
	guard(mutex)(&dp->lock);
	if (on)
		ret = e9820_dp_power_on(dp);
	else
		e9820_dp_power_off(dp);
	return ret ?: len;
}

static ssize_t train_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct e9820_dp *dp = file_inode(f)->i_private;
	int ret;

	guard(mutex)(&dp->lock);
	if (!dp->powered)
		return -ENODEV;
	dp->trained = false;
	ret = e9820_dp_train(dp);
	return ret ?: len;
}

static ssize_t bist_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct e9820_dp *dp = file_inode(f)->i_private;
	bool on;
	int ret;

	ret = kstrtobool_from_user(ubuf, len, &on);
	if (ret)
		return ret;
	guard(mutex)(&dp->lock);
	if (!dp->powered)
		return -ENODEV;
	ret = e9820_dp_video(dp, on, true, &e9820_dp_bist_mode);
	return ret ?: len;
}

static ssize_t video_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct e9820_dp *dp = file_inode(f)->i_private;
	bool on;
	int ret;

	ret = kstrtobool_from_user(ubuf, len, &on);
	if (ret)
		return ret;
	guard(mutex)(&dp->lock);
	if (!dp->powered)
		return -ENODEV;
	ret = e9820_dp_video(dp, on, false, &e9820_dp_bist_mode);
	return ret ?: len;
}

static ssize_t lh_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct e9820_dp *dp = file_inode(f)->i_private;
	bool on;
	int ret;

	ret = kstrtobool_from_user(ubuf, len, &on);
	if (ret)
		return ret;
	guard(mutex)(&dp->lock);
	if (!dp->powered)
		return -ENODEV;
	ret = e9820_dp_lh(dp, on);
	return ret ?: len;
}

static ssize_t dpcd_read(struct file *f, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct e9820_dp *dp = file_inode(f)->i_private;
	u8 buf[DP_AUX_MAX_PAYLOAD_BYTES];
	ssize_t ret;

	if (*ppos >= 0x100000)
		return 0;
	len = min_t(size_t, len, sizeof(buf));
	len = min_t(size_t, len, 0x100000 - *ppos);
	guard(mutex)(&dp->lock);
	ret = drm_dp_dpcd_read(&dp->aux, *ppos, buf, len);
	if (ret <= 0)
		return ret ?: -EIO;
	if (copy_to_user(ubuf, buf, ret))
		return -EFAULT;
	*ppos += ret;
	return ret;
}

static ssize_t dpcd_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	struct e9820_dp *dp = file_inode(f)->i_private;
	u8 buf[DP_AUX_MAX_PAYLOAD_BYTES];
	ssize_t ret;

	if (*ppos >= 0x100000)
		return -ENOSPC;
	len = min_t(size_t, len, sizeof(buf));
	len = min_t(size_t, len, 0x100000 - *ppos);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	guard(mutex)(&dp->lock);
	ret = drm_dp_dpcd_write(&dp->aux, *ppos, buf, len);
	if (ret <= 0)
		return ret ?: -EIO;
	*ppos += ret;
	return ret;
}

static int edid_show(struct seq_file *s, void *unused)
{
	struct e9820_dp *dp = s->private;
	u8 edid[256], offset = 0;
	struct i2c_msg msgs[] = {
		{ .addr = 0x50, .flags = 0, .len = 1, .buf = &offset },
		{ .addr = 0x50, .flags = I2C_M_RD, .len = 128, .buf = edid },
	};
	int ret, len = 128;

	guard(mutex)(&dp->lock);
	if (!dp->powered)
		return -ENODEV;
	ret = i2c_transfer(&dp->aux.ddc, msgs, 2);
	if (ret != 2)
		return ret < 0 ? ret : -EIO;
	if (edid[126]) {
		offset = 128;
		msgs[1].buf = edid + 128;
		if (i2c_transfer(&dp->aux.ddc, msgs, 2) == 2)
			len = 256;
	}
	seq_write(s, edid, len);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(edid);

static int regs_show(struct seq_file *s, void *unused)
{
	struct e9820_dp *dp = s->private;
	static const u32 regs[] = {
		0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x001c,
		0x002c, 0x0108, 0x1000, 0x1018, 0x101c, 0x3000, 0x3004, 0x3008,
		0x5000, 0x5024, 0x5028, 0x5400, 0x5404, 0x5408, 0x5450, 0x5460,
	};
	int i;

	guard(mutex)(&dp->lock);
	seq_printf(s, "powered %d trained %d (rate %d) bist %d video %d lh %d port %d\n",
		   dp->powered, dp->trained, dp->link_rate, dp->bist, dp->video, dp->lh,
		   dp->port_val);
	if (!dp->powered)
		return 0;
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(s, "%04x: %08x\n", regs[i], dp_read(dp, regs[i]));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

static const struct file_operations power_fops = {
	.owner = THIS_MODULE, .write = power_write,
};
static const struct file_operations train_fops = {
	.owner = THIS_MODULE, .write = train_write,
};
static const struct file_operations bist_fops = {
	.owner = THIS_MODULE, .write = bist_write,
};
static const struct file_operations video_fops = {
	.owner = THIS_MODULE, .write = video_write,
};
static const struct file_operations lh_fops = {
	.owner = THIS_MODULE, .write = lh_write,
};
static const struct file_operations dpcd_fops = {
	.owner = THIS_MODULE, .read = dpcd_read, .write = dpcd_write,
	.llseek = default_llseek,
};

static int e9820_dp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct e9820_dp *dp;
	int ret;

	dp = devm_drm_bridge_alloc(dev, struct e9820_dp, bridge, &e9820_dp_bridge_funcs);
	if (IS_ERR(dp))
		return PTR_ERR(dp);
	dp->dev = dev;
	ret = devm_mutex_init(dev, &dp->lock);
	if (ret)
		return ret;

	dp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dp->regs))
		return PTR_ERR(dp->regs);
	dp->gtc = devm_clk_get(dev, "gtc");
	if (IS_ERR(dp->gtc))
		return dev_err_probe(dev, PTR_ERR(dp->gtc), "gtc clock\n");
	dp->phy = devm_phy_get(dev, "dp");
	if (IS_ERR(dp->phy))
		return dev_err_probe(dev, PTR_ERR(dp->phy), "dp phy\n");

	/* AUX switch off until a sink is there, as downstream's probe leaves it */
	dp->aux_en = devm_gpiod_get(dev, "aux-en", GPIOD_OUT_LOW);
	if (IS_ERR(dp->aux_en))
		return dev_err_probe(dev, PTR_ERR(dp->aux_en), "aux-en gpio\n");
	dp->sbu_sel = devm_gpiod_get(dev, "sbu-sel", GPIOD_ASIS);
	if (IS_ERR(dp->sbu_sel))
		return dev_err_probe(dev, PTR_ERR(dp->sbu_sel), "sbu-sel gpio\n");
	/* the USB/DP PHY reads the same input */
	dp->port = devm_gpiod_get(dev, "port", GPIOD_IN | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(dp->port))
		return dev_err_probe(dev, PTR_ERR(dp->port), "port gpio\n");

	dp->aux.name = "exynos9820-dp";
	dp->aux.dev = dev;
	dp->aux.transfer = e9820_dp_aux_transfer;
	drm_dp_aux_init(&dp->aux);

	dev_info(dev, "version %08x\n", dp_read(dp, 0));

	dp->debugfs = debugfs_create_dir("exynos9820-dp", NULL);
	debugfs_create_file("power", 0200, dp->debugfs, dp, &power_fops);
	debugfs_create_file("train", 0200, dp->debugfs, dp, &train_fops);
	debugfs_create_file("bist", 0200, dp->debugfs, dp, &bist_fops);
	debugfs_create_file("video", 0200, dp->debugfs, dp, &video_fops);
	debugfs_create_file("lh", 0200, dp->debugfs, dp, &lh_fops);
	debugfs_create_file("dpcd", 0600, dp->debugfs, dp, &dpcd_fops);
	debugfs_create_file("edid", 0400, dp->debugfs, dp, &edid_fops);
	debugfs_create_file("regs", 0400, dp->debugfs, dp, &regs_fops);

	platform_set_drvdata(pdev, dp);

	INIT_WORK(&dp->hpd_work, e9820_dp_hpd_work);
	INIT_DELAYED_WORK(&dp->cec_work, e9820_dp_cec_work);
	dp->lanes = 2;
	dp->bridge.of_node = dev->of_node;
	dp->bridge.type = DRM_MODE_CONNECTOR_DisplayPort;
	dp->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_MODES | DRM_BRIDGE_OP_HPD;
	ret = devm_drm_bridge_add(dev, &dp->bridge);
	if (ret)
		return ret;

	dp->nb.notifier_call = e9820_dp_port_event;
	return max77705_dp_register_notifier(&dp->nb);
}

static void e9820_dp_remove(struct platform_device *pdev)
{
	struct e9820_dp *dp = platform_get_drvdata(pdev);

	max77705_dp_unregister_notifier(&dp->nb);
	cancel_work_sync(&dp->hpd_work);
	WRITE_ONCE(dp->cec_registered, false);
	cancel_delayed_work_sync(&dp->cec_work);
	debugfs_remove_recursive(dp->debugfs);
	guard(mutex)(&dp->lock);
	e9820_dp_power_off(dp);
}

static const struct of_device_id e9820_dp_of_match[] = {
	{ .compatible = "samsung,exynos9820-dp" },
	{ }
};
MODULE_DEVICE_TABLE(of, e9820_dp_of_match);

static struct platform_driver e9820_dp_driver = {
	.probe = e9820_dp_probe,
	.remove = e9820_dp_remove,
	.driver = {
		.name = "exynos9820-dp",
		.of_match_table = e9820_dp_of_match,
	},
};
module_platform_driver(e9820_dp_driver);

MODULE_DESCRIPTION("Exynos 9820 DisplayPort link controller");
MODULE_LICENSE("GPL");
