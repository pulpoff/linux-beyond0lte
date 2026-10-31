/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DisplayPort alt mode state of the MAX77705 Type-C port, for the DP
 * controller driver. The callback gets the state bits as its action; it
 * runs in the port's IRQ thread and may sleep, but should only queue work.
 */
#ifndef __LINUX_USB_MAX77705_DP_H
#define __LINUX_USB_MAX77705_DP_H

#include <linux/bits.h>
#include <linux/notifier.h>

#define MAX77705_DP_CONFIGURED	BIT(0)	/* alt mode entered and a pin assignment configured */
#define MAX77705_DP_HPD		BIT(1)	/* HPD level from the last Status/Attention */
#define MAX77705_DP_HPD_IRQ	BIT(2)	/* this report carried an IRQ_HPD */
#define MAX77705_DP_FOUR_LANES	BIT(3)	/* pin assignment C or E: DP on all four lanes */

/* registers, then calls @nb once with the current state */
int max77705_dp_register_notifier(struct notifier_block *nb);
void max77705_dp_unregister_notifier(struct notifier_block *nb);

#endif
