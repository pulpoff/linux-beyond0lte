/* SPDX-License-Identifier: GPL-2.0-only */
/* Exynos 9820 DP link: what the DECON2 CRTC needs around its own start/stop */
#ifndef __EXYNOS9820_DP_H
#define __EXYNOS9820_DP_H

struct drm_bridge;
struct drm_display_mode;

/* link up, trained, @mode's timing in the DP timing generator, LH channel on */
int exynos9820_dp_stream_on(struct drm_bridge *bridge, const struct drm_display_mode *mode);
/* everything off again; call once DECON2 is idle */
void exynos9820_dp_stream_off(struct drm_bridge *bridge);

#endif
