/* SPDX-License-Identifier: GPL-2.0-only */
/* What the Galaxy S10e's panel driver needs from the DECON handover driver, its DSI host */
#ifndef __EXYNOS9820_DECON_H
#define __EXYNOS9820_DECON_H

#include <linux/types.h>

struct mipi_dsi_host;

unsigned int exynos9820_decon_hold_frames(struct mipi_dsi_host *host, bool hold);

#endif
