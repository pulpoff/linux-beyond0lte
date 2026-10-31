// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K3P9 (16 MP, rear ultrawide of the Galaxy S10e), S5K3J1 (10 MP,
 * front camera) and S5K2L4 (12 MP dual-pixel, rear wide): the same register
 * model, each with its own tables and power order.
 *
 * A V4L2 sub-device with one source pad, RAW10 on 4 lanes. The sensor is
 * powered only while it streams: stream on powers it, loads the stock global
 * and mode tables, applies the controls and starts it; stream off stops it
 * and powers it off. A lens (lens-focus in DT) is bound through
 * v4l2_async_register_subdev_sensor().
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

struct s5k3p9_reg {
	u16 addr, val;
};

#include "s5k3p9-tables.h"
#include "s5k3j1-tables.h"
#include "s5k2l4-tables.h"

#define S5K3P9_REG_MODEL_ID	0x0000
#define S5K3P9_REG_REVISION	0x0002
#define S5K3P9_REG_STREAM	0x0100
#define S5K3P9_REG_CIT		0x0202
#define S5K3P9_REG_AGAIN	0x0204
#define S5K3P9_REG_FLL		0x0340
#define S5K3P9_REG_TEST_PATTERN	0x0600
#define S5K3P9_REG_PAGE		0xfcfc
#define S5K3P9_REG_INDIRECT	0x6f12
#define S5K3P9_DELAY		0xffff
#define S5K3P9_MCLK_HZ		26000000

#define S5K_MAX_SUPPLIES	7
#define S5K_AGAIN_MIN		0x20	/* code = gain x 32 */
#define S5K_AGAIN_MAX		0x200
#define S5K_FLL_MAX		0xffff

struct s5k_mode {
	u32 width, height;
	u32 llp, fll;			/* line length (pixel clocks), frame length (lines) */
	u64 pixel_rate;
	const struct s5k3p9_reg *regs;
	unsigned int n;
	unsigned int link_freq;		/* index into s5k_data.link_freqs */
	const struct s5k3p9_reg *post;	/* after the mode table */
	unsigned int n_post;
};

struct s5k_data {
	const char *model;			/* the subdev name, which libcamera matches its helper and tuning by */
	/* in the stock power-on order */
	const char *supplies[S5K_MAX_SUPPLIES];
	unsigned int n_supplies;
	unsigned int power_on_delay_us;		/* after MCLK starts (after reset, see below) */
	bool reset_after_mclk;			/* MCLK first, then out of reset */
	unsigned int reset_delay_us;		/* after reset, when reset_after_mclk */
	const struct s5k3p9_reg *tnp;		/* soft reset + firmware patch, before the global table */
	unsigned int n_tnp;
	const struct s5k3p9_reg *global;
	unsigned int n_global;
	const struct s5k_mode *modes;
	unsigned int n_modes;
	const struct s5k3p9_reg *post_mode;	/* after the mode table */
	unsigned int n_post_mode;
	bool dual_sync_master;			/* 0x0a70/0x0a72 before stream on */
	u32 code;				/* Bayer order as streamed */
	u16 stream_on;				/* 0x0100: mode_select, orientation */
	const s64 *link_freqs;			/* Hz: half the lane rate */
	unsigned int n_link_freqs;
	u32 cit_min, cit_margin;		/* coarse integration time, lines */
};

#define S5K_TABLE(t)	(t), ARRAY_SIZE(t)

static const s64 s5k_link_1027[] = { 1027000000 };
static const s64 s5k_link_903[] = { 903500000 };

/* pixel clock (26 MHz / 6 x 0xe2 / 7) x 4 */
#define S5K3P9_PIXEL_RATE	559619047ULL

static const struct s5k_mode s5k3p9_modes[] = {
	{ 2304, 1728, 0x13e0, 0x0728, S5K3P9_PIXEL_RATE, S5K_TABLE(s5k3p9_mode_2304x1728) },	/* 60 fps */
	{ 1152, 864, 0x13e0, 0x03d6, S5K3P9_PIXEL_RATE, S5K_TABLE(s5k3p9_mode_1152x864) },	/* 112 fps */
};

/* VDD_EXT (BUCK2) first, then analog, digital, IO */
static const struct s5k_data s5k3p9_data = {
	.model = "s5k3p9",
	.supplies = { "vddext", "avdd", "dvdd", "dovdd" },
	.n_supplies = 4,
	.power_on_delay_us = 1100,
	.global = s5k3p9_global,
	.n_global = ARRAY_SIZE(s5k3p9_global),
	.modes = s5k3p9_modes,
	.n_modes = ARRAY_SIZE(s5k3p9_modes),
	.post_mode = s5k3p9_dualsync_master,
	.n_post_mode = ARRAY_SIZE(s5k3p9_dualsync_master),
	.code = MEDIA_BUS_FMT_SGRBG10_1X10,
	.stream_on = 0x0100,
	.link_freqs = s5k_link_1027,
	.n_link_freqs = 1,
	.cit_min = 7,
	.cit_margin = 8,
};

static const struct s5k_mode s5k3j1_modes[] = {
	{ 1824, 1368, 0x4940, 0x0597, 806000000ULL, S5K_TABLE(s5k3j1_mode_1824x1368) },	/* 30 fps */
	{ 3648, 2736, 0x24a8, 0x0b30, 807720000ULL, S5K_TABLE(s5k3j1_mode_3648x2736) },	/* 30 fps */
};

/* VDD_EXT, AF, analog, IO, digital; 9 ms after MCLK */
static const struct s5k_data s5k3j1_data = {
	.model = "s5k3j1",
	.supplies = { "vddext", "afvdd", "avdd", "dovdd", "dvdd" },
	.n_supplies = 5,
	.power_on_delay_us = 9000,
	.global = s5k3j1_global,
	.n_global = ARRAY_SIZE(s5k3j1_global),
	.modes = s5k3j1_modes,
	.n_modes = ARRAY_SIZE(s5k3j1_modes),
	.dual_sync_master = true,
	.code = MEDIA_BUS_FMT_SGRBG10_1X10,
	.stream_on = 0x0100,
	.link_freqs = s5k_link_903,
	.n_link_freqs = 1,
	.cit_min = 2,
	.cit_margin = 4,
};

/* 2054 and 2177.5 Mbps per lane */
static const s64 s5k2l4_link_freqs[] = { 1027000000, 1088750000 };

/* the low-noise readout (LN2) stock turns on after the 30 fps full-width modes */
static const struct s5k3p9_reg s5k2l4_ln2[] = {
	{ 0x0104, 0x0101 }, { 0x6028, 0x2000 }, { 0x602a, 0xff22 }, { 0x6f12, 0x0001 },
	{ 0x3012, 0x0201 }, { 0x0104, 0x0001 },
};

/*
 * Stock's pixel clock is 1080.444 MHz with line lengths of 880 and up: in
 * those units a line of the 2016 and 1008 wide modes is shorter than its
 * width, which V4L2's blanking can not say. Everything is in units of a
 * quarter pixel clock instead (the same line and frame times).
 */
#define S5K2L4_PIXEL_RATE	(1080444000ULL * 4)

static const struct s5k_mode s5k2l4_modes[] = {
	{ 4032, 3024, 0x0b20 * 4, 0x3160, S5K2L4_PIXEL_RATE, S5K_TABLE(s5k2l4_mode_4032x3024), 0,
	  S5K_TABLE(s5k2l4_ln2) },						/* 30 fps */
	{ 4032, 2268, 0x0e88 * 4, 0x12e4, S5K2L4_PIXEL_RATE, S5K_TABLE(s5k2l4_mode_4032x2268_60), 1 },	/* 60 fps */
	{ 2016, 1134, 0x0370 * 4, 0x13f8, S5K2L4_PIXEL_RATE, S5K_TABLE(s5k2l4_mode_2016x1134), 0 },	/* 240 fps */
	{ 1008, 756, 0x04d0 * 4, 0x1c80, S5K2L4_PIXEL_RATE, S5K_TABLE(s5k2l4_mode_1008x756), 0 },	/* 120 fps */
};

/* stock power_seq_id 1 without the OIS: retention, analog, digital, AF, IO, MIPI; MCLK, then reset */
static const struct s5k_data s5k2l4_data = {
	.model = "s5k2l4",
	.supplies = { "vddret", "avdd", "avdd2", "dvdd", "afvdd", "dovdd", "vddq" },
	.n_supplies = 7,
	.power_on_delay_us = 1500,
	.reset_after_mclk = true,
	.reset_delay_us = 3000,
	.tnp = s5k2l4_tnp,
	.n_tnp = ARRAY_SIZE(s5k2l4_tnp),
	.global = s5k2l4_global,
	.n_global = ARRAY_SIZE(s5k2l4_global),
	.modes = s5k2l4_modes,
	.n_modes = ARRAY_SIZE(s5k2l4_modes),
	.code = MEDIA_BUS_FMT_SGRBG10_1X10,
	.stream_on = 0x0103,
	.link_freqs = s5k2l4_link_freqs,
	.n_link_freqs = ARRAY_SIZE(s5k2l4_link_freqs),
	.cit_min = 0x10,
	.cit_margin = 0x30,
};

static const char * const s5k3p9_test_patterns[] = {
	"Disabled",
	"Solid Colour",
	"Colour Bars",
	"Fade To Grey Colour Bars",
	"PN9",
};

struct s5k3p9 {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *mclk;
	struct gpio_desc *reset;
	const struct s5k_data *data;
	struct regulator_bulk_data supplies[S5K_MAX_SUPPLIES];

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_rate, *link_freq, *hblank, *vblank, *exposure;

	const struct s5k_mode *mode;
	bool streaming;
};

static inline struct s5k3p9 *to_s5k(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k3p9, sd);
}

static int s5k3p9_read16(struct s5k3p9 *s, u16 reg, u16 *val)
{
	u8 addr[2], buf[2];
	struct i2c_msg msgs[] = {
		{ .addr = s->client->addr, .len = 2, .buf = addr },
		{ .addr = s->client->addr, .flags = I2C_M_RD, .len = 2, .buf = buf },
	};
	int ret;

	put_unaligned_be16(reg, addr);
	ret = i2c_transfer(s->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return ret < 0 ? ret : -EIO;
	*val = get_unaligned_be16(buf);
	return 0;
}

static int s5k3p9_write(struct s5k3p9 *s, u16 reg, const u16 *vals, int n)
{
	u8 *buf;
	int i, ret;

	buf = kmalloc(2 + 2 * n, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	put_unaligned_be16(reg, buf);
	for (i = 0; i < n; i++)
		put_unaligned_be16(vals[i], buf + 2 + 2 * i);
	ret = i2c_master_send(s->client, buf, 2 + 2 * n);
	kfree(buf);
	if (ret < 0)
		return ret;
	return ret == 2 + 2 * n ? 0 : -EIO;
}

static int s5k3p9_write16(struct s5k3p9 *s, u16 reg, u16 val)
{
	return s5k3p9_write(s, reg, &val, 1);
}

/* longer messages time out on this bus (i2c-exynos5 beyond the FIFO?), so single writes for now */
static uint burst_max = 1;
module_param(burst_max, uint, 0644);
MODULE_PARM_DESC(burst_max, "words per 0x6f12 burst (the pointer auto-increments)");

/* runs of 0x6f12 writes go out as bursts, as the stock driver sends them */
static int s5k3p9_write_table(struct s5k3p9 *s, const struct s5k3p9_reg *t, int n)
{
	u16 burst[300];
	int i, j, ret;

	for (i = 0; i < n; i = j) {
		j = i + 1;
		if (t[i].addr == S5K3P9_DELAY) {
			usleep_range(t[i].val, t[i].val + 100);
			continue;
		}
		if (t[i].addr != S5K3P9_REG_INDIRECT) {
			ret = s5k3p9_write16(s, t[i].addr, t[i].val);
		} else {
			burst[0] = t[i].val;
			while (j < n && t[j].addr == S5K3P9_REG_INDIRECT && j - i < min_t(uint, ARRAY_SIZE(burst), max(burst_max, 1U))) {
				burst[j - i] = t[j].val;
				j++;
			}
			ret = s5k3p9_write(s, S5K3P9_REG_INDIRECT, burst, j - i);
		}
		if (ret) {
			dev_err(&s->client->dev, "write %#06x: %d\n", t[i].addr, ret);
			return ret;
		}
	}
	return 0;
}

static int s5k3p9_power_on(struct s5k3p9 *s)
{
	int i, ret;

	gpiod_set_value_cansleep(s->reset, 1);
	for (i = 0; i < s->data->n_supplies; i++) {
		ret = regulator_enable(s->supplies[i].consumer);
		if (ret)
			goto err_reg;
	}
	usleep_range(1000, 1100);
	if (!s->data->reset_after_mclk)
		gpiod_set_value_cansleep(s->reset, 0);
	ret = clk_prepare_enable(s->mclk);
	if (ret)
		goto err_reset;
	usleep_range(s->data->power_on_delay_us, s->data->power_on_delay_us + 400);
	if (s->data->reset_after_mclk) {
		gpiod_set_value_cansleep(s->reset, 0);
		usleep_range(s->data->reset_delay_us, s->data->reset_delay_us + 400);
	}
	return 0;

err_reset:
	gpiod_set_value_cansleep(s->reset, 1);
err_reg:
	while (--i >= 0)
		regulator_disable(s->supplies[i].consumer);
	return ret;
}

static void s5k3p9_power_off(struct s5k3p9 *s)
{
	int i;

	clk_disable_unprepare(s->mclk);
	gpiod_set_value_cansleep(s->reset, 1);
	for (i = s->data->n_supplies - 1; i >= 0; i--)
		regulator_disable(s->supplies[i].consumer);
}

/* the exposure range follows the frame length */
static void s5k3p9_update_exposure_range(struct s5k3p9 *s)
{
	u32 fll = s->mode->height + s->vblank->val;
	u32 max = fll - s->data->cit_margin;

	__v4l2_ctrl_modify_range(s->exposure, s->data->cit_min, max, 1,
				 min_t(u32, s->exposure->val, max));
}

static int s5k3p9_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k3p9 *s = container_of(ctrl->handler, struct s5k3p9, ctrls);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK)
		s5k3p9_update_exposure_range(s);

	if (!s->streaming)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = s5k3p9_write16(s, S5K3P9_REG_CIT, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = s5k3p9_write16(s, S5K3P9_REG_AGAIN, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		ret = s5k3p9_write16(s, S5K3P9_REG_FLL, s->mode->height + ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = s5k3p9_write16(s, S5K3P9_REG_TEST_PATTERN, ctrl->val);
		break;
	default:
		break;
	}
	return ret;
}

static const struct v4l2_ctrl_ops s5k3p9_ctrl_ops = {
	.s_ctrl = s5k3p9_set_ctrl,
};

/* blanking and pixel rate for a new mode */
static void s5k3p9_update_mode_ctrls(struct s5k3p9 *s)
{
	const struct s5k_mode *m = s->mode;
	u32 vblank = m->fll - m->height;

	__v4l2_ctrl_modify_range(s->pixel_rate, m->pixel_rate, m->pixel_rate, 1, m->pixel_rate);
	__v4l2_ctrl_s_ctrl(s->link_freq, m->link_freq);
	__v4l2_ctrl_modify_range(s->hblank, m->llp - m->width, m->llp - m->width, 1,
				 m->llp - m->width);
	__v4l2_ctrl_modify_range(s->vblank, vblank, S5K_FLL_MAX - m->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(s->vblank, vblank);
	s5k3p9_update_exposure_range(s);
}

static int rot_back = 90, rot_front = 270;
module_param(rot_back, int, 0444);
module_param(rot_front, int, 0444);

static int s5k3p9_init_ctrls(struct s5k3p9 *s)
{
	struct v4l2_ctrl_handler *h = &s->ctrls;
	const struct s5k_mode *m = s->mode;
	struct v4l2_fwnode_device_properties props;
	u32 fll_max_cit = m->fll - s->data->cit_margin;
	int ret;

	v4l2_ctrl_handler_init(h, 12);

	s->pixel_rate = v4l2_ctrl_new_std(h, &s5k3p9_ctrl_ops, V4L2_CID_PIXEL_RATE,
					  m->pixel_rate, m->pixel_rate, 1, m->pixel_rate);
	s->link_freq = v4l2_ctrl_new_int_menu(h, &s5k3p9_ctrl_ops, V4L2_CID_LINK_FREQ,
					      s->data->n_link_freqs - 1, m->link_freq,
					      s->data->link_freqs);
	if (s->link_freq)
		s->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	s->hblank = v4l2_ctrl_new_std(h, &s5k3p9_ctrl_ops, V4L2_CID_HBLANK,
				      m->llp - m->width, m->llp - m->width, 1, m->llp - m->width);
	if (s->hblank)
		s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	s->vblank = v4l2_ctrl_new_std(h, &s5k3p9_ctrl_ops, V4L2_CID_VBLANK,
				      m->fll - m->height, S5K_FLL_MAX - m->height, 1,
				      m->fll - m->height);
	s->exposure = v4l2_ctrl_new_std(h, &s5k3p9_ctrl_ops, V4L2_CID_EXPOSURE,
					s->data->cit_min, fll_max_cit, 1, fll_max_cit);
	v4l2_ctrl_new_std(h, &s5k3p9_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5K_AGAIN_MIN, S5K_AGAIN_MAX, 1, S5K_AGAIN_MIN);
	v4l2_ctrl_new_std_menu_items(h, &s5k3p9_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k3p9_test_patterns) - 1, 0, 0,
				     s5k3p9_test_patterns);

	ret = v4l2_fwnode_device_parse(&s->client->dev, &props);
	/* TEMPORARY until the DT carries it: mount rotation by model, for testing without a U-Boot flash */
	if (!ret && props.rotation == V4L2_FWNODE_PROPERTY_UNSET) {
		int r = !strcmp(s->data->model, "s5k3j1") ? rot_front : rot_back;
		if (r >= 0)
			props.rotation = r;
	}
	if (!ret)
		v4l2_ctrl_new_fwnode_properties(h, &s5k3p9_ctrl_ops, &props);

	if (h->error) {
		ret = h->error;
		v4l2_ctrl_handler_free(h);
		return ret;
	}
	s->sd.ctrl_handler = h;
	return 0;
}

static void s5k3p9_fill_fmt(const struct s5k3p9 *s, const struct s5k_mode *m,
			    struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = m->width;
	fmt->height = m->height;
	fmt->code = s->data->code;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5k3p9_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = to_s5k(sd)->data->code;
	return 0;
}

static int s5k3p9_enum_frame_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct s5k3p9 *s = to_s5k(sd);

	if (fse->code != s->data->code || fse->index >= s->data->n_modes)
		return -EINVAL;
	fse->min_width = fse->max_width = s->data->modes[fse->index].width;
	fse->min_height = fse->max_height = s->data->modes[fse->index].height;
	return 0;
}

static int s5k3p9_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct s5k3p9 *s = to_s5k(sd);
	const struct s5k_mode *m;

	m = v4l2_find_nearest_size(s->data->modes, s->data->n_modes, width, height,
				   fmt->format.width, fmt->format.height);
	s5k3p9_fill_fmt(s, m, &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		if (s->streaming)
			return -EBUSY;
		s->mode = m;
		s5k3p9_update_mode_ctrls(s);
	}
	return 0;
}

static int s5k3p9_get_selection(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	const struct s5k_mode *m = &to_s5k(sd)->data->modes[0];
	const struct v4l2_mbus_framefmt *fmt = v4l2_subdev_state_get_format(state, 0);
	int i, w = 0, h = 0;

	/* the largest mode is the full active array */
	for (i = 0; i < to_s5k(sd)->data->n_modes; i++, m++) {
		w = max_t(int, w, m->width);
		h = max_t(int, h, m->height);
	}

	sel->r.left = 0;
	sel->r.top = 0;
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		/* binned modes read the whole array too */
		sel->r.width = fmt->width * (w / fmt->width);
		sel->r.height = fmt->height * (h / fmt->height);
		sel->r.left = (w - sel->r.width) / 2;
		sel->r.top = (h - sel->r.height) / 2;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.width = w;
		sel->r.height = h;
		return 0;
	}
	return -EINVAL;
}

static int s5k3p9_enable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct s5k3p9 *s = to_s5k(sd);
	const struct s5k_mode *m = s->mode;
	int ret;

	ret = s5k3p9_power_on(s);
	if (ret)
		return ret;
	/* the stock driver waits 3 ms before each table */
	usleep_range(3000, 3100);
	if (s->data->tnp)
		ret = s5k3p9_write_table(s, s->data->tnp, s->data->n_tnp);
	if (!ret)
		ret = s5k3p9_write_table(s, s->data->global, s->data->n_global);
	usleep_range(3000, 3100);
	if (!ret)
		ret = s5k3p9_write_table(s, m->regs, m->n);
	usleep_range(3000, 3100);
	if (!ret && m->post)
		ret = s5k3p9_write_table(s, m->post, m->n_post);
	if (!ret && s->data->post_mode)
		ret = s5k3p9_write_table(s, s->data->post_mode, s->data->n_post_mode);
	if (!ret)
		ret = s5k3p9_write16(s, S5K3P9_REG_PAGE, 0x4000);
	if (ret)
		goto err;

	s->streaming = true;
	ret = __v4l2_ctrl_handler_setup(&s->ctrls);
	if (ret)
		goto err;

	if (s->data->dual_sync_master) {
		msleep(8);
		ret = s5k3p9_write16(s, 0x0a70, 0x0001);
		if (!ret)
			ret = s5k3p9_write16(s, 0x0a72, 0x0100);
	}
	if (!ret)
		ret = s5k3p9_write16(s, S5K3P9_REG_STREAM, s->data->stream_on);
	if (ret)
		goto err;
	return 0;

err:
	s->streaming = false;
	s5k3p9_power_off(s);
	return ret;
}

static int s5k3p9_disable_streams(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct s5k3p9 *s = to_s5k(sd);

	s5k3p9_write16(s, S5K3P9_REG_PAGE, 0x4000);
	s5k3p9_write16(s, S5K3P9_REG_STREAM, 0x0000);
	msleep(20);
	s->streaming = false;
	s5k3p9_power_off(s);
	return 0;
}

static int s5k3p9_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	struct s5k3p9 *s = to_s5k(sd);

	s5k3p9_fill_fmt(s, &s->data->modes[0], v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops s5k3p9_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5k3p9_pad_ops = {
	.enum_mbus_code = s5k3p9_enum_mbus_code,
	.enum_frame_size = s5k3p9_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5k3p9_set_fmt,
	.get_selection = s5k3p9_get_selection,
	.enable_streams = s5k3p9_enable_streams,
	.disable_streams = s5k3p9_disable_streams,
};

static const struct v4l2_subdev_ops s5k3p9_subdev_ops = {
	.video = &s5k3p9_video_ops,
	.pad = &s5k3p9_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k3p9_internal_ops = {
	.init_state = s5k3p9_init_state,
};

/* bring-up: a sensor that does not answer keeps its power this long, to measure the rails */
static uint probe_hold_ms;
module_param(probe_hold_ms, uint, 0644);

static int s5k3p9_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5k3p9 *s;
	u16 model = 0, rev = 0;
	int i, ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->client = client;
	s->data = of_device_get_match_data(dev);
	s->mode = &s->data->modes[0];

	s->mclk = devm_clk_get(dev, NULL);
	if (IS_ERR(s->mclk))
		return dev_err_probe(dev, PTR_ERR(s->mclk), "no mclk\n");
	ret = clk_set_rate(s->mclk, S5K3P9_MCLK_HZ);
	if (ret)
		return dev_err_probe(dev, ret, "mclk rate\n");

	/* asserted = held in reset */
	s->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset))
		return dev_err_probe(dev, PTR_ERR(s->reset), "no reset gpio\n");

	for (i = 0; i < s->data->n_supplies; i++)
		s->supplies[i].supply = s->data->supplies[i];
	ret = devm_regulator_bulk_get(dev, s->data->n_supplies, s->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "supplies\n");

	ret = s5k3p9_power_on(s);
	if (ret)
		return dev_err_probe(dev, ret, "power on\n");
	ret = s5k3p9_read16(s, S5K3P9_REG_MODEL_ID, &model);
	if (!ret)
		ret = s5k3p9_read16(s, S5K3P9_REG_REVISION, &rev);
	if (ret && probe_hold_ms) {
		dev_info(dev, "no answer: holding power %u ms\n", probe_hold_ms);
		msleep(probe_hold_ms);
	}
	s5k3p9_power_off(s);
	if (ret)
		return dev_err_probe(dev, ret, "no answer at %#x\n", client->addr);
	dev_info(dev, "model %#06x revision %#06x, mclk %lu Hz\n", model, rev,
		 clk_get_rate(s->mclk));

	v4l2_i2c_subdev_init(&s->sd, client, &s5k3p9_subdev_ops);
	v4l2_i2c_subdev_set_name(&s->sd, client, s->data->model, NULL);
	s->sd.internal_ops = &s5k3p9_internal_ops;
	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = s5k3p9_init_ctrls(s);
	if (ret)
		return dev_err_probe(dev, ret, "controls\n");

	s->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		goto err_ctrls;

	s->sd.state_lock = s->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&s->sd);
	if (ret)
		goto err_entity;

	ret = v4l2_async_register_subdev_sensor(&s->sd);
	if (ret)
		goto err_state;
	return 0;

err_state:
	v4l2_subdev_cleanup(&s->sd);
err_entity:
	media_entity_cleanup(&s->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&s->ctrls);
	return ret;
}

static void s5k3p9_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k3p9 *s = to_s5k(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	if (s->streaming)
		s5k3p9_power_off(s);
}

static const struct of_device_id s5k3p9_of_match[] = {
	{ .compatible = "samsung,s5k2l4", .data = &s5k2l4_data },
	{ .compatible = "samsung,s5k3j1", .data = &s5k3j1_data },
	{ .compatible = "samsung,s5k3p9", .data = &s5k3p9_data },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k3p9_of_match);

static struct i2c_driver s5k3p9_driver = {
	.driver = {
		.name = "s5k3p9",
		.of_match_table = s5k3p9_of_match,
	},
	.probe = s5k3p9_probe,
	.remove = s5k3p9_remove,
};
module_i2c_driver(s5k3p9_driver);

MODULE_DESCRIPTION("Samsung S5K3P9 / S5K3J1 / S5K2L4 image sensors");
MODULE_LICENSE("GPL");
