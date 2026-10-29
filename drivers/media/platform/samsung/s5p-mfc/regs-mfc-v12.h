/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register definition file for Samsung MFC V12.x Interface (FIMV) driver
 *
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 *     http://www.samsung.com/
 */

#ifndef _REGS_MFC_V12_H
#define _REGS_MFC_V12_H

#include <linux/sizes.h>
#include "regs-mfc-v10.h"

/* MFCv12 Context buffer sizes */
#define MFC_CTX_BUF_SIZE_V12		(30 * SZ_1K)
#define MFC_H264_DEC_CTX_BUF_SIZE_V12	(2 * SZ_1M)
#define MFC_OTHER_DEC_CTX_BUF_SIZE_V12	(30 * SZ_1K)
#define MFC_H264_ENC_CTX_BUF_SIZE_V12	(100 * SZ_1K)
#define MFC_HEVC_ENC_CTX_BUF_SIZE_V12	(40 * SZ_1K)
#define MFC_OTHER_ENC_CTX_BUF_SIZE_V12	(25 * SZ_1K)

/* MFCv12 variant defines */
#define MAX_FW_SIZE_V12			(SZ_1M)
#define MAX_CPB_SIZE_V12		(7 * SZ_1M)
#define MFC_VERSION_V12			0xC0
#define MFC_NUM_PORTS_V12		1
#define S5P_FIMV_CODEC_VP9_ENC		27
#define MFC_CHROMA_PAD_BYTES_V12        256
#define S5P_FIMV_D_ALIGN_PLANE_SIZE_V12 256

/* Encoder buffer size for MFCv12 */
#define ENC_V120_BASE_SIZE(x, y) \
	((((x) + 3) * ((y) + 3) * 8) \
	+ ((((y) * 64) + 2304) * ((x) + 7) / 8))

#define ENC_V120_H264_ME_SIZE(x, y) \
	ALIGN((ENC_V120_BASE_SIZE(x, y) \
	+ (DIV_ROUND_UP((x) * (y), 64) * 32)), 256)

#define ENC_V120_MPEG4_ME_SIZE(x, y) \
	ALIGN((ENC_V120_BASE_SIZE(x, y) \
	+ (DIV_ROUND_UP((x) * (y), 128) * 16)), 256)

#define ENC_V120_VP8_ME_SIZE(x, y) \
	ALIGN(ENC_V120_BASE_SIZE((x), (y)), 256)

#define ENC_V120_HEVC_ME_SIZE(x, y)     \
	ALIGN(((((x) + 3) * ((y) + 3) * 32)       \
	+ ((((y) * 128) + 2304) * ((x) + 3) / 4)), 256)

/*
 * MFCv13 (Exynos 9820): the v12 host interface, with the context buffer
 * sizes of the stock driver and a hardware version register the firmware
 * fills in at SYS_INIT.
 */
#define MFC_CTX_BUF_SIZE_V13		(30 * SZ_1K)
#define MFC_H264_DEC_CTX_BUF_SIZE_V13	(2 * SZ_1M)
#define MFC_OTHER_DEC_CTX_BUF_SIZE_V13	(50 * SZ_1K)
#define MFC_H264_ENC_CTX_BUF_SIZE_V13	(100 * SZ_1K)
#define MFC_HEVC_ENC_CTX_BUF_SIZE_V13	(50 * SZ_1K)
#define MFC_OTHER_ENC_CTX_BUF_SIZE_V13	(50 * SZ_1K)
#define MAX_FW_SIZE_V13			(SZ_1M)
#define MAX_CPB_SIZE_V13		(3 * SZ_1M)
#define MFC_VERSION_V13			0xD0
#define MFC_NUM_PORTS_V13		1
#define S5P_FIMV_MFC_VERSION_V13	0xf028
#define MFC_HW_VERSION_V13		0x1300
#define MFC_IOVA_START_V13		0x10000000
/* what the stock v13 driver sets on top of the v6 bits */
#define S5P_FIMV_CLEAR_CTX_MEM_SHIFT_V13	16
#define S5P_FIMV_D_OPT_CONCEAL_SHIFT_V13	8
#define S5P_FIMV_D_OPT_REALLOC_SHIFT_V13	13
#define S5P_FIMV_D_OPT_SPECIAL_PARSING_SHIFT_V13 15
#define S5P_FIMV_D_FORCE_PIXEL_VAL_V13		0xf0c8
#define S5P_FIMV_D_SEI_RECOVERY_SHIFT_V13	2
#define MFC_CONCEAL_COLOR_V13			0x8020000

#endif /*_REGS_MFC_V12_H*/
