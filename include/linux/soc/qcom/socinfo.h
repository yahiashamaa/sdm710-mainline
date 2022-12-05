/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __QCOM_SOCINFO_H__
#define __QCOM_SOCINFO_H__

#include <linux/platform_device.h>

u32 qcom_socinfo_get_hw_plat(struct platform_device *pdev);
u32 qcom_socinfo_get_plat_ver(struct platform_device *pdev);
u32 qcom_socinfo_get_hw_plat_subtype(struct platform_device *pdev);

#endif
