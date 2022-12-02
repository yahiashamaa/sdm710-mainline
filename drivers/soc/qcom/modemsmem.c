// SPDX-License-Identifier: GPL-2.0-only
/*
 * SoC Information over Shared Memory.
 *
 * Copyright (c) 2022, Richard Acayan. All rights reserved.
 */

#include <asm/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>

#define MODEM_SMEM_VERSION 0

#define PLAT_VER_TO_MAJOR_ID(v) (((v) >> 16) & 0xff)
#define PLAT_VER_TO_MINOR_ID(v) ((v) & 0xff)

struct modem_smem_info {
	uint32_t version;
	uint32_t modem_flag;
	uint32_t major_id;
	uint32_t minor_id;
	uint32_t subtype;
	uint32_t platform;
	uint32_t efs_magic;
	uint32_t ftm_magic;
};

static int write_socinfo(struct modem_smem_info *info) {
	u32 plat_ver, hw_plat, hw_plat_subtype;
	struct platform_device *socinfo = qcom_smem_get_socinfo();

	plat_ver = qcom_socinfo_get_plat_ver(socinfo);
	hw_plat = qcom_socinfo_get_hw_plat(socinfo);
	hw_plat_subtype = qcom_socinfo_get_hw_plat_subtype(socinfo);

	writel_relaxed(MODEM_SMEM_VERSION, &info->version);
	writel_relaxed(PLAT_VER_TO_MAJOR_ID(plat_ver), &info->major_id);
	writel_relaxed(PLAT_VER_TO_MINOR_ID(plat_ver), &info->minor_id);
	writel_relaxed(hw_plat, &info->platform);
	writel_relaxed(hw_plat_subtype, &info->subtype);

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
