/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QCOM_SMEM_H__
#define __QCOM_SMEM_H__

#include <linux/err.h>
#include <linux/platform_device.h>

#define QCOM_SMEM_HOST_ANY -1

struct qcom_socinfo;

int qcom_smem_alloc(unsigned host, unsigned item, size_t size);
void *qcom_smem_get(unsigned host, unsigned item, size_t *size);

int qcom_smem_get_free_space(unsigned host);

phys_addr_t qcom_smem_virt_to_phys(void *p);

#ifdef CONFIG_QCOM_SOCINFO
struct platform_device *qcom_smem_get_socinfo(void);
#else
static inline struct platform_device *qcom_smem_get_socinfo(void)
{
	return ERR_PTR(-ENOSYS);
}
#endif

#endif
