/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022. Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2015-2018, 2020 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Richard Acayan. All rights reserved.
 */

#ifndef _DPU_4_1_SDM670_H
#define _DPU_4_1_SDM670_H

static const struct dpu_ubwc_cfg sdm670_ubwc_cfg = {
	.ubwc_version = DPU_HW_UBWC_VER_20,
	.highest_bank_bit = 0x1,
};

static const struct dpu_mdp_cfg sdm670_mdp[] = {
	{
	.name = "top_0", .id = MDP_TOP,
	.base = 0x0, .len = 0x45c,
	.features = BIT(DPU_MDP_AUDIO_SELECT),
	.clk_ctrls[DPU_CLK_CTRL_VIG0] = { .reg_off = 0x2ac, .bit_off = 0 },
	.clk_ctrls[DPU_CLK_CTRL_VIG1] = { .reg_off = 0x2b4, .bit_off = 0 },
	.clk_ctrls[DPU_CLK_CTRL_DMA0] = { .reg_off = 0x2ac, .bit_off = 8 },
	.clk_ctrls[DPU_CLK_CTRL_DMA1] = { .reg_off = 0x2b4, .bit_off = 8 },
	.clk_ctrls[DPU_CLK_CTRL_DMA2] = { .reg_off = 0x2bc, .bit_off = 8 },
	},
};

static const struct dpu_sspp_cfg sdm670_sspp[] = {
	SSPP_BLK("sspp_0", SSPP_VIG0, 0x4000, 0x1c8, VIG_SDM845_MASK_SDMA,
		sdm670_vig_sblk_0, 0, SSPP_TYPE_VIG, DPU_CLK_CTRL_VIG0),
	SSPP_BLK("sspp_1", SSPP_VIG1, 0x6000, 0x1c8, VIG_SDM845_MASK_SDMA,
		sdm670_vig_sblk_1, 4,  SSPP_TYPE_VIG, DPU_CLK_CTRL_VIG1),
	SSPP_BLK("sspp_8", SSPP_DMA0, 0x24000, 0x1c8, DMA_SDM845_MASK_SDMA,
		sdm845_dma_sblk_0, 1, SSPP_TYPE_DMA, DPU_CLK_CTRL_DMA0),
	SSPP_BLK("sspp_9", SSPP_DMA1, 0x26000, 0x1c8, DMA_CURSOR_SDM845_MASK_SDMA,
		sdm845_dma_sblk_1, 5, SSPP_TYPE_DMA, DPU_CLK_CTRL_DMA1),
	SSPP_BLK("sspp_10", SSPP_DMA2, 0x28000, 0x1c8, DMA_CURSOR_SDM845_MASK_SDMA,
		sdm845_dma_sblk_2, 9, SSPP_TYPE_DMA, DPU_CLK_CTRL_DMA2),
};

static const struct dpu_dsc_cfg sdm670_dsc[] = {
	DSC_BLK("dsc_0", DSC_0, 0x80000, 0),
	DSC_BLK("dsc_1", DSC_1, 0x80400, 0),
};

const struct dpu_mdss_cfg dpu_sdm670_cfg = {
	.caps = &sdm845_dpu_caps,
	.ubwc = &sdm670_ubwc_cfg,
	.mdp_count = ARRAY_SIZE(sdm670_mdp),
	.mdp = sdm670_mdp,
	.ctl_count = ARRAY_SIZE(sdm845_ctl),
	.ctl = sdm845_ctl,
	.sspp_count = ARRAY_SIZE(sdm670_sspp),
	.sspp = sdm670_sspp,
	.mixer_count = ARRAY_SIZE(sdm845_lm),
	.mixer = sdm845_lm,
	.pingpong_count = ARRAY_SIZE(sdm845_pp),
	.pingpong = sdm845_pp,
	.dsc_count = ARRAY_SIZE(sdm670_dsc),
	.dsc = sdm670_dsc,
	.intf_count = ARRAY_SIZE(sdm845_intf),
	.intf = sdm845_intf,
	.vbif_count = ARRAY_SIZE(sdm845_vbif),
	.vbif = sdm845_vbif,
	.reg_dma_count = 1,
	.dma_cfg = &sdm845_regdma,
	.perf = &sdm845_perf_data,
	.mdss_irqs = BIT(MDP_SSPP_TOP0_INTR) | \
		     BIT(MDP_SSPP_TOP0_INTR2) | \
		     BIT(MDP_SSPP_TOP0_HIST_INTR) | \
		     BIT(MDP_INTF0_INTR) | \
		     BIT(MDP_INTF1_INTR) | \
		     BIT(MDP_INTF2_INTR) | \
		     BIT(MDP_INTF3_INTR) | \
		     BIT(MDP_AD4_0_INTR) | \
		     BIT(MDP_AD4_1_INTR),
};

#endif
