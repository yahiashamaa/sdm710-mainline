// SPDX-License-Identifier: GPL-2.0-only
/*
 * SoC Information over Shared Memory.
 *
 * Copyright (c) 2022-2024, Richard Acayan. All rights reserved.
 */

#include <asm/byteorder.h>
#include <asm/io.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>

#define MODEM_SMEM_VERSION 0

#define PLAT_VER_TO_MAJOR_ID(v) (((v) >> 16) & 0xff)
#define PLAT_VER_TO_MINOR_ID(v) ((v) & 0xff)

struct modem_smem_info {
	u32 version;
	u32 modem_flag;
	u32 major_id;
	u32 minor_id;
	u32 subtype;
	u32 platform;
	u32 efs_magic;
	u32 ftm_magic;
};

static int write_socinfo(struct modem_smem_info *target) {
	u32 plat_ver, hw_plat, hw_plat_subtype;
	struct socinfo *socinfo;

	socinfo = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID, NULL);
	if (IS_ERR(socinfo))
		return PTR_ERR(socinfo);

	// The socinfo struct must not be prior to version 0.6
	if (socinfo->ver < 0x00000006)
		return -ENOTSUPP;

	plat_ver = __le32_to_cpu(socinfo->plat_ver);
	hw_plat = __le32_to_cpu(socinfo->hw_plat);
	hw_plat_subtype = __le32_to_cpu(socinfo->hw_plat_subtype);

	writel_relaxed(MODEM_SMEM_VERSION, &target->version);
	writel_relaxed(PLAT_VER_TO_MAJOR_ID(plat_ver), &target->major_id);
	writel_relaxed(PLAT_VER_TO_MINOR_ID(plat_ver), &target->minor_id);
	writel_relaxed(hw_plat, &target->platform);
	writel_relaxed(hw_plat_subtype, &target->subtype);

	return 0;
}

static int modemsmem_probe(struct platform_device *pdev) {
	int smem_id;
	int ret;
	size_t len;
	struct modem_smem_info *info;

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,smem", &smem_id);
	if (ret) {
		dev_err(&pdev->dev, "Could not read smem id: %d\n", ret);
		return ret;
	}

	ret = qcom_smem_alloc(QCOM_SMEM_HOST_ANY, smem_id, sizeof(struct modem_smem_info));
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "Could not allocate modem smem\n");
		return ret;
	}

	info = qcom_smem_get(QCOM_SMEM_HOST_ANY, smem_id, &len);

	ret = write_socinfo(info);
	if (ret) {
		dev_err(&pdev->dev, "Could not submit info to modemsmem\n");
		return ret;
	}

	return 0;
}

static int modemsmem_remove(struct platform_device *pdev) {
	return 0;
}

static struct of_device_id modemsmem_of_match[] = {
	{ .compatible = "google,modemsmem" },
};

static struct platform_driver modemsmem_driver = {
	.probe = modemsmem_probe,
	.remove = modemsmem_remove,
	.driver = {
		.name = "modemsmem",
		.of_match_table = modemsmem_of_match,
	},
};
module_platform_driver(modemsmem_driver);

MODULE_AUTHOR("Richard Acayan <mailingradian@gmail.com>");
MODULE_DESCRIPTION("SoC Information over SMEM driver");
MODULE_LICENSE("GPL");
