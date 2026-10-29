// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX77705 CCIC: USB Type-C port and USB PD sink
 *
 * The MAX77705 CCIC runs Type-C detection and the USB PD policy engine in
 * its own firmware. The host talks to it through status and interrupt
 * registers and a mailbox: an opcode and its data are written from 0x21,
 * a write of 0x41 starts the command, the APCmdRes interrupt reports the
 * result, and the opcode echo and response are read from 0x51.
 *
 * This driver reports the port to the typec class, switches the USB data
 * role, sources VBUS through the charger's OTG regulator when a sink is
 * attached, and as a sink selects a fixed PDO of at most 9 V and 15 W,
 * sequencing the charger input current limit around the request.
 *
 * The protocol is taken from the Samsung vendor driver; references of the
 * form usbc.c:NNN are to its drivers/usb/typec/maxim/max77705_usbc.c (and
 * pd.c, cc.c, max77705.h next to it).
 */

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/usb/pd.h>
#include <linux/usb/role.h>
#include <linux/usb/typec.h>
#include <linux/workqueue.h>

/* Registers, max77705.h:60-92 */
#define MAX77705_UIC_HW_REV		0x00
#define MAX77705_UIC_FW_REV		0x01
#define MAX77705_UIC_INT		0x02
#define MAX77705_CC_INT			0x03
#define MAX77705_PD_INT			0x04
#define MAX77705_USBC_STATUS2		0x07
#define MAX77705_UIC_FW_MINOR		0x09
#define MAX77705_CC_STATUS0		0x0a
#define MAX77705_PD_STATUS0		0x0c
#define MAX77705_PD_STATUS1		0x0d
#define MAX77705_UIC_INT_M		0x0e
#define MAX77705_CC_INT_M		0x0f
#define MAX77705_PD_INT_M		0x10
#define MAX77705_VDM_INT_M		0x11
#define MAX77705_OPCODE_WRITE		0x21
#define MAX77705_OPCODE_WRITE_END	0x41
#define MAX77705_OPCODE_READ		0x51
#define MAX77705_MAX_REG		0x71

#define MAX77705_OPCODE_DATA_LEN	32

/* UIC_INT, max77705.h:104-110 */
#define MAX77705_UIC_APCMDRES		BIT(7)
#define MAX77705_UIC_SYSMSG		BIT(6)

/* PD_INT, stock max77705-irq.c:102-109 */
#define MAX77705_PD_PDMSG		BIT(7)
#define MAX77705_PD_PSRDY		BIT(6)
#define MAX77705_PD_DATAROLE		BIT(5)

/* CC_STATUS0, max77705.h:154-157 */
#define MAX77705_CCPINSTAT		GENMASK(7, 6)
#define MAX77705_CCISTAT		GENMASK(5, 4)
#define MAX77705_CCVCNSTAT		BIT(3)
#define MAX77705_CCSTAT			GENMASK(2, 0)

/* PD_STATUS1, max77705.h:178-181 */
#define MAX77705_PD_DATAROLE_DFP	BIT(7)
#define MAX77705_PD_PSRDY_STAT		BIT(4)

/*
 * Interrupt masks as stock leaves them after probe: usbc.c:966-969 writes
 * the *_INIT values, then usbc.c:3839-3840 masks the display-port VDM
 * sources in PD_INT and all of VDM_INT.
 */
#define MAX77705_UIC_INT_M_INIT		0x04
#define MAX77705_CC_INT_M_INIT		0x20
#define MAX77705_PD_INT_M_INIT		0x1c
#define MAX77705_VDM_INT_M_INIT		0xff

/* CCStat, max77705.h:365-374 */
enum max77705_ccstat {
	MAX77705_CC_NONE = 0,
	MAX77705_CC_SINK,	/* we are the sink */
	MAX77705_CC_SOURCE,	/* we are the source */
	MAX77705_CC_AUDIO,
	MAX77705_CC_DEBUG,
};

/* CCIStat: the partner's Rp advertisement, max77705.h:300-306 */
enum max77705_ccistat {
	MAX77705_CCI_NONE = 0,
	MAX77705_CCI_500MA,
	MAX77705_CCI_1_5A,
	MAX77705_CCI_3_0A,
};

/* SYSMsg values the driver acts on, max77705.h:376-384 */
#define MAX77705_SYSMSG_BOOT_WDT	0x03
#define MAX77705_SYSMSG_BOOT_POR	0x05

/* PDMsg values the driver acts on, max77705.h:428-460 */
#define MAX77705_PDMSG_SNK_PSRDY		0x01
#define MAX77705_PDMSG_SRC_SENDER_TIMEOUT	0x06
#define MAX77705_PDMSG_REJECT_RECEIVED		0x12
#define MAX77705_PDMSG_NOT_SUPPORTED_RECEIVED	0x13
#define MAX77705_PDMSG_HARDRESET_RECEIVED	0x16
#define MAX77705_PDMSG_VBUS_TURN_OFF		0x18
#define MAX77705_PDMSG_HARDRESET_SENT		0x19
#define MAX77705_PDMSG_PRSWAP_SRCTOSWAP		0x1a
#define MAX77705_PDMSG_PRSWAP_SWAPTOSNK		0x1b
#define MAX77705_PDMSG_PRSWAP_SWAPTOSRC		0x1d

/* Opcodes this driver sends, max77705.h:519-575 */
#define MAX77705_OP_SET_SNKCAP		0x2e
#define MAX77705_OP_CURRENT_SRCCAP	0x30
#define MAX77705_OP_SRCCAP_REQUEST	0x32
#define MAX77705_OP_FACTORY_TEST	0x54
#define MAX77705_OP_FW_AUTOIBUS		0x57
#define MAX77705_OP_CTRL1_W		0x06
#define MAX77705_OP_SET_ALTMODE		0x55

/* MUIC CTRL1: COMN1SW and COMP2SW both 1, D+/D- routed to the USB PHY */
#define MAX77705_MUIC_COM_USB		0x09
/* SET_ALTERNATEMODE: send Source_Capabilities (bit 0) and VDMs (bit 1) */
#define MAX77705_ALTMODE_SRC_VDM	0x03

/* FACTORY_TEST data: firmware-driven VBUS off, usbc.c:251-262 */
#define MAX77705_AUTO_VBUS_OFF		0x00
/* FW_AUTOIBUS data: firmware on, auto input limit off, pd.c:442-444 */
#define MAX77705_AUTOIBUS_AT_OFF	0x01

/* SRCCAP_REQUEST results, pd.c:173-198 */
#define MAX77705_REQ_SINK_TX_NG		0xfe
#define MAX77705_REQ_NOT_READY		0xff

/* CURRENT_SRCCAP response header, pd.c:825-829 */
#define MAX77705_SRCCAP_SEL		GENMASK(5, 3)
#define MAX77705_SRCCAP_NUM		GENMASK(2, 0)
#define MAX77705_MAX_PDO		7

#define MAX77705_CMD_TIMEOUT_MS		3000	/* OPCODE_WAIT_TIMEOUT, max77705.h:507 */
#define MAX77705_CMD_POLL_US		5000

/*
 * Sink policy, ADR-003: stock's battery,max_input_voltage (9 V),
 * battery,max_input_current (3 A) and battery,pd_charging_charge_power
 * (15 W). Fixed PDOs only; never an APDO.
 */
#define MAX77705_PD_MAX_MV		9000
#define MAX77705_PD_MAX_MA		3000
#define MAX77705_PD_MAX_MW		15000
/* power difference below which PDOs count as equal */
#define MAX77705_PD_MW_SLACK		100

#define MAX77705_ILIM_SAFE_UA		500000
#define MAX77705_PD_MAX_REQUESTS	3
#define MAX77705_PD_RETRY_MS		500
#define MAX77705_PD_BACKOFF_MS		60000
#define MAX77705_VBUS_RESTORE_MS	760	/* pd.c:1232 */
/* VBUS enable refused (-EBUSY while the old source's VBUS decays): retry */
#define MAX77705_VBUS_RETRY_MS		100
#define MAX77705_VBUS_RETRIES		10
/* CCIC resets recovered per window before the port is held in a safe state */
#define MAX77705_RECOVERY_BURST		4
#define MAX77705_RECOVERY_WINDOW_MS	60000
/* after a command timeout, how long a late APCmdRes is waited out */
#define MAX77705_CMD_SETTLE_MS		50
/* interrupt events queued between the handler and the work */
#define MAX77705_EV_FIFO_SIZE		16

/*
 * Sink capabilities when the connector node has no sink-pdos: stock's
 * max77705,snkcap_data without its variable and PPS entries. Stock sends
 * 5 V 3 A (dual-role power, higher capability, USB communication,
 * dual-role data), 9 V 2 A fixed, 5-9 V 2 A variable and 3.3-11 V 4 A PPS.
 */
static const u32 max77705_default_snk_pdos[] = {
	PDO_FIXED(5000, 3000, PDO_FIXED_DUAL_ROLE | PDO_FIXED_HIGHER_CAP |
		  PDO_FIXED_USB_COMM | PDO_FIXED_DATA_SWAP),
	PDO_FIXED(9000, 2000, 0),
};

/*
 * One interrupt, as the handler saw it. PDMsg and SYSMsg are reports, not
 * levels: a later message overwrites PD_STATUS0 and USBC_STATUS2, so they
 * are read in the handler, with the interrupt that announced them.
 */
struct max77705_usbc_ev {
	u8 uic;
	u8 pd;
	u8 pdmsg;
	u8 sysmsg;
};

struct max77705_usbc {
	struct device *dev;
	struct regmap *regmap;
	struct workqueue_struct *wq;
	struct work_struct work;
	struct delayed_work pd_retry_work;
	struct delayed_work vbus_work;
	struct delayed_work recover_work;

	/* serializes the mailbox and all port state below */
	struct mutex lock;
	struct completion cmd_done;
	bool irq_ready;

	/* events queued by the handler, consumed in order by the work */
	spinlock_t ev_lock;
	DECLARE_KFIFO(ev_fifo, struct max77705_usbc_ev, MAX77705_EV_FIFO_SIZE);
	bool ev_overflow;	/* an event was dropped: resync from levels */
	bool ev_reset_lost;	/* a dropped event reported a CCIC reset */

	struct typec_port *port;
	struct typec_partner *partner;
	struct typec_partner_desc partner_desc;
	struct usb_pd_identity partner_ident;
	struct usb_role_switch *role_sw;
	struct regulator *vbus;
	struct power_supply *chg_psy;
	struct power_supply *psy;

	u32 snk_pdos[MAX77705_MAX_PDO];
	unsigned int nr_snk_pdos;

	enum max77705_ccstat ccstat;
	u8 cc_status0;
	bool vbus_on;		/* we hold an enable reference on vbus */
	unsigned int vbus_retries;

	/* CCIC reset recovery, rate limited */
	unsigned int recoveries;
	unsigned long recovery_window;
	bool halted;		/* held safe: VBUS off, input limit low */
	u8 masks[4];		/* UIC/CC/PD/VDM_INT_M as read after hw_init */

	/* sink PD state, reset on detach */
	bool pd_contract;
	u32 src_pdos[MAX77705_MAX_PDO];
	unsigned int nr_src_pdos;
	unsigned int sel_pos;
	unsigned int req_pos;
	unsigned int nr_requests;
	unsigned long pd_backoff_until;

	int ilim_base_ua;
	int voltage_uv;
	int current_ua;
};

static bool max77705_usbc_writeable_reg(struct device *dev, unsigned int reg)
{
	/* The masks and the mailbox window; never the CCIC reset at 0x80. */
	return (reg >= MAX77705_UIC_INT_M && reg <= MAX77705_VDM_INT_M) ||
	       (reg >= MAX77705_OPCODE_WRITE && reg <= MAX77705_OPCODE_WRITE_END);
}

static const struct regmap_config max77705_usbc_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_MAX_REG,
	.writeable_reg = max77705_usbc_writeable_reg,
};

static bool max77705_pdo_acceptable(u32 pdo)
{
	return pdo_type(pdo) == PDO_TYPE_FIXED &&
	       pdo_fixed_voltage(pdo) >= 5000 &&
	       pdo_fixed_voltage(pdo) <= MAX77705_PD_MAX_MV;
}

/*
 * The opcodes the driver may send, with their payloads checked. Anything
 * else is refused before it reaches the chip: the firmware-update entry
 * (0xd0 at 0x21), the RAM test (0xd1), APDO/PPS requests (0x3a, 0x3c), the
 * PPS status read (0x3b) and the ADC self-test (0x59). ADR-003, invariants.
 */
static bool max77705_usbc_cmd_allowed(struct max77705_usbc *u, u8 op,
				      const u8 *data, size_t len)
{
	unsigned int i, n;

	switch (op) {
	case MAX77705_OP_CURRENT_SRCCAP:
		return len == 0;
	case MAX77705_OP_FACTORY_TEST:
		return len == 1 && data[0] == MAX77705_AUTO_VBUS_OFF;
	case MAX77705_OP_FW_AUTOIBUS:
		return len == 1 && data[0] == MAX77705_AUTOIBUS_AT_OFF;
	case MAX77705_OP_CTRL1_W:
		return len == 1 && data[0] == MAX77705_MUIC_COM_USB;
	case MAX77705_OP_SET_ALTMODE:
		return len == 1 && data[0] == MAX77705_ALTMODE_SRC_VDM;
	case MAX77705_OP_SET_SNKCAP:
		if (len < 1)
			return false;
		n = data[0];
		if (n < 1 || n > MAX77705_MAX_PDO || len != 1 + 4 * n)
			return false;
		for (i = 0; i < n; i++)
			if (!max77705_pdo_acceptable(get_unaligned_le32(&data[1 + 4 * i])))
				return false;
		return true;
	case MAX77705_OP_SRCCAP_REQUEST:
		if (len != 1 || data[0] < 1 || data[0] > u->nr_src_pdos)
			return false;
		return max77705_pdo_acceptable(u->src_pdos[data[0] - 1]);
	default:
		return false;
	}
}

static int max77705_usbc_wait_cmd(struct max77705_usbc *u)
{
	unsigned int val;
	int ret;

	if (u->irq_ready) {
		unsigned long timeout = msecs_to_jiffies(MAX77705_CMD_TIMEOUT_MS);

		if (!wait_for_completion_timeout(&u->cmd_done, timeout))
			return -ETIMEDOUT;
		return 0;
	}

	/*
	 * Before the interrupt is requested (probe), poll UIC_INT. The read
	 * clears it; the other bits are dropped. The only one the work acts
	 * on is SYSMsg, and a BOOT_POR reported now is the reset hw_init is
	 * already answering; CC and PD state are synced from levels after.
	 */
	ret = regmap_read_poll_timeout(u->regmap, MAX77705_UIC_INT, val,
				       val & MAX77705_UIC_APCMDRES,
				       MAX77705_CMD_POLL_US,
				       MAX77705_CMD_TIMEOUT_MS * USEC_PER_MSEC);
	return ret;
}

/*
 * Run one mailbox command (usbc.c:1883-1960): write [opcode, data] from
 * 0x21, start it by writing 0x41 unless the data filled the window, wait
 * for APCmdRes, then read the opcode echo at 0x51 and the response after it.
 */
static int max77705_usbc_cmd(struct max77705_usbc *u, u8 op,
			     const u8 *wdata, size_t wlen,
			     u8 *rdata, size_t rlen)
{
	u8 buf[1 + MAX77705_OPCODE_DATA_LEN];
	int ret;

	lockdep_assert_held(&u->lock);

	if (wlen > MAX77705_OPCODE_DATA_LEN || rlen > MAX77705_OPCODE_DATA_LEN)
		return -EMSGSIZE;

	if (!max77705_usbc_cmd_allowed(u, op, wdata, wlen)) {
		dev_err(u->dev, "refusing opcode 0x%02x\n", op);
		return -EPERM;
	}

	buf[0] = op;
	if (wlen)
		memcpy(&buf[1], wdata, wlen);

	reinit_completion(&u->cmd_done);

	ret = regmap_bulk_write(u->regmap, MAX77705_OPCODE_WRITE, buf, wlen + 1);
	if (ret)
		return ret;

	if (wlen < MAX77705_OPCODE_DATA_LEN) {
		ret = regmap_write(u->regmap, MAX77705_OPCODE_WRITE_END, 0);
		if (ret)
			return ret;
	}

	ret = max77705_usbc_wait_cmd(u);
	if (ret) {
		dev_warn(u->dev, "opcode 0x%02x: no response (%d)\n", op, ret);
		/*
		 * A late APCmdRes for this command would complete the next
		 * command's wait, and a repeated opcode (CTRL1_W) passes the
		 * echo check. Wait it out and discard it while the mailbox is
		 * still ours.
		 */
		if (u->irq_ready) {
			msleep(MAX77705_CMD_SETTLE_MS);
			reinit_completion(&u->cmd_done);
		}
		return ret;
	}

	ret = regmap_bulk_read(u->regmap, MAX77705_OPCODE_READ, buf, rlen + 1);
	if (ret)
		return ret;

	if (buf[0] != op) {
		dev_warn(u->dev, "opcode 0x%02x: echo 0x%02x\n", op, buf[0]);
		return -EIO;
	}

	if (rlen)
		memcpy(rdata, &buf[1], rlen);

	return 0;
}

static int max77705_usbc_cmd_byte(struct max77705_usbc *u, u8 op, u8 data)
{
	return max77705_usbc_cmd(u, op, &data, 1, NULL, 0);
}

/* Charger input current limit */

static int max77705_usbc_get_ilim(struct max77705_usbc *u, int *ua)
{
	union power_supply_propval val;
	int ret;

	ret = power_supply_get_property(u->chg_psy,
					POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &val);
	if (!ret)
		*ua = val.intval;
	return ret;
}

/*
 * Set the limit and read it back. The charger quantizes; if it rounded
 * above the target, step down until it does not, so a power cap is never
 * exceeded by the rounding.
 */
static int max77705_usbc_set_ilim(struct max77705_usbc *u, int ua)
{
	enum power_supply_property psp = POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;
	union power_supply_propval val = { .intval = ua };
	int ret, i, now = 0;

	if (!u->chg_psy)
		return -ENODEV;

	for (i = 0; i < 3; i++) {
		ret = power_supply_set_property(u->chg_psy, psp, &val);
		if (ret)
			return ret;

		ret = max77705_usbc_get_ilim(u, &now);
		if (ret)
			return ret;

		if (now <= ua)
			return 0;

		val.intval -= now - ua;
	}

	dev_warn(u->dev, "input limit %d uA above target %d uA\n", now, ua);
	return -ERANGE;
}

/* The input current limit a fixed PDO allows under the sink policy. */
static int max77705_pdo_ilim_ua(u32 pdo)
{
	unsigned int mv = pdo_fixed_voltage(pdo);
	unsigned int ma = min(pdo_max_current(pdo), MAX77705_PD_MAX_MA);

	ma = min(ma, MAX77705_PD_MAX_MW * 1000 / mv);
	return ma * 1000;
}

static void max77705_usbc_restore_ilim(struct max77705_usbc *u)
{
	if (u->chg_psy && u->ilim_base_ua > 0)
		max77705_usbc_set_ilim(u, u->ilim_base_ua);
}

/* VBUS */

/*
 * vbus_on tracks the enable reference this driver holds on the OTG
 * regulator. The regulator core keeps the reference when a disable fails,
 * so vbus_on stays true then and the next call to turn VBUS off retries.
 */
static int max77705_usbc_vbus_set(struct max77705_usbc *u, bool on)
{
	int ret;

	if (!u->vbus || u->vbus_on == on)
		return 0;

	if (on) {
		if (u->halted)
			return -EPERM;

		/* VBUS is sourced only while the port is the source. */
		if (WARN_ON_ONCE(u->ccstat != MAX77705_CC_SOURCE))
			return -EINVAL;

		ret = regulator_enable(u->vbus);
		if (ret) {
			/*
			 * -EBUSY: the charger sees CHGIN, so something is
			 * powering the bus (the partner, or its VBUS still
			 * decaying after a swap). The caller may retry.
			 */
			dev_warn(u->dev, "VBUS not sourced (%d)\n", ret);
			return ret;
		}
	} else {
		ret = regulator_disable(u->vbus);
		if (ret) {
			dev_err(u->dev, "failed to stop VBUS (%d), will retry\n", ret);
			return ret;
		}
	}

	u->vbus_on = on;
	dev_dbg(u->dev, "VBUS %s\n", on ? "on" : "off");
	return 0;
}

static void max77705_usbc_vbus_off(struct max77705_usbc *u)
{
	cancel_delayed_work(&u->vbus_work);
	max77705_usbc_vbus_set(u, false);
}

/*
 * Source VBUS now if the port is the source; if the charger refuses (a
 * partner's VBUS still decaying), retry from vbus_work a bounded number
 * of times. vbus_work re-checks CCStat on each try.
 */
static void max77705_usbc_vbus_source(struct max77705_usbc *u,
				      unsigned int delay_ms)
{
	u->vbus_retries = 0;

	if (!delay_ms && u->ccstat == MAX77705_CC_SOURCE &&
	    !max77705_usbc_vbus_set(u, true))
		return;

	queue_delayed_work(u->wq, &u->vbus_work,
			   msecs_to_jiffies(delay_ms ?: MAX77705_VBUS_RETRY_MS));
}

static void max77705_usbc_vbus_work(struct work_struct *work)
{
	struct max77705_usbc *u = container_of(to_delayed_work(work),
					       struct max77705_usbc, vbus_work);

	mutex_lock(&u->lock);
	if (u->ccstat == MAX77705_CC_SOURCE && !u->halted &&
	    max77705_usbc_vbus_set(u, true)) {
		if (++u->vbus_retries < MAX77705_VBUS_RETRIES)
			queue_delayed_work(u->wq, &u->vbus_work,
					   msecs_to_jiffies(MAX77705_VBUS_RETRY_MS));
		else
			dev_warn(u->dev, "VBUS not sourced after %u tries\n",
				 u->vbus_retries);
	}
	mutex_unlock(&u->lock);
}

/* Type-C */

static void max77705_usbc_set_data_role(struct max77705_usbc *u,
					enum typec_data_role role)
{
	/*
	 * The firmware leaves the MUIC switch open across an attach; D+/D-
	 * reach the PHY only with COM_USB selected (stock's MUIC driver does
	 * the same on USB attach).
	 */
	if (max77705_usbc_cmd_byte(u, MAX77705_OP_CTRL1_W, MAX77705_MUIC_COM_USB))
		dev_warn(u->dev, "cannot route D+/D- to USB\n");

	usb_role_switch_set_role(u->role_sw, role == TYPEC_HOST ?
				 USB_ROLE_HOST : USB_ROLE_DEVICE);
	typec_set_data_role(u->port, role);
}

static void max77705_usbc_register_partner(struct max77705_usbc *u,
					   enum typec_accessory accessory)
{
	struct typec_partner *partner;

	if (u->partner) {
		typec_unregister_partner(u->partner);
		u->partner = NULL;
	}

	memset(&u->partner_ident, 0, sizeof(u->partner_ident));
	u->partner_desc.identity = &u->partner_ident;
	u->partner_desc.usb_pd = u->pd_contract;
	u->partner_desc.accessory = accessory;

	partner = typec_register_partner(u->port, &u->partner_desc);
	if (IS_ERR(partner)) {
		dev_err(u->dev, "failed to register partner (%pe)\n", partner);
		return;
	}
	u->partner = partner;
}

static enum typec_pwr_opmode max77705_usbc_opmode(u8 cc_status0)
{
	switch (FIELD_GET(MAX77705_CCISTAT, cc_status0)) {
	case MAX77705_CCI_3_0A:
		return TYPEC_PWR_MODE_3_0A;
	case MAX77705_CCI_1_5A:
		return TYPEC_PWR_MODE_1_5A;
	default:
		return TYPEC_PWR_MODE_USB;
	}
}

static void max77705_usbc_set_orientation(struct max77705_usbc *u,
					  u8 cc_status0)
{
	enum typec_orientation orientation;

	switch (FIELD_GET(MAX77705_CCPINSTAT, cc_status0)) {
	case 1:
		orientation = TYPEC_ORIENTATION_NORMAL;
		break;
	case 2:
		orientation = TYPEC_ORIENTATION_REVERSE;
		break;
	default:
		orientation = TYPEC_ORIENTATION_NONE;
		break;
	}

	typec_set_orientation(u->port, orientation);
}

static void max77705_usbc_pd_reset(struct max77705_usbc *u)
{
	cancel_delayed_work(&u->pd_retry_work);
	u->pd_contract = false;
	u->nr_src_pdos = 0;
	u->sel_pos = 0;
	u->req_pos = 0;
	u->voltage_uv = 0;
	u->current_ua = 0;
}

static void max77705_usbc_detach(struct max77705_usbc *u)
{
	max77705_usbc_vbus_off(u);

	/* Nothing else enables the OTG regulator; it must be off now. */
	if (u->vbus && !u->vbus_on && regulator_is_enabled(u->vbus) > 0)
		dev_err(u->dev, "VBUS still on after detach\n");

	if (u->partner) {
		typec_unregister_partner(u->partner);
		u->partner = NULL;
	}

	usb_role_switch_set_role(u->role_sw, USB_ROLE_NONE);
	typec_set_data_role(u->port, TYPEC_DEVICE);
	typec_set_pwr_role(u->port, TYPEC_SINK);
	typec_set_vconn_role(u->port, TYPEC_SINK);
	typec_set_pwr_opmode(u->port, TYPEC_PWR_MODE_USB);
	typec_set_orientation(u->port, TYPEC_ORIENTATION_NONE);

	if (u->ccstat == MAX77705_CC_SINK)
		max77705_usbc_restore_ilim(u);

	max77705_usbc_pd_reset(u);
	u->nr_requests = 0;
	u->ccstat = MAX77705_CC_NONE;
	power_supply_changed(u->psy);
}

/*
 * Follow CC_STATUS0 (cc.c:640-730). Attach, detach and a change of power
 * role (a PR swap completing) are all taken from CCStat.
 */
static void max77705_usbc_update_cc(struct max77705_usbc *u, u8 cc_status0)
{
	enum max77705_ccstat ccstat = FIELD_GET(MAX77705_CCSTAT, cc_status0);
	enum max77705_ccstat old = u->ccstat;

	if (ccstat > MAX77705_CC_DEBUG)	/* error, disabled */
		ccstat = MAX77705_CC_NONE;

	u->cc_status0 = cc_status0;

	/*
	 * VBUS only while we are the source, checked on every update, not
	 * just on a change of CCStat.
	 */
	if (ccstat != MAX77705_CC_SOURCE)
		max77705_usbc_vbus_off(u);

	if (ccstat == MAX77705_CC_NONE) {
		if (old != MAX77705_CC_NONE) {
			dev_dbg(u->dev, "detached\n");
			max77705_usbc_detach(u);
		}
		return;
	}

	max77705_usbc_set_orientation(u, cc_status0);
	typec_set_vconn_role(u->port, (cc_status0 & MAX77705_CCVCNSTAT) ?
			     TYPEC_SOURCE : TYPEC_SINK);

	if (ccstat == old) {
		if (ccstat == MAX77705_CC_SINK && !u->pd_contract)
			typec_set_pwr_opmode(u->port, max77705_usbc_opmode(cc_status0));
		return;
	}

	dev_dbg(u->dev, "ccstat %d -> %d\n", old, ccstat);
	u->ccstat = ccstat;

	switch (ccstat) {
	case MAX77705_CC_SINK:
		typec_set_pwr_role(u->port, TYPEC_SINK);
		typec_set_pwr_opmode(u->port, max77705_usbc_opmode(cc_status0));
		if (old == MAX77705_CC_NONE) {
			max77705_usbc_set_data_role(u, TYPEC_DEVICE);
			max77705_usbc_register_partner(u, TYPEC_ACCESSORY_NONE);
		}
		u->voltage_uv = 5000000;
		break;
	case MAX77705_CC_SOURCE:
		typec_set_pwr_role(u->port, TYPEC_SOURCE);
		typec_set_pwr_opmode(u->port, TYPEC_PWR_MODE_USB);
		if (old == MAX77705_CC_NONE) {
			max77705_usbc_set_data_role(u, TYPEC_HOST);
			max77705_usbc_register_partner(u, TYPEC_ACCESSORY_NONE);
		} else {
			max77705_usbc_restore_ilim(u);
			max77705_usbc_pd_reset(u);
		}
		max77705_usbc_vbus_source(u, 0);
		break;
	case MAX77705_CC_AUDIO:
	case MAX77705_CC_DEBUG:
		max77705_usbc_register_partner(u, ccstat == MAX77705_CC_AUDIO ?
					       TYPEC_ACCESSORY_AUDIO :
					       TYPEC_ACCESSORY_DEBUG);
		break;
	default:
		break;
	}

	power_supply_changed(u->psy);
}

static void max77705_usbc_update_data_role(struct max77705_usbc *u)
{
	unsigned int pd_status1;

	if (u->ccstat != MAX77705_CC_SINK && u->ccstat != MAX77705_CC_SOURCE)
		return;

	if (regmap_read(u->regmap, MAX77705_PD_STATUS1, &pd_status1))
		return;

	max77705_usbc_set_data_role(u, (pd_status1 & MAX77705_PD_DATAROLE_DFP) ?
				    TYPEC_HOST : TYPEC_DEVICE);
}

/* Sink PD */

static int max77705_usbc_read_srccaps(struct max77705_usbc *u)
{
	u8 resp[1 + 4 * MAX77705_MAX_PDO];
	unsigned int i, n;
	int ret;

	ret = max77705_usbc_cmd(u, MAX77705_OP_CURRENT_SRCCAP, NULL, 0,
				resp, sizeof(resp));
	if (ret)
		return ret;

	n = min_t(unsigned int, FIELD_GET(MAX77705_SRCCAP_NUM, resp[0]),
		  MAX77705_MAX_PDO);
	for (i = 0; i < n; i++)
		u->src_pdos[i] = get_unaligned_le32(&resp[1 + 4 * i]);
	u->nr_src_pdos = n;
	u->sel_pos = FIELD_GET(MAX77705_SRCCAP_SEL, resp[0]);

	if (u->sel_pos > n)
		u->sel_pos = 0;

	return 0;
}

/* The fixed PDO giving the most power under the policy; ties go higher. */
static unsigned int max77705_usbc_best_pos(struct max77705_usbc *u)
{
	unsigned int i, best = 0, best_mw = 0, best_mv = 0;

	for (i = 0; i < u->nr_src_pdos; i++) {
		u32 pdo = u->src_pdos[i];
		unsigned int mw, mv;

		if (!max77705_pdo_acceptable(pdo))
			continue;

		mv = pdo_fixed_voltage(pdo);
		mw = mv * (max77705_pdo_ilim_ua(pdo) / 1000) / 1000;
		/*
		 * Under the power cap 5 V 3 A and 9 V 1.66 A are the same
		 * 15 W; prefer the higher voltage (less current in the cable),
		 * as stock does, instead of losing it to rounding.
		 */
		if (mw > best_mw + MAX77705_PD_MW_SLACK ||
		    (mw + MAX77705_PD_MW_SLACK >= best_mw && mv > best_mv)) {
			best_mw = max(mw, best_mw);
			best_mv = mv;
			best = i + 1;
		}
	}

	return best;
}

static void max77705_usbc_pd_request(struct max77705_usbc *u)
{
	unsigned int best, cur_mv;
	u32 cur, next;
	u8 pos, result;
	int ret;

	if (!u->pd_contract || !u->sel_pos || !u->chg_psy)
		return;

	best = max77705_usbc_best_pos(u);
	if (!best || best == u->sel_pos)
		return;

	if (u->nr_requests >= MAX77705_PD_MAX_REQUESTS ||
	    time_before(jiffies, u->pd_backoff_until))
		return;

	cur = u->src_pdos[u->sel_pos - 1];
	next = u->src_pdos[best - 1];
	cur_mv = pdo_type(cur) == PDO_TYPE_FIXED ? pdo_fixed_voltage(cur) : 5000;

	/*
	 * Lower the input limit to what both contracts allow before asking
	 * for more voltage; it is raised again once PSRDY confirms the new
	 * contract.
	 */
	if (pdo_fixed_voltage(next) > cur_mv) {
		int ua = max77705_pdo_ilim_ua(next);

		if (pdo_type(cur) == PDO_TYPE_FIXED)
			ua = min(ua, max77705_pdo_ilim_ua(cur));

		ret = max77705_usbc_set_ilim(u, ua);
		if (ret) {
			dev_warn(u->dev, "cannot set input limit (%d), staying at %u mV\n",
				 ret, cur_mv);
			return;
		}
	}

	u->nr_requests++;
	pos = best;
	ret = max77705_usbc_cmd(u, MAX77705_OP_SRCCAP_REQUEST, &pos, 1,
				&result, 1);
	if (!ret && !result) {
		u->req_pos = best;
		dev_info(u->dev, "requested PDO %u: %u mV %u mA\n", best,
			 pdo_fixed_voltage(next), pdo_max_current(next));
		return;
	}

	/* Not sent: the current contract stands. */
	if (pdo_type(cur) == PDO_TYPE_FIXED)
		max77705_usbc_set_ilim(u, max77705_pdo_ilim_ua(cur));

	if (!ret && (result == MAX77705_REQ_SINK_TX_NG ||
		     result == MAX77705_REQ_NOT_READY)) {
		dev_dbg(u->dev, "request not sent (0x%02x), retrying\n", result);
		queue_delayed_work(u->wq, &u->pd_retry_work,
				   msecs_to_jiffies(MAX77705_PD_RETRY_MS));
	} else {
		dev_warn(u->dev, "request for PDO %u failed (%d, 0x%02x)\n",
			 best, ret, ret ? 0 : result);
	}
}

/*
 * PSRDY as sink (pd.c:1492-1550): read the contract from CURRENT_SRCCAP,
 * set the input limit it allows, then ask for a better PDO if there is one.
 * Stock triggers this from both the PSRDY interrupt and the PDMsg report,
 * which race; this is idempotent, so either may run it.
 */
static void max77705_usbc_pd_ready(struct max77705_usbc *u)
{
	unsigned int pd_status1;
	u32 pdo;
	int ret;

	if (u->ccstat != MAX77705_CC_SINK)
		return;

	if (regmap_read(u->regmap, MAX77705_PD_STATUS1, &pd_status1) ||
	    !(pd_status1 & MAX77705_PD_PSRDY_STAT))
		return;

	ret = max77705_usbc_read_srccaps(u);
	if (ret || !u->sel_pos)
		return;

	pdo = u->src_pdos[u->sel_pos - 1];

	if (!u->pd_contract) {
		u->pd_contract = true;
		typec_set_pwr_opmode(u->port, TYPEC_PWR_MODE_PD);
		max77705_usbc_register_partner(u, TYPEC_ACCESSORY_NONE);
	}

	if (pdo_type(pdo) == PDO_TYPE_FIXED) {
		u->voltage_uv = pdo_fixed_voltage(pdo) * 1000;
		u->current_ua = pdo_max_current(pdo) * 1000;
		if (u->chg_psy)
			max77705_usbc_set_ilim(u, max77705_pdo_ilim_ua(pdo));
	} else {
		/* Never one we ask for; hold the charger low until we move off it. */
		dev_warn(u->dev, "contract on non-fixed PDO %u\n", u->sel_pos);
		u->voltage_uv = 0;
		u->current_ua = 0;
		if (u->chg_psy)
			max77705_usbc_set_ilim(u, MAX77705_ILIM_SAFE_UA);
	}

	if (u->req_pos && u->req_pos == u->sel_pos)
		dev_info(u->dev, "contract: %d mV %d mA\n",
			 u->voltage_uv / 1000, u->current_ua / 1000);
	u->req_pos = 0;

	power_supply_changed(u->psy);

	max77705_usbc_pd_request(u);
}

static void max77705_usbc_pd_retry_work(struct work_struct *work)
{
	struct max77705_usbc *u = container_of(to_delayed_work(work),
					       struct max77705_usbc, pd_retry_work);

	mutex_lock(&u->lock);
	max77705_usbc_pd_request(u);
	mutex_unlock(&u->lock);
}

static void max77705_usbc_hard_reset(struct max77705_usbc *u)
{
	if (u->ccstat == MAX77705_CC_SOURCE) {
		/* Drop VBUS and bring it back, as pd.c:1221-1270 does. */
		max77705_usbc_vbus_off(u);
		max77705_usbc_vbus_source(u, MAX77705_VBUS_RESTORE_MS);
		return;
	}

	/* Not the source: whatever happened before, VBUS is not ours. */
	max77705_usbc_vbus_off(u);

	if (u->ccstat != MAX77705_CC_SINK)
		return;

	/*
	 * A hard reset right after our request means the source did not
	 * like it; do not ask again for a while, even across a re-attach.
	 */
	if (u->req_pos) {
		dev_warn(u->dev, "hard reset after requesting PDO %u\n", u->req_pos);
		u->pd_backoff_until = jiffies + msecs_to_jiffies(MAX77705_PD_BACKOFF_MS);
	}

	max77705_usbc_restore_ilim(u);
	max77705_usbc_pd_reset(u);
	typec_set_pwr_opmode(u->port, max77705_usbc_opmode(u->cc_status0));
	u->voltage_uv = 5000000;
	power_supply_changed(u->psy);
}

/*
 * The source refused our request and the old contract stands: give the
 * charger back the input limit that contract allows.
 */
static void max77705_usbc_pd_rejected(struct max77705_usbc *u)
{
	u32 pdo;

	if (u->ccstat != MAX77705_CC_SINK || !u->req_pos || !u->sel_pos)
		return;

	dev_info(u->dev, "request for PDO %u refused\n", u->req_pos);
	u->req_pos = 0;

	pdo = u->src_pdos[u->sel_pos - 1];
	max77705_usbc_set_ilim(u, pdo_type(pdo) == PDO_TYPE_FIXED ?
			       max77705_pdo_ilim_ua(pdo) : MAX77705_ILIM_SAFE_UA);
}

/* PDMsg reports, pd.c:1144-1300 */
static void max77705_usbc_pdmsg(struct max77705_usbc *u, u8 msg)
{
	dev_dbg(u->dev, "PDMsg 0x%02x\n", msg);

	switch (msg) {
	case MAX77705_PDMSG_REJECT_RECEIVED:
	case MAX77705_PDMSG_NOT_SUPPORTED_RECEIVED:
		max77705_usbc_pd_rejected(u);
		break;
	case MAX77705_PDMSG_HARDRESET_RECEIVED:
	case MAX77705_PDMSG_HARDRESET_SENT:
	case MAX77705_PDMSG_SRC_SENDER_TIMEOUT:
		max77705_usbc_hard_reset(u);
		break;
	case MAX77705_PDMSG_VBUS_TURN_OFF:
	case MAX77705_PDMSG_PRSWAP_SRCTOSWAP:
	case MAX77705_PDMSG_PRSWAP_SWAPTOSNK:
		max77705_usbc_vbus_off(u);
		break;
	case MAX77705_PDMSG_PRSWAP_SWAPTOSRC:
		/*
		 * Only as the source: CC_STATUS0 is read before this report
		 * is handled, so CCStat is current. If the firmware has not
		 * moved to SOURCE yet, or the partner's VBUS has not decayed,
		 * vbus_work retries and re-checks CCStat.
		 */
		max77705_usbc_vbus_source(u, 0);
		break;
	default:
		break;
	}
}

static int max77705_usbc_set_snkcap(struct max77705_usbc *u)
{
	u8 buf[1 + 4 * MAX77705_MAX_PDO];
	unsigned int i;

	buf[0] = u->nr_snk_pdos;
	for (i = 0; i < u->nr_snk_pdos; i++)
		put_unaligned_le32(u->snk_pdos[i], &buf[1 + 4 * i]);

	return max77705_usbc_cmd(u, MAX77705_OP_SET_SNKCAP, buf,
				 1 + 4 * u->nr_snk_pdos, NULL, 0);
}

/*
 * What stock does at probe and again after a CCIC watchdog or power-on
 * reset (usbc.c:2870-2925, 3835-3841): interrupt masks, firmware VBUS
 * control off, firmware input limit off, sink capabilities.
 */
static int max77705_usbc_hw_init(struct max77705_usbc *u)
{
	static const struct reg_sequence masks[] = {
		{ MAX77705_UIC_INT_M, MAX77705_UIC_INT_M_INIT },
		{ MAX77705_CC_INT_M, MAX77705_CC_INT_M_INIT },
		{ MAX77705_PD_INT_M, MAX77705_PD_INT_M_INIT },
		{ MAX77705_VDM_INT_M, MAX77705_VDM_INT_M_INIT },
	};
	int ret;

	ret = regmap_multi_reg_write(u->regmap, masks, ARRAY_SIZE(masks));
	if (ret)
		return ret;

	/* Kept to notice a CCIC reset that no SYSMsg reported. */
	ret = regmap_bulk_read(u->regmap, MAX77705_UIC_INT_M, u->masks,
			       sizeof(u->masks));
	if (ret)
		return ret;

	ret = max77705_usbc_cmd_byte(u, MAX77705_OP_FACTORY_TEST,
				     MAX77705_AUTO_VBUS_OFF);
	if (ret)
		return ret;

	ret = max77705_usbc_cmd_byte(u, MAX77705_OP_FW_AUTOIBUS,
				     MAX77705_AUTOIBUS_AT_OFF);
	if (ret)
		return ret;

	ret = max77705_usbc_set_snkcap(u);
	if (ret)
		return ret;

	/*
	 * Without this the firmware never completes a source attach for an
	 * Rd partner: CC toggles, CCStat stays 0. Stock sends it once
	 * userspace is ready (max77705_alternate.c:1213-1218).
	 */
	return max77705_usbc_cmd_byte(u, MAX77705_OP_SET_ALTMODE,
				      MAX77705_ALTMODE_SRC_VDM);
}

static bool max77705_sysmsg_is_reset(u8 msg)
{
	return msg == MAX77705_SYSMSG_BOOT_WDT || msg == MAX77705_SYSMSG_BOOT_POR;
}

/*
 * The CCIC cannot be trusted to follow policy: VBUS off, the input limit
 * at the safe floor, and the port ignored until a re-initialization
 * succeeds.
 */
static void max77705_usbc_fail_safe(struct max77705_usbc *u, const char *why)
{
	dev_err(u->dev, "%s: holding the port safe (VBUS off, input limit %d uA)\n",
		why, MAX77705_ILIM_SAFE_UA);

	if (u->ccstat != MAX77705_CC_NONE)
		max77705_usbc_detach(u);
	else
		max77705_usbc_vbus_off(u);

	if (u->chg_psy)
		max77705_usbc_set_ilim(u, MAX77705_ILIM_SAFE_UA);

	u->halted = true;

	/* Try again once the recovery window has passed. */
	queue_delayed_work(u->wq, &u->recover_work,
			   msecs_to_jiffies(MAX77705_RECOVERY_WINDOW_MS));
}

/*
 * After a CCIC watchdog or power-on reset, redo hw_init. At most
 * MAX77705_RECOVERY_BURST times per MAX77705_RECOVERY_WINDOW_MS; a CCIC
 * resetting faster than that is held safe until the window has passed.
 */
static void max77705_usbc_recover(struct max77705_usbc *u, const char *what)
{
	unsigned long window = msecs_to_jiffies(MAX77705_RECOVERY_WINDOW_MS);
	int ret;

	if (!u->recoveries || time_after(jiffies, u->recovery_window + window)) {
		u->recovery_window = jiffies;
		u->recoveries = 0;
	}

	if (u->recoveries >= MAX77705_RECOVERY_BURST) {
		if (!u->halted)
			max77705_usbc_fail_safe(u, "CCIC reset too often");
		return;
	}
	u->recoveries++;

	dev_warn(u->dev, "CCIC %s, re-initializing\n", what);

	/* The firmware has forgotten any contract and our settings. */
	if (u->ccstat != MAX77705_CC_NONE)
		max77705_usbc_detach(u);

	ret = max77705_usbc_hw_init(u);
	if (ret) {
		dev_err(u->dev, "re-initialization failed (%d)\n", ret);
		if (!u->halted)
			max77705_usbc_fail_safe(u, "CCIC not initialized");
		return;
	}

	if (u->halted) {
		dev_info(u->dev, "CCIC re-initialized, resuming\n");
		u->halted = false;
		max77705_usbc_restore_ilim(u);
	}
}

/*
 * Belt and braces for a reset whose SYSMsg was never seen: the interrupt
 * masks return to their reset values with the firmware, so masks other
 * than those read back after hw_init mean the CCIC has reset since.
 */
static bool max77705_usbc_masks_lost(struct max77705_usbc *u)
{
	u8 masks[sizeof(u->masks)];

	if (regmap_bulk_read(u->regmap, MAX77705_UIC_INT_M, masks, sizeof(masks)))
		return false;

	if (!memcmp(masks, u->masks, sizeof(masks)))
		return false;

	dev_warn(u->dev, "interrupt masks %*ph, expected %*ph\n",
		 (int)sizeof(masks), masks, (int)sizeof(u->masks), u->masks);
	return true;
}

/*
 * Handle one interrupt event. CC and data-role state are read as levels
 * here; PDMsg and SYSMsg come from the event, as read by the handler.
 */
static void max77705_usbc_handle_ev(struct max77705_usbc *u,
				    const struct max77705_usbc_ev *ev)
{
	unsigned int cc_status0;

	if ((ev->uic & MAX77705_UIC_SYSMSG) && ev->sysmsg) {
		if (max77705_sysmsg_is_reset(ev->sysmsg))
			max77705_usbc_recover(u, ev->sysmsg == MAX77705_SYSMSG_BOOT_WDT ?
					      "watchdog reset" : "power-on reset");
		else
			dev_dbg(u->dev, "SYSMsg 0x%02x\n", ev->sysmsg);
	}

	if (u->halted)
		return;

	if (regmap_read(u->regmap, MAX77705_CC_STATUS0, &cc_status0))
		return;

	max77705_usbc_update_cc(u, cc_status0);

	if (ev->pd & MAX77705_PD_DATAROLE)
		max77705_usbc_update_data_role(u);

	if (ev->pd & MAX77705_PD_PDMSG)
		max77705_usbc_pdmsg(u, ev->pdmsg);

	if ((ev->pd & MAX77705_PD_PSRDY) ||
	    ((ev->pd & MAX77705_PD_PDMSG) && ev->pdmsg == MAX77705_PDMSG_SNK_PSRDY) ||
	    (u->ccstat == MAX77705_CC_SINK && !u->pd_contract))
		max77705_usbc_pd_ready(u);
}

static void max77705_usbc_recover_work(struct work_struct *work)
{
	struct max77705_usbc *u = container_of(to_delayed_work(work),
					       struct max77705_usbc, recover_work);
	struct max77705_usbc_ev ev = { };

	mutex_lock(&u->lock);
	if (u->halted) {
		max77705_usbc_recover(u, "held after resets");
		/* Pick up whatever is attached now. */
		max77705_usbc_handle_ev(u, &ev);
	}
	mutex_unlock(&u->lock);
}

static void max77705_usbc_work(struct work_struct *work)
{
	struct max77705_usbc *u = container_of(work, struct max77705_usbc, work);
	struct max77705_usbc_ev ev;
	bool overflow, reset_lost, got, any = false;
	unsigned long flags;

	mutex_lock(&u->lock);

	for (;;) {
		spin_lock_irqsave(&u->ev_lock, flags);
		got = kfifo_get(&u->ev_fifo, &ev);
		spin_unlock_irqrestore(&u->ev_lock, flags);
		if (!got)
			break;

		max77705_usbc_handle_ev(u, &ev);
		any = true;
	}

	spin_lock_irqsave(&u->ev_lock, flags);
	overflow = u->ev_overflow;
	reset_lost = u->ev_reset_lost;
	u->ev_overflow = false;
	u->ev_reset_lost = false;
	spin_unlock_irqrestore(&u->ev_lock, flags);

	if (overflow)
		dev_warn(u->dev, "interrupt events dropped, resyncing\n");

	if (reset_lost || max77705_usbc_masks_lost(u)) {
		max77705_usbc_recover(u, reset_lost ? "reset (queued late)" :
				      "reset (masks lost)");
		any = false;
	}

	/*
	 * The first run after probe, an overflow and a recovery resync from
	 * levels; the PD handlers are idempotent.
	 */
	if (!any || overflow) {
		memset(&ev, 0, sizeof(ev));
		max77705_usbc_handle_ev(u, &ev);
	}

	mutex_unlock(&u->lock);
}

/*
 * Nested in the MAX77705 top's interrupt thread. Reading the four
 * interrupt registers clears them (stock max77705-irq.c:339-341). Mailbox
 * commands wait for APCmdRes here, so all other work is deferred; the
 * PDMsg and SYSMsg reports are read now, before a later message can
 * overwrite them, and queued in order.
 */
static irqreturn_t max77705_usbc_irq(int irq, void *data)
{
	struct max77705_usbc *u = data;
	struct max77705_usbc_ev ev = { };
	unsigned long flags;
	unsigned int val;
	u8 ints[4];

	if (regmap_bulk_read(u->regmap, MAX77705_UIC_INT, ints, sizeof(ints)))
		return IRQ_NONE;

	if (ints[0] & MAX77705_UIC_APCMDRES)
		complete(&u->cmd_done);

	ev.uic = ints[0] & ~MAX77705_UIC_APCMDRES;
	ev.pd = ints[2];
	if (!ev.uic && !ints[1] && !ev.pd)
		return IRQ_HANDLED;

	if ((ev.uic & MAX77705_UIC_SYSMSG) &&
	    !regmap_read(u->regmap, MAX77705_USBC_STATUS2, &val))
		ev.sysmsg = val;
	if ((ev.pd & MAX77705_PD_PDMSG) &&
	    !regmap_read(u->regmap, MAX77705_PD_STATUS0, &val))
		ev.pdmsg = val;

	spin_lock_irqsave(&u->ev_lock, flags);
	if (!kfifo_put(&u->ev_fifo, ev)) {
		u->ev_overflow = true;
		if (max77705_sysmsg_is_reset(ev.sysmsg))
			u->ev_reset_lost = true;
	}
	spin_unlock_irqrestore(&u->ev_lock, flags);

	queue_work(u->wq, &u->work);

	return IRQ_HANDLED;
}

/* Power supply */

static const enum power_supply_property max77705_usbc_psy_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int max77705_usbc_psy_get(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct max77705_usbc *u = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&u->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = u->ccstat == MAX77705_CC_SINK;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = u->pd_contract ? POWER_SUPPLY_USB_TYPE_PD :
					       POWER_SUPPLY_USB_TYPE_C;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = u->ccstat == MAX77705_CC_SINK ? u->voltage_uv : 0;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		if (u->ccstat != MAX77705_CC_SINK)
			val->intval = 0;
		else if (u->pd_contract)
			val->intval = u->current_ua;
		else if (max77705_usbc_opmode(u->cc_status0) == TYPEC_PWR_MODE_3_0A)
			val->intval = 3000000;
		else if (max77705_usbc_opmode(u->cc_status0) == TYPEC_PWR_MODE_1_5A)
			val->intval = 1500000;
		else
			val->intval = 500000;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&u->lock);

	return ret;
}

static const struct power_supply_desc max77705_usbc_psy_desc = {
	.name = "max77705-usbc",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_C) | BIT(POWER_SUPPLY_USB_TYPE_PD),
	.properties = max77705_usbc_psy_props,
	.num_properties = ARRAY_SIZE(max77705_usbc_psy_props),
	.get_property = max77705_usbc_psy_get,
};

/* Probe */

static int max77705_usbc_get_snk_pdos(struct max77705_usbc *u,
				      struct fwnode_handle *connector)
{
	u32 pdos[MAX77705_MAX_PDO];
	unsigned int i, n = 0;
	int cnt;

	cnt = fwnode_property_count_u32(connector, "sink-pdos");
	if (cnt <= 0) {
		memcpy(u->snk_pdos, max77705_default_snk_pdos,
		       sizeof(max77705_default_snk_pdos));
		u->nr_snk_pdos = ARRAY_SIZE(max77705_default_snk_pdos);
		return 0;
	}

	if (cnt > MAX77705_MAX_PDO)
		return dev_err_probe(u->dev, -EINVAL, "too many sink-pdos\n");

	if (fwnode_property_read_u32_array(connector, "sink-pdos", pdos, cnt))
		return dev_err_probe(u->dev, -EINVAL, "bad sink-pdos\n");

	for (i = 0; i < cnt; i++) {
		if (!max77705_pdo_acceptable(pdos[i])) {
			dev_warn(u->dev, "sink-pdos: dropping 0x%08x (fixed, 5-9 V only)\n",
				 pdos[i]);
			continue;
		}
		u->snk_pdos[n++] = pdos[i];
	}

	if (!n || pdo_fixed_voltage(u->snk_pdos[0]) != 5000)
		return dev_err_probe(u->dev, -EINVAL,
				     "sink-pdos must start with fixed 5 V\n");

	u->nr_snk_pdos = n;
	return 0;
}

static void max77705_usbc_teardown(void *data)
{
	struct max77705_usbc *u = data;

	cancel_work_sync(&u->work);
	cancel_delayed_work_sync(&u->pd_retry_work);
	cancel_delayed_work_sync(&u->vbus_work);
	cancel_delayed_work_sync(&u->recover_work);

	mutex_lock(&u->lock);
	if (u->ccstat != MAX77705_CC_NONE)
		max77705_usbc_detach(u);
	else
		max77705_usbc_vbus_off(u);
	mutex_unlock(&u->lock);

	typec_unregister_port(u->port);
	usb_role_switch_put(u->role_sw);
}

static int max77705_usbc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_capability cap = { };
	struct power_supply_config psy_cfg = { };
	struct fwnode_handle *connector;
	struct max77705_usbc *u;
	unsigned int rev[3];
	u8 ints[4];
	int ret;

	if (!client->irq)
		return dev_err_probe(dev, -EINVAL, "no interrupt\n");

	u = devm_kzalloc(dev, sizeof(*u), GFP_KERNEL);
	if (!u)
		return -ENOMEM;

	u->dev = dev;
	mutex_init(&u->lock);
	spin_lock_init(&u->ev_lock);
	init_completion(&u->cmd_done);
	INIT_WORK(&u->work, max77705_usbc_work);
	INIT_DELAYED_WORK(&u->pd_retry_work, max77705_usbc_pd_retry_work);
	INIT_DELAYED_WORK(&u->vbus_work, max77705_usbc_vbus_work);
	INIT_DELAYED_WORK(&u->recover_work, max77705_usbc_recover_work);
	INIT_KFIFO(u->ev_fifo);

	u->regmap = devm_regmap_init_i2c(client, &max77705_usbc_regmap_config);
	if (IS_ERR(u->regmap))
		return dev_err_probe(dev, PTR_ERR(u->regmap), "regmap init failed\n");

	ret = regmap_read(u->regmap, MAX77705_UIC_HW_REV, &rev[0]);
	ret = ret ?: regmap_read(u->regmap, MAX77705_UIC_FW_REV, &rev[1]);
	ret = ret ?: regmap_read(u->regmap, MAX77705_UIC_FW_MINOR, &rev[2]);
	if (ret)
		return dev_err_probe(dev, ret, "CCIC not responding\n");

	dev_info(dev, "CCIC hw 0x%02x fw %02X.%02X\n", rev[0], rev[1], rev[2] & 0x7);

	u->wq = devm_alloc_ordered_workqueue(dev, "%s", 0, dev_name(dev));
	if (!u->wq)
		return -ENOMEM;

	u->vbus = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(u->vbus)) {
		if (PTR_ERR(u->vbus) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(u->vbus), "vbus-supply\n");
		u->vbus = NULL;
	}

	/*
	 * VBUS left on (a previous instance of this driver, the bootloader):
	 * a consumer can only drop a reference it holds, so take one and
	 * drop it, which switches the boost off. The enable does not touch
	 * the hardware when it is already on. The first state sync turns it
	 * back on if a sink is attached. If the disable fails the core keeps
	 * our reference, and vbus_on records that so the sync retries.
	 */
	if (u->vbus && regulator_is_enabled(u->vbus) > 0) {
		dev_warn(dev, "VBUS on at probe, switching it off\n");
		ret = regulator_enable(u->vbus);
		if (ret)
			return dev_err_probe(dev, ret, "cannot take over VBUS\n");
		ret = regulator_disable(u->vbus);
		if (ret) {
			dev_err(dev, "failed to stop VBUS (%d), will retry\n", ret);
			u->vbus_on = true;
		}
	}

	/* Without the charger the sink stays at 5 V; see pd_request(). */
	u->chg_psy = devm_power_supply_get_by_reference(dev, "maxim,charger");
	if (!u->chg_psy)
		return dev_err_probe(dev, -EPROBE_DEFER, "charger not ready\n");
	if (IS_ERR(u->chg_psy)) {
		dev_info(dev, "no charger reference, PD sink stays at 5 V\n");
		u->chg_psy = NULL;
	} else {
		ret = max77705_usbc_get_ilim(u, &u->ilim_base_ua);
		if (ret)
			return dev_err_probe(dev, ret, "cannot read input limit\n");
	}

	psy_cfg.drv_data = u;
	psy_cfg.fwnode = dev_fwnode(dev);
	u->psy = devm_power_supply_register(dev, &max77705_usbc_psy_desc, &psy_cfg);
	if (IS_ERR(u->psy))
		return dev_err_probe(dev, PTR_ERR(u->psy), "power supply\n");

	connector = device_get_named_child_node(dev, "connector");
	if (!connector)
		return dev_err_probe(dev, -ENODEV, "no connector node\n");

	ret = max77705_usbc_get_snk_pdos(u, connector);
	if (ret)
		goto err_put_connector;

	u->role_sw = fwnode_usb_role_switch_get(connector);
	if (IS_ERR(u->role_sw)) {
		ret = dev_err_probe(dev, PTR_ERR(u->role_sw), "role switch\n");
		goto err_put_connector;
	}

	cap.type = TYPEC_PORT_DRP;
	cap.data = TYPEC_PORT_DRD;
	cap.prefer_role = TYPEC_NO_PREFERRED_ROLE;
	ret = typec_get_fw_cap(&cap, connector);
	if (ret) {
		dev_err_probe(dev, ret, "connector properties\n");
		goto err_put_role;
	}
	cap.revision = USB_TYPEC_REV_1_2;
	cap.pd_revision = 0x300;
	cap.driver_data = u;

	u->port = typec_register_port(dev, &cap);
	if (IS_ERR(u->port)) {
		ret = dev_err_probe(dev, PTR_ERR(u->port), "typec port\n");
		goto err_put_role;
	}
	fwnode_handle_put(connector);

	ret = devm_add_action_or_reset(dev, max77705_usbc_teardown, u);
	if (ret)
		return ret;

	/*
	 * Initialize by polling with the USBC source still masked at the top
	 * (stock unmasks INTSRC_MASK only after init, usbc.c:3446-3460);
	 * requesting the interrupt below unmasks it.
	 */
	mutex_lock(&u->lock);
	ret = regmap_bulk_read(u->regmap, MAX77705_UIC_INT, ints, sizeof(ints));
	ret = ret ?: max77705_usbc_hw_init(u);
	mutex_unlock(&u->lock);
	if (ret)
		return dev_err_probe(dev, ret, "CCIC init failed\n");

	ret = devm_request_threaded_irq(dev, client->irq, NULL, max77705_usbc_irq,
					IRQF_ONESHOT, dev_name(dev), u);
	if (ret)
		return dev_err_probe(dev, ret, "irq %d\n", client->irq);

	mutex_lock(&u->lock);
	u->irq_ready = true;
	mutex_unlock(&u->lock);

	/* Pick up whatever is attached already. */
	queue_work(u->wq, &u->work);

	return 0;

err_put_role:
	usb_role_switch_put(u->role_sw);
err_put_connector:
	fwnode_handle_put(connector);
	return ret;
}

static const struct of_device_id max77705_usbc_of_match[] = {
	{ .compatible = "maxim,max77705-usbc" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77705_usbc_of_match);

static struct i2c_driver max77705_usbc_driver = {
	.driver = {
		.name = "max77705-usbc",
		.of_match_table = max77705_usbc_of_match,
	},
	.probe = max77705_usbc_probe,
};
module_i2c_driver(max77705_usbc_driver);

MODULE_DESCRIPTION("Maxim MAX77705 USB Type-C and PD sink driver");
MODULE_LICENSE("GPL");
