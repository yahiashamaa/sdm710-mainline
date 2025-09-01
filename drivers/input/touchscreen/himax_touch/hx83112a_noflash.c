// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 * Copyright (C) 2025 Yahia Shamaa <yehiashamaa987@gmail.com>
 */

#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/rtc.h>
#include <linux/hrtimer.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/task_work.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/machine.h>
#include <linux/regulator/consumer.h>

#ifdef CONFIG_FB
#include <linux/fb.h>
#include <linux/notifier.h>
#endif

#include <linux/spi/spi.h>

#include "hx83112a_noflash.h"
#include "himax_common.h"
#include "himax_spi.h"

unsigned int tp_debug = 0;

#define TPD_DEVICE "himax,hx83112a_nf"
#define TPD_INFO(a, arg...)  pr_info("[TP]"TPD_DEVICE ": " a, ##arg)
#define TPD_DEBUG(a, arg...)\
    do{\
        if (LEVEL_DEBUG == tp_debug)\
            pr_info("[TP]"TPD_DEVICE ": " a, ##arg);\
    }while(0)

#define TPD_DETAIL(a, arg...)\
    do{\
        if (LEVEL_BASIC != tp_debug)\
            pr_info("[TP]"TPD_DEVICE ": " a, ##arg);\
    }while(0)

#define TPD_DEBUG_NTAG(a, arg...)\
    do{\
        if (tp_debug)\
            printk(a, ##arg);\
    }while(0)

struct hx83112a_nf_report_data *hx83112a_nf_touch_data;
struct chip_data_hx83112a_nf *hx83112a_nf_chip_info;
int hx83112a_nf_touch_data_size = 128;
int HX83112A_NF_HW_RESET_ACTIVATE = 0;
static int HX83112A_NF_TOUCH_INFO_POINT_CNT   = 0;
int hx83112a_nf_lcd_vendor = 0;
int hx83112a_nf_irq_en_cnt = 0;

int hx83112a_nf_1kind_raw_size = 0;
uint32_t hx83112a_nf_rslt_data_len;
char *hx83112a_nf_rslt_data;
int **hx83112a_nf_inspection_criteria;
int *hx83112a_nf_inspt_crtra_flag;
int HX83112A_NF_CRITERIA_ITEM = 4;
int HX83112A_NF_CRITERIA_SIZE;
char *hx83112a_nf_file_path_OK;
char *hx83112a_nf_file_path_NG;
bool hx83112a_nf_isEA006proj = false;
bool hx83112a_nf_isBD12proj = false;
bool hx83112a_nf_isRead_csv = true;
int hx83112a_nf_fail_write_count;
uint32_t fw_file_id = 0;

extern int hx83112a_nf_f_0f_updat;
/* 128k+ */
int hx83112a_nf_cfg_crc = -1;
int hx83112a_nf_cfg_sz;
uint8_t hx83112a_nf_sram_min[4];
unsigned char *hx83112a_nf_FW_buf;
/* 128k- */


int hx83112a_nf_check_point_format;
unsigned char hx83112a_nf_switch_algo;
uint8_t HX83112A_NF_HX_PROC_SEND_FLAG;


bool p_sensor_rec = false;


/*******Part0: SPI Interface***************/
const struct mt_chip_conf hx83112a_nf_hx_spi_ctrdata = {
    .setuptime = 25,
    .holdtime = 25,
    .high_time = 3, /* 16.6MHz */
    .low_time = 3,
    .cs_idletime = 2,
    .ulthgh_thrsh = 0,

    .cpol = 0,
    .cpha = 0,

    .rx_mlsb = 1,
    .tx_mlsb = 1,

    .tx_endian = 0,
    .rx_endian = 0,

    .com_mod = DMA_TRANSFER,

    .pause = 0,
    .finish_intr = 1,
    .deassert = 0,
    .ulthigh = 0,
    .tckdly = 0,
};

static ssize_t hx83112a_nf_spi_sync(struct touchpanel_data *ts, struct spi_message *message)
{
    int status;

    status = spi_sync(ts->s_client, message);

    if (status == 0) {
        status = message->status;
        if (status == 0)
            status = message->actual_length;
    }
    return status;
}

static int hx83112a_nf_spi_read(uint8_t *command, uint8_t command_len, uint8_t *data, uint32_t length, uint8_t toRetry)
{
    struct spi_message message;
    struct spi_transfer xfer[2];
    int retry = 0;
    int error = -1;

    spi_message_init(&message);
    memset(xfer, 0, sizeof(xfer));

    xfer[0].tx_buf = command;
    xfer[0].len = command_len;
    spi_message_add_tail(&xfer[0], &message);

    xfer[1].rx_buf = data;
    xfer[1].len = length;
    spi_message_add_tail(&xfer[1], &message);

    for (retry = 0; retry < toRetry; retry++) {
        error = spi_sync(hx83112a_nf_pri_ts->s_client, &message);
        if (error) {
            TPD_INFO("SPI read error: %d\n", error);
        } else {
            break;
        }
    }
    if (retry == toRetry) {
        TPD_INFO("%s: SPI read error retry over %d\n",
                 __func__, toRetry);
        return -EIO;
    }

    return 0;
}

static int hx83112a_nf_spi_write(uint8_t *buf, uint32_t length)
{

    struct spi_transfer t = {
        .tx_buf = buf,
        .len = length,
    };
    struct spi_message    m;
    spi_message_init(&m);
    spi_message_add_tail(&t, &m);

    return hx83112a_nf_spi_sync(hx83112a_nf_pri_ts, &m);

}

static int hx83112a_nf_bus_read(uint8_t command, uint32_t length, uint8_t *data)
{
    int result = 0;
    uint8_t spi_format_buf[3];

    mutex_lock(&(hx83112a_nf_chip_info->spi_lock));
    spi_format_buf[0] = 0xF3;
    spi_format_buf[1] = command;
    spi_format_buf[2] = 0x00;

    result = hx83112a_nf_spi_read(&spi_format_buf[0], 3, data, length, 10);
    mutex_unlock(&(hx83112a_nf_chip_info->spi_lock));

    return result;
}

static int hx83112a_nf_bus_write(uint8_t command, uint32_t length, uint8_t *data)
{
    /* uint8_t spi_format_buf[length + 2]; */
    int result = 0;
    static uint8_t *spi_format_buf;
    int alloc_size = 0;
    alloc_size = 49156;
    mutex_lock(&(hx83112a_nf_chip_info->spi_lock));
    if (spi_format_buf == NULL) {
        spi_format_buf = kzalloc((alloc_size + 2) * sizeof(uint8_t), GFP_KERNEL);
    }

    if (spi_format_buf == NULL) {
        TPD_INFO("%s: Can't allocate enough buf\n", __func__);
        return -ENOMEM;
    }
    spi_format_buf[0] = 0xF2;
    spi_format_buf[1] = command;

    memcpy((uint8_t *)(&spi_format_buf[2]), data, length);

    result = hx83112a_nf_spi_write(spi_format_buf, length + 2);
    mutex_unlock(&(hx83112a_nf_chip_info->spi_lock));

    return result;
}

/*******Part1: Function Declearation*******/
static int hx83112a_nf_power_control(void *chip_data, bool enable);
static int hx83112a_nf_get_chip_info(void *chip_data);
static int hx83112a_nf_mode_switch(void *chip_data, work_mode mode, bool flag);
static uint32_t hx83112a_nf_hw_check_CRC(uint8_t *start_addr, int reload_length);
static fw_check_state hx83112a_nf_fw_check(void *chip_data, struct resolution_info *resolution_info, struct panel_info *panel_data);
static void hx83112a_nf_read_FW_ver(void);
static int hx83112a_nf_resetgpio_set(struct hw_resource *hw_res, bool on);
static uint32_t hx83112a_nf_get_fw_id(struct chip_data_hx83112a_nf *chip_info);

/*******Part2:Call Back Function implement*******/

/* add for himax */
void hx83112a_nf_flash_write_burst(uint8_t *reg_byte, uint8_t *write_data)
{
    uint8_t data_byte[8];
    int i = 0;
    int j = 0;

    for (i = 0; i < 4; i++) {
        data_byte[i] = reg_byte[i];
    }
    for (j = 4; j < 8; j++) {
        data_byte[j] = write_data[j - 4];
    }

    if (hx83112a_nf_bus_write(0x00, 8, data_byte) < 0) {
        TPD_INFO("%s: i2c access fail!\n", __func__);
        return;
    }
}

void hx83112a_nf_flash_write_burst_length(uint8_t *reg_byte, uint8_t *write_data, int length)
{
    uint8_t *data_byte;
    data_byte = kzalloc(sizeof(uint8_t) * (length + 4), GFP_KERNEL);

    if (data_byte == NULL) {
        TPD_INFO("%s: Can't allocate enough buf\n", __func__);
        return;
    }
    memcpy(data_byte, reg_byte, 4); /* assign addr 4bytes */
    memcpy(data_byte + 4, write_data, length); /* assign data n bytes */

    if (hx83112a_nf_bus_write(0, length + 4, data_byte) < 0) {
        TPD_INFO("%s: i2c access fail!\n", __func__);
    }
    kfree(data_byte);
}

void hx83112a_nf_burst_enable(uint8_t auto_add_4_byte)
{
    uint8_t tmp_data[4];
    tmp_data[0] = 0x31;

    if (hx83112a_nf_bus_write(0x13, 1, tmp_data) < 0) {
        TPD_INFO("%s: i2c access fail!\n", __func__);
        return;
    }

    tmp_data[0] = (0x10 | auto_add_4_byte);
    if (hx83112a_nf_bus_write(0x0D, 1, tmp_data) < 0) {
        TPD_INFO("%s: i2c access fail!\n", __func__);
        return;
    }
    /*isBusrtOn = true;*/
}

void hx83112a_nf_register_read(uint8_t *read_addr, int read_length, uint8_t *read_data, bool hx83112a_nf_cfg_flag)
{
    uint8_t tmp_data[4];
    int ret;
    if(hx83112a_nf_cfg_flag == false) {
        if(read_length > 256) {
            TPD_INFO("%s: read len over 256!\n", __func__);
            return;
        }
        if (read_length > 4) {
            hx83112a_nf_burst_enable(1);
        } else {
            hx83112a_nf_burst_enable(0);
        }

        tmp_data[0] = read_addr[0];
        tmp_data[1] = read_addr[1];
        tmp_data[2] = read_addr[2];
        tmp_data[3] = read_addr[3];
        ret = hx83112a_nf_bus_write(0x00, 4, tmp_data);
        if (ret < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return;
        }
        tmp_data[0] = 0x00;
        ret = hx83112a_nf_bus_write(0x0C, 1, tmp_data);
        if (ret < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return;
        }

        if (hx83112a_nf_bus_read(0x08, read_length, read_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return;
        }
        if (read_length > 4) {
            hx83112a_nf_burst_enable(0);
        }
    } else if(hx83112a_nf_cfg_flag == true) {
        if(hx83112a_nf_bus_read(read_addr[0], read_length, read_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return;
        }
    } else {
        TPD_INFO("%s: hx83112a_nf_cfg_flag = %d, value is wrong!\n", __func__, hx83112a_nf_cfg_flag);
        return;
    }
}

void hx83112a_nf_register_write(uint8_t *write_addr, int write_length, uint8_t *write_data, bool hx83112a_nf_cfg_flag)
{
    int i = 0;
    int address = 0;
    if (hx83112a_nf_cfg_flag == false) {
        address = (write_addr[3] << 24) + (write_addr[2] << 16) + (write_addr[1] << 8) + write_addr[0];

        for (i = address; i < address + write_length; i++) {
            if (write_length > 4) {
                hx83112a_nf_burst_enable(1);
            } else {
                hx83112a_nf_burst_enable(0);
            }
            hx83112a_nf_flash_write_burst_length(write_addr, write_data, write_length);
        }
    } else if(hx83112a_nf_cfg_flag == true) {
        if(hx83112a_nf_bus_write(write_addr[0], write_length, write_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return;
        }
    } else {
        TPD_INFO("%s: hx83112a_nf_cfg_flag = %d, value is wrong!\n", __func__, hx83112a_nf_cfg_flag);
        return;
    }
}

static int hx83112a_nf_mcu_register_write(uint8_t *write_addr, uint32_t write_length, uint8_t *write_data, uint8_t hx83112a_nf_cfg_flag)
{
    int total_read_times = 0;
    int max_bus_size = 128, test = 0;
    int total_size_temp = 0;
    int address = 0;
    int i = 0;

    uint8_t tmp_addr[4];
    uint8_t *tmp_data;

    total_size_temp = write_length;
    TPD_INFO("%s, Entering - total write size=%d\n", __func__, total_size_temp);

    if (write_length > 49152) {
        max_bus_size = 49152;
    } else {
        max_bus_size = write_length;
    }

    hx83112a_nf_burst_enable(1);

    tmp_addr[3] = write_addr[3];
    tmp_addr[2] = write_addr[2];
    tmp_addr[1] = write_addr[1];
    tmp_addr[0] = write_addr[0];
    TPD_INFO("%s, write addr = 0x%02X%02X%02X%02X\n", __func__, tmp_addr[3], tmp_addr[2], tmp_addr[1], tmp_addr[0]);

    tmp_data = kzalloc (sizeof (uint8_t) * max_bus_size, GFP_KERNEL);
    if (tmp_data == NULL) {
        TPD_INFO("%s: Can't allocate enough buf \n", __func__);
        return -1;
    }

    if (total_size_temp % max_bus_size == 0) {
        total_read_times = total_size_temp / max_bus_size;
    } else {
        total_read_times = total_size_temp / max_bus_size + 1;
    }

    for (i = 0; i < (total_read_times); i++) {
        if (total_size_temp >= max_bus_size) {
            memcpy (tmp_data, write_data + (i * max_bus_size), max_bus_size);
            hx83112a_nf_flash_write_burst_length (tmp_addr, tmp_data, max_bus_size);

            total_size_temp = total_size_temp - max_bus_size;
        } else {
            test = total_size_temp % max_bus_size;
            memcpy (tmp_data, write_data + (i * max_bus_size), test);
            TPD_DEBUG("last total_size_temp=%d\n", total_size_temp % max_bus_size);

            hx83112a_nf_flash_write_burst_length (tmp_addr, tmp_data, max_bus_size);
        }

        address = ((i + 1) * max_bus_size);
        tmp_addr[0] = write_addr[0] + (uint8_t) ((address) & 0x00FF);

        if (tmp_addr[0] <  write_addr[0]) {
            tmp_addr[1] = write_addr[1] + (uint8_t) ((address >> 8) & 0x00FF) + 1;
        } else {
            tmp_addr[1] = write_addr[1] + (uint8_t) ((address >> 8) & 0x00FF);
        }

        udelay (100);
    }
    TPD_DETAIL("%s, End \n", __func__);
    kfree (tmp_data);
    return 0;
}


bool hx83112a_nf_sense_off(void)
{
    uint8_t cnt = 0;
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];

    tmp_addr[3] = 0x90;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x5C;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0xA5;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    msleep(20);

    do {
        //===========================================
        //  0x31 ==> 0x27
        //===========================================
        tmp_data[0] = 0x27;
        if (hx83112a_nf_bus_write(0x31, 1, tmp_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return false;
        }
        //===========================================
        //  0x32 ==> 0x95
        //===========================================
        tmp_data[0] = 0x95;
        if (hx83112a_nf_bus_write(0x32, 1, tmp_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return false;
        }

        // ======================
        // Check enter_save_mode
        // ======================
        tmp_addr[3] = 0x90;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0xA8;
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);

        TPD_INFO("%s: Check enter_save_mode data[0]=%X \n", __func__, tmp_data[0]);

        if (tmp_data[0] == 0x0C) {
            //=====================================
            // Reset TCON
            //=====================================
            tmp_addr[3] = 0x80;
            tmp_addr[2] = 0x02;
            tmp_addr[1] = 0x00;
            tmp_addr[0] = 0x20;
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x00;
            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            msleep(1);
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x01;
            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

            //=====================================
            // Reset ADC
            //=====================================
            tmp_addr[3] = 0x80;
            tmp_addr[2] = 0x02;
            tmp_addr[1] = 0x00;
            tmp_addr[0] = 0x94;
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x00;
            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            msleep(1);
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x01;
            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            return true;
        } else {
            msleep(10);
#ifdef HX_RST_PIN_FUNC
            hx83112a_nf_ic_reset(false, false);
#endif
        }
    } while (cnt++ < 15);

    return false;
}

bool hx83112a_nf_enter_safe_mode(void)
{
    uint8_t cnt = 0;
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];

    tmp_addr[3] = 0x90;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x5C;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0xA5;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    msleep(20);

    do {
        //===========================================
        //  0x31 ==> 0x27
        //===========================================
        tmp_data[0] = 0x27;
        if (hx83112a_nf_bus_write(0x31, 1, tmp_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return false;
        }
        //===========================================
        //  0x32 ==> 0x95
        //===========================================
        tmp_data[0] = 0x95;
        if (hx83112a_nf_bus_write(0x32, 1, tmp_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return false;
        }

        //===========================================
        //  0x31 ==> 0x00
        //===========================================
        tmp_data[0] = 0x00;
        if (hx83112a_nf_bus_write(0x31, 1, tmp_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return false;
        }
        //msleep(10);
        //===========================================
        //  0x31 ==> 0x27
        //===========================================
        tmp_data[0] = 0x27;
        if (hx83112a_nf_bus_write(0x31, 1, tmp_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return false;
        }
        //===========================================
        //  0x32 ==> 0x95
        //===========================================
        tmp_data[0] = 0x95;
        if (hx83112a_nf_bus_write(0x32, 1, tmp_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return false;
        }

        // ======================
        // Check enter_save_mode
        // ======================
        tmp_addr[3] = 0x90;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0xA8;
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);

        TPD_DETAIL("%s: Check enter_save_mode data[0]=%X \n", __func__, tmp_data[0]);

        if (tmp_data[0] == 0x0C) {
            //=====================================
            // Reset TCON
            //=====================================
            tmp_addr[3] = 0x80;
            tmp_addr[2] = 0x02;
            tmp_addr[1] = 0x00;
            tmp_addr[0] = 0x20;
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x00;
            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            msleep(1);
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x01;
            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

            //=====================================
            // Reset ADC
            //=====================================
            tmp_addr[3] = 0x80;
            tmp_addr[2] = 0x02;
            tmp_addr[1] = 0x00;
            tmp_addr[0] = 0x94;
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x00;
            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            msleep(1);
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x01;
            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            return true;
        } else {
            msleep(10);
#ifdef HX_RST_PIN_FUNC
            hx83112a_nf_ic_reset(false, false);
#endif
        }
    } while (cnt++ < 20);

    return false;
}

void hx83112a_nf_interface_on(void)
{
    uint8_t tmp_data[5];
    uint8_t tmp_data2[2];
    int cnt = 0;

    //Read a dummy register to wake up I2C.
    if ( hx83112a_nf_bus_read(0x08, 4, tmp_data) < 0) { // to knock I2C
        TPD_INFO("%s: i2c access fail!\n", __func__);
        return;
    }

    do {
        //===========================================
        // Enable continuous burst mode : 0x13 ==> 0x31
        //===========================================
        tmp_data[0] = 0x31;
        if (hx83112a_nf_bus_write(0x13, 1, tmp_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return;
        }
        //===========================================
        // AHB address auto +4 : 0x0D ==> 0x11
        // Do not AHB address auto +4 : 0x0D ==> 0x10
        //===========================================
        tmp_data[0] = (0x10);
        if (hx83112a_nf_bus_write(0x0D, 1, tmp_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return;
        }

        // Check cmd
        hx83112a_nf_bus_read(0x13, 1, tmp_data);
        hx83112a_nf_bus_read(0x0D, 1, tmp_data2);

        if (tmp_data[0] == 0x31 && tmp_data2[0] == 0x10) {
            //isBusrtOn = true;
            break;
        }
        msleep(1);
    } while (++cnt < 10);

    if (cnt > 0) {
        TPD_DETAIL("%s:Polling burst mode: %d times", __func__, cnt);
    }
}

void hx83112a_nf_diag_register_set(uint8_t diag_command)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];

    TPD_INFO("diag_command = %d\n", diag_command );

    hx83112a_nf_interface_on();

    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x02;
    tmp_addr[1] = 0x04;
    tmp_addr[0] = 0xB4;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = diag_command;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
    TPD_INFO("%s: tmp_data[3] = 0x%02X, tmp_data[2] = 0x%02X, tmp_data[1] = 0x%02X, tmp_data[0] = 0x%02X!\n",
             __func__, tmp_data[3], tmp_data[2], tmp_data[1], tmp_data[0]);

}


bool hx83112a_nf_wait_wip(int Timing)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    uint8_t in_buffer[10];
    //uint8_t out_buffer[20];
    int retry_cnt = 0;

    //=====================================
    // SPI Transfer Format : 0x8000_0010 ==> 0x0002_0780
    //=====================================
    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x10;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x02;
    tmp_data[1] = 0x07;
    tmp_data[0] = 0x80;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    in_buffer[0] = 0x01;

    do {
        //=====================================
        // SPI Transfer Control : 0x8000_0020 ==> 0x4200_0003
        //=====================================
        tmp_addr[3] = 0x80;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0x20;
        tmp_data[3] = 0x42;
        tmp_data[2] = 0x00;
        tmp_data[1] = 0x00;
        tmp_data[0] = 0x03;
        hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

        //=====================================
        // SPI Command : 0x8000_0024 ==> 0x0000_0005
        // read 0x8000_002C for 0x01, means wait success
        //=====================================
        tmp_addr[3] = 0x80;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0x24;
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = 0x00;
        tmp_data[0] = 0x05;
        hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

        in_buffer[0] = in_buffer[1] = in_buffer[2] = in_buffer[3] = 0xFF;
        tmp_addr[3] = 0x80;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0x2C;
        hx83112a_nf_register_read(tmp_addr, 4, in_buffer, false);

        if ((in_buffer[0] & 0x01) == 0x00) {
            return true;
        }

        retry_cnt++;

        if (in_buffer[0] != 0x00 || in_buffer[1] != 0x00 || in_buffer[2] != 0x00 || in_buffer[3] != 0x00) {
            TPD_INFO("%s:Wait wip retry_cnt:%d, buffer[0]=%d, buffer[1]=%d, buffer[2]=%d, buffer[3]=%d \n", __func__,
                     retry_cnt, in_buffer[0], in_buffer[1], in_buffer[2], in_buffer[3]);
        }

        if (retry_cnt > 100) {
            TPD_INFO("%s: Wait wip error!\n", __func__);
            return false;
        }
        msleep(Timing);
    } while ((in_buffer[0] & 0x01) == 0x01);
    return true;
}


void hx83112a_nf_sense_on(uint8_t FlashMode)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    int retry = 0;

    TPD_DETAIL("Enter %s  \n", __func__);

    hx83112a_nf_interface_on();
    tmp_addr[3] = 0x90;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x5C;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0x00;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
    //msleep(20);

    if (!FlashMode) {
        //===AHBI2C_SystemReset==========
        tmp_addr[3] = 0x90;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0x18;
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = 0x00;
        tmp_data[0] = 0x55;
        hx83112a_nf_register_write(tmp_addr, 4, tmp_data, false);
    } else {
        do {
            tmp_addr[3] = 0x90;
            tmp_addr[2] = 0x00;
            tmp_addr[1] = 0x00;
            tmp_addr[0] = 0x98;
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x53;
            hx83112a_nf_register_write(tmp_addr, 4, tmp_data, false);

            tmp_addr[0] = 0xE4;
            hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);

            TPD_INFO("%s:Read status from IC = %X, %X\n", __func__, tmp_data[0], tmp_data[1]);
        } while ((tmp_data[1] != 0x01 || tmp_data[0] != 0x00) && retry++ < 5);

        if (retry >= 5) {
            TPD_INFO("%s: Fail:\n", __func__);

            //===AHBI2C_SystemReset==========
            tmp_addr[3] = 0x90;
            tmp_addr[2] = 0x00;
            tmp_addr[1] = 0x00;
            tmp_addr[0] = 0x18;
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x55;
            hx83112a_nf_register_write(tmp_addr, 4, tmp_data, false);
        } else {
            TPD_DETAIL("%s:OK and Read status from IC = %X, %X\n", __func__, tmp_data[0], tmp_data[1]);

            /* reset code*/
            tmp_data[0] = 0x00;
            if (hx83112a_nf_bus_write(0x31, 1, tmp_data) < 0) {
                TPD_INFO("%s: i2c access fail!\n", __func__);
            }
            if (hx83112a_nf_bus_write(0x32, 1, tmp_data) < 0) {
                TPD_INFO("%s: i2c access fail!\n", __func__);
            }

            tmp_addr[3] = 0x90;
            tmp_addr[2] = 0x00;
            tmp_addr[1] = 0x00;
            tmp_addr[0] = 0x98;
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x00;
            hx83112a_nf_register_write(tmp_addr, 4, tmp_data, false);
        }
    }
}

// from this
/**
 * hx83112a_nf_enable_interrupt -   Device interrupt ability control.
 * @chip_info: struct include i2c resource.
 * @enable: disable or enable control purpose.
 * Return  0: succeed, -1: failed.
 */
static int hx83112a_nf_enable_interrupt(struct chip_data_hx83112a_nf *chip_info, bool enable)
{
    TPD_INFO("%s enter, enable = %d.\n", __func__, enable);

    if (enable == true && hx83112a_nf_irq_en_cnt == 0) {
        enable_irq(chip_info->hx_irq);
        hx83112a_nf_irq_en_cnt = 1;
    } else if (enable == false && hx83112a_nf_irq_en_cnt == 1) {
        disable_irq_nosync(chip_info->hx_irq);
        hx83112a_nf_irq_en_cnt = 0;
    } else {
        TPD_DETAIL("irq is not pairing! enable= %d, cnt = %d\n", enable, hx83112a_nf_irq_en_cnt);
    }

    return 0;
}

#ifdef HX_ZERO_FLASH
struct hx83112a_nf_core_fp g_hx83112a_nf_core_fp;
struct hx83112a_nf_operation *hx83112a_nf_pzf_op = NULL;

bool hx83112a_nf_auto_update_flag = false;
int HX83112A_NF_POWERONOF = 1;

void hx83112a_nf_in_parse_assign_cmd(uint32_t addr, uint8_t *cmd, int len)
{
    switch (len) {
    case 1:
        cmd[0] = addr;
        break;

    case 2:
        cmd[0] = addr % 0x100;
        cmd[1] = (addr >> 8) % 0x100;
        break;

    case 4:
        cmd[0] = addr % 0x100;
        cmd[1] = (addr >> 8) % 0x100;
        cmd[2] = (addr >> 16) % 0x100;
        cmd[3] = addr / 0x1000000;
        break;

    default:
        TPD_INFO("%s: input length fault, len = %d!\n", __func__, len);
    }
}

void hx83112a_nf_update_dirly_0f(void)
{
    TPD_INFO("It will update fw after esd event in zero flash mode!\n");
    g_hx83112a_nf_core_fp.fp_0f_operation_dirly();
}

void hx83112a_nf_mcu_sys_reset(void)
{
    hx83112a_nf_register_write (hx83112a_nf_pzf_op->addr_system_reset,  4,  hx83112a_nf_pzf_op->data_system_reset,  false);
}

int hx83112a_nf_dis_rload_0f(int disable)
{
    /*Diable Flash Reload*/
    int retry = 10;
    int check_val = 0;
    uint8_t tmp_data[4] = {0};

    mdelay(100);
    TPD_DETAIL("%s: Entering !\n", __func__);

    do {
        hx83112a_nf_flash_write_burst(hx83112a_nf_pzf_op->addr_dis_flash_reload,  hx83112a_nf_pzf_op->data_dis_flash_reload);
        hx83112a_nf_register_read(hx83112a_nf_pzf_op->addr_dis_flash_reload, 4, tmp_data, false);
        TPD_DETAIL("Now data: tmp_data[3] = 0x%02X || tmp_data[2] = 0x%02X || tmp_data[1] = 0x%02X || tmp_data[0] = 0x%02X\n", tmp_data[3], tmp_data[2], tmp_data[1], tmp_data[0]);
        if( tmp_data[3] != 0x00 || tmp_data[2] != 0x00 || tmp_data[1] != 0x9A || tmp_data[0] != 0xA9) {
            TPD_INFO("Now data: tmp_data[3] = 0x%02X || tmp_data[2] = 0x%02X || tmp_data[1] = 0x%02X || tmp_data[0] = 0x%02X\n", tmp_data[3], tmp_data[2], tmp_data[1], tmp_data[0]);
            TPD_INFO("Not Same,Write Fail, there is %d retry times!\n", retry);
        } else {
            check_val = 1;
            TPD_INFO("It's same! Write success!\n");
        }
        msleep(5);
    } while(check_val == 0 && retry-- > 0);

    TPD_DETAIL("%s: END !\n", __func__);

    return check_val;
}

void hx83112a_nf_mcu_clean_sram_0f(uint8_t *addr, int write_len, int type)
{
    int total_read_times = 0;
    int max_bus_size = MAX_TRANS_SZ;
    int total_size_temp = 0;
    int total_size = 0;
    int address = 0;
    int i = 0;

    uint8_t fix_data = 0x00;
    uint8_t tmp_addr[4];
    uint8_t tmp_data[MAX_TRANS_SZ] = {0};

    TPD_DETAIL("%s, Entering \n", __func__);

    total_size = write_len;
    total_size_temp = write_len;

    if (total_size > MAX_TRANS_SZ) {
        max_bus_size = MAX_TRANS_SZ;
    }

    total_size_temp = write_len;

    hx83112a_nf_burst_enable(1);

    tmp_addr[3] = addr[3];
    tmp_addr[2] = addr[2];
    tmp_addr[1] = addr[1];
    tmp_addr[0] = addr[0];
    TPD_DETAIL("%s, write addr tmp_addr[3] = 0x%2.2X, tmp_addr[2] = 0x%2.2X, tmp_addr[1] = 0x%2.2X, tmp_addr[0] = 0x%2.2X\n", __func__, tmp_addr[3], tmp_addr[2], tmp_addr[1], tmp_addr[0]);

    switch (type) {
    case 0:
        fix_data = 0x00;
        break;
    case 1:
        fix_data = 0xAA;
        break;
    case 2:
        fix_data = 0xBB;
        break;
    }

    for (i = 0; i < MAX_TRANS_SZ; i++) {
        tmp_data[i] = fix_data;
    }

    TPD_DETAIL("%s, total size=%d\n", __func__, total_size);

    if (total_size_temp % max_bus_size == 0) {
        total_read_times = total_size_temp / max_bus_size;
    } else {
        total_read_times = total_size_temp / max_bus_size + 1;
    }

    for (i = 0; i < (total_read_times); i++) {
        /*TPD_DETAIL("[log]write %d time start!\n", i);*/
        if (total_size_temp >= max_bus_size) {
            hx83112a_nf_flash_write_burst_length(tmp_addr, tmp_data,  max_bus_size);
            total_size_temp = total_size_temp - max_bus_size;
        } else {
            TPD_DETAIL("last total_size_temp=%d\n", total_size_temp);
            hx83112a_nf_flash_write_burst_length(tmp_addr, tmp_data,  total_size_temp % max_bus_size);
        }
        address = ((i + 1) * max_bus_size);
        tmp_addr[1] = addr[1] + (uint8_t) ((address >> 8) & 0x00FF);
        tmp_addr[0] = addr[0] + (uint8_t) ((address) & 0x00FF);

        msleep (10);
    }

    TPD_DETAIL("%s, END \n", __func__);
}

void hx83112a_nf_mcu_write_sram_0f(const struct firmware *fw_entry, uint8_t *addr, int start_index, uint32_t write_len)
{
    int total_read_times = 0;
    int max_bus_size = MAX_TRANS_SZ;
    int total_size_temp = 0;
    int total_size = 0;
    int address = 0;
    int i = 0;

    uint8_t tmp_addr[4];
    uint32_t now_addr;

    TPD_DETAIL("%s, ---Entering \n", __func__);

    total_size = fw_entry->size;

    total_size_temp = write_len;
    if (write_len > 49152) {
        max_bus_size = 49152;
    } else {
        max_bus_size = write_len;
    }
    hx83112a_nf_burst_enable(1);

    tmp_addr[3] = addr[3];
    tmp_addr[2] = addr[2];
    tmp_addr[1] = addr[1];
    tmp_addr[0] = addr[0];

    TPD_DETAIL("%s, write addr tmp_addr[3] = 0x%2.2X, tmp_addr[2] = 0x%2.2X, tmp_addr[1] = 0x%2.2X, tmp_addr[0] = 0x%2.2X\n", __func__, tmp_addr[3], tmp_addr[2], tmp_addr[1], tmp_addr[0]);
    
    now_addr = (addr[3] << 24) + (addr[2] << 16) + (addr[1] << 8) + addr[0];
    TPD_DETAIL("now addr= 0x%08X\n", now_addr);

    TPD_DETAIL("%s,  total size=%d\n", __func__, total_size);

    if (hx83112a_nf_chip_info->tmp_data == NULL) {
        hx83112a_nf_chip_info->tmp_data = kzalloc (sizeof (uint8_t) * firmware_update_space, GFP_KERNEL);
        if (hx83112a_nf_chip_info->tmp_data == NULL) {
            TPD_INFO("%s, alloc hx83112a_nf_chip_info->tmp_data failed\n", __func__);
            return;
        }
    }
    memcpy (hx83112a_nf_chip_info->tmp_data, fw_entry->data, total_size);
    if (total_size_temp % max_bus_size == 0) {
        total_read_times = total_size_temp / max_bus_size;
    } else {
        total_read_times = total_size_temp / max_bus_size + 1;
    }

    for (i = 0; i < (total_read_times); i++) {
        if (total_size_temp >= max_bus_size) {
            hx83112a_nf_flash_write_burst_length(tmp_addr, &(hx83112a_nf_chip_info->tmp_data[start_index + i * max_bus_size]),  max_bus_size);
            total_size_temp = total_size_temp - max_bus_size;
        } else {
            TPD_DETAIL("last total_size_temp=%d\n", total_size_temp);
            hx83112a_nf_flash_write_burst_length(tmp_addr, &(hx83112a_nf_chip_info->tmp_data[start_index + i * max_bus_size]),  total_size_temp % max_bus_size);
        }

        address = ((i + 1) * max_bus_size);
        tmp_addr[0] = addr[0] + (uint8_t) ((address) & 0x00FF);

        if (tmp_addr[0] < addr[0]) {
            tmp_addr[1] = addr[1] + (uint8_t) ((address >> 8) & 0x00FF) + 1;
        } else {
            tmp_addr[1] = addr[1] + (uint8_t) ((address >> 8) & 0x00FF);
        }


        udelay (100);
    }
    TPD_DETAIL("%s, ----END \n", __func__);
    memset(hx83112a_nf_chip_info->tmp_data, 0, total_size);
}

int hx83112a_nf_sram_write_crc_check(const struct firmware *fw_entry, uint8_t *addr, int strt_idx, uint32_t len)
{
    int retry = 0;
    int crc = -1;

    do {
        g_hx83112a_nf_core_fp.fp_write_sram_0f(fw_entry, addr, strt_idx, len);
        crc = hx83112a_nf_hw_check_CRC (hx83112a_nf_pzf_op->data_sram_start_addr, HX_48K_SZ);
        retry++;
    } while (crc != 0 && retry < 10);

    return crc;
}
static int hx83112a_nf_mcu_Calculate_CRC_with_AP(unsigned char *FW_content, int CRC_from_FW, int len)
{
    int i, j, length = 0;
    int fw_data;
    int fw_data_2;
    int CRC = 0xFFFFFFFF;
    int PolyNomial = 0x82F63B78;

    length = len / 4;

    for (i = 0; i < length; i++) {
        fw_data = FW_content[i * 4];

        for (j = 1; j < 4; j++) {
            fw_data_2 = FW_content[i * 4 + j];
            fw_data += (fw_data_2) << (8 * j);
        }

        CRC = fw_data ^ CRC;

        for (j = 0; j < 32; j++) {
            if ((CRC % 2) != 0) {
                CRC = ((CRC >> 1) & 0x7FFFFFFF) ^ PolyNomial;
            } else {
                CRC = (((CRC >> 1) & 0x7FFFFFFF) /*& 0x7FFFFFFF*/);
            }
        }
    }

    return CRC;
}

bool hx83112a_nf_parse_bin_cfg_data(const struct firmware *fw_entry)
{
    int part_num = 0;
    int i = 0;
    uint8_t buf[16];
    int i_max = 0;
    int i_min = 0;
    uint32_t dsram_base = 0xFFFFFFFF;
    uint32_t dsram_max = 0;
    struct hx83112a_nf_info *hx83112a_nf_info_arr;

    /*1. get number of partition*/
    part_num = fw_entry->data[HX64K + 12];
    TPD_INFO("%s, Number of partition is %d\n", __func__, part_num);
    if (part_num <= 1) {
        TPD_INFO("%s, size of cfg part failed! part_num = %d\n", __func__, part_num);
        return false;
    }
    /*2. initial struct of array*/
    hx83112a_nf_info_arr = kzalloc(part_num * sizeof(struct hx83112a_nf_info), GFP_KERNEL);
    if (hx83112a_nf_info_arr == NULL) {
        TPD_INFO("%s, Allocate ZF info array failed!\n", __func__);
        return false;
    }

    for (i = 0; i < part_num; i++) {
        /*3. get all partition*/
        memcpy(buf, &fw_entry->data[i * 0x10 + HX64K], 16);
        memcpy(hx83112a_nf_info_arr[i].sram_addr, buf, 4);
        hx83112a_nf_info_arr[i].write_size = buf[5] << 8 | buf[4];
        hx83112a_nf_info_arr[i].fw_addr = buf[9] << 8 | buf[8];
        hx83112a_nf_info_arr[i].cfg_addr = hx83112a_nf_info_arr[i].sram_addr[0];
        hx83112a_nf_info_arr[i].cfg_addr += hx83112a_nf_info_arr[i].sram_addr[1] << 8;
        hx83112a_nf_info_arr[i].cfg_addr += hx83112a_nf_info_arr[i].sram_addr[2] << 16;
        hx83112a_nf_info_arr[i].cfg_addr += hx83112a_nf_info_arr[i].sram_addr[3] << 24;

        if (i == 0)
            continue;
        if (dsram_base > hx83112a_nf_info_arr[i].cfg_addr) {
            dsram_base = hx83112a_nf_info_arr[i].cfg_addr;
            i_min = i;
        } else if (dsram_max < hx83112a_nf_info_arr[i].cfg_addr) {
            dsram_max = hx83112a_nf_info_arr[i].cfg_addr;
            i_max = i;
        }
    }
    for (i = 0; i < 4; i++)
        hx83112a_nf_sram_min[i] = hx83112a_nf_info_arr[i_min].sram_addr[i];
    hx83112a_nf_cfg_sz = (dsram_max - dsram_base) + hx83112a_nf_info_arr[i_max].write_size;
    hx83112a_nf_cfg_sz = hx83112a_nf_cfg_sz + (hx83112a_nf_cfg_sz % 16);

    if (hx83112a_nf_FW_buf == NULL)
        hx83112a_nf_FW_buf = kzalloc(sizeof(unsigned char) * FW_BIN_16K_SZ, GFP_KERNEL);

    for (i = 1; i < part_num; i++)
        memcpy(hx83112a_nf_FW_buf + (hx83112a_nf_info_arr[i].cfg_addr - dsram_base), (unsigned char *)&fw_entry->data[hx83112a_nf_info_arr[i].fw_addr], hx83112a_nf_info_arr[i].write_size);

    hx83112a_nf_cfg_crc = hx83112a_nf_mcu_Calculate_CRC_with_AP(hx83112a_nf_FW_buf, 0, hx83112a_nf_cfg_sz);
    
    kfree(hx83112a_nf_info_arr);
    return true;
}

static int hx83112a_nf_part_info(const struct firmware *fw_entry)
{
    struct timespec64 timeStart, timeEnd, timeDelta;
    int retry = 0;
    int crc = -1;
    bool ret = false;

    if (!hx83112a_nf_parse_bin_cfg_data(fw_entry))
        TPD_INFO("%s, Parse cfg from bin failed\n", __func__);
    
    hx83112a_nf_register_write(hx83112a_nf_pzf_op->addr_system_reset, 4, hx83112a_nf_pzf_op->data_system_reset, false);
    hx83112a_nf_enter_safe_mode();
    
    ktime_get_real_ts64(&timeStart);
    /* first 48K */

    hx83112a_nf_sram_write_crc_check(fw_entry, hx83112a_nf_pzf_op->data_sram_start_addr, 0, HX_48K_SZ);
    crc = hx83112a_nf_hw_check_CRC(hx83112a_nf_pzf_op->data_sram_start_addr, HX_48K_SZ);

    ret = (crc == 0) ? true : false;
    if (crc != 0)
        TPD_INFO("48k CRC Failed! CRC = %X", crc);

    do {

        hx83112a_nf_mcu_register_write(hx83112a_nf_sram_min, hx83112a_nf_cfg_sz, hx83112a_nf_FW_buf, 0);
        crc = hx83112a_nf_hw_check_CRC(hx83112a_nf_sram_min, hx83112a_nf_cfg_sz);

        if (crc != hx83112a_nf_cfg_crc)
            TPD_INFO("Config CRC FAIL, HW CRC = %X, SW CRC = %X, retry time = %d", crc, hx83112a_nf_cfg_crc, retry);

        retry++;
    } while (!ret && retry < 10);
    if (HX83112A_NF_POWERONOF == 1)
        g_hx83112a_nf_core_fp.fp_write_sram_0f(fw_entry, hx83112a_nf_pzf_op->data_mode_switch, 0xC33C, 4);
    else
        g_hx83112a_nf_core_fp.fp_clean_sram_0f(hx83112a_nf_pzf_op->data_mode_switch, 4, 2);

    ktime_get_real_ts64(&timeEnd);
    
    timeDelta.tv_nsec = (timeEnd.tv_sec * 1000000000 + timeEnd.tv_nsec) - (timeStart.tv_sec * 1000000000 + timeStart.tv_nsec);
    
    TPD_INFO("Update firmware time = %ld us\n", timeDelta.tv_nsec / 1000);
    return 0;
}

void hx83112a_nf_mcu_firmware_update_0f(const struct firmware *fw_entry)
{
    int retry = 0;
    int crc = -1;
    int ret = 0;
    uint8_t temp_addr[4];
    uint8_t temp_data[4];
    struct firmware *request_fw_headfile = NULL;
    const struct firmware *tmp_fw_entry = NULL;
    bool reload = false;

    TPD_DETAIL("%s, Entering \n", __func__);

fw_reload:
    if (fw_entry == NULL || reload) {
        TPD_INFO("Get FW from headfile\n");
        if (request_fw_headfile == NULL) {
            request_fw_headfile = kzalloc(sizeof(struct firmware), GFP_KERNEL);
        }
        if(request_fw_headfile == NULL) {
            TPD_INFO("%s kzalloc failed!\n", __func__);
            return;
        }
        if (hx83112a_nf_chip_info->g_fw_sta) {
            TPD_INFO("request firmware failed, get from g_fw_buf\n");
            request_fw_headfile->size = hx83112a_nf_chip_info->g_fw_len;
            request_fw_headfile->data = hx83112a_nf_chip_info->g_fw_buf;
            tmp_fw_entry = request_fw_headfile;

        } else {
            TPD_INFO("request firmware failed, get from headfile\n");
            if(hx83112a_nf_chip_info->p_firmware_headfile->firmware_data) {
                request_fw_headfile->size = hx83112a_nf_chip_info->p_firmware_headfile->firmware_size;
                request_fw_headfile->data = hx83112a_nf_chip_info->p_firmware_headfile->firmware_data;
                tmp_fw_entry = request_fw_headfile;
                hx83112a_nf_chip_info->using_headfile = true;
            } else {
                TPD_INFO("firmware_data is NULL! exit firmware update!\n");
                if(request_fw_headfile != NULL) {
                    kfree(request_fw_headfile);
                    request_fw_headfile = NULL;
                }
                return;
            }
        }
    } else {
        tmp_fw_entry = fw_entry;
    }

    if ((int)tmp_fw_entry->size > HX64K) {
        ret = hx83112a_nf_part_info(tmp_fw_entry);
    } else {
        
        hx83112a_nf_register_write(hx83112a_nf_pzf_op->addr_system_reset, 4, hx83112a_nf_pzf_op->data_system_reset, false);
        hx83112a_nf_enter_safe_mode();
        /* first 48K */
        do {
            g_hx83112a_nf_core_fp.fp_write_sram_0f (tmp_fw_entry, hx83112a_nf_pzf_op->data_sram_start_addr, 0, HX_48K_SZ);
            crc = hx83112a_nf_hw_check_CRC (hx83112a_nf_pzf_op->data_sram_start_addr,  HX_48K_SZ);

            temp_addr[3] = 0x08;
            temp_addr[2] = 0x00;
            temp_addr[1] = 0xBF;
            temp_addr[0] = 0xFC;
            hx83112a_nf_register_read(temp_addr, 4, temp_data, false);

            if (crc == 0) {
                TPD_DETAIL("%s, HW CRC OK in %d time \n", __func__, retry);
                break;
            }
            else {
                TPD_INFO("%s, HW CRC FAIL in %d time !\n", __func__, retry);
            }
            retry++;
        } while (((temp_data[3] == 0 && temp_data[2] == 0 && temp_data[1] == 0 && temp_data[0] == 0 && retry < 80) || (crc != 0 && retry < 30)) && !(hx83112a_nf_chip_info->using_headfile && retry < 3));

        if (crc != 0) {
            TPD_INFO("Last time CRC Fail!\n");
            if(reload) {
                return;
            } 
            else {
                reload = true;
                goto fw_reload;
            }
        }

        /* if HX83112A_NF_POWERONOF!= 1, it will be clean mode! */
        /*config and setting */
        /*config info*/
        if (HX83112A_NF_POWERONOF == 1) {
            retry = 0;
            do {
                g_hx83112a_nf_core_fp.fp_write_sram_0f(tmp_fw_entry, hx83112a_nf_pzf_op->data_cfg_info, 0xC000, 128);//132
                crc = hx83112a_nf_hw_check_CRC(hx83112a_nf_pzf_op->data_cfg_info, 128);
                if (crc == 0) {
                    TPD_DETAIL("%s, config info ok in %d time \n", __func__, retry);
                    break;
                } 
                else {
                    TPD_INFO("%s, config info fail in %d time !\n", __func__, retry);
                }
                retry++;
            } while ((crc != 0 && retry < 30) || (hx83112a_nf_chip_info->using_headfile && retry < 15));

            if (crc != 0) {
                TPD_INFO("config info CRC Fail!\n");
                if (!reload) {
                    reload = true;
                    goto fw_reload;
                }
            }
        } 
        else {
            g_hx83112a_nf_core_fp.fp_clean_sram_0f(hx83112a_nf_pzf_op->data_cfg_info, 128, 2);
        }
        /*FW config*/
        if (HX83112A_NF_POWERONOF == 1) {
            retry = 0;
            do {
                g_hx83112a_nf_core_fp.fp_write_sram_0f(tmp_fw_entry, hx83112a_nf_pzf_op->data_fw_cfg_p1, 0xC100, 528);//482
                crc = hx83112a_nf_hw_check_CRC(hx83112a_nf_pzf_op->data_fw_cfg_p1, 528);
                if (crc == 0) {
                    TPD_DETAIL("%s, 1 FW config ok in %d time \n", __func__, retry);
                    break;
                } 
                else {
                    TPD_INFO("%s, 1 FW config fail in %d time !\n", __func__, retry);
                }
                retry++;
            } while ((crc != 0 && retry < 30) || (hx83112a_nf_chip_info->using_headfile && retry < 15));

            if (crc != 0) {
                TPD_INFO("1 FW config CRC Fail!\n");
                if (!reload) {
                    reload = true;
                    goto fw_reload;
                }
            }
        } 
        else {
            g_hx83112a_nf_core_fp.fp_clean_sram_0f(hx83112a_nf_pzf_op->data_fw_cfg_p1, 528, 1);
        }

        if (HX83112A_NF_POWERONOF == 1) {
            retry = 0;
            do {
                g_hx83112a_nf_core_fp.fp_write_sram_0f(tmp_fw_entry, hx83112a_nf_pzf_op->data_fw_cfg_p3, 0xCA00, 128);
                crc = hx83112a_nf_hw_check_CRC(hx83112a_nf_pzf_op->data_fw_cfg_p3, 128);
                if (crc == 0) {
                    TPD_DETAIL("%s, 3 FW config ok in %d time \n", __func__, retry);
                    break;
                } 
                else {
                    TPD_INFO("%s, 3 FW config fail in %d time !\n", __func__, retry);
                }
                retry++;
            } while ((crc != 0 && retry < 30) || (hx83112a_nf_chip_info->using_headfile && retry < 15));

            if (crc != 0) {
                TPD_INFO("3 FW config CRC Fail!\n");
                if (!reload) {
                    reload = true;
                    goto fw_reload;
                }
            }
        } 
        else {
            g_hx83112a_nf_core_fp.fp_clean_sram_0f(hx83112a_nf_pzf_op->data_fw_cfg_p3, 128, 1);
        }

        /*ADC config*/
        if (HX83112A_NF_POWERONOF == 1) {
            retry = 0;
            do {
                g_hx83112a_nf_core_fp.fp_write_sram_0f(tmp_fw_entry, hx83112a_nf_pzf_op->data_adc_cfg_1, 0xD640, 1200);
                crc = hx83112a_nf_hw_check_CRC(hx83112a_nf_pzf_op->data_adc_cfg_1, 1200);
                if (crc == 0) {
                    TPD_DETAIL("%s, 1 ADC config ok in %d time \n", __func__, retry);
                    break;
                } else {
                    TPD_DETAIL("%s, 1 ADC config fail in %d time !\n", __func__, retry);
                }
                retry++;
            } while ((crc != 0 && retry < 30) || (hx83112a_nf_chip_info->using_headfile && retry < 15));

            if (crc != 0) {
                TPD_INFO("1 ADC config CRC Fail!\n");
                if (!reload) {
                    reload = true;
                    goto fw_reload;
                }
            }
        } 
        else {
            g_hx83112a_nf_core_fp.fp_clean_sram_0f(hx83112a_nf_pzf_op->data_adc_cfg_1, 1200, 2);
        }

        if (HX83112A_NF_POWERONOF == 1) {
            retry = 0;
            do {
                g_hx83112a_nf_core_fp.fp_write_sram_0f(tmp_fw_entry, hx83112a_nf_pzf_op->data_adc_cfg_2, 0xD320, 800);
                crc = hx83112a_nf_hw_check_CRC(hx83112a_nf_pzf_op->data_adc_cfg_2, 800);
                if (crc == 0) {
                    TPD_DETAIL("%s, 2 ADC config ok in %d time \n", __func__, retry);
                    break;
                } 
                else {
                    TPD_INFO("%s, 2 ADC config fail in %d time !\n", __func__, retry);
                }
                retry++;
            } while ((crc != 0 && retry < 30) || (hx83112a_nf_chip_info->using_headfile && retry < 15));

            if (crc != 0) {
                TPD_INFO("2 ADC config CRC Fail!\n");
                if (!reload) {
                    reload = true;
                    goto fw_reload;
                }
            }
        } 
        else {
            g_hx83112a_nf_core_fp.fp_clean_sram_0f(hx83112a_nf_pzf_op->data_adc_cfg_2, 800, 2);
        }

        /*mapping table*/
        if (HX83112A_NF_POWERONOF == 1) {
            retry = 0;
            do {
                g_hx83112a_nf_core_fp.fp_write_sram_0f(tmp_fw_entry, hx83112a_nf_pzf_op->data_map_table, 0xE000, 1536);
                crc = hx83112a_nf_hw_check_CRC(hx83112a_nf_pzf_op->data_map_table, 1536);
                if (crc == 0) {
                    TPD_DETAIL("%s, mapping table ok in %d time \n", __func__, retry);
                    break;
                } 
                else {
                    TPD_INFO("%s, mapping table fail in %d time !\n", __func__, retry);
                }
                retry++;
            } while ((crc != 0 && retry < 30) || (hx83112a_nf_chip_info->using_headfile && retry < 15));

            if (crc != 0) {
                TPD_INFO("mapping table CRC Fail!\n");
                if (!reload) {
                    reload = true;
                    goto fw_reload;
                }
            }
        } 
        else {
            g_hx83112a_nf_core_fp.fp_clean_sram_0f(hx83112a_nf_pzf_op->data_map_table, 1536, 2);
        }
    }

    /* switch mode*/
    if (HX83112A_NF_POWERONOF == 1) {
        g_hx83112a_nf_core_fp.fp_write_sram_0f(tmp_fw_entry, hx83112a_nf_pzf_op->data_mode_switch, 0xC30C, 4);
    } 
    else {
        g_hx83112a_nf_core_fp.fp_clean_sram_0f(hx83112a_nf_pzf_op->data_mode_switch, 4, 2);
    }

    hx83112a_nf_fw_check(hx83112a_nf_pri_ts->chip_data, &hx83112a_nf_pri_ts->resolution_info, &hx83112a_nf_pri_ts->panel_data);

    if (request_fw_headfile != NULL) {
        kfree(request_fw_headfile);
        request_fw_headfile = NULL;
    }
    TPD_DETAIL("%s, END \n", __func__);
}
int hx83112a_nf_0f_op_file_dirly(char *file_name)
{
    int err = NO_ERR;
    const struct firmware *fw_entry = NULL;


    TPD_INFO("%s, Entering \n", __func__);
    TPD_INFO("file name = %s\n", file_name);
    if (hx83112a_nf_pri_ts->fw_update_app_support) {
        err = request_firmware_select(&fw_entry, file_name, hx83112a_nf_pri_ts->dev);
    } else {
        err = request_firmware(&fw_entry, file_name, hx83112a_nf_pri_ts->dev);
    }
    if (err < 0) {
        TPD_INFO("%s, fail in line%d error code=%d, file maybe fail\n", __func__, __LINE__, err);
        return err;
    }

    if(hx83112a_nf_f_0f_updat == 1) {
        TPD_INFO("%s: [Warning]Other thread is updating now!\n", __func__);
        release_firmware(fw_entry);
        err = -1;
        return err;
    } else {
        TPD_INFO("%s: Entering Update Flow!\n", __func__);
        hx83112a_nf_f_0f_updat = 1;
    }

    hx83112a_nf_enable_interrupt(hx83112a_nf_chip_info, false);

    /* trigger reset */
    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, false); // reset gpio
    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, true); // reset gpio

    g_hx83112a_nf_core_fp.fp_firmware_update_0f (fw_entry);
    release_firmware(fw_entry);

    hx83112a_nf_f_0f_updat = 0;
    TPD_INFO("%s, END \n", __func__);
    return err;
}
int hx83112a_nf_mcu_0f_operation_dirly(void)
{
    int err = NO_ERR;

    TPD_DETAIL("%s, Entering \n", __func__);

    if(hx83112a_nf_f_0f_updat == 1) {
        TPD_INFO("%s:[Warning]Other thread is updating now!\n", __func__);
        err = -1;
        return err;
    } else {
        TPD_INFO("%s: Entering Update Flow!\n", __func__);
        hx83112a_nf_f_0f_updat = 1;
    }

    g_hx83112a_nf_core_fp.fp_firmware_update_0f(NULL);

    hx83112a_nf_f_0f_updat = 0;
    TPD_DETAIL("%s, END \n", __func__);
    return err;
}

void hx83112a_nf_mcu_0f_operation(struct work_struct *work)
{
    TPD_INFO("%s, Entering \n", __func__);
    TPD_INFO("TP firmware has been requested.\n");


    if (hx83112a_nf_f_0f_updat == 1) {
        TPD_INFO("%s:[Warning]Other thread is updating now!\n", __func__);
        return ;
    } else {
        TPD_INFO("%s:Entering Update Flow!\n", __func__);
        hx83112a_nf_f_0f_updat = 1;
    }

    hx83112a_nf_enable_interrupt(hx83112a_nf_chip_info, false);

    /* trigger reset */
    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, false); // reset gpio
    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, true); // reset gpio

    g_hx83112a_nf_core_fp.fp_firmware_update_0f(NULL);

    g_hx83112a_nf_core_fp.fp_reload_disable(0);

    msleep (10);
    hx83112a_nf_read_FW_ver();
    msleep (10);
    hx83112a_nf_sense_on(0x00);
    msleep (10);
    TPD_INFO("%s:End \n", __func__);

    hx83112a_nf_enable_interrupt(hx83112a_nf_chip_info, true);

    hx83112a_nf_f_0f_updat = 0;
    TPD_INFO("%s, END \n", __func__);
    return ;
}

#ifdef HX_0F_DEBUG
void hx83112a_nf_mcu_read_sram_0f(const struct firmware *fw_entry, uint8_t *addr, int start_index, int read_len)
{
    int total_read_times = 0;
    int max_bus_size = MAX_TRANS_SZ;
    int total_size_temp = 0;
    int total_size = 0;
    int address = 0;
    int i = 0, j = 0;
    int not_same = 0;

    uint8_t tmp_addr[4];
    uint8_t *temp_info_data;
    int *not_same_buff;

    TPD_INFO("%s, Entering \n", __func__);

    hx83112a_nf_burst_enable(1);

    total_size = read_len;

    total_size_temp = read_len;
    if (read_len > 2048) {
        max_bus_size = 2048;
    } else {
        max_bus_size = read_len;
    }
    temp_info_data = kzalloc (sizeof (uint8_t) * total_size, GFP_KERNEL);
    not_same_buff = kzalloc (sizeof (int) * total_size, GFP_KERNEL);


    tmp_addr[3] = addr[3];
    tmp_addr[2] = addr[2];
    tmp_addr[1] = addr[1];
    tmp_addr[0] = addr[0];
    TPD_INFO("%s, read addr tmp_addr[3] = 0x%2.2X, tmp_addr[2] = 0x%2.2X, tmp_addr[1] = 0x%2.2X, tmp_addr[0] = 0x%2.2X\n", __func__, tmp_addr[3], tmp_addr[2], tmp_addr[1], tmp_addr[0]);

    TPD_INFO("%s, total size=%d\n", __func__, total_size);

    hx83112a_nf_burst_enable(1);

    if (total_size % max_bus_size == 0) {
        total_read_times = total_size / max_bus_size;
    } else {
        total_read_times = total_size / max_bus_size + 1;
    }

    for (i = 0; i < (total_read_times); i++) {
        if (total_size_temp >= max_bus_size) {
            hx83112a_nf_register_read(tmp_addr, max_bus_size, &temp_info_data[i * max_bus_size], false);
            total_size_temp = total_size_temp - max_bus_size;
        } else {
            hx83112a_nf_register_read(tmp_addr, total_size_temp % max_bus_size, &temp_info_data[i * max_bus_size], false);
        }

        address = ((i + 1) * max_bus_size);
        tmp_addr[0] = addr[0] + (uint8_t) ((address) & 0x00FF);
        if (tmp_addr[0] < addr[0]) {
            tmp_addr[1] = addr[1] + (uint8_t) ((address >> 8) & 0x00FF) + 1;
        } else {
            tmp_addr[1] = addr[1] + (uint8_t) ((address >> 8) & 0x00FF);
        }

        msleep (10);
    }
    TPD_INFO("%s, READ Start \n", __func__);
    TPD_INFO("%s, start_index = %d \n", __func__, start_index);
    j = start_index;
    for (i = 0; i < read_len; i++, j++) {
        if (hx83112a_nf_chip_info->g_fw_buf[j] != temp_info_data[i]) {
            not_same++;
            not_same_buff[i] = 1;
        }

        TPD_INFO("0x%2.2X, ", temp_info_data[i]);

        if (i > 0 && i % 16 == 15) {
            printk ("\n");
        }
    }
    TPD_INFO("%s, READ END \n", __func__);
    TPD_INFO("%s, Not Same count=%d\n", __func__, not_same);
    if (not_same != 0) {
        j = start_index;
        for (i = 0; i < read_len; i++, j++) {
            if (not_same_buff[i] == 1) {
                TPD_INFO("bin = [%d] 0x%2.2X\n", i, hx83112a_nf_chip_info->g_fw_buf[j]);
            }
        }
        for (i = 0; i < read_len; i++, j++) {
            if (not_same_buff[i] == 1) {
                TPD_INFO("sram = [%d] 0x%2.2X \n", i, temp_info_data[i]);
            }
        }
    }
    TPD_INFO("%s, READ END \n", __func__);
    TPD_INFO("%s, Not Same count=%d\n", __func__, not_same);
    TPD_INFO("%s, END \n", __func__);

    kfree (not_same_buff);
    kfree (temp_info_data);
}

void hx83112a_nf_mcu_read_all_sram(uint8_t *addr, int read_len)
{
    int total_read_times = 0;
    int max_bus_size = MAX_RECVS_SZ;
    int total_size_temp = 0;
    int total_size = 0;
    int address = 0;
    int i = 0;

    uint8_t tmp_addr[4];
    uint8_t *temp_info_data;

    TPD_INFO("%s, Entering \n", __func__);

    hx83112a_nf_burst_enable(1);

    total_size = read_len;

    total_size_temp = read_len;

    temp_info_data = kzalloc (sizeof (uint8_t) * total_size, GFP_KERNEL);


    tmp_addr[3] = addr[3];
    tmp_addr[2] = addr[2];
    tmp_addr[1] = addr[1];
    tmp_addr[0] = addr[0];

    if (total_size % max_bus_size == 0) {
        total_read_times = total_size / max_bus_size;
    } else {
        total_read_times = total_size / max_bus_size + 1;
    }

    for (i = 0; i < (total_read_times); i++) {
        if (total_size_temp >= max_bus_size) {
            hx83112a_nf_register_read(tmp_addr, max_bus_size, &temp_info_data[i * max_bus_size], false);
            total_size_temp = total_size_temp - max_bus_size;
        } else {
            hx83112a_nf_register_read(tmp_addr, total_size_temp % max_bus_size, &temp_info_data[i * max_bus_size], false);
        }

        address = ((i + 1) * max_bus_size);
        tmp_addr[1] = addr[1] + (uint8_t) ((address >> 8) & 0x00FF);
        tmp_addr[0] = addr[0] + (uint8_t) ((address) & 0x00FF);

        msleep (10);
    }
    kfree (temp_info_data);
}

void hx83112a_nf_mcu_firmware_read_0f(const struct firmware *fw_entry, int type)
{
    uint8_t tmp_addr[4] = {0};

    TPD_INFO("%s, Entering \n", __func__);
    if (type == 0) { /* first 48K */
        g_hx83112a_nf_core_fp.fp_read_sram_0f (fw_entry, hx83112a_nf_pzf_op->data_sram_start_addr, 0, HX_48K_SZ);
        g_hx83112a_nf_core_fp.fp_read_all_sram (tmp_addr, 0xC000);
    } else { /*last 16k*/
        g_hx83112a_nf_core_fp.fp_read_sram_0f (fw_entry, hx83112a_nf_pzf_op->data_cfg_info, 0xC000, 132);
        g_hx83112a_nf_core_fp.fp_read_sram_0f (fw_entry, hx83112a_nf_pzf_op->data_fw_cfg, 0xC0FE, 512);
        g_hx83112a_nf_core_fp.fp_read_sram_0f (fw_entry, hx83112a_nf_pzf_op->data_adc_cfg_1, 0xD000, 376);
        g_hx83112a_nf_core_fp.fp_read_sram_0f (fw_entry, hx83112a_nf_pzf_op->data_adc_cfg_2, 0xD178, 376);
        g_hx83112a_nf_core_fp.fp_read_sram_0f (fw_entry, hx83112a_nf_pzf_op->data_adc_cfg_3, 0xD000, 376);
        g_hx83112a_nf_core_fp.fp_read_all_sram (hx83112a_nf_pzf_op->data_sram_clean, HX_32K_SZ);
    }
    TPD_INFO("%s, END \n", __func__);
}

void hx83112a_nf_mcu_0f_operation_check(int type)
{


    TPD_INFO("%s, Entering \n", __func__);

    TPD_INFO("first 4 bytes 0x%2X, 0x%2X, 0x%2X, 0x%2X !\n", hx83112a_nf_chip_info->g_fw_buf[0], hx83112a_nf_chip_info->g_fw_buf[1], hx83112a_nf_chip_info->g_fw_buf[2], hx83112a_nf_chip_info->g_fw_buf[3]);
    TPD_INFO("next 4 bytes 0x%2X, 0x%2X, 0x%2X, 0x%2X !\n", hx83112a_nf_chip_info->g_fw_buf[4], hx83112a_nf_chip_info->g_fw_buf[5], hx83112a_nf_chip_info->g_fw_buf[6], hx83112a_nf_chip_info->g_fw_buf[7]);
    TPD_INFO("and next 4 bytes 0x%2X, 0x%2X, 0x%2X, 0x%2X !\n", hx83112a_nf_chip_info->g_fw_buf[8], hx83112a_nf_chip_info->g_fw_buf[9], hx83112a_nf_chip_info->g_fw_buf[10], hx83112a_nf_chip_info->g_fw_buf[11]);

    g_hx83112a_nf_core_fp.fp_firmware_read_0f(NULL, type);

    TPD_INFO("%s, END \n", __func__);
    return ;
}
#endif


int hx83112a_nf_0f_init(void)
{
    hx83112a_nf_pzf_op = kzalloc(sizeof(struct hx83112a_nf_operation), GFP_KERNEL);

    g_hx83112a_nf_core_fp.fp_reload_disable = hx83112a_nf_dis_rload_0f;
    g_hx83112a_nf_core_fp.fp_sys_reset = hx83112a_nf_mcu_sys_reset;
    g_hx83112a_nf_core_fp.fp_clean_sram_0f = hx83112a_nf_mcu_clean_sram_0f;
    g_hx83112a_nf_core_fp.fp_write_sram_0f = hx83112a_nf_mcu_write_sram_0f;
    g_hx83112a_nf_core_fp.fp_firmware_update_0f = hx83112a_nf_mcu_firmware_update_0f;
    g_hx83112a_nf_core_fp.fp_0f_operation = hx83112a_nf_mcu_0f_operation;
    g_hx83112a_nf_core_fp.fp_0f_operation_dirly = hx83112a_nf_mcu_0f_operation_dirly;
    g_hx83112a_nf_core_fp.fp_0f_op_file_dirly = hx83112a_nf_0f_op_file_dirly;
#ifdef HX_0F_DEBUG
    g_hx83112a_nf_core_fp.fp_read_sram_0f = hx83112a_nf_mcu_read_sram_0f;
    g_hx83112a_nf_core_fp.fp_read_all_sram = hx83112a_nf_mcu_read_all_sram;
    g_hx83112a_nf_core_fp.fp_firmware_read_0f = hx83112a_nf_mcu_firmware_read_0f;
    g_hx83112a_nf_core_fp.fp_0f_operation_check = hx83112a_nf_mcu_0f_operation_check;
#endif

    hx83112a_nf_in_parse_assign_cmd(zf_addr_dis_flash_reload, hx83112a_nf_pzf_op->addr_dis_flash_reload, sizeof(hx83112a_nf_pzf_op->addr_dis_flash_reload));
    hx83112a_nf_in_parse_assign_cmd(zf_data_dis_flash_reload, hx83112a_nf_pzf_op->data_dis_flash_reload, sizeof(hx83112a_nf_pzf_op->data_dis_flash_reload));
    hx83112a_nf_in_parse_assign_cmd(zf_addr_system_reset, hx83112a_nf_pzf_op->addr_system_reset, sizeof(hx83112a_nf_pzf_op->addr_system_reset));
    hx83112a_nf_in_parse_assign_cmd(zf_data_system_reset, hx83112a_nf_pzf_op->data_system_reset, sizeof(hx83112a_nf_pzf_op->data_system_reset));
    hx83112a_nf_in_parse_assign_cmd(zf_data_sram_start_addr, hx83112a_nf_pzf_op->data_sram_start_addr, sizeof(hx83112a_nf_pzf_op->data_sram_start_addr));
    hx83112a_nf_in_parse_assign_cmd(zf_data_sram_clean, hx83112a_nf_pzf_op->data_sram_clean, sizeof(hx83112a_nf_pzf_op->data_sram_clean));
    hx83112a_nf_in_parse_assign_cmd(zf_data_cfg_info, hx83112a_nf_pzf_op->data_cfg_info, sizeof(hx83112a_nf_pzf_op->data_cfg_info));
    hx83112a_nf_in_parse_assign_cmd(zf_data_fw_cfg_p1, hx83112a_nf_pzf_op->data_fw_cfg_p1, sizeof(hx83112a_nf_pzf_op->data_fw_cfg_p1));
    hx83112a_nf_in_parse_assign_cmd(zf_data_fw_cfg_p2, hx83112a_nf_pzf_op->data_fw_cfg_p2, sizeof(hx83112a_nf_pzf_op->data_fw_cfg_p2));
    hx83112a_nf_in_parse_assign_cmd(zf_data_fw_cfg_p3, hx83112a_nf_pzf_op->data_fw_cfg_p3, sizeof(hx83112a_nf_pzf_op->data_fw_cfg_p3));
    hx83112a_nf_in_parse_assign_cmd(zf_data_adc_cfg_1, hx83112a_nf_pzf_op->data_adc_cfg_1, sizeof(hx83112a_nf_pzf_op->data_adc_cfg_1));
    hx83112a_nf_in_parse_assign_cmd(zf_data_adc_cfg_2, hx83112a_nf_pzf_op->data_adc_cfg_2, sizeof(hx83112a_nf_pzf_op->data_adc_cfg_2));
    hx83112a_nf_in_parse_assign_cmd(zf_data_adc_cfg_3, hx83112a_nf_pzf_op->data_adc_cfg_3, sizeof(hx83112a_nf_pzf_op->data_adc_cfg_3));
    hx83112a_nf_in_parse_assign_cmd(zf_data_map_table, hx83112a_nf_pzf_op->data_map_table, sizeof(hx83112a_nf_pzf_op->data_map_table));
    hx83112a_nf_in_parse_assign_cmd(zf_data_mode_switch, hx83112a_nf_pzf_op->data_mode_switch, sizeof(hx83112a_nf_pzf_op->data_mode_switch));


    return 0;
}

#endif

bool hx83112a_nf_ic_package_check(void)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    uint8_t ret_data = 0x00;
    int i = 0;

    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, true); // reset gpio
    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, false); // reset gpio
    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, true); // reset gpio
    msleep(5);

    hx83112a_nf_enter_safe_mode();

    for (i = 0; i < 5; i++) {
        // Product ID
        // Touch
        tmp_addr[3] = 0x90;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0xD0;
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
        TPD_INFO("Read driver IC ID = %X, %X, %X\n", tmp_data[3], tmp_data[2], tmp_data[1]);
        if (!((tmp_data[3] == 0x83) && (tmp_data[2] == 0x11) && (tmp_data[1] == 0x2A)))
        {
            TPD_INFO("WARNING: THE TOUCHPANEL DID NOT CORRECTLY IDENTIFY ITSELF, EXPECTED OUTPUT -> '83112A' (MIGHT BE PROBLEMATIC)\n");
            
        }
        hx83112a_nf_get_fw_id(hx83112a_nf_chip_info);

        HX83112A_NF_IC_TYPE = HX_83112A_SERIES_PWON;
        HX83112A_NF_IC_CHECKSUM = HX_TP_BIN_CHECKSUM_CRC;
        
        //Himax: Set FW and CFG Flash Address
        HX83112A_NF_FW_VER_MAJ_FLASH_ADDR   = 49157;  //0x00C005
        HX83112A_NF_FW_VER_MAJ_FLASH_LENG   = 1;
        HX83112A_NF_FW_VER_MIN_FLASH_ADDR   = 49158;  //0x00C006
        HX83112A_NF_FW_VER_MIN_FLASH_LENG   = 1;
        HX83112A_NF_CFG_VER_MAJ_FLASH_ADDR = 49408;  //0x00C100
        HX83112A_NF_CFG_VER_MAJ_FLASH_LENG = 1;
        HX83112A_NF_CFG_VER_MIN_FLASH_ADDR = 49409;  //0x00C101
        HX83112A_NF_CFG_VER_MIN_FLASH_LENG = 1;
        HX83112A_NF_CID_VER_MAJ_FLASH_ADDR = 49154;  //0x00C002
        HX83112A_NF_CID_VER_MAJ_FLASH_LENG = 1;
        HX83112A_NF_CID_VER_MIN_FLASH_ADDR = 49155;  //0x00C003
        HX83112A_NF_CID_VER_MIN_FLASH_LENG = 1;

#ifdef HX_AUTO_UPDATE_FW
            g_i_FW_VER = i_CTPM_FW[HX83112A_NF_FW_VER_MAJ_FLASH_ADDR] << 8 | i_CTPM_FW[HX83112A_NF_FW_VER_MIN_FLASH_ADDR];
            g_i_CFG_VER = i_CTPM_FW[HX83112A_NF_CFG_VER_MAJ_FLASH_ADDR] << 8 | i_CTPM_FW[HX83112A_NF_CFG_VER_MIN_FLASH_ADDR];
            g_i_CID_MAJ = i_CTPM_FW[HX83112A_NF_CID_VER_MAJ_FLASH_ADDR];
            g_i_CID_MIN = i_CTPM_FW[HX83112A_NF_CID_VER_MIN_FLASH_ADDR];
#endif
        ret_data = true;
        break;
    }

    return ret_data;
}


void hx83112a_nf_power_on_init(void)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];

    /*RawOut select initial*/
    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x02;
    tmp_addr[1] = 0x04;
    tmp_addr[0] = 0xB4;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0x00;
    hx83112a_nf_register_write(tmp_addr, 4, tmp_data, false);

    /*DSRAM func initial*/
    tmp_addr[3] = 0x10;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x07;
    tmp_addr[0] = 0xFC;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0x00;
    hx83112a_nf_register_write(tmp_addr, 4, tmp_data, false);

    hx83112a_nf_sense_on(0x00);
}


static void hx83112a_nf_read_FW_ver(void)
{
    uint8_t cmd[4];
    uint8_t data[64];
    uint8_t data2[64];
    int retry = 200;
    int reload_status = 0;

    hx83112a_nf_sense_on(0);

    while(reload_status == 0) {

        /* FW ID bin address : 0xc014  -  TP IC address : 0x10007014 */
        cmd[3] = 0x10;
        cmd[2] = 0x00;
        cmd[1] = 0x7f;
        cmd[0] = 0x00;

        hx83112a_nf_register_read(cmd, 4, data, false);

        cmd[3] = 0x10;
        cmd[2] = 0x00;
        cmd[1] = 0x72;
        cmd[0] = 0xc0;

        hx83112a_nf_register_read(cmd, 4, data2, false);

        if ((data[1] == 0x3A && data[0] == 0xA3) || (data2[1] == 0x72 && data2[0] == 0xc0)) {
            TPD_INFO("Reload OK! \n");
            reload_status = 1;
            break;
        } 
        else if (retry == 0) {
            TPD_INFO("Reload 20 times! fail \n");
            return;
        } 
        else {
            retry--;
            msleep(10);
            TPD_INFO("Reload fail, delay 10ms retry=%d\n", retry);
        }
    }

    hx83112a_nf_sense_off();

    //=====================================
    // Read FW version : 0x1000_7004  but 05,06 are the real addr for FW Version
    //=====================================

    cmd[3] = 0x10;
    cmd[2] = 0x00;
    cmd[1] = 0x70;
    cmd[0] = 0x04;
    hx83112a_nf_register_read(cmd, 4, data, false);


    TPD_INFO("PANEL_VER : %X \n", data[0]);
    TPD_INFO("FW_VER : %X \n", data[1] << 8 | data[2]);

    cmd[3] = 0x10;
    cmd[2] = 0x00;
    cmd[1] = 0x70;
    cmd[0] = 0x84;
    hx83112a_nf_register_read(cmd, 4, data, false);

    TPD_INFO("CFG_VER : %X \n", data[2] << 8 | data[3]);
    TPD_INFO("TOUCH_VER : %X \n", data[2]);
    TPD_INFO("DISPLAY_VER : %X \n", data[3]);

    cmd[3] = 0x10;
    cmd[2] = 0x00;
    cmd[1] = 0x70;
    cmd[0] = 0x00;
    hx83112a_nf_register_read(cmd, 4, data, false);

    return;
}


void hx83112a_nf_read_OPPO_FW_ver(struct chip_data_hx83112a_nf *chip_info)
{
    uint8_t cmd[4];
    uint8_t data[4];
    uint32_t touch_ver = 0;

    /* FW ID bin address : 0xc014  -  TP IC address : 0x10007014 */
    cmd[3] = 0x10;
    cmd[2] = 0x00;
    cmd[1] = 0x70;
    cmd[0] = 0x14;

    hx83112a_nf_register_read(cmd, 4, data, false);
    
    if (data[0] == 0xBD && data[1] == 0x12) {
        hx83112a_nf_isBD12proj = true;
    }

    if (data[0] == 0xEA && data[1] == 0x00 && (data[2] & 0xF0) == 0x60) {
        hx83112a_nf_isEA006proj = true;
    }
    
    chip_info->fw_id = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
    
    /* FW ID bin address : 0xc014  -  TP IC address : 0x10007014 */
    cmd[3] = 0x10;  
    cmd[2] = 0x00;
    cmd[1] = 0x70;
    cmd[0] = 0x84;

    hx83112a_nf_register_read(cmd, 4, data, false);
    
    touch_ver = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];

    cmd[3] = 0x10;
    cmd[2] = 0x00;
    cmd[1] = 0x70;
    cmd[0] = 0x00;

    hx83112a_nf_register_read(cmd, 4, data, false);
    chip_info->fw_ver = data[2] << 8 | data[3];
    return;
}

uint32_t hx83112a_nf_hw_check_CRC(uint8_t *start_addr, int reload_length)
{
    uint32_t result = 0;
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    int cnt = 0;
    int length = reload_length / 4;

    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x05;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x20;

    hx83112a_nf_flash_write_burst(tmp_addr, start_addr);

    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x05;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x28;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x99;
    tmp_data[1] = (length >> 8);
    tmp_data[0] = length;

    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    cnt = 0;
    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x05;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x00;

    do {
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);

        if ((tmp_data[0] & 0x01) != 0x01) {
            tmp_addr[3] = 0x80;
            tmp_addr[2] = 0x05;
            tmp_addr[1] = 0x00;
            tmp_addr[0] = 0x18;

            hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
            result = ((tmp_data[3] << 24) + (tmp_data[2] << 16) + (tmp_data[1] << 8) + tmp_data[0]);
            break;
        }
    } while (cnt++ < 100);

    return result;
}


bool hx83112a_nf_calculateChecksum(bool change_iref)
{
    uint8_t CRC_result = 0;
    uint8_t tmp_data[4];

    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0x00;

    CRC_result = hx83112a_nf_hw_check_CRC(tmp_data, FW_SIZE_64k);

    msleep(50);

    return !CRC_result;
}

int hx83112a_nf_cal_data_len(int raw_cnt_rmd, int HX_MAX_PT, int raw_cnt_max)
{
    int RawDataLen;
    if (raw_cnt_rmd != 0x00) {
        RawDataLen = 128 - ((HX_MAX_PT + raw_cnt_max + 3) * 4) - 1;
    } else {
        RawDataLen = 128 - ((HX_MAX_PT + raw_cnt_max + 2) * 4) - 1;
    }
    return RawDataLen;
}


int hx83112a_nf_report_data_init(int max_touch_point, int tx_num, int rx_num)
{
    if (hx83112a_nf_touch_data->hx_coord_buf != NULL) {
        kfree(hx83112a_nf_touch_data->hx_coord_buf);
    }

    if (hx83112a_nf_touch_data->diag_mutual != NULL) {
        kfree(hx83112a_nf_touch_data->diag_mutual);
    }

    hx83112a_nf_touch_data->event_size = 128;
    hx83112a_nf_touch_data->touch_all_size = 128; 
    HX83112A_NF_TOUCH_INFO_POINT_CNT = max_touch_point * 4 ;

    if ((max_touch_point % 4) == 0) {
        HX83112A_NF_TOUCH_INFO_POINT_CNT += (max_touch_point / 4) * 4 ;
    }
    else {
        HX83112A_NF_TOUCH_INFO_POINT_CNT += ((max_touch_point / 4) + 1) * 4 ;
    }

    hx83112a_nf_touch_data->raw_cnt_max = max_touch_point / 4;
    hx83112a_nf_touch_data->raw_cnt_rmd = max_touch_point % 4;

    if (hx83112a_nf_touch_data->raw_cnt_rmd != 0x00) { //more than 4 fingers
        hx83112a_nf_touch_data->rawdata_size = hx83112a_nf_cal_data_len(hx83112a_nf_touch_data->raw_cnt_rmd, max_touch_point, hx83112a_nf_touch_data->raw_cnt_max);
        hx83112a_nf_touch_data->touch_info_size = (max_touch_point + hx83112a_nf_touch_data->raw_cnt_max + 2) * 4;
    } 
    else { //less than 4 fingers
        hx83112a_nf_touch_data->rawdata_size = hx83112a_nf_cal_data_len(hx83112a_nf_touch_data->raw_cnt_rmd, max_touch_point, hx83112a_nf_touch_data->raw_cnt_max);
        hx83112a_nf_touch_data->touch_info_size = (max_touch_point + hx83112a_nf_touch_data->raw_cnt_max + 1) * 4;
    }

    if ((tx_num * rx_num + tx_num + rx_num) % hx83112a_nf_touch_data->rawdata_size == 0) {
        hx83112a_nf_touch_data->rawdata_frame_size = (tx_num * rx_num + tx_num + rx_num) / hx83112a_nf_touch_data->rawdata_size;
    } 
    else {
        hx83112a_nf_touch_data->rawdata_frame_size = (tx_num * rx_num + tx_num + rx_num) / hx83112a_nf_touch_data->rawdata_size + 1;
    }

    hx83112a_nf_touch_data->hx_coord_buf = kzalloc(sizeof(uint8_t) * (hx83112a_nf_touch_data->touch_info_size), GFP_KERNEL);
    
    if (hx83112a_nf_touch_data->hx_coord_buf == NULL) {
        goto mem_alloc_fail;
    }

    hx83112a_nf_touch_data->diag_mutual = kzalloc(tx_num * rx_num * sizeof(int32_t), GFP_KERNEL);

    if (hx83112a_nf_touch_data->diag_mutual == NULL) {
        goto mem_alloc_fail;
    }

    hx83112a_nf_touch_data->hx_rawdata_buf = kzalloc(sizeof(uint8_t) * (hx83112a_nf_touch_data->touch_all_size - hx83112a_nf_touch_data->touch_info_size), GFP_KERNEL);
    
    if (hx83112a_nf_touch_data->hx_rawdata_buf == NULL) {
        goto mem_alloc_fail;
    }

    hx83112a_nf_touch_data->hx_event_buf = kzalloc(sizeof(uint8_t) * (hx83112a_nf_touch_data->event_size), GFP_KERNEL);
    
    if (hx83112a_nf_touch_data->hx_event_buf == NULL) {
        goto mem_alloc_fail;
    }

    return NO_ERR;

mem_alloc_fail:
    kfree(hx83112a_nf_touch_data->hx_coord_buf);
    kfree(hx83112a_nf_touch_data->hx_rawdata_buf);
    kfree(hx83112a_nf_touch_data->hx_event_buf);
    TPD_INFO("%s: Memory allocate fail!\n", __func__);
    return MEM_ALLOC_FAIL;

}

bool hx83112a_nf_read_event_stack(uint8_t *buf, uint8_t length)
{
    uint8_t cmd[4];

    //  AHB_I2C Burst Read Off
    cmd[0] = 0x00;
    if (hx83112a_nf_bus_write(0x11, 1, cmd) < 0) {
        TPD_INFO("%s: i2c access fail!\n", __func__);
        return 0;
    }

    hx83112a_nf_bus_read(0x30, length, buf);

    //  AHB_I2C Burst Read On
    cmd[0] = 0x01;
    if (hx83112a_nf_bus_write(0x11, 1, cmd) < 0) {
        TPD_INFO("%s: i2c access fail!\n", __func__);
        return 0;
    }
    return 1;
}

int hx83112a_nf_zero_event_count = 0;
int hx83112a_nf_ic_esd_recovery(int hx_esd_event, int hx_zero_event, int length)
{
    if (hx_esd_event == length) {
        hx83112a_nf_zero_event_count = 0;
        goto checksum_fail;
    } else if (hx_zero_event == length) {
        hx83112a_nf_zero_event_count++;
        TPD_INFO("[HIMAX TP MSG]: ALL Zero event is %d times.\n", hx83112a_nf_zero_event_count);
        if (hx83112a_nf_zero_event_count > 5) {
            hx83112a_nf_zero_event_count = 0;
            TPD_INFO("[HIMAX TP MSG]: ESD event checked - ALL Zero.\n");
            goto checksum_fail;
        }
        goto err_workqueue_out;
    }

checksum_fail:
    return CHECKSUM_FAIL;
err_workqueue_out:
    return WORK_OUT;
}

static int hx83112a_nf_resetgpio_set(struct hw_resource *hw_res, bool on)
{
    int ret = 0;
    if (gpio_is_valid(hw_res->reset_gpio)) {
        TPD_DETAIL("Set the reset_gpio on=%d \n", on);
        ret = gpio_direction_output(hw_res->reset_gpio, on);
        if (ret) {
            TPD_INFO("Set the reset_gpio on=%d fail\n", on);
        } else {
            HX83112A_NF_RESET_STATE = on;
        }
        msleep(RESET_TO_NORMAL_TIME);
        TPD_DETAIL("%s hw_res->reset_gpio = %d\n", __func__, hw_res->reset_gpio);
    }

    return ret;
}

void hx83112a_nf_esd_hw_reset(struct chip_data_hx83112a_nf *chip_info)
{
    int ret = 0;
    int load_fw_times = 10;

    TPD_DETAIL("START_Himax TP: ESD - Reset\n");
    HX83112A_NF_ESD_RESET_ACTIVATE = 1;

    hx83112a_nf_enable_interrupt(hx83112a_nf_chip_info, false);

    do {
        hx83112a_nf_resetgpio_set(chip_info->hw_res, true); // reset gpio
        hx83112a_nf_resetgpio_set(chip_info->hw_res, false); // reset gpio
        hx83112a_nf_resetgpio_set(chip_info->hw_res, true); // reset gpio

        TPD_DETAIL("%s: ESD reset finished\n", __func__);

        TPD_DETAIL("It will update fw after esd event in zero flash mode!\n");

        load_fw_times--;
        g_hx83112a_nf_core_fp.fp_0f_operation_dirly();
        ret = g_hx83112a_nf_core_fp.fp_reload_disable(0); // Returns 1 on success, Returns 0 if failed
    } while (!ret && load_fw_times > 0);

    if (!load_fw_times) {
        TPD_INFO("%s: load_fw_times over 10 times\n", __func__);
    }

    hx83112a_nf_sense_on(0x00);

    hx83112a_nf_enable_interrupt(hx83112a_nf_chip_info, true);
}

static fw_check_state hx83112a_nf_fw_check(void *chip_data, struct resolution_info *resolution_info, struct panel_info *panel_data)
{
    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;

    // FW check normal need update TP_FW  && device info
    panel_data->TP_FW = hx83112a_nf_get_fw_id(chip_info);
    if (panel_data->manufacture_info.version)
        sprintf(&panel_data->manufacture_info.version[panel_data->vid_len], "%02X", panel_data->TP_FW);

    return FW_NORMAL;
}

static int hx83112a_nf_ftm_process(void *chip_data)
{
    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, false); // reset gpio
    return 0;
}

static int hx83112a_nf_get_vendor(void *chip_data, struct panel_info *panel_data)
{
    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;

    chip_info->tp_type = panel_data->tp_type;
    chip_info->p_tp_fw = &panel_data->TP_FW;
    return 0;
}


static int hx83112a_nf_get_chip_info(void *chip_data)
{
    return 1;
}


static uint32_t hx83112a_nf_get_fw_id(struct chip_data_hx83112a_nf *chip_info)
{
    uint32_t current_firmware = 0;
    uint8_t cmd[4];
    uint8_t data[64];
    
    /* FW ID bin address : 0xc014  -  TP IC address : 0x10007014 */
    cmd[3] = 0x10;
    cmd[2] = 0x00;
    cmd[1] = 0x70;
    cmd[0] = 0x14;

    hx83112a_nf_register_read(cmd, 4, data, false);

    TPD_DEBUG("%s : data[0] = 0x%2.2X, data[1] = 0x%2.2X, data[2] = 0x%2.2X, data[3] = 0x%2.2X\n", __func__, data[0], data[1], data[2], data[3]);

    current_firmware = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    
    TPD_DEBUG("CURRENT_FIRMWARE_ID = 0x%x\n", current_firmware);    

    return current_firmware;

}

static u8 hx83112a_nf_trigger_reason(void *chip_data, int gesture_enable, int is_suspended)
{
    if ((gesture_enable == 1) && is_suspended) {
        return IRQ_GESTURE;
    } else {
        return IRQ_TOUCH;
    }
}

static void hx83112a_nf_exit_esd_mode(void *chip_data)
{
    TPD_INFO("exit esd mode ok\n");
    return;
}

/*
 * Returns 0 on success, Returns negative if failed
 */
static int hx83112a_nf_reset(void *chip_data)
{
    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;
    int ret = 0;
    int load_fw_times = 10;

    TPD_INFO("%s.\n", __func__);

    hx83112a_nf_zero_event_count = 0;

    clear_view_touchdown_flag();

    HX83112A_NF_ESD_RESET_ACTIVATE = 0;

    disable_irq_nosync(chip_info->hx_irq);


    do {
        load_fw_times--;
        g_hx83112a_nf_core_fp.fp_0f_operation_dirly();
        ret = g_hx83112a_nf_core_fp.fp_reload_disable(0); // Returns 1 on success, Returns 0 if failed
    } while (!ret && load_fw_times > 0);

    if (!load_fw_times) {
        TPD_INFO("%s: load_fw_times over 10 times\n", __func__);
    }
    hx83112a_nf_sense_on(0x00);

    enable_irq(chip_info->hx_irq);
    return ret;
}

void hx83112a_nf_ultra_enter(void)
{
    uint8_t tmp_data[4];
    int rtimes = 0;

    TPD_INFO("%s:entering\n", __func__);

    /* 34 -> 11 */
    do {
        if (rtimes > 10) {
            TPD_INFO("%s:1/6 retry over 10 times!\n", __func__);
            return;
        }
        tmp_data[0] = 0x11;
        if (hx83112a_nf_bus_write(0x34, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi write fail!\n", __func__);
            continue;
        }
        tmp_data[0] = 0x00;
        if (hx83112a_nf_bus_read(0x34, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi read fail!\n", __func__);
            continue;
        }

        TPD_INFO("%s:retry times %d, addr = 0x34, correct 0x11 = current 0x%2.2X\n", __func__, rtimes, tmp_data[0]);
        rtimes++;
    } while (tmp_data[0] != 0x11);

    /* 33 -> 33 */
    rtimes = 0;
    do {
        if (rtimes > 10) {
            TPD_INFO("%s:2/6 retry over 10 times!\n", __func__);
            return;
        }
        tmp_data[0] = 0x33;
        if (hx83112a_nf_bus_write(0x33, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi write fail!\n", __func__);
            continue;
        }
        tmp_data[0] = 0x00;
        if (hx83112a_nf_bus_read(0x33, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi read fail!\n", __func__);
            continue;
        }

        TPD_INFO("%s:retry times %d, addr = 0x33, correct 0x33 = current 0x%2.2X\n", __func__, rtimes, tmp_data[0]);
        rtimes++;
    } while (tmp_data[0] != 0x33);

    /* 34 -> 22 */
    rtimes = 0;
    do {
        if (rtimes > 10) {
            TPD_INFO("%s:3/6 retry over 10 times!\n", __func__);
            return;
        }
        tmp_data[0] = 0x22;
        if (hx83112a_nf_bus_write(0x34, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi write fail!\n", __func__);
            continue;
        }
        tmp_data[0] = 0x00;
        if (hx83112a_nf_bus_read(0x34, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi read fail!\n", __func__);
            continue;
        }

        TPD_INFO("%s:retry times %d, addr = 0x34, correct 0x22 = current 0x%2.2X\n", __func__, rtimes, tmp_data[0]);
        rtimes++;
    } while (tmp_data[0] != 0x22);

    /* 33 -> AA */
    rtimes = 0;
    do {
        if (rtimes > 10) {
            TPD_INFO("%s:4/6 retry over 10 times!\n", __func__);
            return;
        }
        tmp_data[0] = 0xAA;
        if (hx83112a_nf_bus_write(0x33, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi write fail!\n", __func__);
            continue;
        }
        tmp_data[0] = 0x00;
        if (hx83112a_nf_bus_read(0x33, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi read fail!\n", __func__);
            continue;
        }

        TPD_INFO("%s:retry times %d, addr = 0x33, correct 0xAA = current 0x%2.2X\n", __func__, rtimes, tmp_data[0]);
        rtimes++;
    } while (tmp_data[0] != 0xAA);

    /* 33 -> 33 */
    rtimes = 0;
    do {
        if (rtimes > 10) {
            TPD_INFO("%s:5/6 retry over 10 times!\n", __func__);
            return;
        }
        tmp_data[0] = 0x33;
        if (hx83112a_nf_bus_write(0x33, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi write fail!\n", __func__);
            continue;
        }
        tmp_data[0] = 0x00;
        if (hx83112a_nf_bus_read(0x33, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi read fail!\n", __func__);
            continue;
        }

        TPD_INFO("%s:retry times %d, addr = 0x33, correct 0x33 = current 0x%2.2X\n", __func__, rtimes, tmp_data[0]);
        rtimes++;
    } while (tmp_data[0] != 0x33);

    /* 33 -> AA */
    rtimes = 0;
    do {
        if (rtimes > 10) {
            TPD_INFO("%s:6/6 retry over 10 times!\n", __func__);
            return;
        }
        tmp_data[0] = 0xAA;
        if (hx83112a_nf_bus_write(0x33, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi write fail!\n", __func__);
            continue;
        }
        tmp_data[0] = 0x00;
        if (hx83112a_nf_bus_read(0x33, 1, tmp_data) < 0) {
            TPD_INFO("%s: spi read fail!\n", __func__);
            continue;
        }

        TPD_INFO("%s:retry times %d, addr = 0x33, correct 0xAA = current 0x%2.2X\n", __func__, rtimes, tmp_data[0]);
        rtimes++;
    } while (tmp_data[0] != 0xAA);

    TPD_INFO("%s:END\n", __func__);
}

int hx83112a_nf_checksum_cal(struct chip_data_hx83112a_nf *chip_info, uint8_t *buf, int ts_status)
{
    int hx_EB_event = 0;
    int hx_EC_event = 0;
    int hx_ED_event = 0;
    int hx_esd_event = 0;
    int hx_zero_event = 0;
    int shaking_ret = 0;

    uint16_t check_sum_cal = 0;
    int32_t loop_i = 0;
    int length = 0;

    mdelay(10);

    /* Normal */
    if (ts_status == HX_REPORT_COORD) {
        length = hx83112a_nf_touch_data->touch_info_size;
    }

    /* SMWP */
    else if (ts_status == HX_REPORT_SMWP_EVENT) {
        length = (GEST_PTLG_ID_LEN + GEST_PTLG_HDR_LEN);
    } 
    else {
        TPD_INFO("%s, Neither Normal Nor SMWP error!\n", __func__);
    }

    for (loop_i = 0; loop_i < length; loop_i++) {
        check_sum_cal += buf[loop_i];
        /* #ifdef HX_ESD_RECOVERY  */
        if (ts_status == HX_REPORT_COORD) {
            /* case 1 ESD recovery flow */
            if (buf[loop_i] == 0xEB) {
                hx_EB_event++;
            }
            else if (buf[loop_i] == 0xEC) {
                hx_EC_event++;
            }
            else if (buf[loop_i] == 0xED) {
                hx_ED_event++;
            }
            else if (buf[loop_i] == 0x00) {/* case 2 ESD recovery flow-Disable */
                hx_zero_event++;
            }
            else {
                hx_EB_event = 0;
                hx_EC_event = 0;
                hx_ED_event = 0;
                hx_zero_event = 0;
                hx83112a_nf_zero_event_count = 0;
            }

            if (hx_EB_event == length) {
                hx_esd_event = length;
                hx83112a_nf_EB_event_flag++;
                TPD_INFO("[HIMAX TP MSG]: ESD event checked - ALL 0xEB.\n");
            }
            
            else if (hx_EC_event == length) {
                hx_esd_event = length;
                hx83112a_nf_EC_event_flag++;
                TPD_INFO("[HIMAX TP MSG]: ESD event checked - ALL 0xEC.\n");
            }
            
            else if (hx_ED_event == length) {
                hx_esd_event = length;
                hx83112a_nf_ED_event_flag++;
                TPD_INFO("[HIMAX TP MSG]: ESD event checked - ALL 0xED.\n");
            } 
            
            else {
                hx_esd_event = 0;
            }
        }
        /* #endif */
    }

    if (ts_status == HX_REPORT_COORD) {
        //#ifdef HX_ESD_RECOVERY
        if (hx_esd_event == length || hx_zero_event == length) {
            shaking_ret = hx83112a_nf_ic_esd_recovery(hx_esd_event, hx_zero_event, length);
            if (shaking_ret == CHECKSUM_FAIL) {
                hx83112a_nf_esd_hw_reset(chip_info);
                goto checksum_fail;
            } 

            else if (shaking_ret == ERR_WORK_OUT) {
                goto err_workqueue_out;
            } 

            else {
                goto workqueue_out;
            }
        } 

        else if (HX83112A_NF_ESD_RESET_ACTIVATE) {
            HX83112A_NF_ESD_RESET_ACTIVATE = 0;
            TPD_INFO("[HX83112A_NF_ESD_RESET_ACTIVATE]:%s: Back from reset, ready to serve.\n", __func__);
            goto checksum_fail;
        } 

        else if (HX83112A_NF_HW_RESET_ACTIVATE) {
            HX83112A_NF_HW_RESET_ACTIVATE = 0;
            TPD_INFO("[HX83112A_NF_HW_RESET_ACTIVATE]:%s: Back from reset, ready to serve.\n", __func__);
            goto ready_to_serve;
        }
    }
    
    if ((check_sum_cal % 0x100 != 0) ) {
        TPD_INFO("[HIMAX TP MSG] checksum fail : check_sum_cal: 0x%02X\n", check_sum_cal);
        goto workqueue_out;
    }

    return NO_ERR;

ready_to_serve:
    return READY_TO_SERVE;
checksum_fail:
    return CHECKSUM_FAIL;
err_workqueue_out:
    return ERR_WORK_OUT;
workqueue_out:
    return WORK_OUT;
}

void hx83112a_nf_log_touch_data(uint8_t *buf, struct hx83112a_nf_report_data *hx83112a_nf_touch_data)
{
    int loop_i = 0;
    int print_size = 0;

    if (!hx83112a_nf_touch_data->diag_cmd) {
        print_size = hx83112a_nf_touch_data->touch_info_size;
    } else {
        print_size = hx83112a_nf_touch_data->touch_all_size;
    }

    for (loop_i = 0; loop_i < print_size; loop_i++) {
        printk("0x%02X ",  buf[loop_i]);
        if((loop_i + 1) % 8 == 0) {
            printk("\n");
        }
        if (loop_i == (print_size - 1)) {
            printk("\n");
        }
    }
}

void hx83112a_nf_idle_mode(int disable)
{
    int retry = 20;
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    uint8_t switch_cmd = 0x00;

    TPD_INFO("%s:entering\n", __func__);
    do {
        TPD_INFO("%s,now %d times\n!", __func__, retry);

        tmp_addr[3] = 0x10;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x70;
        tmp_addr[0] = 0x88;
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);

        if (disable)
            switch_cmd = 0x17;
        else
            switch_cmd = 0x1F;

        tmp_data[0] = switch_cmd;
        hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
        TPD_INFO("%s:After turn ON/OFF IDLE Mode [0] = 0x%02X, [1] = 0x%02X, [2] = 0x%02X, [3] = 0x%02X\n", __func__, tmp_data[0], tmp_data[1], tmp_data[2], tmp_data[3]);

        retry--;
        msleep(10);
    } while ((tmp_data[0] != switch_cmd) && retry > 0);

    TPD_INFO("%s: setting OK!\n", __func__);
}


void hx83112a_nf_reload_disable(int on)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];

    TPD_INFO("%s:entering\n", __func__);

    if (on) { /*reload disable*/
        tmp_addr[3] = 0x10;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x7F;
        tmp_addr[0] = 0x00;
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = 0xA5;
        tmp_data[0] = 0x5A;
    } else { /*reload enable*/
        tmp_addr[3] = 0x10;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x7F;
        tmp_addr[0] = 0x00;
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = 0x00;
        tmp_data[0] = 0x00;
    }

    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    TPD_INFO("%s: Setting OK!\n", __func__);
}

void hx83112a_nf_switch_data_type(uint8_t checktype)
{
    uint8_t datatype = 0x00;

    switch (checktype) {
        case HIMAX_INSPECTION_OPEN:
            datatype = DATA_OPEN;
            break;
        case HIMAX_INSPECTION_MICRO_OPEN:
            datatype = DATA_MICRO_OPEN;
            break;
        case HIMAX_INSPECTION_SHORT:
            datatype = DATA_SHORT;
            break;
        case HIMAX_INSPECTION_RAWDATA:
            datatype = DATA_RAWDATA;
            break;
        case HIMAX_INSPECTION_NOISE:
            datatype = DATA_NOISE;
            break;
        case HIMAX_INSPECTION_BACK_NORMAL:
            datatype = DATA_BACK_NORMAL;
            break;
        case HIMAX_INSPECTION_LPWUG_RAWDATA:
            datatype = DATA_LPWUG_RAWDATA;
            break;
        case HIMAX_INSPECTION_LPWUG_NOISE:
            datatype = DATA_LPWUG_NOISE;
            break;
        case HIMAX_INSPECTION_LPWUG_IDLE_RAWDATA:
            datatype = DATA_LPWUG_IDLE_RAWDATA;
            break;
        case HIMAX_INSPECTION_LPWUG_IDLE_NOISE:
            datatype = DATA_LPWUG_IDLE_NOISE;
            break;
        default:
            TPD_INFO("Wrong type=%d\n", checktype);
            break;
        }
    hx83112a_nf_diag_register_set(datatype);
}

int hx83112a_nf_switch_mode(int mode)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    TPD_INFO("%s: Entering\n", __func__);

    //Stop Handshaking
    tmp_addr[3] = 0x10;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x00;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0x00;
    hx83112a_nf_flash_write_burst_length(tmp_addr, tmp_data, 4);

    //Swtich Mode
    tmp_addr[3] = 0x10;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x7F;
    tmp_addr[0] = 0x04;
    switch (mode) {
    case HIMAX_INSPECTION_SORTING:
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = PWD_SORTING_START;
        tmp_data[0] = PWD_SORTING_START;
        break;
    case HIMAX_INSPECTION_OPEN:
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = PWD_OPEN_START;
        tmp_data[0] = PWD_OPEN_START;
        break;
    case HIMAX_INSPECTION_MICRO_OPEN:
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = PWD_OPEN_START;
        tmp_data[0] = PWD_OPEN_START;
        break;
    case HIMAX_INSPECTION_SHORT:
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = PWD_SHORT_START;
        tmp_data[0] = PWD_SHORT_START;
        break;
    case HIMAX_INSPECTION_RAWDATA:
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = PWD_RAWDATA_START;
        tmp_data[0] = PWD_RAWDATA_START;
        break;
    case HIMAX_INSPECTION_NOISE:
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = PWD_NOISE_START;
        tmp_data[0] = PWD_NOISE_START;
        break;
    case HIMAX_INSPECTION_LPWUG_RAWDATA:
    case HIMAX_INSPECTION_LPWUG_NOISE:
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = PWD_LPWUG_START;
        tmp_data[0] = PWD_LPWUG_START;
        break;
    case HIMAX_INSPECTION_LPWUG_IDLE_RAWDATA:
    case HIMAX_INSPECTION_LPWUG_IDLE_NOISE:
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = PWD_LPWUG_IDLE_START;
        tmp_data[0] = PWD_LPWUG_IDLE_START;
        break;
    }
    hx83112a_nf_flash_write_burst_length(tmp_addr, tmp_data, 4);

    TPD_INFO("%s: End of setting!\n", __func__);

    return 0;

}

uint32_t hx83112a_nf_check_mode(uint8_t checktype)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    uint8_t wait_pwd[2];
    // uint8_t count = 0;

    wait_pwd[0] = PWD_NONE;
    wait_pwd[1] = PWD_NONE;

    switch (checktype) {
    case HIMAX_INSPECTION_OPEN:
        wait_pwd[0] = PWD_OPEN_END;
        wait_pwd[1] = PWD_OPEN_END;
        break;
    case HIMAX_INSPECTION_MICRO_OPEN:
        wait_pwd[0] = PWD_OPEN_END;
        wait_pwd[1] = PWD_OPEN_END;
        break;
    case HIMAX_INSPECTION_SHORT:
        wait_pwd[0] = PWD_SHORT_END;
        wait_pwd[1] = PWD_SHORT_END;
        break;
    case HIMAX_INSPECTION_RAWDATA:
        wait_pwd[0] = PWD_RAWDATA_END;
        wait_pwd[1] = PWD_RAWDATA_END;
        break;
    case HIMAX_INSPECTION_NOISE:
        wait_pwd[0] = PWD_NOISE_END;
        wait_pwd[1] = PWD_NOISE_END;
        break;
    case HIMAX_INSPECTION_LPWUG_RAWDATA:
    case HIMAX_INSPECTION_LPWUG_NOISE:
        wait_pwd[0] = PWD_LPWUG_END;
        wait_pwd[1] = PWD_LPWUG_END;
        break;
    case HIMAX_INSPECTION_LPWUG_IDLE_RAWDATA:
    case HIMAX_INSPECTION_LPWUG_IDLE_NOISE:
        wait_pwd[0] = PWD_LPWUG_IDLE_END;
        wait_pwd[1] = PWD_LPWUG_IDLE_END;
        break;
    default:
        TPD_INFO("Wrong type=%d\n", checktype);
        break;
    }

    tmp_addr[3] = 0x10;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x7F;
    tmp_addr[0] = 0x04;
    hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
    TPD_INFO("%s: hx83112a_nf_wait_sorting_mode, tmp_data[0]=%x, tmp_data[1]=%x\n", __func__, tmp_data[0], tmp_data[1]);

    if (wait_pwd[0] == tmp_data[0] && wait_pwd[1] == tmp_data[1]) {
        TPD_INFO("Change to mode=%s\n", hx83112a_nf_inspection_mode[checktype]);
        return 0;
    } else
        return 1;
}

void hx83112a_nf_get_noise_base(void)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    uint8_t ratio, threshold, threshold_LPWUG;

    tmp_addr[3] = 0x10;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x70;
    tmp_addr[0] = 0x94; /*ratio*/
    hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
    ratio = tmp_data[1];

    tmp_addr[3] = 0x10;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x70;
    tmp_addr[0] = 0xA0; /*threshold_LPWUG*/
    hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
    threshold_LPWUG = tmp_data[0];

    tmp_addr[3] = 0x10;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x70;
    tmp_addr[0] = 0x9C; /*threshold*/
    hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
    threshold = tmp_data[0];

    NOISEMAX = ratio * threshold;
    LPWUG_NOISEMAX = ratio * threshold_LPWUG;
    TPD_INFO("NOISEMAX=%d LPWUG_NOISE_MAX=%d \n", NOISEMAX, LPWUG_NOISEMAX);
}

uint16_t hx83112a_nf_get_noise_weight(void)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    uint16_t weight;

    tmp_addr[3] = 0x10;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x72;
    tmp_addr[0] = 0xC8;
    hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
    weight = (tmp_data[1] << 8) | tmp_data[0];
    TPD_INFO("%s: weight = %d ", __func__, weight);

    return weight;
}

uint32_t hx83112a_nf_wait_sorting_mode(uint8_t checktype)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    uint8_t wait_pwd[2];
    uint8_t count = 0;

    wait_pwd[0] = PWD_NONE;
    wait_pwd[1] = PWD_NONE;

    switch (checktype) {
    case HIMAX_INSPECTION_OPEN:
        wait_pwd[0] = PWD_OPEN_END;
        wait_pwd[1] = PWD_OPEN_END;
        break;
    case HIMAX_INSPECTION_MICRO_OPEN:
        wait_pwd[0] = PWD_OPEN_END;
        wait_pwd[1] = PWD_OPEN_END;
        break;
    case HIMAX_INSPECTION_SHORT:
        wait_pwd[0] = PWD_SHORT_END;
        wait_pwd[1] = PWD_SHORT_END;
        break;
    case HIMAX_INSPECTION_RAWDATA:
        wait_pwd[0] = PWD_RAWDATA_END;
        wait_pwd[1] = PWD_RAWDATA_END;
        break;
    case HIMAX_INSPECTION_NOISE:
        wait_pwd[0] = PWD_NOISE_END;
        wait_pwd[1] = PWD_NOISE_END;
        break;
    case HIMAX_INSPECTION_LPWUG_RAWDATA:
    case HIMAX_INSPECTION_LPWUG_NOISE:
        wait_pwd[0] = PWD_LPWUG_END;
        wait_pwd[1] = PWD_LPWUG_END;
        break;
    case HIMAX_INSPECTION_LPWUG_IDLE_RAWDATA:
    case HIMAX_INSPECTION_LPWUG_IDLE_NOISE:
        wait_pwd[0] = PWD_LPWUG_IDLE_END;
        wait_pwd[1] = PWD_LPWUG_IDLE_END;
        break;
    default:
        TPD_INFO("Wrong type=%d\n", checktype);
        break;
    }

    do {
        tmp_addr[3] = 0x10;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x7F;
        tmp_addr[0] = 0x04;
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
        TPD_INFO("%s: hx83112a_nf_wait_sorting_mode, tmp_data[0]=%x, tmp_data[1]=%x\n", __func__, tmp_data[0], tmp_data[1]);

        if (wait_pwd[0] == tmp_data[0] && wait_pwd[1] == tmp_data[1]) {
            return 0;
        }
        tmp_addr[3] = 0x90;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0xA8;
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
        TPD_INFO("%s: 0x900000A8, tmp_data[0]=%x, tmp_data[1]=%x, tmp_data[2]=%x, tmp_data[3]=%x \n", __func__, tmp_data[0], tmp_data[1], tmp_data[2], tmp_data[3]);

        tmp_addr[3] = 0x90;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0xE4;
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
        TPD_INFO("%s: 0x900000E4, tmp_data[0]=%x, tmp_data[1]=%x, tmp_data[2]=%x, tmp_data[3]=%x \n", __func__, tmp_data[0], tmp_data[1], tmp_data[2], tmp_data[3]);

        tmp_addr[3] = 0x90;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0xF8;
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
        TPD_INFO("%s: 0x900000F8, tmp_data[0]=%x, tmp_data[1]=%x, tmp_data[2]=%x, tmp_data[3]=%x \n", __func__, tmp_data[0], tmp_data[1], tmp_data[2], tmp_data[3]);
        TPD_INFO("Now retry %d times!\n", count++);
        msleep(50);
    } while (count < 50);

    return 1;
}

static int hx83112a_nf_diff_str(char *str1, char *str2)
{
    int i = 0;
    int result = 0; /* zero is all same, non-zero is not same index*/
    int str1_len = strlen(str1);
    int str2_len = strlen(str2);

    if (str1_len != str2_len) {
        TPD_DEBUG("%s:Size different!\n", __func__);
        return LENGTH_FAIL;
    }

    for (i = 0; i < str1_len; i++) {
        if (str1[i] != str2[i]) {
            result = i + 1;
            TPD_INFO("%s: different in %d!\n", __func__, result);
            return result;
        }
    }

    return result;
}

/* get idx of criteria whe parsing file */

int hx83112a_nf_find_crtra_id(char *input)
{
    int i = 0;
    int result = 0;

    for (i = 0 ; i < HX83112A_NF_CRITERIA_SIZE ; i++) {
        if (hx83112a_nf_diff_str(hx83112a_nf_inspt_crtra_name[i], input) == 0) {
            result = i;
            TPD_INFO("find the str=%s, idx=%d\n",
                     hx83112a_nf_inspt_crtra_name[i], i);
            break;
        }
    }
    if (i > (HX83112A_NF_CRITERIA_SIZE - 1)) {
        TPD_INFO("%s: find Fail!\n", __func__);
        return LENGTH_FAIL;
    }

    return result;
}

int hx83112a_nf_print_crtra_after_parsing(struct chip_data_hx83112a_nf *chip_info)
{
    int i = 0, j = 0;
    int all_mut_len = chip_info->hw_res->TX_NUM * chip_info->hw_res->RX_NUM;

    for (i = 0; i < HX83112A_NF_CRITERIA_SIZE; i++) {
        TPD_DETAIL("Now is %s\n", hx83112a_nf_inspt_crtra_name[i]);
        if (hx83112a_nf_inspt_crtra_flag[i] == 1) {
            for (j = 0; j < all_mut_len; j++) {
                TPD_DEBUG("%d, ", hx83112a_nf_inspection_criteria[i][j]);
                if (j % 16 == 15)
                    TPD_DEBUG("\n");
            }
        } else {
            TPD_DEBUG("No this Item in this criteria file!\n");
        }
        TPD_DEBUG("\n");
    }

    return 0;
}

/* Get sub-string from original string by using some charaters
 * return size of result
 */

int hx83112a_nf_get_size_str_arr(char **input)
{
    int i = 0;
    int result = 0;

    while (input[i] != NULL)
        i++;

    result = i;
    TPD_DEBUG("There is %d in [0]=%s\n", result, input[0]);

    return result;
}

void hx83112a_nf_init_psl(void) //power saving level
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];

    //==============================================================
    // SCU_Power_State_PW : 0x9000_00A0 ==> 0x0000_0000 (Reset PSL)
    //==============================================================
    tmp_addr[3] = 0x90;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0xA0;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0x00;
    hx83112a_nf_register_write(tmp_addr, 4, tmp_data, false);

    TPD_INFO("%s: Power saving level reset OK!\n", __func__);
}


void hx83112a_nf_chip_erase(void)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];

    hx83112a_nf_interface_on();

    /* init psl */
    hx83112a_nf_init_psl();

    //=====================================
    // SPI Transfer Format : 0x8000_0010 ==> 0x0002_0780
    //=====================================
    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x10;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x02;
    tmp_data[1] = 0x07;
    tmp_data[0] = 0x80;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    //=====================================
    // Chip Erase
    // Write Enable : 1. 0x8000_0020 ==> 0x4700_0000
    //                2. 0x8000_0024 ==> 0x0000_0006
    //=====================================
    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x20;
    tmp_data[3] = 0x47;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0x00;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x24;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0x06;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    //=====================================
    // Chip Erase
    // Erase Command : 0x8000_0024 ==> 0x0000_00C7
    //=====================================
    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x24;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x00;
    tmp_data[1] = 0x00;
    tmp_data[0] = 0xC7;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    msleep(2000);

    if (!hx83112a_nf_wait_wip(100)) {
        TPD_INFO("%s:83112_Chip_Erase Fail\n", __func__);
    }

}

void hx83112a_nf_flash_programming(uint8_t *FW_content, int FW_Size)
{
    int page_prog_start = 0;
    int program_length = 48;
    int i = 0, j = 0, k = 0;
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    uint8_t buring_data[256];    // Read for flash data, 128K
    // 4 bytes for 0x80002C padding

    hx83112a_nf_interface_on();

    //=====================================
    // SPI Transfer Format : 0x8000_0010 ==> 0x0002_0780
    //=====================================
    tmp_addr[3] = 0x80;
    tmp_addr[2] = 0x00;
    tmp_addr[1] = 0x00;
    tmp_addr[0] = 0x10;
    tmp_data[3] = 0x00;
    tmp_data[2] = 0x02;
    tmp_data[1] = 0x07;
    tmp_data[0] = 0x80;
    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

    for (page_prog_start = 0; page_prog_start < FW_Size; page_prog_start = page_prog_start + 256) {
        //msleep(5);
        //=====================================
        // Write Enable : 1. 0x8000_0020 ==> 0x4700_0000
        //                2. 0x8000_0024 ==> 0x0000_0006
        //=====================================
        tmp_addr[3] = 0x80;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0x20;
        tmp_data[3] = 0x47;
        tmp_data[2] = 0x00;
        tmp_data[1] = 0x00;
        tmp_data[0] = 0x00;
        hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

        tmp_addr[3] = 0x80;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0x24;
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = 0x00;
        tmp_data[0] = 0x06;
        hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

        //=================================
        // SPI Transfer Control
        // Set 256 bytes page write : 0x8000_0020 ==> 0x610F_F000
        // Set read start address   : 0x8000_0028 ==> 0x0000_0000
        //=================================
        tmp_addr[3] = 0x80;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0x20;
        tmp_data[3] = 0x61;
        tmp_data[2] = 0x0F;
        tmp_data[1] = 0xF0;
        tmp_data[0] = 0x00;

        // data bytes should be 0x6100_0000 + ((word_number)*4-1)*4096 = 0x6100_0000 + 0xFF000 = 0x610F_F000
        // Programmable size = 1 page = 256 bytes, word_number = 256 byte / 4 = 64

        hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

        tmp_addr[3] = 0x80;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0x28;

        if (page_prog_start < 0x100) {
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = (uint8_t)page_prog_start;
        } else if (page_prog_start >= 0x100 && page_prog_start < 0x10000) {
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = (uint8_t)(page_prog_start >> 8);
            tmp_data[0] = (uint8_t)page_prog_start;
        } else if (page_prog_start >= 0x10000 && page_prog_start < 0x1000000) {
            tmp_data[3] = 0x00;
            tmp_data[2] = (uint8_t)(page_prog_start >> 16);
            tmp_data[1] = (uint8_t)(page_prog_start >> 8);
            tmp_data[0] = (uint8_t)page_prog_start;
        }

        hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);


        //=================================
        // Send 16 bytes data : 0x8000_002C ==> 16 bytes data
        //=================================
        buring_data[0] = 0x2C;
        buring_data[1] = 0x00;
        buring_data[2] = 0x00;
        buring_data[3] = 0x80;

        for (i = /*0*/page_prog_start, j = 0; i < 16 + page_prog_start/**/; i++, j++) {
            buring_data[j + 4] = FW_content[i];
        }


        if (hx83112a_nf_bus_write(0x00, 20, buring_data) < 0) {
            TPD_INFO("%s: i2c access fail!\n", __func__);
            return;
        }
        //=================================
        // Write command : 0x8000_0024 ==> 0x0000_0002
        //=================================
        tmp_addr[3] = 0x80;
        tmp_addr[2] = 0x00;
        tmp_addr[1] = 0x00;
        tmp_addr[0] = 0x24;
        tmp_data[3] = 0x00;
        tmp_data[2] = 0x00;
        tmp_data[1] = 0x00;
        tmp_data[0] = 0x02;
        hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

        //=================================
        // Send 240 bytes data : 0x8000_002C ==> 240 bytes data
        //=================================

        for (j = 0; j < 5; j++) {
            for (i = (page_prog_start + 16 + (j * 48)), k = 0; i < (page_prog_start + 16 + (j * 48)) + program_length; i++, k++) {
                buring_data[k + 4] = FW_content[i]; //(byte)i;
            }

            if (hx83112a_nf_bus_write(0x00, program_length + 4, buring_data) < 0) {
                TPD_INFO("%s: i2c access fail!\n", __func__);
                return;
            }
        }

        if (!hx83112a_nf_wait_wip(1)) {
            TPD_INFO("%s: 83112_Flash_Programming Fail\n", __func__);
        }
    }
}

/*IC_BASED_END*/

int hx83112a_nf_write_read_reg(uint8_t *tmp_addr, uint8_t *tmp_data, uint8_t hb, uint8_t lb)
{
    int cnt = 0;

    do {
        hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);

        msleep(20);
        hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
        TPD_INFO("%s:Now tmp_data[0] = 0x%02X, [1] = 0x%02X, [2] = 0x%02X, [3] = 0x%02X\n", __func__, tmp_data[0], tmp_data[1], tmp_data[2], tmp_data[3]);
    } while ((tmp_data[1] != hb && tmp_data[0] != lb) && cnt++ < 100);

    if (cnt >= 99) {
        TPD_INFO("hx83112a_nf_write_read_reg ERR Now register 0x%08X : high byte = 0x%02X, low byte = 0x%02X\n", tmp_addr[3], tmp_data[1], tmp_data[0]);
        return -1;
    }

    TPD_INFO("Current register 0x%08X : high byte = 0x%02X, low byte = 0x%02X\n", tmp_addr[3], tmp_data[1], tmp_data[0]);
    return NO_ERR;
}

void hx83112a_nf_diag_parse_raw_data(struct hx83112a_nf_report_data *hx83112a_nf_touch_data, int mul_num, int self_num, uint8_t diag_cmd, int32_t *mutual_data, int32_t *self_data)
{
    int RawDataLen_word;
    int index = 0;
    int temp1, temp2, i;

    if (hx83112a_nf_touch_data->hx_rawdata_buf[0] == 0x3A
        && hx83112a_nf_touch_data->hx_rawdata_buf[1] == 0xA3
        && hx83112a_nf_touch_data->hx_rawdata_buf[2] > 0
        && hx83112a_nf_touch_data->hx_rawdata_buf[3] == diag_cmd) {
        RawDataLen_word = hx83112a_nf_touch_data->rawdata_size / 2;
        index = (hx83112a_nf_touch_data->hx_rawdata_buf[2] - 1) * RawDataLen_word;
        for (i = 0; i < RawDataLen_word; i++) {
            temp1 = index + i;

            if (temp1 < mul_num) {
                //mutual
                mutual_data[index + i] = ((int8_t)hx83112a_nf_touch_data->hx_rawdata_buf[i * 2 + 4 + 1]) * 256 + hx83112a_nf_touch_data->hx_rawdata_buf[i * 2 + 4]; /* 4: RawData Header, 1:HSB  */
            } else {
                //self
                temp1 = i + index;
                temp2 = self_num + mul_num;

                if (temp1 >= temp2) {
                    break;
                }
                self_data[i + index - mul_num] = (((int8_t)hx83112a_nf_touch_data->hx_rawdata_buf[i * 2 + 4 + 1]) << 8) | hx83112a_nf_touch_data->hx_rawdata_buf[i * 2 + 4]; /* 4: RawData Header */
            }
        }
    }

}

bool hx83112a_nf_diag_check_sum(struct hx83112a_nf_report_data *hx83112a_nf_touch_data) /*return checksum value  */
{
    uint16_t check_sum_cal = 0;
    int i;

    //Check 128th byte CRC
    for (i = 0, check_sum_cal = 0; i < (hx83112a_nf_touch_data->touch_all_size - hx83112a_nf_touch_data->touch_info_size); i = i + 2) {
        check_sum_cal += (hx83112a_nf_touch_data->hx_rawdata_buf[i + 1] * 256 + hx83112a_nf_touch_data->hx_rawdata_buf[i]);
    }
    if (check_sum_cal % 0x10000 != 0) {
        TPD_INFO("%s fail=%2X \n", __func__, check_sum_cal);
        return 0;
    }

    return 1;
}

static int hx83112a_nf_configuration_init(struct chip_data_hx83112a_nf *chip_info, bool config)
{
    int ret = 0;
    TPD_DETAIL("%s, configuration init = %d\n", __func__, config);
    return ret;
}

int hx83112a_nf_ic_reset(struct chip_data_hx83112a_nf *chip_info, uint8_t loadconfig, uint8_t int_off)
{
    int ret = 0;
    HX83112A_NF_HW_RESET_ACTIVATE = 1;

    TPD_INFO("%s, status: loadconfig=%d, int_off=%d\n", __func__, loadconfig, int_off);

    if (chip_info->hw_res->reset_gpio) {
        if (int_off) {

            ret = hx83112a_nf_enable_interrupt(chip_info, false);
            if (ret < 0) {
                TPD_INFO("%s: hx83112a_nf enable interrupt failed.\n", __func__);
                return ret;
            }
        }

        hx83112a_nf_resetgpio_set(chip_info->hw_res, false); // reset gpio

        hx83112a_nf_resetgpio_set(chip_info->hw_res, true); // reset gpio

        if (loadconfig) {
            ret = hx83112a_nf_configuration_init(chip_info, false);
            if (ret < 0) {
                TPD_INFO("%s: hx83112a_nf configuration init failed.\n", __func__);
                return ret;
            }
            ret = hx83112a_nf_configuration_init(chip_info, true);
            if (ret < 0) {
                TPD_INFO("%s: hx83112a_nf configuration init failed.\n", __func__);
                return ret;
            }
        }
        if (int_off) {
            ret = hx83112a_nf_enable_interrupt(chip_info, true);
            if (ret < 0) {
                TPD_INFO("%s: hx83112a_nf enable interrupt failed.\n", __func__);
                return ret;
            }
        }
    }
    return 0;
}

static int hx83112a_nf_get_touch_points(void *chip_data, struct point_info *points, int max_num)
{
    int i, x, y, z = 1, obj_attention = 0;

    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;
    char buf[128];
    uint16_t mutual_num;
    uint16_t self_num;
    int ret = 0;
    int check_sum_cal;
    int ts_status = HX_REPORT_COORD;
    int hx_point_num;
    uint8_t hx_state_info_pos;

    if (!hx83112a_nf_touch_data) {
        TPD_INFO("%s:%d hx83112a_nf_touch_data is NULL\n", __func__, __LINE__);
    }

    if (!hx83112a_nf_touch_data->hx_coord_buf) {
        TPD_INFO("%s:%d hx83112a_nf_touch_data->hx_coord_buf is NULL\n", __func__, __LINE__);
        return 0;
    }

    hx83112a_nf_burst_enable(0);
    if (hx83112a_nf_diag_command)
        ret = hx83112a_nf_read_event_stack(buf, 128);
    else
        ret = hx83112a_nf_read_event_stack(buf, hx83112a_nf_touch_data->touch_info_size);
    if (!ret) {
        TPD_INFO("%s: Can't read data from chip in normal!\n", __func__);
        goto checksum_fail;
    }

    if (LEVEL_DEBUG == tp_debug) {
        hx83112a_nf_log_touch_data(buf, hx83112a_nf_touch_data);
    }

    check_sum_cal = hx83112a_nf_checksum_cal(chip_info, buf, ts_status);
    if (check_sum_cal == CHECKSUM_FAIL) {
        goto checksum_fail;
    } else if (check_sum_cal == ERR_WORK_OUT) {
        goto err_workqueue_out;
    } else if (check_sum_cal == WORK_OUT) {
        goto workqueue_out;
    }

    hx_state_info_pos = hx83112a_nf_touch_data->touch_info_size - 6;
    if(ts_status == HX_REPORT_COORD) {
        memcpy(hx83112a_nf_touch_data->hx_coord_buf, &buf[0], hx83112a_nf_touch_data->touch_info_size);
        
        if(buf[hx_state_info_pos] != 0xFF && buf[hx_state_info_pos + 1] != 0xFF) {
            memcpy(hx83112a_nf_touch_data->hx_state_info, &buf[hx_state_info_pos], 5);
        } 
        else {
            memset(hx83112a_nf_touch_data->hx_state_info, 0x00, sizeof(hx83112a_nf_touch_data->hx_state_info));
        }
    }
    if (hx83112a_nf_diag_command) {
        mutual_num = chip_info->hw_res->TX_NUM * chip_info->hw_res->RX_NUM;

        self_num = chip_info->hw_res->TX_NUM + chip_info->hw_res->RX_NUM;
        memcpy(hx83112a_nf_touch_data->hx_rawdata_buf, &buf[hx83112a_nf_touch_data->touch_info_size], hx83112a_nf_touch_data->touch_all_size - hx83112a_nf_touch_data->touch_info_size);

        if (!hx83112a_nf_diag_check_sum(hx83112a_nf_touch_data)) {
            goto err_workqueue_out;
        }
        hx83112a_nf_diag_parse_raw_data(hx83112a_nf_touch_data, mutual_num, self_num, hx83112a_nf_diag_command, hx83112a_nf_touch_data->diag_mutual, hx83112a_nf_diag_self);
    }

    if (hx83112a_nf_touch_data->hx_coord_buf[HX83112A_NF_TOUCH_INFO_POINT_CNT] == 0xff)
        hx_point_num = 0;
    else
        hx_point_num = hx83112a_nf_touch_data->hx_coord_buf[HX83112A_NF_TOUCH_INFO_POINT_CNT] & 0x0f;


    for (i = 0; i < 10; i++) {
        x = hx83112a_nf_touch_data->hx_coord_buf[i * 4] << 8 | hx83112a_nf_touch_data->hx_coord_buf[i * 4 + 1];
        y = (hx83112a_nf_touch_data->hx_coord_buf[i * 4 + 2] << 8 | hx83112a_nf_touch_data->hx_coord_buf[i * 4 + 3]);
        z = hx83112a_nf_touch_data->hx_coord_buf[i + 40];
        if(x >= 0 && x <= hx83112a_nf_pri_ts->resolution_info.max_x && y >= 0 && y <= hx83112a_nf_pri_ts->resolution_info.max_y) {
            points[i].x = x;
            points[i].y = y;
            points[i].width_major = z;
            points[i].touch_major = z;
            points[i].status = 1;
            obj_attention = obj_attention | (0x0001 << i);
        }
    }

checksum_fail:
    return obj_attention;
err_workqueue_out:
workqueue_out:
    return -EINVAL;

}

static int hx83112a_nf_enable_black_gesture(struct chip_data_hx83112a_nf *chip_info, bool enable)
{
    int ret = 0;
    struct touchpanel_data *ts = spi_get_drvdata(chip_info->hx_spi);

    TPD_INFO("%s: Enable=%d, ts->is_suspended=%d \n", __func__, enable, ts->is_suspended);

    if (ts->is_suspended) {
        if (enable) {
		if(p_sensor_rec){
			p_sensor_rec = false;
			hx83112a_nf_resetgpio_set(chip_info->hw_res, true); // reset gpio
			hx83112a_nf_resetgpio_set(chip_info->hw_res, false); // reset gpio
			hx83112a_nf_resetgpio_set(chip_info->hw_res, true); // reset gpio

		}
        } 
        else {
            p_sensor_rec = true;
            hx83112a_nf_ultra_enter();
        }
    } 
    else {
        hx83112a_nf_sense_on(0);
    }

    return ret;
}

/*1: On -- 0: Off */
static int hx83112a_nf_jitter_switch (struct chip_data_hx83112a_nf *chip_info, bool on)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    int rtimes = 0;
    int ret = 0;

    TPD_INFO("%s:entering\n", __func__);

    if (!on) { // Jitter off
        do {
            if (rtimes > 10) {
                TPD_INFO("%s: Tried 10 times, jitter off failed!\n", __func__);
                ret = -1;
                break;
            }

            tmp_addr[3] = 0x10;
            tmp_addr[2] = 0x00;
            tmp_addr[1] = 0x7F;
            tmp_addr[0] = 0xE0;
            tmp_data[3] = 0xA5;
            tmp_data[2] = 0x5A;
            tmp_data[1] = 0xA5;
            tmp_data[0] = 0x5A;

            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
            rtimes++;

        } while (tmp_data[3] != 0xA5 || tmp_data[2] != 0x5A || tmp_data[1] != 0xA5 || tmp_data[0] != 0x5A);

        TPD_INFO("%s: Jitter off success!\n", __func__);
    } else { // Jitter on
        do {
            if (rtimes > 10) {
                TPD_INFO("%s: Tried 10 times, jitter on failed!\n", __func__);
                ret = -1;
                break;
            }

            tmp_addr[3] = 0x10;
            tmp_addr[2] = 0x00;
            tmp_addr[1] = 0x7F;
            tmp_addr[0] = 0xE0;
            tmp_data[3] = 0x00;
            tmp_data[2] = 0x00;
            tmp_data[1] = 0x00;
            tmp_data[0] = 0x00;

            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
            rtimes++;
        
        } while (tmp_data[3] == 0xA5 && tmp_data[2] == 0x5A && tmp_data[1] == 0xA5 && tmp_data[0] == 0x5A);

        TPD_INFO("%s: Jitter on success!\n", __func__);
    }
    TPD_INFO("%s:END\n", __func__);
    return ret;
}

static int hx83112a_nf_enable_headset_mode(struct chip_data_hx83112a_nf *chip_info, bool enable)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    int rtimes = 0;
    int ret = 0;
    struct touchpanel_data *ts = spi_get_drvdata(chip_info->hx_spi);

    if (ts->headset_pump_support) {
        if (enable) { /* insert headset */
            do {
                if (rtimes > 10) {
                    TPD_INFO("%s: Insert headset failed!\n", __func__);
                    ret = -1;
                    break;
                }

                tmp_addr[3] = 0x10;
                tmp_addr[2] = 0x00;
                tmp_addr[1] = 0x7F;
                tmp_addr[0] = 0xE8;
                tmp_data[3] = 0xA5;
                tmp_data[2] = 0x5A;
                tmp_data[1] = 0xA5;
                tmp_data[0] = 0x5A;

                hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
                hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
                rtimes++;

            } while (tmp_data[3] != 0xA5 || tmp_data[2] != 0x5A || tmp_data[1] != 0xA5 || tmp_data[0] != 0x5A);

            TPD_INFO("%s: Insert headset success!\n", __func__);
        }
        else { /* remove headset  */
            do {
                if (rtimes > 10) {
                    TPD_INFO("%s: Remove headset failed!\n", __func__);
                    ret = -1;
                    break;
                }

                tmp_addr[3] = 0x10;
                tmp_addr[2] = 0x00;
                tmp_addr[1] = 0x7F;
                tmp_addr[0] = 0xE8;
                tmp_data[3] = 0x00;
                tmp_data[2] = 0x00;
                tmp_data[1] = 0x00;
                tmp_data[0] = 0x00;

                hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
                hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
                rtimes++;

            } while (tmp_data[3] != 0x00 || tmp_data[2] != 0x00 || tmp_data[1] != 0x00 || tmp_data[0] != 0x00);

            TPD_INFO("%s: Remove headset success!\n", __func__);
        }
    }
    return ret;
}

// Mode = 0: off   1: Normal   2: Turn right    3: Turn left
static int hx83112a_nf_rotative_switch(struct chip_data_hx83112a_nf *chip_info, int mode)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    int rtimes = 0;
    int ret = 0;
    struct touchpanel_data *ts = spi_get_drvdata(chip_info->hx_spi);

    TPD_DETAIL("%s:entering\n", __func__);

    if (ts->fw_edge_limit_support) {
        if (mode == 1 || VERTICAL_SCREEN == chip_info->touch_direction) {/* vertical */
            do {
                if (rtimes > 10) {
                    TPD_INFO("%s: Rotative <-> 'Normal' failed!\n", __func__);
                    ret = -1;
                    break;
                }

                tmp_addr[3] = 0x10;
                tmp_addr[2] = 0x00;
                tmp_addr[1] = 0x7F;
                tmp_addr[0] = 0x3C;
                tmp_data[3] = 0xA5;
                tmp_data[2] = 0x5A;
                tmp_data[1] = 0xA5;
                tmp_data[0] = 0x5A;

                hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
                hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
                rtimes++;

            } while (tmp_data[3] != 0xA5 || tmp_data[2] != 0x5A || tmp_data[1] != 0xA5 || tmp_data[0] != 0x5A);

            TPD_INFO("%s: Rotative <-> 'Normal' success!\n", __func__);

        } else {
            rtimes = 0;
            if (LANDSCAPE_SCREEN_270 == chip_info->touch_direction) { // Turn right
                do {
                    if (rtimes > 10) {
                        TPD_INFO("%s: Rotative -> 'Right' failed!\n", __func__);
                        ret = -1;
                        break;
                    }

                    tmp_addr[3] = 0x10;
                    tmp_addr[2] = 0x00;
                    tmp_addr[1] = 0x7F;
                    tmp_addr[0] = 0x3C;
                    tmp_data[3] = 0xA3;
                    tmp_data[2] = 0x3A;
                    tmp_data[1] = 0xA3;
                    tmp_data[0] = 0x3A;

                    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
                    hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
                    rtimes++;

                } while (tmp_data[3] != 0xA3 || tmp_data[2] != 0x3A || tmp_data[1] != 0xA3 || tmp_data[0] != 0x3A);

                TPD_INFO("%s: Rotative -> 'Right' success!\n", __func__);

            } else if(LANDSCAPE_SCREEN_90 == chip_info->touch_direction) { // Turn left
                do {
                    if (rtimes > 10) {
                        TPD_INFO("%s: Rotative -> 'Left' failed!\n", __func__);
                        ret = -1;
                        break;
                    }

                    tmp_addr[3] = 0x10;
                    tmp_addr[2] = 0x00;
                    tmp_addr[1] = 0x7F;
                    tmp_addr[0] = 0x3C;
                    tmp_data[3] = 0xA1;
                    tmp_data[2] = 0x1A;
                    tmp_data[1] = 0xA1;
                    tmp_data[0] = 0x1A;

                    hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
                    hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
                    rtimes++;

                } while (tmp_data[3] != 0xA1 || tmp_data[2] != 0x1A || tmp_data[1] != 0xA1 || tmp_data[0] != 0x1A);

                TPD_INFO("%s: Rotative -> 'Left' success!\n", __func__);
            }
        }
    } else {
        if (mode) { // Open
            do {
                if (rtimes > 10) {
                    TPD_INFO("%s: Open edge limit failed!\n", __func__);
                    ret = -1;
                    break;
                }

                tmp_addr[3] = 0x10;
                tmp_addr[2] = 0x00;
                tmp_addr[1] = 0x7F;
                tmp_addr[0] = 0x3C;
                tmp_data[3] = 0xA5;
                tmp_data[2] = 0x5A;
                tmp_data[1] = 0xA5;
                tmp_data[0] = 0x5A;

                hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
                hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
                rtimes++;

            } while (tmp_data[3] != 0xA5 || tmp_data[2] != 0x5A || tmp_data[1] != 0xA5 || tmp_data[0] != 0x5A);

            TPD_INFO("%s: Open edge limit success!\n", __func__);

        } else { // Close
            do {
                if (rtimes > 10) {
                    TPD_INFO("%s: Close edge limit failed!\n", __func__);
                    ret = -1;
                    break;
                }

                tmp_addr[3] = 0x10;
                tmp_addr[2] = 0x00;
                tmp_addr[1] = 0x7F;
                tmp_addr[0] = 0x3C;
                tmp_data[3] = 0xA9;
                tmp_data[2] = 0x9A;
                tmp_data[1] = 0xA9;
                tmp_data[0] = 0x9A;

                hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
                hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
                rtimes++;

            } while (tmp_data[3] != 0xA9 || tmp_data[2] != 0x9A || tmp_data[1] != 0xA9 || tmp_data[0] != 0x9A);

            TPD_INFO("%s: Close edge limit success!\n", __func__);
        }
    }
    TPD_DETAIL("%s: END\n", __func__);
    return ret;
}

static int hx83112a_nf_mode_switch(void *chip_data, work_mode mode, bool flag)
{
    int ret = -1;
    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;

    TPD_INFO("MODE IS: %i", mode);
    
    switch(mode) { 
        case MODE_NORMAL:
            TPD_DETAIL("%s: p_sensor_rec = %d \n", __func__ , (int)p_sensor_rec);
            p_sensor_rec = false;
            ret = hx83112a_nf_configuration_init(chip_info, true);
            if (ret < 0) {
                TPD_INFO("%s: hx83112a_nf configuration init failed.\n", __func__);
                return ret;
            }

            break;

        case MODE_SLEEP:
            /*device control: sleep mode*/
            ret = hx83112a_nf_configuration_init(chip_info, false) ;
            if (ret < 0) {
                TPD_INFO("%s: hx83112a_nf configuration init failed.\n", __func__);
                return ret;
            }

            break;

        case MODE_GESTURE:
            ret = hx83112a_nf_enable_black_gesture(chip_info, flag);
            if (ret < 0) {
                TPD_INFO("%s: hx83112a_nf enable gesture failed.\n", __func__);
                return ret;
            }

            break;

        case MODE_EDGE:
            ret = hx83112a_nf_rotative_switch(chip_info, flag);
            if (ret < 0) {
                TPD_INFO("%s: hx83112a_nf enable edg & corner limit failed.\n", __func__);
                return ret;
            }

            break;

        case MODE_HEADSET:
            ret = hx83112a_nf_enable_headset_mode(chip_info, flag);
            if (ret < 0) {
                TPD_INFO("%s: enable headset mode : %d failed\n", __func__, flag);
            }
            break;

        case MODE_GAME:
            ret = hx83112a_nf_jitter_switch(chip_info, !flag);
            if (ret < 0) {
                TPD_INFO("%s: enable game mode : %d failed\n", __func__, !flag);
            }
            break;

        default:
            TPD_INFO("%s: Wrong mode! >:(\n", __func__);
    }

    return ret;
}

static int hx83112a_nf_get_gesture_info(void *chip_data, struct gesture_info *gesture)
{
    int i = 0;
    int gesture_sign = 0;
    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;
    uint8_t *buf;
    int gest_len;
    int check_FC = 0;

    int check_sum_cal;
    int ts_status = HX_REPORT_SMWP_EVENT;


    buf = kzalloc(hx83112a_nf_touch_data->event_size * sizeof(uint8_t), GFP_KERNEL);
    if (!buf) {
        return -1;
    }

    hx83112a_nf_burst_enable(0);

    if (!hx83112a_nf_read_event_stack(buf, hx83112a_nf_touch_data->event_size)) {
        kfree(buf);
        return -1;
    }

    for (i = 0; i < 128; i++) {
        if (!i) {
        }
        
        printk("%02d ", buf[i]);
        
        if ((i + 1) % 8 == 0) {
            printk("\n");
        }
        
        if (i == (128 - 1)) {
            printk("\n");
        }
    }

    check_sum_cal = hx83112a_nf_checksum_cal(chip_info, buf, ts_status);
    
    if (check_sum_cal == CHECKSUM_FAIL) {
        return -1;
    } 
    else if (check_sum_cal == ERR_WORK_OUT) {
        goto err_workqueue_out;
    }

    for (i = 0; i < 4; i++) {
        if (check_FC == 0) {
            if ((buf[0] != 0x00) && ((buf[0] < 0x0E))) {
                check_FC = 1;
                gesture_sign = buf[i];
            } 
            else {
                check_FC = 0;
                break;
            }
        } 
        else {
            if (buf[i] != gesture_sign) {
                check_FC = 0;
                break;
            }
        }
    }

    if (buf[GEST_PTLG_ID_LEN] != GEST_PTLG_HDR_ID1 ||
        buf[GEST_PTLG_ID_LEN + 1] != GEST_PTLG_HDR_ID2) {
        goto RET_OUT;
    }

    if (buf[GEST_PTLG_ID_LEN] == GEST_PTLG_HDR_ID1 &&
        buf[GEST_PTLG_ID_LEN + 1] == GEST_PTLG_HDR_ID2) {
        
        gest_len = buf[GEST_PTLG_ID_LEN + 2];
        if (gest_len > 52) {
            gest_len = 52;
        }

        i = 0;
        hx83112a_nf_gest_pt_cnt = 0;
        while (i < (gest_len + 1) / 2) {

            if (i == 6) {
                hx83112a_nf_gest_pt_x[hx83112a_nf_gest_pt_cnt] = buf[GEST_PTLG_ID_LEN + 4 + i * 2];
            } 
            else {
                hx83112a_nf_gest_pt_x[hx83112a_nf_gest_pt_cnt] = buf[GEST_PTLG_ID_LEN + 4 + i * 2] * hx83112a_nf_pri_ts->resolution_info.max_x / 255;
            }
            
            hx83112a_nf_gest_pt_y[hx83112a_nf_gest_pt_cnt] = buf[GEST_PTLG_ID_LEN + 4 + i * 2 + 1] * hx83112a_nf_pri_ts->resolution_info.max_y / 255;
            
            i++;
            hx83112a_nf_gest_pt_cnt += 1;

        }

        if (hx83112a_nf_gest_pt_cnt) {
            gesture->gesture_type = gesture_sign;              /* id */
            gesture->Point_start.x = hx83112a_nf_gest_pt_x[0]; /* start x */
            gesture->Point_start.y = hx83112a_nf_gest_pt_y[0]; /* start y */
            gesture->Point_end.x = hx83112a_nf_gest_pt_x[1];   /* end x */
            gesture->Point_end.y = hx83112a_nf_gest_pt_y[1];   /* end y */
            gesture->Point_1st.x = hx83112a_nf_gest_pt_x[2];   /* 1 */
            gesture->Point_1st.y = hx83112a_nf_gest_pt_y[2];
            gesture->Point_2nd.x = hx83112a_nf_gest_pt_x[3];   /* 2 */
            gesture->Point_2nd.y = hx83112a_nf_gest_pt_y[3];
            gesture->Point_3rd.x = hx83112a_nf_gest_pt_x[4];   /* 3 */
            gesture->Point_3rd.y = hx83112a_nf_gest_pt_y[4];   
            gesture->Point_4th.x = hx83112a_nf_gest_pt_x[5];   /* 4 */
            gesture->Point_4th.y = hx83112a_nf_gest_pt_y[5];
            gesture->clockwise = hx83112a_nf_gest_pt_x[6];     /* 1, 0 */
        }
    }

RET_OUT:
    if (buf) {
        kfree(buf);
    }
    return 0;

err_workqueue_out:
    return -1;
}

static int hx83112a_nf_power_control(void *chip_data, bool enable)
{
    int ret = 0;
    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;

    if (true == enable) {
        ret = tp_powercontrol_2v8(chip_info->hw_res, true);
        
        if (ret)
            return -1;

        ret = tp_powercontrol_1v8(chip_info->hw_res, true);
        
        if (ret)
            return -1;

        ret = hx83112a_nf_resetgpio_set(chip_info->hw_res, true);
        
        if (ret)
            return -1;
    }
    else {
        ret = hx83112a_nf_resetgpio_set(chip_info->hw_res, false);
        
        if (ret)
            return -1;
        
        ret = tp_powercontrol_1v8(chip_info->hw_res, false);

        if (ret)
            return -1;
   
        ret = tp_powercontrol_2v8(chip_info->hw_res, false);
    
        if (ret)
            return -1;
    }

    return ret;
}

static fw_update_state hx83112a_nf_fw_update(void *chip_data, const struct firmware *fw, bool force)
{
    uint32_t FIRMWARE_ID = 0;
    fw_file_id = 0;

    uint8_t cmd[4];
    uint8_t data[64];

    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;
    const uint8_t *p_fw_id = NULL ;

    msleep(10);

    if (fw) {
        if (chip_info->g_fw_buf) {
            chip_info->g_fw_len = fw->size;
            memcpy(chip_info->g_fw_buf, fw->data, fw->size);
            chip_info->g_fw_sta = true;
        }
    }
    if (fw == NULL) {
        TPD_INFO("fw is NULL\n");
    }

    p_fw_id = fw->data + 49172;

    if (!chip_info) {
        TPD_INFO("Chip info is NULL\n");
        return 0;
    }

    TPD_INFO("%s is called\n", __func__);

    // Step 2: Get FW version from IC && determine whether we need get into update flow.

    fw_file_id = (*p_fw_id << 24) | (*(p_fw_id + 1) << 16) | (*(p_fw_id + 2) << 8) | *(p_fw_id + 3);

    /* FW ID bin address : 0xc014  -  TP IC address : 0x10007014 */
    cmd[3] = 0x10;
    cmd[2] = 0x00;
    cmd[1] = 0x70;
    cmd[0] = 0x14;
    hx83112a_nf_register_read(cmd, 4, data, false);

    FIRMWARE_ID = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

    TPD_INFO("FIRMWARE FILE ID is 0x%x, PANEL'S FIRMWARE ID is 0x%x\n", fw_file_id, FIRMWARE_ID);

    // Step 3: Flash firmware zone
    TPD_INFO("FLASHING TOUCHPANEL FIRMWARE!\n");

    /* trigger reset */
    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, false); // reset gpio
    hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, true); // reset gpio
    
    msleep(20);

    g_hx83112a_nf_core_fp.fp_firmware_update_0f(NULL);

    msleep(20);

    g_hx83112a_nf_core_fp.fp_reload_disable(0);

    TPD_INFO("Firmware flash over!\n");
    
    hx83112a_nf_register_read(cmd, 4, data, false);
    FIRMWARE_ID = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

    TPD_INFO("NEW PANEL'S FIRMWARE ID is 0x%x\n", FIRMWARE_ID);

    hx83112a_nf_read_OPPO_FW_ver(chip_info);
    hx83112a_nf_sense_on(0x00);

    enable_irq(chip_info->hx_irq);

    chip_info->first_download_finished = true;
    return FW_UPDATE_SUCCESS;
}

static int hx83112a_nf_reset_gpio_control(void *chip_data, bool enable)
{
    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;
    if (gpio_is_valid(chip_info->hw_res->reset_gpio)) {
        TPD_INFO("%s: set reset state %d\n", __func__, enable);
        hx83112a_nf_resetgpio_set(hx83112a_nf_chip_info->hw_res, enable);
        TPD_DETAIL("%s: set reset state END\n", __func__);
    }
    return 0;
}

static void hx83112a_nf_set_touch_direction(void *chip_data, uint8_t dir)
{
    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;

    chip_info->touch_direction = dir;
}

static uint8_t hx83112a_nf_get_touch_direction(void *chip_data)
{
    struct chip_data_hx83112a_nf *chip_info = (struct chip_data_hx83112a_nf *)chip_data;

    return chip_info->touch_direction;
}


int hx83112a_nf_freq_point = 0;
void hx83112a_nf_freq_hop_trigger(void *chip_data)
{
    uint8_t tmp_addr[4];
    uint8_t tmp_data[4];
    int rtimes = 0;

    hx83112a_nf_freq_point = 1 - hx83112a_nf_freq_point;
    if (hx83112a_nf_freq_point) {//hop to frequency 130K
        do {
            if (rtimes > 10) {
                TPD_INFO("%s: Frequency hopping failed!\n", __func__);
                break;
            }

            tmp_addr[3] = 0x10;
            tmp_addr[2] = 0x00;
            tmp_addr[1] = 0x7F;
            tmp_addr[0] = 0xC4;
            tmp_data[3] = 0xA5;
            tmp_data[2] = 0x5A;
            tmp_data[1] = 0xA5;
            tmp_data[0] = 0x5A;

            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
            
            rtimes++;
        } while (tmp_data[3] != 0xA5 || tmp_data[2] != 0x5A || tmp_data[1] != 0xA5 || tmp_data[0] != 0x5A);

        if (rtimes <= 10) {
            TPD_INFO("%s:hopping frequency to 130K success!\n", __func__);
        }

    } 
    else { //hop to frequency 75K
        do {
            if (rtimes > 10) {
                TPD_INFO("%s:frequency hopping failed!\n", __func__);
                TPD_INFO("%s:correct tmp_data[0,1,2,3] = 0x3A,0xA3,0x3A,0xA3\n", __func__);
                break;
            }

            tmp_addr[3] = 0x10;
            tmp_addr[2] = 0x00;
            tmp_addr[1] = 0x7F;
            tmp_addr[0] = 0xC4;
            tmp_data[3] = 0xA3;
            tmp_data[2] = 0x3A;
            tmp_data[1] = 0xA3;
            tmp_data[0] = 0x3A;

            hx83112a_nf_flash_write_burst(tmp_addr, tmp_data);
            hx83112a_nf_register_read(tmp_addr, 4, tmp_data, false);
            
            rtimes++;
        } while (tmp_data[3] != 0xA3 || tmp_data[2] != 0x3A || tmp_data[1] != 0xA3 || tmp_data[0] != 0x3A);

        if (rtimes <= 10) {
            TPD_INFO("%s: Hopping frequency to 75K success!\n", __func__);
        }
    }
}

static struct oppo_touchpanel_operations hx83112a_nf_ops = {
    .ftm_process      = hx83112a_nf_ftm_process,
    .get_vendor       = hx83112a_nf_get_vendor,
    .get_chip_info    = hx83112a_nf_get_chip_info,
    .reset            = hx83112a_nf_reset,
    .power_control    = hx83112a_nf_power_control,
    .fw_check         = hx83112a_nf_fw_check,
    .fw_update        = hx83112a_nf_fw_update,
    .trigger_reason   = hx83112a_nf_trigger_reason,
    .get_touch_points = hx83112a_nf_get_touch_points,
    .get_gesture_info = hx83112a_nf_get_gesture_info,
    .mode_switch      = hx83112a_nf_mode_switch,
    .exit_esd_mode    = hx83112a_nf_exit_esd_mode,
    .reset_gpio_control = hx83112a_nf_reset_gpio_control,
    .set_touch_direction    = hx83112a_nf_set_touch_direction,
    .get_touch_direction    = hx83112a_nf_get_touch_direction,
    .freq_hop_trigger = hx83112a_nf_freq_hop_trigger,
};

static int hx83112a_nf_tp_probe(struct spi_device *spi)
{
    struct chip_data_hx83112a_nf *chip_info = NULL;
    struct touchpanel_data *ts = NULL;
    uint32_t flashed_fw = 0;

    int ret = -1;

    TPD_INFO("%s  is called\n", __func__);

    // Step1: Alloc chip_info
    chip_info = kzalloc(sizeof(struct chip_data_hx83112a_nf), GFP_KERNEL);
    if (chip_info == NULL) {
        TPD_INFO("chip info kzalloc error\n");
        ret = -ENOMEM;
        return ret;
    }
    memset(chip_info, 0, sizeof(*chip_info));
    hx83112a_nf_chip_info = chip_info;

    /* Allocate himax report data */
    hx83112a_nf_touch_data = kzalloc(sizeof(struct hx83112a_nf_report_data), GFP_KERNEL);
    if (hx83112a_nf_touch_data == NULL) {
        goto err_register_driver;
    }

    // Step2: Alloc common ts
    ts = common_touch_data_alloc();
    if (ts == NULL) {
        TPD_INFO("ts kzalloc error\n");
        goto err_register_driver;
    }
    memset(ts, 0, sizeof(*ts));

    chip_info->g_fw_buf = vmalloc(128 * 1024);
    if (chip_info->g_fw_buf == NULL) {
        TPD_INFO("fw buf vmalloc error\n");
        goto err_g_fw_buf;
    }

    // Step3: Binding dev for easy operate
    chip_info->hx_spi = spi;
    ts->s_client = spi;
    
    chip_info->hx_irq = spi->irq;
    ts->irq = spi->irq;
    spi_set_drvdata(spi, ts);
    
    ts->dev = &spi->dev;
    ts->chip_data = chip_info;
    
    chip_info->hw_res = &ts->hw_res;
    mutex_init(&(chip_info->spi_lock));
    chip_info->touch_direction = VERTICAL_SCREEN;
    
    chip_info->using_headfile = false;
    chip_info->first_download_finished = false;

    if (ts->s_client->controller->flags & SPI_CONTROLLER_HALF_DUPLEX) {
        TPD_INFO("Full duplex not supported by master\n");
        ret = -EIO;
        goto err_spi_setup;
    }
    
    ts->s_client->bits_per_word = 8;
    ts->s_client->mode = SPI_MODE_3;
    ts->s_client->chip_select[0] = 0;

    /* old usage of MTK spi API */
    memcpy(&chip_info->hx_spi_mcc, &hx83112a_nf_hx_spi_ctrdata, sizeof(struct mt_chip_conf));
    ts->s_client->controller_data = (void *)&chip_info->hx_spi_mcc;

    ret = spi_setup(ts->s_client);
    if (ret < 0) {
        TPD_INFO("Failed to perform SPI setup\n");
        goto err_spi_setup;
    }
    chip_info->p_spuri_fp_touch = &(ts->spuri_fp_touch);

    // Step4:file_operations callback binding
    ts->ts_ops = &hx83112a_nf_ops;

    hx83112a_nf_pri_ts = ts;

    // Step5:register common touch
    hx83112a_nf_0f_init();

    ret = register_common_touch_device(ts);
    if (ret < 0) {
        goto err_register_driver;
    }

    disable_irq_nosync(chip_info->hx_irq);
    if (hx83112a_nf_ic_package_check() == false) {
        TPD_INFO("Himax chip does NOT EXIST");
        goto err_register_driver;
    }

#ifdef HX_ZERO_FLASH
    chip_info->p_firmware_headfile = &ts->panel_data.firmware_headfile;
    hx83112a_nf_auto_update_flag = true;
    chip_info->himax_0f_update_wq = create_singlethread_workqueue("HMX_0f_update_reuqest");
    INIT_DELAYED_WORK(&chip_info->work_0f_update, hx83112a_nf_mcu_0f_operation);
#else
    hx83112a_nf_read_FW_ver();
    hx83112a_nf_calculateChecksum(false);
#endif

    hx83112a_nf_power_on_init();

    //Touch data init
    ret = hx83112a_nf_report_data_init(ts->max_num, ts->hw_res.TX_NUM, ts->hw_res.RX_NUM);
    if (ret) {
        goto err_register_driver;
    }

    ts->tp_suspend_order = TP_LCD_SUSPEND;
    ts->tp_resume_order = LCD_TP_RESUME;
    ts->skip_suspend_operate = true;
    ts->skip_reset_in_resume = true;
 
    hx83112a_nf_irq_en_cnt = 1;
    
    hx83112a_nf_fw_update(chip_info, ts->fw, 0);
    release_firmware(ts->fw);
    
    flashed_fw = hx83112a_nf_get_fw_id(chip_info);
    if (flashed_fw != fw_file_id) {
        TPD_INFO("OH NOES, FW ID IS INCORRECT, IS: 0x%x BUT SHOULD BE: 0x%x. NO MORE FLASHING! :(\n", flashed_fw, fw_file_id);
        return -1;
    }
    else {
        TPD_INFO("FW ID IS CORRECT :), IS: 0x%x AND SHOULD BE: 0x%x.\n", flashed_fw, fw_file_id);
    }

    TPD_INFO("%s: - probed successfully :)\n", __func__);

    return 0;
err_spi_setup:
    if (chip_info->g_fw_buf) {
        vfree(chip_info->g_fw_buf);
    }
err_g_fw_buf:
err_register_driver:
    disable_irq_nosync(chip_info->hx_irq);

    kfree(ts);
    ts = NULL;

    if (hx83112a_nf_touch_data) {
        kfree(hx83112a_nf_touch_data);
    }

    if (chip_info) {
        kfree(chip_info);
    }

    ret = -1;

    TPD_INFO("%s: Probe error! :(\n", __func__);

    return ret;
}

static void hx83112a_nf_tp_remove(struct spi_device *spi)
{
    struct touchpanel_data *ts = spi_get_drvdata(spi);

    ts->s_client = NULL;
    spi_set_drvdata(spi, NULL);

    TPD_INFO("%s is called\n", __func__);
    kfree(ts);

    return;
}

static const struct spi_device_id tp_id[] = {
    { TPD_DEVICE, 0 },
    { }
};

static struct of_device_id tp_match_table[] = {
    { .compatible = TPD_DEVICE,},
    { },
};

static struct spi_driver hx83112a_nf_common_driver = {
    .probe      = hx83112a_nf_tp_probe,
    .remove     = hx83112a_nf_tp_remove,
    .id_table   = tp_id,
    .driver = {
        .name = TPD_DEVICE,
        .owner = THIS_MODULE,
        .of_match_table = tp_match_table,

    },
};

static int __init tp_driver_init(void)
{
    int status = 0;

    TPD_INFO("%s is called\n", __func__);
    // get_lcd_vendor();
    
    status = spi_register_driver(&hx83112a_nf_common_driver);
    if (status < 0) {
        TPD_INFO("%s, Failed to register SPI driver.\n", __func__);
        return -EINVAL;
    }

    return status;
}

/* should never be called */
static void __exit tp_driver_exit(void)
{
    spi_unregister_driver(&hx83112a_nf_common_driver);
    return;
}

module_init(tp_driver_init);
module_exit(tp_driver_exit);

MODULE_DESCRIPTION("Touchscreen Driver");
MODULE_LICENSE("GPL");