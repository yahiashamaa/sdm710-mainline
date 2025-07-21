// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

#include "touchpanel_common.h"
#include "himax_common.h"

/*******Part0:LOG TAG Declear********************/

#define TPD_DEVICE "himax_common"
#define TPD_INFO(a, arg...)  pr_err("[TP]"TPD_DEVICE ": " a, ##arg)
#define TPD_DEBUG(a, arg...)\
    do{\
        if (LEVEL_DEBUG == tp_debug)\
            pr_err("[TP]"TPD_DEVICE ": " a, ##arg);\
    }while(0)

#define TPD_DETAIL(a, arg...)\
    do{\
        if (LEVEL_BASIC != tp_debug)\
            pr_err("[TP]"TPD_DEVICE ": " a, ##arg);\
    }while(0)


/*******Part1:Call Back Function implement*******/
void himax_parse_header(struct image_header_data *header, const unsigned char *fw_image)
{
    return;
}

void himax_limit_read(struct seq_file *s, struct touchpanel_data *ts)
{
    struct hx_limit_data *limit_data = NULL;
    struct himax_proc_operations *syna_ops;
    int i;
    int j;

    TPD_INFO("%s: Entering\n", __func__);
    syna_ops = (struct himax_proc_operations *)ts->private_data;
    limit_data = kzalloc(sizeof(struct hx_limit_data), GFP_KERNEL);
    if (!limit_data) {
        TPD_INFO("limit_data allocat fail!\n");
        goto FAIL_ALLOC_MEM;
    }
    if (!syna_ops->fp_hx_limit_get){
        TPD_INFO("%s:Doesn't support!\n", __func__);
        goto FAIL_FUNC;
    } else {
        syna_ops->fp_hx_limit_get(ts, limit_data);
    }
    if (limit_data != NULL) {
        for (i = 0 ; i < limit_data->item_size; i++) {
            seq_printf(s, "%s:\n",limit_data->item_name[i]);
            TPD_INFO("%s: [%d]Size of =%d\n", __func__, i, (int)sizeof(limit_data->item_name[i]));

                for (j = 0 ; j < limit_data->rawdata_size; j++) {
                    if (j % ( ts->hw_res.RX_NUM) == 0)
                        seq_printf(s, "\n[%2d] ", (j / ts->hw_res.RX_NUM));
                    seq_printf(s, "%4d, ", limit_data->crtra_val[i][j]);
                }
            seq_printf(s, "\n");
        }
    }
    seq_printf(s, "\n");
    TPD_INFO("%s: END PRINT!\n", __func__);
FAIL_FUNC:
    if (limit_data != NULL) {
        for (i = 0 ; i < limit_data->item_size; i++) {
            kfree(limit_data->item_name[i]);
            kfree(limit_data->crtra_val[i]);
        }
        kfree(limit_data);
    }
FAIL_ALLOC_MEM:
    TPD_INFO("%s: End\n", __func__);
}
