// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 * Copyright (C) 2025 Yahia Shamaa <yehiashamaa987@gmail.com>
 */

#include <linux/uaccess.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/rtc.h>
#include <linux/syscalls.h>
#include <linux/timer.h>
#include <linux/of_gpio.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/consumer.h>
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_qos.h>
#include <linux/proc_fs.h>

#ifndef TPD_USE_EINT
#include <linux/hrtimer.h>
#endif
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/suspend.h>


#include "himax_common.h"
    
/*******Part0:LOG TAG Declear************************/
#define TPD_PRINT_POINT_NUM 150
#define TPD_DEVICE "touchpanel"
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

#define TPD_SPECIFIC_PRINT(count, a, arg...)\
    do{\
        if (count++ == TPD_PRINT_POINT_NUM || LEVEL_DEBUG == tp_debug) {\
            TPD_INFO(TPD_DEVICE ": " a, ##arg);\
            count = 0;\
        }\
    }while(0)

/*******Part1:Global variables Area********************/
// unsigned int tp_debug = 0;
unsigned int tp_register_times = 0;
struct touchpanel_data *g_tp = NULL;
static DECLARE_WAIT_QUEUE_HEAD(waiter);

static bool register_is_16bit = 0;
static struct mutex i2c_mutex;

// static struct pm_qos_request pm_qos_req;
// static int pm_qos_value = PM_QOS_DEFAULT_VALUE;
// static int pm_qos_state = 0;
#define PM_QOS_TOUCH_WAKEUP_VALUE 400

static int sigle_num = 0;
static struct timespec64 tpstart, tpend;
static int pointx[2] = {0, 0};
static int pointy[2] = {0, 0};

#define ABS(a, b) ((a - b > 0) ? a - b : b - a)

/*******Part2:declear Area********************************/
static void speedup_resume(struct work_struct *work);
static void lcd_trigger_load_tp_fw(struct work_struct *work);
static void lcd_tp_refresh_work(struct work_struct *work);

void esd_handle_switch(struct esd_information *esd_info, bool flag);

#ifdef TPD_USE_EINT
static irqreturn_t tp_irq_thread_fn(int irq, void *dev_id);
#endif

#ifdef CONFIG_FB
static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data);
#endif

static void tp_touch_release(struct touchpanel_data *ts);
static void tp_btnkey_release(struct touchpanel_data *ts);
static void tp_fw_update_work(struct work_struct *work);
static void tp_work_func(struct touchpanel_data *ts);
__attribute__((weak)) int register_devinfo(char *name, struct manufacture_info *info)
{
    return 1;
}
__attribute__((weak)) int preconfig_power_control(struct touchpanel_data *ts)
{
    return 0;
}
__attribute__((weak)) int reconfig_power_control(struct touchpanel_data *ts)
{
    return 0;
}



static void tp_touch_down(struct touchpanel_data *ts, struct point_info points, int touch_report_num, int id)
{
    static int last_width_major;
    // static int point_num = 0;

    if (ts->input_dev == NULL)
        return;

    input_report_key(ts->input_dev, BTN_TOUCH, 1);
    input_report_key(ts->input_dev, BTN_TOOL_FINGER, 1);
    if (touch_report_num == 1) {
        input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, points.width_major);
        last_width_major = points.width_major;
    } else if (!(touch_report_num & 0x7f) || touch_report_num == 30) {
        //if touch_report_num == 127, every 127 points, change width_major
        //down and keep long time, auto repeat per 5 seconds, for weixing
        //report move event after down event, for weixing voice delay problem, 30 -> 300ms in order to avoid the intercept by shortcut
        if (last_width_major == points.width_major) {
            last_width_major = points.width_major + 1;
        } else {
            last_width_major = points.width_major;
        }
        input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, last_width_major);
    }
    if ((points.x > ts->touch_major_limit.width_range) && (points.x < ts->resolution_info.max_x - ts->touch_major_limit.width_range) && \
        (points.y > ts->touch_major_limit.height_range) && (points.y < ts->resolution_info.max_y - ts->touch_major_limit.height_range)) {
        if (ts->smart_gesture_support) {
            if (points.touch_major > SMART_GESTURE_THRESHOLD) {
                input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, points.touch_major);
            } else {
                input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, SMART_GESTURE_LOW_VALUE);
            }
        }

        if (ts->pressure_report_support) {
            input_report_abs(ts->input_dev, ABS_MT_PRESSURE, points.touch_major);   //add for fixing gripview tap no function issue
        }
    }

    input_report_abs(ts->input_dev, ABS_MT_POSITION_X, points.x);
    input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, points.y);

    // TPD_SPECIFIC_PRINT(point_num, "Touchpanel id %d :Down[%4d %4d %4d]\n", id, points.x, points.y, points.z);

#ifndef TYPE_B_PROTOCOL
    input_mt_sync(ts->input_dev);
#endif
}

//called by other report device
void external_report_touch(int id, bool down_status, int x, int y)
{

    if (g_tp == NULL)
        return;

    mutex_lock(&g_tp->mutex);
    g_tp->external_touch_status = down_status;
    if (down_status) {
        input_mt_slot(g_tp->input_dev, id);
        input_mt_report_slot_state(g_tp->input_dev, MT_TOOL_FINGER, 1);
        input_report_key(g_tp->input_dev, BTN_TOUCH, 1);
        input_report_key(g_tp->input_dev, BTN_TOOL_FINGER, 1);
        input_report_abs(g_tp->input_dev, ABS_MT_POSITION_X, x);
        input_report_abs(g_tp->input_dev, ABS_MT_POSITION_Y, y);
    } else {
        input_mt_slot(g_tp->input_dev, id);
        input_mt_report_slot_state(g_tp->input_dev, MT_TOOL_FINGER, 0);
        if (!g_tp->touch_count) {
            input_report_key(g_tp->input_dev, BTN_TOUCH, 0);
            input_report_key(g_tp->input_dev, BTN_TOOL_FINGER, 0);
        }
    }
    input_sync(g_tp->input_dev);
    mutex_unlock(&g_tp->mutex);
}
EXPORT_SYMBOL(external_report_touch);

static void tp_touch_up(struct touchpanel_data *ts)
{
    if (ts->input_dev == NULL)
        return;

    if (ts->external_touch_support && (true == ts->external_touch_status)) {
        TPD_DETAIL("external touch device is down, skip this touch up.\n");
        return;
    }

    input_report_key(ts->input_dev, BTN_TOUCH, 0);
    input_report_key(ts->input_dev, BTN_TOOL_FINGER, 0);
#ifndef TYPE_B_PROTOCOL
    input_mt_sync(ts->input_dev);
#endif
}

static void tp_exception_handle(struct touchpanel_data *ts)
{
    if (!ts->ts_ops->reset) {
        TPD_INFO("not support ts->ts_ops->reset callback\n");
        return;
    }

    ts->ts_ops->reset(ts->chip_data);    // after reset, all registers set to default
    // operate_mode_switch(ts);

    tp_btnkey_release(ts);
    tp_touch_release(ts);
    if (ts->fingerprint_underscreen_support) {
        ts->fp_info.touch_state = 0;
        opticalfp_irq_handler(&ts->fp_info);
    }
}

static void tp_fw_auto_reset_handle(struct touchpanel_data *ts)
{
    TPD_INFO("%s\n", __func__);

    if(ts->ts_ops->write_ps_status) {
        ts->ts_ops->write_ps_status(ts->chip_data, ts->ps_status);

        if (!ts->ps_status) {
            if (ts->ts_ops->exit_esd_mode) {
                ts->ts_ops->exit_esd_mode(ts->chip_data);
            }
        }
    }

    // operate_mode_switch(ts);

    tp_btnkey_release(ts);
    tp_touch_release(ts);
}

static void tp_geture_info_transform(struct gesture_info *gesture, struct resolution_info *resolution_info)
{
    gesture->Point_start.x = gesture->Point_start.x * resolution_info->LCD_WIDTH  / (resolution_info->max_x);
    gesture->Point_start.y = gesture->Point_start.y * resolution_info->LCD_HEIGHT / (resolution_info->max_y);
    gesture->Point_end.x   = gesture->Point_end.x   * resolution_info->LCD_WIDTH  / (resolution_info->max_x);
    gesture->Point_end.y   = gesture->Point_end.y   * resolution_info->LCD_HEIGHT / (resolution_info->max_y);
    gesture->Point_1st.x   = gesture->Point_1st.x   * resolution_info->LCD_WIDTH  / (resolution_info->max_x);
    gesture->Point_1st.y   = gesture->Point_1st.y   * resolution_info->LCD_HEIGHT / (resolution_info->max_y);
    gesture->Point_2nd.x   = gesture->Point_2nd.x   * resolution_info->LCD_WIDTH  / (resolution_info->max_x);
    gesture->Point_2nd.y   = gesture->Point_2nd.y   * resolution_info->LCD_HEIGHT / (resolution_info->max_y);
    gesture->Point_3rd.x   = gesture->Point_3rd.x   * resolution_info->LCD_WIDTH  / (resolution_info->max_x);
    gesture->Point_3rd.y   = gesture->Point_3rd.y   * resolution_info->LCD_HEIGHT / (resolution_info->max_y);
    gesture->Point_4th.x   = gesture->Point_4th.x   * resolution_info->LCD_WIDTH  / (resolution_info->max_x);
    gesture->Point_4th.y   = gesture->Point_4th.y   * resolution_info->LCD_HEIGHT / (resolution_info->max_y);
}

int sec_double_tap(struct gesture_info *gesture)
{
    uint32_t timeuse = 0;

    if (sigle_num == 0) {
        ktime_get_real_ts64(&tpstart);
        pointx[0] = gesture->Point_start.x;
        pointy[0] = gesture->Point_start.y;
        sigle_num++;
        TPD_DEBUG("first enter double tap\n");
    } else if (sigle_num == 1) {
        ktime_get_real_ts64(&tpend);
        pointx[1] = gesture->Point_start.x;
        pointy[1] = gesture->Point_start.y;
        sigle_num = 0;
        timeuse = 1000000 * (tpend.tv_sec-tpstart.tv_sec) + (tpend.tv_nsec-tpstart.tv_nsec) / 1000;
        TPD_DEBUG("timeuse = %d, distance[x] = %d, distance[y] = %d\n", timeuse, ABS(pointx[0], pointx[1]), ABS(pointy[0], pointy[1]));
        if ((ABS(pointx[0], pointx[1]) < 150) && (ABS(pointy[0], pointy[1]) < 200) && (timeuse < 500000)) {
            return 1;
        } else {
            TPD_DEBUG("not match double tap\n");
            ktime_get_real_ts64(&tpstart);
            pointx[0] = gesture->Point_start.x;
            pointy[0] = gesture->Point_start.y;
            sigle_num = 1;
        }
    }
    return 0;
}

static void tp_gesture_handle(struct touchpanel_data *ts)
{
    struct gesture_info gesture_info_temp;

    if (!ts->ts_ops->get_gesture_info) {
        TPD_INFO("not support ts->ts_ops->get_gesture_info callback\n");
        return;
    }

    memset(&gesture_info_temp, 0, sizeof(struct gesture_info));
    ts->ts_ops->get_gesture_info(ts->chip_data, &gesture_info_temp);
    tp_geture_info_transform(&gesture_info_temp, &ts->resolution_info);
    if (ts->single_tap_support) {
        if (gesture_info_temp.gesture_type == SingleTap) {
            if (sec_double_tap(&gesture_info_temp) == 1) {
                gesture_info_temp.gesture_type  = DouTap;
            }
        }
    }

    TPD_INFO("detect %s gesture\n", gesture_info_temp.gesture_type == DouTap ? "double tap" :
             gesture_info_temp.gesture_type == UpVee ? "up vee" :
             gesture_info_temp.gesture_type == DownVee ? "down vee" :
             gesture_info_temp.gesture_type == LeftVee ? "(>)" :
             gesture_info_temp.gesture_type == RightVee ? "(<)" :
             gesture_info_temp.gesture_type == Circle ? "circle" :
             gesture_info_temp.gesture_type == DouSwip ? "(||)" :
             gesture_info_temp.gesture_type == Left2RightSwip ? "(-->)" :
             gesture_info_temp.gesture_type == Right2LeftSwip ? "(<--)" :
             gesture_info_temp.gesture_type == Up2DownSwip ? "up to down |" :
             gesture_info_temp.gesture_type == Down2UpSwip ? "down to up |" :
             gesture_info_temp.gesture_type == Mgestrue ? "(M)" :
             gesture_info_temp.gesture_type == Wgestrue ? "(W)" :
             gesture_info_temp.gesture_type == FingerprintDown ? "(fingerprintdown)" :
             gesture_info_temp.gesture_type == FingerprintUp ? "(fingerprintup)" :
             gesture_info_temp.gesture_type == SingleTap ? "single tap" :
             gesture_info_temp.gesture_type == Heart ? "heart" : "unknown");
#if GESTURE_COORD_GET
    if (ts->ts_ops->get_gesture_coord) {
        ts->ts_ops->get_gesture_coord(ts->chip_data, gesture_info_temp.gesture_type);
    }
#endif
    if (gesture_info_temp.gesture_type == DouTap && CHK_BIT(ts->gesture_enable_indep, (1 << gesture_info_temp.gesture_type))) {
        memcpy(&ts->gesture, &gesture_info_temp, sizeof(struct gesture_info));
        input_report_key(ts->input_dev, KEY_WAKEUP, 1);
        input_sync(ts->input_dev);
        input_report_key(ts->input_dev, KEY_WAKEUP, 0);
        input_sync(ts->input_dev);
    } else if (gesture_info_temp.gesture_type != UnkownGesture && gesture_info_temp.gesture_type != FingerprintDown && gesture_info_temp.gesture_type != FingerprintUp && CHK_BIT(ts->gesture_enable_indep, (1 << gesture_info_temp.gesture_type))) {
        memcpy(&ts->gesture, &gesture_info_temp, sizeof(struct gesture_info));
#if GESTURE_RATE_MODE
        if(ts->geature_ignore)
            return;
#endif
        input_report_key(ts->input_dev, KEY_GESTURE_START + gesture_info_temp.gesture_type, 1);
        input_sync(ts->input_dev);
        input_report_key(ts->input_dev, KEY_GESTURE_START + gesture_info_temp.gesture_type, 0);
        input_sync(ts->input_dev);

    } else if (gesture_info_temp.gesture_type == FingerprintDown) {
        ts->fp_info.touch_state = 1;
        if (ts->screenoff_fingerprint_info_support) {
            ts->fp_info.x = gesture_info_temp.Point_start.x;
            ts->fp_info.y = gesture_info_temp.Point_start.y;
        }
        opticalfp_irq_handler(&ts->fp_info);
        notify_display_fpd(true);
    } else if (gesture_info_temp.gesture_type == FingerprintUp) {
        ts->fp_info.touch_state = 0;
        if (ts->screenoff_fingerprint_info_support) {
            ts->fp_info.x = gesture_info_temp.Point_start.x;
            ts->fp_info.y = gesture_info_temp.Point_start.y;
        }
        opticalfp_irq_handler(&ts->fp_info);
        notify_display_fpd(false);
    }
}

void tp_touch_btnkey_release(void)
{
    struct touchpanel_data *ts = g_tp;

    if (!ts) {
        TPD_INFO("ts is NULL\n");
        return ;
    }

    tp_touch_release(ts);
    tp_btnkey_release(ts);
}

static void tp_touch_release(struct touchpanel_data *ts)
{
#ifdef TYPE_B_PROTOCOL

    int i = 0;

    if (ts->report_flow_unlock_support) {
        mutex_lock(&ts->report_mutex);
    }
    for (i = 0; i < ts->max_num; i++) {
        input_mt_slot(ts->input_dev, i);
        input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
    }
    input_report_key(ts->input_dev, BTN_TOUCH, 0);
    input_report_key(ts->input_dev, BTN_TOOL_FINGER, 0);
    input_sync(ts->input_dev);
    if (ts->report_flow_unlock_support) {
        mutex_unlock(&ts->report_mutex);
    }
#else
    input_report_key(ts->input_dev, BTN_TOUCH, 0);
    input_report_key(ts->input_dev, BTN_TOOL_FINGER, 0);
    input_mt_sync(ts->input_dev);
    input_sync(ts->input_dev);
#endif
    TPD_DETAIL("release all touch point and key, clear tp touch down flag\n");
    ts->view_area_touched = 0; //realse all touch point,must clear this flag
    ts->touch_count = 0;
    ts->irq_slot = 0;
}

static bool edge_point_process(struct touchpanel_data *ts, struct point_info points)
{
    if (ts->limit_edge) {
        if (points.x > ts->edge_limit.left_x2 && points.x < ts->edge_limit.right_x2) {
            if (ts->edge_limit.in_which_area == AREA_EDGE)
                tp_touch_release(ts);
            ts->edge_limit.in_which_area = AREA_NORMAL;
        } else if ((points.x > ts->edge_limit.left_x1 && points.x < ts->edge_limit.left_x2) || (points.x > ts->edge_limit.right_x2 && points.x < ts->edge_limit.right_x1)) { //area2
            if (ts->edge_limit.in_which_area == AREA_EDGE) {
                ts->edge_limit.in_which_area = AREA_CRITICAL;
            }
        } else if (points.x < ts->edge_limit.left_x1 || points.x > ts->edge_limit.right_x1) {        //area 1
            if (ts->edge_limit.in_which_area == AREA_CRITICAL) {
                ts->edge_limit.in_which_area = AREA_EDGE;
                return true;
            }
            if (ts->edge_limit.in_which_area ==  AREA_NORMAL)
                return true;

            ts->edge_limit.in_which_area = AREA_EDGE;
        }
    }

    return false;
}

static bool corner_point_process(struct touchpanel_data *ts, struct corner_info *corner, struct point_info *points, int i)
{
    int j;
    if (ts->limit_corner) {
        if ((ts->limit_corner & (1 << CORNER_TOPLEFT)) && (points[i].x < ts->edge_limit.left_x3 && points[i].y < ts->edge_limit.left_y1)) {
            points[i].type  = AREA_CORNER;
            if (ts->edge_limit.in_which_area == AREA_NORMAL)
                return true;

            corner[CORNER_TOPLEFT].id = i;
            corner[CORNER_TOPLEFT].point = points[i];
            corner[CORNER_TOPLEFT].flag = true;

            ts->edge_limit.in_which_area = points[i].type;
        }
        if ((ts->limit_corner & (1 << CORNER_TOPRIGHT))  && (points[i].x < ts->edge_limit.left_x3 && points[i].y > ts->edge_limit.right_y1)) {
            points[i].type  = AREA_CORNER;
            if (ts->edge_limit.in_which_area == AREA_NORMAL)
                return true;

            corner[CORNER_TOPRIGHT].id = i;
            corner[CORNER_TOPRIGHT].point = points[i];
            corner[CORNER_TOPRIGHT].flag = true;

            ts->edge_limit.in_which_area = points[i].type;
        }
        if ((ts->limit_corner & (1 << CORNER_BOTTOMLEFT))  && (points[i].x > ts->edge_limit.right_x3 && points[i].y < ts->edge_limit.left_y1)) {
            points[i].type  = AREA_CORNER;
            if (ts->edge_limit.in_which_area == AREA_NORMAL)
                return true;

            corner[CORNER_BOTTOMLEFT].id = i;
            corner[CORNER_BOTTOMLEFT].point = points[i];
            corner[CORNER_BOTTOMLEFT].flag = true;

            ts->edge_limit.in_which_area = points[i].type;
        }
        if ((ts->limit_corner & (1 << CORNER_BOTTOMRIGHT))  && (points[i].x > ts->edge_limit.right_x3 && points[i].y > ts->edge_limit.right_y1)) {
            points[i].type  = AREA_CORNER;
            if (ts->edge_limit.in_which_area == AREA_NORMAL)
                return true;

            corner[CORNER_BOTTOMRIGHT].id = i;
            corner[CORNER_BOTTOMRIGHT].point = points[i];
            corner[CORNER_BOTTOMRIGHT].flag = true;

            ts->edge_limit.in_which_area = points[i].type;
        }

        if (points[i].type != AREA_CORNER) {
            if (ts->edge_limit.in_which_area == AREA_CORNER) {
                for (j = 0; j < 4; j++) {
                    if (corner[j].flag) {
#ifdef TYPE_B_PROTOCOL
                        input_mt_slot(ts->input_dev, corner[j].id);
                        input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
#endif
                    }
                }
            }
            points[i].type = AREA_NORMAL;
            ts->edge_limit.in_which_area = points[i].type;
        }

    }

    return false;
}

static void tp_touch_handle(struct touchpanel_data *ts)
{
    int i = 0;
    uint8_t finger_num = 0, touch_near_edge = 0, finger_num_center = 0;
    int obj_attention = 0;
    struct point_info points[10];
    struct corner_info corner[4];
    static bool up_status = false;
    static struct point_info last_point = {.x = 0, .y = 0};
    static int touch_report_num = 0;
    static unsigned int repeat_count = 0;

    if (!ts->ts_ops->get_touch_points) {
        TPD_INFO("not support ts->ts_ops->get_touch_points callback\n");
        return;
    }

    memset(points, 0, sizeof(points));
    memset(corner, 0, sizeof(corner));

    if (time_after(jiffies, ts->monitor_data.monitor_down) && ts->monitor_data.monitor_down) {
        ts->monitor_data.miss_irq++;
        TPD_DEBUG("ts->monitor_data.monitor_down %lu jiffies %lu\n", ts->monitor_data.monitor_down, jiffies);
    }
    obj_attention = ts->ts_ops->get_touch_points(ts->chip_data, points, ts->max_num);
    if ((obj_attention == -EINVAL) || (obj_attention < 0)) {
        TPD_DEBUG("Invalid points, ignore..\n");
        return;
    }

    mutex_lock(&ts->report_mutex);

    if ((obj_attention & TOUCH_BIT_CHECK) != 0) {
        up_status = false;
        ts->monitor_data.monitor_down = (jiffies + 2 * HZ) * (!ts->is_suspended);
        for (i = 0; i < ts->max_num; i++) {
            if (((obj_attention & TOUCH_BIT_CHECK) >> i) & 0x01 && (points[i].status == 0)) // buf[0] == 0 is wrong point, no process
                continue;
            if (((obj_attention & TOUCH_BIT_CHECK) >> i) & 0x01 && (points[i].status != 0)) {
                //Edge process before report abs
                if (ts->edge_limit_support) {
                    if (corner_point_process(ts, corner, points, i) || (!ts->drlimit_remove_support && edge_point_process(ts, points[i])))
                        continue;
                }
#ifdef TYPE_B_PROTOCOL
                input_mt_slot(ts->input_dev, i);
                input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 1);
#endif
                touch_report_num++;
                tp_touch_down(ts, points[i], touch_report_num, i);
                SET_BIT(ts->irq_slot, (1 << i));
                finger_num++;
                if (ts->ear_sense_support && ts->es_enable && \
                    (((points[i].y < ts->resolution_info.max_y / 2) && (points[i].x > 30) && (points[i].x < ts->resolution_info.max_x - 30)) || \
                     ((points[i].y > ts->resolution_info.max_y / 2) && (points[i].x > 70) && (points[i].x < ts->resolution_info.max_x - 70)))) {
                    finger_num_center++;
                }
                if (ts->face_detect_support && ts->fd_enable && \
                    (points[i].y < ts->resolution_info.max_y / 2) && (points[i].x > 30) && (points[i].x < ts->resolution_info.max_x - 30)) {
                    finger_num_center++;
                }
                if (points[i].x > ts->resolution_info.max_x / 100 && points[i].x < ts->resolution_info.max_x * 99 / 100) {
                    ts->view_area_touched = finger_num;
                } else {
                    touch_near_edge++;
                }
                /*strore  the last point data*/
                memcpy(&last_point, &points[i], sizeof(struct point_info));
            }
#ifdef TYPE_B_PROTOCOL
            else {
                input_mt_slot(ts->input_dev, i);
            
                input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
            }
#endif
        }

        if (touch_near_edge == finger_num) {        //means all the touchpoint is near the edge
            ts->view_area_touched = 0;
        }
        if(ts->ear_sense_support && ts->es_enable && (finger_num_center > (ts->touch_count & 0x0F))) {
            ts->delta_state = TYPE_DELTA_BUSY;
            queue_work(ts->delta_read_wq, &ts->read_delta_work);
        }
    } else {
        if (up_status) {
            tp_touch_up(ts);
            mutex_unlock(&ts->report_mutex);
            return;
        }
        if (time_before(jiffies, ts->monitor_data.monitor_up) && ts->monitor_data.monitor_up) {
            repeat_count++;
            if (repeat_count == 5) {
                ts->monitor_data.repeat_finger++;
            }
        } else {
            repeat_count = 0;
        }
        ts->total_operate_times++;
        ts->monitor_data.monitor_up = jiffies + 4;
        ts->monitor_data.monitor_down = 0;
        finger_num = 0;
        finger_num_center = 0;
        touch_report_num = 0;
        repeat_count = 0;
#ifdef TYPE_B_PROTOCOL
        for (i = 0; i < ts->max_num; i++) {
            input_mt_slot(ts->input_dev, i);
            input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
        }
#endif
        tp_touch_up(ts);
        ts->view_area_touched = 0;
        ts->irq_slot = 0;
        up_status = true;
        if (ts->edge_limit_support)
            ts->edge_limit.in_which_area = AREA_NOTOUCH;
    }
    input_sync(ts->input_dev);
    ts->touch_count = (finger_num << 4) | (finger_num_center & 0x0F);

    mutex_unlock(&ts->report_mutex);
}

static void tp_btnkey_release(struct touchpanel_data *ts)
{
    if (CHK_BIT(ts->vk_bitmap, BIT_MENU))
        input_report_key_oppo(ts->kpd_input_dev, KEY_MENU, 0);
    if (CHK_BIT(ts->vk_bitmap, BIT_HOME))
        input_report_key_oppo(ts->kpd_input_dev, KEY_HOMEPAGE, 0);
    if (CHK_BIT(ts->vk_bitmap, BIT_BACK))
        input_report_key_oppo(ts->kpd_input_dev, KEY_BACK, 0);
    input_sync(ts->kpd_input_dev);
}

static void tp_btnkey_handle(struct touchpanel_data *ts)
{
    u8 touch_state = 0;

    if (ts->vk_type != TYPE_AREA_SEPRATE) {
        TPD_DEBUG("TP vk_type not proper, checktouchpanel, button-type\n");

        return;
    }
    if (!ts->ts_ops->get_keycode) {
        TPD_INFO("not support ts->ts_ops->get_keycode callback\n");

        return;
    }
    touch_state = ts->ts_ops->get_keycode(ts->chip_data);

    if (CHK_BIT(ts->vk_bitmap, BIT_MENU))
        input_report_key_oppo(ts->kpd_input_dev, KEY_MENU, CHK_BIT(touch_state, BIT_MENU));
    if (CHK_BIT(ts->vk_bitmap, BIT_HOME))
        input_report_key_oppo(ts->kpd_input_dev, KEY_HOMEPAGE, CHK_BIT(touch_state, BIT_HOME));
    if (CHK_BIT(ts->vk_bitmap, BIT_BACK))
        input_report_key_oppo(ts->kpd_input_dev, KEY_BACK, CHK_BIT(touch_state, BIT_BACK));
    input_sync(ts->kpd_input_dev);
}

static void tp_config_handle(struct touchpanel_data *ts)
{
    int ret = 0;
    if (!ts->ts_ops->fw_handle) {
        TPD_INFO("not support ts->ts_ops->fw_handle callback\n");
        return;
    }

    ret = ts->ts_ops->fw_handle(ts->chip_data);
}


static void tp_async_work_callback(void)
{
    struct touchpanel_data *ts = g_tp;

    if (ts == NULL){
        return;
    }
    TPD_INFO("%s: async work\n", __func__);
    if (ts->use_resume_notify && ts->suspend_state == TP_RESUME_COMPLETE) {
        complete(&ts->resume_complete);
        return;
    }

    if (ts->in_test_process) {
        TPD_INFO("%s: In test process, do not switch mode\n", __func__);
        return;
    }
    TPD_INFO("%s schedule_work  start !!", __func__);
    schedule_work(&ts->async_work);
}

static void tp_async_work_lock(struct work_struct *work)
{
    struct touchpanel_data *ts = container_of(work, struct touchpanel_data, async_work);
    mutex_lock(&ts->mutex);
    if (ts->ts_ops->async_work) {
        ts->ts_ops->async_work(ts->chip_data);
    }
    mutex_unlock(&ts->mutex);
}

static void tp_work_common_callback(void)
{
    struct touchpanel_data *ts;

    if (g_tp == NULL)
        return;
    ts = g_tp;
    tp_work_func(ts);
}

static void tp_work_func(struct touchpanel_data *ts)
{
    u32 cur_event = 0;

    if (!ts->ts_ops->trigger_reason && !ts->ts_ops->u32_trigger_reason) {
        TPD_INFO("not support ts_ops->trigger_reason callback\n");
        return;
    }
    /*
     *  trigger_reason:this callback determine which trigger reason should be
     *  The value returned has some policy!
     *  1.IRQ_EXCEPTION /IRQ_GESTURE /IRQ_IGNORE /IRQ_FW_CONFIG --->should be only reported  individually
     *  2.IRQ_TOUCH && IRQ_BTN_KEY --->should depends on real situation && set correspond bit on trigger_reason
     */
    if (ts->ts_ops->u32_trigger_reason) {
        cur_event = ts->ts_ops->u32_trigger_reason(ts->chip_data, (ts->gesture_enable || ts->fp_enable), ts->is_suspended);
    } else {
        cur_event = ts->ts_ops->trigger_reason(ts->chip_data, (ts->gesture_enable || ts->fp_enable), ts->is_suspended);
    }
    if (CHK_BIT(cur_event, IRQ_TOUCH) || CHK_BIT(cur_event, IRQ_BTN_KEY) || CHK_BIT(cur_event, IRQ_FW_HEALTH) || \
        CHK_BIT(cur_event, IRQ_FACE_STATE) || CHK_BIT(cur_event, IRQ_FINGERPRINT)) {
        if (CHK_BIT(cur_event, IRQ_BTN_KEY)) {
            tp_btnkey_handle(ts);
        }
        if (CHK_BIT(cur_event, IRQ_TOUCH) && (!ts->is_suspended)) {
            tp_touch_handle(ts);
        }
    } else if (CHK_BIT(cur_event, IRQ_GESTURE)) {
        tp_gesture_handle(ts);
    } else if (CHK_BIT(cur_event, IRQ_EXCEPTION)) {
        tp_exception_handle(ts);
    } else if (CHK_BIT(cur_event, IRQ_FW_CONFIG)) {
        tp_config_handle(ts);
    }  else if (CHK_BIT(cur_event, IRQ_FW_AUTO_RESET)) {
        tp_fw_auto_reset_handle(ts);
    } else {
        TPD_DEBUG("unknown irq trigger reason\n");
    }
}

static void tp_work_func_unlock(struct touchpanel_data *ts)
{
    if (ts->ts_ops->irq_handle_unlock) {
        ts->ts_ops->irq_handle_unlock(ts->chip_data);
    }
}

void __attribute__((weak)) display_esd_check_enable_bytouchpanel(bool enable)
{
    return;
}

static void tp_fw_update_work(struct work_struct *work)
{
    const struct firmware *fw = NULL;
    int ret, fw_update_result = 0;
    int count_tmp = 0, retry = 5;
    char *p_node = NULL;
    char *fw_name_fae = NULL;
    char *postfix = "_FAE";
    uint8_t copy_len = 0;
    // u64 start_time = 0;

    struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
                                 fw_update_work);

    if (!ts->ts_ops->fw_check || !ts->ts_ops->reset) {
        TPD_INFO("not support ts_ops->fw_check callback\n");
        complete(&ts->fw_complete);
        return;
    }
    
    ts->panel_data.fw_name = "FW_HX83112A_NF_DSJM.img";

    TPD_INFO("%s: fw_name = %s\n", __func__, ts->panel_data.fw_name);

    mutex_lock(&ts->mutex);

    if (!ts->irq_trigger_hdl_support && ts->int_mode == BANNABLE) {
        disable_irq_nosync(ts->irq);
    }

    ts->loading_fw = true;

    if (ts->esd_handle_support) {
        esd_handle_switch(&ts->esd_info, false);
    }

    display_esd_check_enable_bytouchpanel(0);

    if (ts->ts_ops->fw_update) {
        do {
            if(ts->firmware_update_type == 0 || ts->firmware_update_type == 1) {
                if(ts->fw_update_app_support) {
                    fw_name_fae = kzalloc(MAX_FW_NAME_LENGTH, GFP_KERNEL);
                    if(fw_name_fae == NULL) {
                        TPD_INFO("fw_name_fae kzalloc error!\n");
                        goto EXIT;
                    }
                    p_node  = strstr(ts->panel_data.fw_name, ".");
                    if(p_node == NULL) {
                        TPD_INFO("p_node strstr error!\n");
                        goto EXIT;
                    }
                    copy_len = p_node - ts->panel_data.fw_name;
                    memcpy(fw_name_fae, ts->panel_data.fw_name, copy_len);
                    strlcat(fw_name_fae, postfix, MAX_FW_NAME_LENGTH);
                    strlcat(fw_name_fae, p_node, MAX_FW_NAME_LENGTH);
                    TPD_INFO("fw_name_fae is %s\n", fw_name_fae);
                    ret = request_firmware(&fw, fw_name_fae, ts->dev);
                    if (!ret)
                        break;
                } else {
                    ret = request_firmware(&fw, ts->panel_data.fw_name, ts->dev);
                    if (!ret)
                        break;
                }
            } else {
                ret = request_firmware_select(&fw, ts->panel_data.fw_name, ts->dev);
                if (!ret)
                    break;
            }
        } while((ret < 0) && (--retry > 0));

        TPD_DETAIL("retry times %d\n", 5 - retry);

        if (!ret || ts->is_noflash_ic) {
            // do {
            //     count_tmp++;
            //     ret = ts->ts_ops->fw_update(ts->chip_data, fw, ts->force_update);
            //     fw_update_result = ret;
            //     if (ret == FW_NO_NEED_UPDATE) {
            //         TPD_INFO("NO NEED TO UPDATE");
            //         break;
            //     }

            //     if(!ts->is_noflash_ic) {        //noflash update fw in reset and do bootloader reset in get_chip_info
            //         ret |= ts->ts_ops->reset(ts->chip_data);
            //         ret |= ts->ts_ops->get_chip_info(ts->chip_data);
            //     }

            //     ret |= ts->ts_ops->fw_check(ts->chip_data, &ts->resolution_info, &ts->panel_data);
            // } while((count_tmp < 2) && (ret != 0));

            if(fw != NULL) {
                ts->fw = fw;
                // release_firmware(fw);
            }
        } else {
            TPD_INFO("%s: fw_name request failed %s %d\n", __func__, ts->panel_data.fw_name, ret);
            goto EXIT;
        }
    }

    if (ts->ts_ops->bootup_test && ts->health_monitor_support) {
        ret = request_firmware(&fw, ts->panel_data.test_limit_name, ts->dev);
        if (ret < 0) {
            TPD_INFO("Request firmware failed - %s (%d)\n", ts->panel_data.test_limit_name, ret);
        } else {
            ts->ts_ops->bootup_test(ts->chip_data, fw, &ts->monitor_data, &ts->hw_res);
            TPD_DETAIL("firmware released");
            release_firmware(fw);
        }
    }

    tp_touch_release(ts);
    tp_btnkey_release(ts);
    // operate_mode_switch(ts);
    if (fw_update_result != FW_NO_NEED_UPDATE) {
        if (ts->spurious_fp_support && ts->ts_ops->finger_proctect_data_get) {
            ts->ts_ops->finger_proctect_data_get(ts->chip_data);
        }
    }

EXIT:
    ts->loading_fw = false;


    if (ts->esd_handle_support) {
        esd_handle_switch(&ts->esd_info, true);
    }

    kfree(fw_name_fae);
    fw_name_fae = NULL;
    if (ts->int_mode == BANNABLE) {
        enable_irq(ts->irq);
    }
    mutex_unlock(&ts->mutex);
    ts->force_update = 0;

    complete(&ts->fw_complete); //notify to init.rc that fw update finished
    return;
}

#ifndef TPD_USE_EINT
static enum hrtimer_restart touchpanel_timer_func(struct hrtimer *timer)
{
    struct touchpanel_data *ts = container_of(timer, struct touchpanel_data, timer);

    mutex_lock(&ts->mutex);
    tp_work_func(ts);
    mutex_unlock(&ts->mutex);
    hrtimer_start(&ts->timer, ktime_set(0, 12500000), HRTIMER_MODE_REL);

    return HRTIMER_NORESTART;
}
#else
static irqreturn_t tp_irq_thread_fn(int irq, void *dev_id)
{
    struct touchpanel_data *ts = (struct touchpanel_data *)dev_id;
    if (ts->ts_ops->tp_irq_throw_away) {
        if (ts->ts_ops->tp_irq_throw_away(ts->chip_data)) {
            return IRQ_HANDLED;
        }
    }

    if (ts->irq_need_dev_resume_ok) {
        if (ts->i2c_ready == false) {
            //TPD_INFO("Wait device resume!");
            wait_event_interruptible_timeout(waiter,
                                             ts->i2c_ready,
                                             msecs_to_jiffies(50));
            //TPD_INFO("Device maybe resume!");
        }
        if (ts->i2c_ready == false) {
            TPD_INFO("The device not resume 50ms!");
            return IRQ_HANDLED;
        }
    }

    if(ts->sec_long_low_trigger) {
        disable_irq_nosync(ts->irq);
    }
    if (ts->int_mode == BANNABLE) {
        mutex_lock(&ts->mutex);
        tp_work_func(ts);
        mutex_unlock(&ts->mutex);
    } else {
        tp_work_func_unlock(ts);
    }
    if(ts->sec_long_low_trigger) {
        enable_irq(ts->irq);
    }

    return IRQ_HANDLED;
}
#endif

static void tp_freq_hop_work(struct work_struct *work)
{
    struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
                                 freq_hop_info.freq_hop_work.work);

    TPD_INFO("syna_tcm_freq_hop_work\n");
    if (!ts->is_suspended) {
        TPD_INFO("trigger frequency hopping~~~~\n");

        if (!ts->ts_ops->freq_hop_trigger) {
            TPD_INFO("%s:not support ts_ops->freq_hop_trigger callback\n", __func__);
            return;
        }
        ts->ts_ops->freq_hop_trigger(ts->chip_data);
    }

    if (ts->freq_hop_info.freq_hop_simulating) {
        TPD_INFO("queue_delayed_work again\n");
        queue_delayed_work(ts->freq_hop_info.freq_hop_workqueue, &ts->freq_hop_info.freq_hop_work, ts->freq_hop_info.freq_hop_freq * HZ);
    }
}

/**
 * tp_gesture_enable_flag -   expose gesture control status for other module.
 * Return gesture_enable status.
 */
int tp_gesture_enable_flag(void)
{
    if (!g_tp || !g_tp->is_incell_panel)
        return LCD_POWER_OFF;

    TPD_INFO("g_tp->gesture_enable is %d\n", g_tp->gesture_enable);

    return (g_tp->gesture_enable > 0) ? LCD_POWER_ON : LCD_POWER_OFF;
}
EXPORT_SYMBOL(tp_gesture_enable_flag);
/*
*Interface for lcd to control reset pin
*/
int tp_control_reset_gpio(bool enable)
{
    if (!g_tp) {
        return 0;
    }

    if (gpio_is_valid(g_tp->hw_res.reset_gpio)) {
        if (g_tp->ts_ops->reset_gpio_control) {
            g_tp->ts_ops->reset_gpio_control(g_tp->chip_data, enable);
        }
    }

    return 0;
}


int tp_control_cs_gpio(bool enable)
{
    if(!g_tp){
        return 0;
    }

    if (!(IS_ERR(g_tp->hw_res.cs_gpiod))) {
        if (g_tp->ts_ops->cs_gpio_control) {
            g_tp->ts_ops->cs_gpio_control(g_tp->chip_data, enable);
        }
    }

    return 0;
}

EXPORT_SYMBOL(tp_control_cs_gpio);


/*
 * check_touchirq_triggered--used for stop system going sleep when touch irq is triggered
 * 1 if irq triggered, otherwise is 0
*/
int check_touchirq_triggered(void)
{
    int value = -1;

    if (!g_tp) {
        return 0;
    }
    if ((1 != (g_tp->gesture_enable & 0x01)) && (0 == g_tp->fp_enable)) {
        return 0;
    }

    value = gpio_get_value(g_tp->hw_res.irq_gpio);
    if ((0 == value) && (g_tp->irq_flags & IRQF_TRIGGER_LOW)) {
        TPD_INFO("touch irq is triggered.\n");
        return 1; //means irq is triggered
    }
    if ((1 == value) && (g_tp->irq_flags & IRQF_TRIGGER_HIGH)) {
        TPD_INFO("touch irq is triggered.\n");
        return 1; //means irq is triggered
    }

    return 0;
}
EXPORT_SYMBOL(check_touchirq_triggered);


/*
 *  * check_headset_state----expose to be called by audio int to get headset state
 *   * @headset_state : 1 if headset checked, otherwise is 0
 *   */
void switch_headset_state(int headset_state)
{
    if (!g_tp) {
        return;
    }
    TPD_INFO("%s: ENTER\n", __func__);

    if (g_tp->headset_pump_support && (g_tp->is_headset_checked != headset_state)) {
        g_tp->is_headset_checked = !!headset_state;
        TPD_INFO("%s: check headset state : %d, is_suspended: %d\n", __func__, headset_state, g_tp->is_suspended);
        if (!g_tp->is_suspended && (g_tp->suspend_state == TP_SPEEDUP_RESUME_COMPLETE)
            && !g_tp->loading_fw) {
            mutex_lock(&g_tp->mutex);
            g_tp->ts_ops->mode_switch(g_tp->chip_data, MODE_HEADSET, g_tp->is_headset_checked);
            mutex_unlock(&g_tp->mutex);
        }
    }
    TPD_INFO("%s: END\n", __func__);
}
EXPORT_SYMBOL(switch_headset_state);

static ssize_t cap_vk_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct button_map *button_map;
    if (!g_tp)
        return sprintf(buf, "not support");

    button_map = &g_tp->button_map;
    return sprintf(buf,
                   __stringify(EV_KEY) ":" __stringify(KEY_MENU)   ":%d:%d:%d:%d"
                   ":" __stringify(EV_KEY) ":" __stringify(KEY_HOMEPAGE)   ":%d:%d:%d:%d"
                   ":" __stringify(EV_KEY) ":" __stringify(KEY_BACK)   ":%d:%d:%d:%d"
                   "\n", button_map->coord_menu.x, button_map->coord_menu.y, button_map->width_x, button_map->height_y, \
                   button_map->coord_home.x, button_map->coord_home.y, button_map->width_x, button_map->height_y, \
                   button_map->coord_back.x, button_map->coord_back.y, button_map->width_x, button_map->height_y);
}

static struct kobj_attribute virtual_keys_attr = {
    .attr = {
        .name = "virtualkeys."TPD_DEVICE,
        .mode = S_IRUGO,
    },
    .show = &cap_vk_show,
};

static struct attribute *properties_attrs[] = {
    &virtual_keys_attr.attr,
    NULL
};

static struct attribute_group properties_attr_group = {
    .attrs = properties_attrs,
};

static int finger_protect_handler(void *data)
{
    struct touchpanel_data *ts = (struct touchpanel_data *)data;
    if (!ts) {
        TPD_INFO("ts is null should nerver get here!\n");
        return 0;
    };
    if (!ts->ts_ops->spurious_fp_check) {
        TPD_INFO("not support spurious_fp_check call back\n");
        return 0;
    }

    do {
        if (ts->spuri_fp_touch.lcd_trigger_fp_check)
            wait_event_interruptible(waiter, ts->spuri_fp_touch.fp_trigger && ts->i2c_ready && ts->spuri_fp_touch.lcd_resume_ok);
        else
            wait_event_interruptible(waiter, ts->spuri_fp_touch.fp_trigger && ts->i2c_ready);
        ts->spuri_fp_touch.fp_trigger = false;
        ts->spuri_fp_touch.fp_touch_st = FINGER_PROTECT_NOTREADY;

        mutex_lock(&ts->mutex);
        if (g_tp->spuri_fp_touch.lcd_trigger_fp_check && !g_tp->spuri_fp_touch.lcd_resume_ok) {
            TPD_INFO("LCD is suspend, can not detect finger touch in incell panel\n");
            mutex_unlock(&ts->mutex);
            continue;
        }

        ts->spuri_fp_touch.fp_touch_st = ts->ts_ops->spurious_fp_check(ts->chip_data);
        if (ts->view_area_touched) {
            TPD_INFO("%s tp touch down,clear flag\n", __func__);
            ts->view_area_touched = 0;
        }
        // operate_mode_switch(ts);
        mutex_unlock(&ts->mutex);
    } while (!kthread_should_stop());
    return 0;
}

/**
 * init_input_device - Using for register input device
 * @ts: touchpanel_data struct using for common driver
 *
 * we should using this function setting input report capbility && register input device
 * Returning zero(success) or negative errno(failed)
 */
static int init_input_device(struct touchpanel_data *ts)
{
    int ret = 0, i = 0;
    struct kobject *vk_properties_kobj;

    TPD_INFO("%s is called\n", __func__);
    ts->input_dev = input_allocate_device();
    if (ts->input_dev == NULL) {
        ret = -ENOMEM;
        TPD_INFO("Failed to allocate input device\n");
        return ret;
    }

    ts->kpd_input_dev  = input_allocate_device();
    if (ts->kpd_input_dev == NULL) {
        ret = -ENOMEM;
        TPD_INFO("Failed to allocate key input device\n");
        return ret;
    }

    if (ts->face_detect_support) {
        ts->ps_input_dev  = input_allocate_device();
        if (ts->ps_input_dev == NULL) {
            ret = -ENOMEM;
            TPD_INFO("Failed to allocate ps input device\n");
            return ret;
        }

        ts->ps_input_dev->name = TPD_DEVICE"_ps";
        set_bit(EV_MSC, ts->ps_input_dev->evbit);
        set_bit(MSC_RAW, ts->ps_input_dev->mscbit);
    }

    ts->input_dev->name = TPD_DEVICE;
    set_bit(EV_SYN, ts->input_dev->evbit);
    set_bit(EV_ABS, ts->input_dev->evbit);
    set_bit(EV_KEY, ts->input_dev->evbit);
    set_bit(ABS_MT_TOUCH_MAJOR, ts->input_dev->absbit);
    set_bit(ABS_MT_WIDTH_MAJOR, ts->input_dev->absbit);
    set_bit(ABS_MT_POSITION_X, ts->input_dev->absbit);
    set_bit(ABS_MT_POSITION_Y, ts->input_dev->absbit);
    set_bit(ABS_MT_PRESSURE, ts->input_dev->absbit);
    set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);
    set_bit(BTN_TOUCH, ts->input_dev->keybit);
    if (ts->black_gesture_support) {
        set_bit(KEY_F4, ts->input_dev->keybit);
        set_bit(KEY_WAKEUP, ts->input_dev->keybit);
        for (i = UpVee; i <= SGESTRUE; i++) {
            set_bit(KEY_GESTURE_START + i, ts->input_dev->keybit);
        }
    }

    ts->kpd_input_dev->name = TPD_DEVICE"_kpd";
    set_bit(EV_KEY, ts->kpd_input_dev->evbit);
    set_bit(EV_SYN, ts->kpd_input_dev->evbit);

    switch(ts->vk_type) {
    case TYPE_PROPERTIES : {
        TPD_DEBUG("Type 1: using board_properties\n");
        vk_properties_kobj = kobject_create_and_add("board_properties", NULL);
        if (vk_properties_kobj)
            ret = sysfs_create_group(vk_properties_kobj, &properties_attr_group);
        if (!vk_properties_kobj || ret)
            TPD_DEBUG("failed to create board_properties\n");
        break;
    }
    case TYPE_AREA_SEPRATE: {
        TPD_DEBUG("Type 2:using same IC (button zone &&  touch zone are seprate)\n");
        if (CHK_BIT(ts->vk_bitmap, BIT_MENU))
            set_bit(KEY_MENU, ts->kpd_input_dev->keybit);
        if (CHK_BIT(ts->vk_bitmap, BIT_HOME))
            set_bit(KEY_HOMEPAGE, ts->kpd_input_dev->keybit);
        if (CHK_BIT(ts->vk_bitmap, BIT_BACK))
            set_bit(KEY_BACK, ts->kpd_input_dev->keybit);
        break;
    }
    default :
        break;
    }

#ifdef TYPE_B_PROTOCOL
    if (ts->external_touch_support) {
        input_mt_init_slots(ts->input_dev, ts->max_num + 1, 0);
    } else {
        input_mt_init_slots(ts->input_dev, ts->max_num, 0);
    }
#endif
    input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0); // Max value is overkill
    input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0); // Max values is overkill
    input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, ts->resolution_info.max_x - 1, 0, 0);
    input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, ts->resolution_info.max_y - 1, 0, 0);
    input_set_drvdata(ts->input_dev, ts);
    input_set_drvdata(ts->kpd_input_dev, ts);

    if (input_register_device(ts->input_dev)) {
        TPD_INFO("%s: Failed to register input device\n", __func__);
        input_free_device(ts->input_dev);
        return -1;
    }

    if (input_register_device(ts->kpd_input_dev)) {
        TPD_INFO("%s: Failed to register key input device\n", __func__);
        input_free_device(ts->kpd_input_dev);
        return -1;
    }

    if (ts->face_detect_support) {
        if (input_register_device(ts->ps_input_dev)) {
            TPD_INFO("%s: Failed to register ps input device\n", __func__);
            input_free_device(ts->ps_input_dev);
            return -1;
        }
    }

    return 0;
}

/**
 * init_parse_dts - parse dts, get resource defined in Dts
 * @dev: i2c_client->dev using to get device tree
 * @ts: touchpanel_data, using for common driver
 *
 * If there is any Resource needed by chip_data, we can add a call-back func in this function
 * Do not care the result : Returning void type
 */
static int init_parse_dts(struct device *dev, struct touchpanel_data *ts)
{
    int rc;
    struct device_node *np;
    int temp_array[8];
    int tx_rx_num[2];
    int val = 0;
    int i = 0;

    np = dev->of_node;
    rc = of_property_count_u32_elems(np, "platform_support_project");
    ts->panel_data.project_num = rc;
    if (!rc) {
        TPD_INFO("project not specified\n");
    }
    if (ts->panel_data.project_num >0){
        rc = of_property_read_u32_array(np, "platform_support_project", ts->panel_data.platform_support_project,ts->panel_data.project_num);
        if (rc)  {
            TPD_INFO("platform_support_project not specified");
            return -1;
        }
        rc = of_property_read_u32_array(np, "platform_support_project_dir", ts->panel_data.platform_support_project_dir,ts->panel_data.project_num);
        if (rc) {
            TPD_INFO("platform_support_project_dir not specified");
            return -1;
        }
        for (i=0; i<ts->panel_data.project_num; i++){
            ts->panel_data.platform_support_commandline[i] = devm_kzalloc(dev, 100, GFP_KERNEL);
            ts->panel_data.platform_support_external_name[i] = devm_kzalloc(dev, 7, GFP_KERNEL);
            if (ts->panel_data.platform_support_commandline[i] == NULL || ts->panel_data.platform_support_external_name[i] == NULL) {
                TPD_INFO("panel_data.platform_support_commandline or platform_support_external_name kzalloc error\n");
                goto commandline_kazalloc_error;
            }
            rc = of_property_read_string_index(np, "platform_support_project_external_name",i, (const char **)&ts->panel_data.platform_support_external_name[i]);
            if (rc) {
                TPD_INFO("platform_support_project_external_name not specified");
            }
        }
    }
    rc = of_property_read_u32(np, "tp_type", &ts->panel_data.tp_type);
    if (rc) {
        TPD_DETAIL("tp_type not specified\n");
    }


    /* Although most is unused in rmx1851, other devices use the hx83112a_nf and might need such proprties */
    ts->register_is_16bit       = of_property_read_bool(np, "register-is-16bit");
    ts->edge_limit_support      = of_property_read_bool(np, "edge_limit_support");
    ts->fw_edge_limit_support   = of_property_read_bool(np, "fw_edge_limit_support");
    ts->drlimit_remove_support  = of_property_read_bool(np, "drlimit_remove_support");
    ts->glove_mode_support      = of_property_read_bool(np, "glove_mode_support");
    ts->esd_handle_support      = of_property_read_bool(np, "esd_handle_support");
    ts->spurious_fp_support     = of_property_read_bool(np, "spurious_fingerprint_support");
    ts->charger_pump_support    = of_property_read_bool(np, "charger_pump_support");
    ts->wireless_charger_support = of_property_read_bool(np, "wireless_charger_support");
    ts->headset_pump_support    = of_property_read_bool(np, "headset_pump_support");
    ts->black_gesture_support   = of_property_read_bool(np, "black_gesture_support");
    ts->black_gesture_indep_support   = true;
    ts->single_tap_support      = of_property_read_bool(np, "single_tap_support");
    ts->gesture_test_support    = of_property_read_bool(np, "black_gesture_test_support");
    ts->fw_update_app_support   = of_property_read_bool(np, "fw_update_app_support");
    ts->game_switch_support     = of_property_read_bool(np, "game_switch_support");
    ts->ear_sense_support       = of_property_read_bool(np, "ear_sense_support");
    ts->smart_gesture_support   = of_property_read_bool(np, "smart_gesture_support");
    ts->pressure_report_support = of_property_read_bool(np, "pressure_report_support");
    ts->is_noflash_ic           = of_property_read_bool(np, "noflash_support");
    ts->face_detect_support     = of_property_read_bool(np, "face_detect_support");
    ts->sec_long_low_trigger     = of_property_read_bool(np, "sec_long_low_trigger");
    ts->external_touch_support  = of_property_read_bool(np, "external_touch_support");
    ts->kernel_grip_support     = of_property_read_bool(np, "kernel_grip_support");
    ts->kernel_grip_support_special = of_property_read_bool(np, "kernel_grip_support_special");
    ts->fw_grip_support     = of_property_read_bool(np, "fw_grip_support");
    ts->spuri_fp_touch.lcd_trigger_fp_check = of_property_read_bool(np, "lcd_trigger_fp_check");
    ts->health_monitor_support = of_property_read_bool(np, "health_monitor_support");
    ts->health_monitor_v2_support = of_property_read_bool(np, "health_monitor_v2_support");
    ts->lcd_trigger_load_tp_fw_support = of_property_read_bool(np, "lcd_trigger_load_tp_fw_support");
    ts->fingerprint_underscreen_support = of_property_read_bool(np, "fingerprint_underscreen_support");
    ts->suspend_gesture_cfg   = of_property_read_bool(np, "suspend_gesture_cfg");
    ts->auto_test_force_pass_support = of_property_read_bool(np, "auto_test_force_pass_support");
    ts->freq_hop_simulate_support = of_property_read_bool(np, "freq_hop_simulate_support");
    ts->irq_trigger_hdl_support = of_property_read_bool(np, "irq_trigger_hdl_support");
    ts->noise_modetest_support = of_property_read_bool(np, "noise_modetest_support");
    ts->fw_update_in_probe_with_headfile = of_property_read_bool(np, "fw_update_in_probe_with_headfile");
    ts->report_point_first_support = of_property_read_bool(ts->dev->of_node, "report_point_first_support");
    ts->lcd_wait_tp_resume_finished_support = of_property_read_bool(np, "lcd_wait_tp_resume_finished_support");
    ts->spuri_fp_touch.lcd_resume_ok = true;
    ts->new_set_irq_wake_support = of_property_read_bool(np, "new_set_irq_wake_support");
    ts->report_flow_unlock_support = of_property_read_bool(np, "report_flow_unlock_support");
    ts->screenoff_fingerprint_info_support = of_property_read_bool(np, "screenoff_fingerprint_info_support");
    ts->irq_need_dev_resume_ok =  of_property_read_bool(np, "irq_need_dev_resume_ok");
    ts->report_rate_white_list_support = of_property_read_bool(np, "report_rate_white_list_support");
    ts->lcd_tp_refresh_support = of_property_read_bool(np, "lcd_tp_refresh_support");
    ts->auto_test_need_cal_support = of_property_read_bool(np, "auto_test_need_cal_support");
    ts->smooth_level_support = of_property_read_bool(np, "smooth_level_support");
    ts->cs_gpio_need_pull = of_property_read_bool(np, "cs_gpio_need_pull");

    rc = of_property_read_u32(np, "smooth_level", &val);
    if (rc) {
        TPD_DETAIL("smooth_level not specified\n");
    } 
    else {
        ts->smooth_level = val;
    }


    rc = of_property_read_string(np, "chip-name", &ts->panel_data.chip_name);
    if (rc < 0) {
        TPD_INFO("failed to get chip name, firmware/limit name might be invalid\n");
    }

    ts->hw_res.irq_gpiod = devm_gpiod_get(dev, "irq", GPIOD_IN);
    if (IS_ERR(ts->hw_res.irq_gpiod)) {
        TPD_INFO("Failed to get IRQ gpio: %ld\n", PTR_ERR(ts->hw_res.irq_gpiod));
        return PTR_ERR(ts->hw_res.irq_gpiod);
    }
    ts->hw_res.irq_gpio = desc_to_gpio(ts->hw_res.irq_gpiod);

    // reset gpio
    ts->hw_res.reset_gpiod = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(ts->hw_res.reset_gpiod)) {
        TPD_INFO("unable to request gpio [%d]\n", ts->hw_res.reset_gpio);
        return PTR_ERR(ts->hw_res.reset_gpiod);
    }
    ts->hw_res.reset_gpio = desc_to_gpio(ts->hw_res.reset_gpiod);

    dev_set_drvdata(dev, ts);
    

    ts->hw_res.pinctrl = devm_pinctrl_get(dev);
    if (IS_ERR_OR_NULL(ts->hw_res.pinctrl)) {
        TPD_INFO("Getting pinctrl handle failed");
    } else {
        ts->hw_res.pin_set_high = pinctrl_lookup_state(ts->hw_res.pinctrl, "pin_set_high");
        if (IS_ERR_OR_NULL(ts->hw_res.pin_set_high)) {
            TPD_DETAIL("Failed to get the high state pinctrl handle\n");
        }

        ts->hw_res.pin_set_low = pinctrl_lookup_state(ts->hw_res.pinctrl, "pin_set_low");
        if (IS_ERR_OR_NULL(ts->hw_res.pin_set_low)) {
            TPD_DETAIL(" Failed to get the low state pinctrl handle\n");
        }

        ts->hw_res.pin_set_nopull = pinctrl_lookup_state(ts->hw_res.pinctrl, "pin_set_nopull");
        if (IS_ERR_OR_NULL(ts->hw_res.pin_set_nopull)) {
            TPD_DETAIL("Failed to get the input state pinctrl handle\n");
        }
    }
    ts->hw_res.enable2v8_gpio = of_get_named_gpio(np, "enable2v8_gpio", 0);
    if (ts->hw_res.enable2v8_gpio < 0) {
        TPD_DETAIL("ts->hw_res.enable2v8_gpio not specified\n");
    } else {
        if (gpio_is_valid(ts->hw_res.enable2v8_gpio)) {
            rc = gpio_request(ts->hw_res.enable2v8_gpio, "vdd2v8-gpio");
            if (rc) {
                TPD_INFO("unable to request gpio [%d] %d\n", ts->hw_res.enable2v8_gpio, rc);
            }
        }
    }

    ts->hw_res.enable1v8_gpio = of_get_named_gpio(np, "enable1v8_gpio", 0);
    if (ts->hw_res.enable1v8_gpio < 0) {
        TPD_DETAIL("ts->hw_res.enable1v8_gpio not specified\n");
    } else {
        if (gpio_is_valid(ts->hw_res.enable1v8_gpio)) {
            rc = gpio_request(ts->hw_res.enable1v8_gpio, "vcc1v8-gpio");
            if (rc) {
                TPD_INFO("unable to request gpio [%d], %d\n", ts->hw_res.enable1v8_gpio, rc);
            }
        }
    }

    // interrupt mode
    ts->int_mode = BANNABLE;
    rc = of_property_read_u32(np, "touchpanel,int-mode", &val);
    if (rc) {
        TPD_DETAIL("int-mode not specified\n");
    } else {
        if (val < INTERRUPT_MODE_MAX) {
            ts->int_mode = val;
        }
    }



    // resolution info
    rc = of_property_read_u32(np, "touchpanel,max-num-support", &ts->max_num);
    if (rc) {
        TPD_INFO("ts->max_num not specified\n");
        ts->max_num = 10;
    }

    rc = of_property_read_u32_array(np, "touchpanel,tx-rx-num", tx_rx_num, 2);
    if (rc) {
        TPD_INFO("tx-rx-num not set\n");
        ts->hw_res.TX_NUM = 0;
        ts->hw_res.RX_NUM = 0;
    } else {
        ts->hw_res.TX_NUM = tx_rx_num[0];
        ts->hw_res.RX_NUM = tx_rx_num[1];
    }
    TPD_DETAIL("TX_NUM = %d, RX_NUM = %d \n", ts->hw_res.TX_NUM, ts->hw_res.RX_NUM);

    rc = of_property_read_u32_array(np, "earsense,tx-rx-num", tx_rx_num, 2);
    if (rc) {
        TPD_DETAIL("tx-rx-num not set\n");
        ts->hw_res.EARSENSE_TX_NUM = ts->hw_res.TX_NUM;
        ts->hw_res.EARSENSE_RX_NUM = ts->hw_res.RX_NUM / 2;
    } else {
        ts->hw_res.EARSENSE_TX_NUM = tx_rx_num[0];
        ts->hw_res.EARSENSE_RX_NUM = tx_rx_num[1];
    }
    TPD_DETAIL("EARSENSE_TX_NUM = %d, EARSENSE_RX_NUM = %d \n", ts->hw_res.EARSENSE_TX_NUM, ts->hw_res.EARSENSE_RX_NUM);

    rc = of_property_read_u32_array(np, "touchpanel,display-coords", temp_array, 2);
    if (rc) {
        TPD_INFO("Lcd size not set\n");
        ts->resolution_info.LCD_WIDTH = 0;
        ts->resolution_info.LCD_HEIGHT = 0;
    } else {
        ts->resolution_info.LCD_WIDTH = temp_array[0];
        ts->resolution_info.LCD_HEIGHT = temp_array[1];
    }

    rc = of_property_read_u32_array(np, "touchpanel,panel-coords", temp_array, 2);
    if (rc) {
        ts->resolution_info.max_x = 0;
        ts->resolution_info.max_y = 0;
    } else {
        ts->resolution_info.max_x = temp_array[0];
        ts->resolution_info.max_y = temp_array[1];
    }
    rc = of_property_read_u32_array(np, "touchpanel,touchmajor-limit", temp_array, 2);
    if (rc) {
        ts->touch_major_limit.width_range = 0;
        ts->touch_major_limit.height_range = 54;    //set default value
    } else {
        ts->touch_major_limit.width_range = temp_array[0];
        ts->touch_major_limit.height_range = temp_array[1];
    }
    TPD_DETAIL("LCD_WIDTH = %d, LCD_HEIGHT = %d, max_x = %d, max_y = %d, limit_witdh = %d, limit_height = %d\n",
             ts->resolution_info.LCD_WIDTH, ts->resolution_info.LCD_HEIGHT, ts->resolution_info.max_x, ts->resolution_info.max_y, \
             ts->touch_major_limit.width_range, ts->touch_major_limit.height_range);

    rc = of_property_read_u32_array(np, "touchpanel,smooth-level", temp_array, SMOOTH_LEVEL_NUM);
    if (rc) {
        TPD_DETAIL("smooth_level_array not specified %d\n", rc);
    } else {
        ts->smooth_level_array_support = true;
        for (i=0; i < SMOOTH_LEVEL_NUM; i++) {
            ts->smooth_level_array[i] = temp_array[i];
        }
    }

    rc = of_property_read_u32_array(np, "touchpanel,smooth-level-charging", temp_array, SMOOTH_LEVEL_NUM);
    if (rc) {
        TPD_DETAIL("smooth_level_charging_array not specified %d\n", rc);
    } else {
        ts->smooth_level_charging_array_support = true;
        for (i=0; i < SMOOTH_LEVEL_NUM; i++) {
            ts->smooth_level_charging_array[i] = temp_array[i];
        }
    }

    rc = of_property_read_u32_array(np, "touchpanel,sensitive-level", temp_array, SENSITIVE_LEVEL_NUM);
    if (rc) {
        TPD_DETAIL("sensitive_level_array not specified %d\n", rc);
    } else {
        ts->sensitive_level_array_support = true;
        for (i=0; i < SENSITIVE_LEVEL_NUM; i++) {
            ts->sensitive_level_array[i] = temp_array[i];
        }
    }
    rc = of_property_read_u32(ts->dev->of_node, "touchpanel,default_hor_area", &ts->default_hor_area);
    if (rc) {
        ts->default_hor_area = 0;
    } else {
        TPD_INFO("set default horizontal area value:%d.\n", ts->default_hor_area);
    }

    // virturl key Related
    rc = of_property_read_u32_array(np, "touchpanel,button-type", temp_array, 2);
    if (rc < 0) {
        TPD_DETAIL("error:button-type should be setting in dts!");
    } else {
        ts->vk_type = temp_array[0];
        ts->vk_bitmap = temp_array[1] & 0xFF;
        if (ts->vk_type == TYPE_PROPERTIES) {
            rc = of_property_read_u32_array(np, "touchpanel,button-map", temp_array, 8);
            if (rc) {
                TPD_INFO("button-map not set\n");
            } else {
                ts->button_map.coord_menu.x = temp_array[0];
                ts->button_map.coord_menu.y = temp_array[1];
                ts->button_map.coord_home.x = temp_array[2];
                ts->button_map.coord_home.y = temp_array[3];
                ts->button_map.coord_back.x = temp_array[4];
                ts->button_map.coord_back.y = temp_array[5];
                ts->button_map.width_x = temp_array[6];
                ts->button_map.height_y = temp_array[7];
            }
        }
    }

    //touchkey take tx num and rx num
    rc = of_property_read_u32_array(np, "touchpanel.button-TRx", temp_array, 2);
    if(rc < 0) {
        TPD_DETAIL("error:button-TRx should be setting in dts!\n");
        ts->hw_res.key_TX = 0;
        ts->hw_res.key_RX = 0;
    } else {
        ts->hw_res.key_TX = temp_array[0];
        ts->hw_res.key_RX = temp_array[1];
        TPD_INFO("key_tx is %d, key_rx is %d\n", ts->hw_res.key_TX, ts->hw_res.key_RX);
    }

    //set incell panel parameter, for of_property_read_bool return 1 when success and return 0 when item is not exist
    rc = ts->is_incell_panel = of_property_read_bool(np, "incell_screen");
    if(rc > 0) {
        TPD_DETAIL("panel is incell!\n");
        ts->is_incell_panel = 1;
    } else {
        TPD_DETAIL("panel is oncell!\n");
        ts->is_incell_panel = 0;
    }

    rc = of_property_read_u32(np, "touchpanel,curved-size", &ts->curved_size);
    if (rc < 0) {
        TPD_INFO("ts->curved_size not specified\n");
        ts->curved_size = 0;
    } else {
        TPD_INFO("ts->curved_size is %d\n", ts->curved_size);
    }

    rc = of_property_read_u32(np, "touchpanel,single-optimized-time", &ts->single_optimized_time);
    if (rc) {
        TPD_DETAIL("ts->single_optimized_time not specified\n");
        ts->single_optimized_time = 0;
        ts->optimized_show_support = false;
    } else {
        ts->total_operate_times = 0;
        ts->optimized_show_support = true;
    }

    return 0;
commandline_kazalloc_error:
    return -1;	
    // We can Add callback fuction here if necessary seprate some dts config for chip_data
}

int init_power_control(struct touchpanel_data *ts)
{
    int ret = 0;

    // 1.8v
    ts->hw_res.vcc_1v8 = regulator_get(ts->dev, "vcc_1v8");
    if (IS_ERR_OR_NULL(ts->hw_res.vcc_1v8)) {
        TPD_INFO("Regulator get failed vcc_1v8, ret = %d\n", ret);
    } else {
        if (regulator_count_voltages(ts->hw_res.vcc_1v8) > 0) {
            ret = regulator_set_voltage(ts->hw_res.vcc_1v8, 1800000, 1800000);
            if (ret) {
                dev_err(ts->dev, "Regulator set_vtg failed vcc_i2c rc = %d\n", ret);
                goto regulator_vcc_1v8_put;
            }

            ret = regulator_set_load(ts->hw_res.vcc_1v8, 200000);
            if (ret < 0) {
                dev_err(ts->dev, "Failed to set vcc_1v8 mode(rc:%d)\n", ret);
                goto regulator_vcc_1v8_put;
            }
        }
    }
    // vdd 2.8v
    ts->hw_res.vdd_2v8 = regulator_get(ts->dev, "vdd_2v8");
    if (IS_ERR_OR_NULL(ts->hw_res.vdd_2v8)) {
        TPD_INFO("Regulator vdd2v8 get failed, ret = %d\n", ret);
    } else {
        if (regulator_count_voltages(ts->hw_res.vdd_2v8) > 0) {
            TPD_INFO("set avdd voltage to %d uV\n", ts->hw_res.vdd_volt);
            if (ts->hw_res.vdd_volt) {
                ret = regulator_set_voltage(ts->hw_res.vdd_2v8, ts->hw_res.vdd_volt, ts->hw_res.vdd_volt);
            } else {
                ret = regulator_set_voltage(ts->hw_res.vdd_2v8, 3100000, 3100000);
            }
            if (ret) {
                dev_err(ts->dev, "Regulator set_vtg failed vdd rc = %d\n", ret);
                goto regulator_vdd_2v8_put;
            }

            ret = regulator_set_load(ts->hw_res.vdd_2v8, 200000);
            if (ret < 0) {
                dev_err(ts->dev, "Failed to set vdd_2v8 mode(rc:%d)\n", ret);
                goto regulator_vdd_2v8_put;
            }
        }
    }

    return 0;

regulator_vdd_2v8_put:
    regulator_put(ts->hw_res.vdd_2v8);
    ts->hw_res.vdd_2v8 = NULL;
regulator_vcc_1v8_put:
    if (!IS_ERR_OR_NULL(ts->hw_res.vcc_1v8)) {
        regulator_put(ts->hw_res.vcc_1v8);
        ts->hw_res.vcc_1v8 = NULL;
    }

    return ret;
}

int tp_powercontrol_1v8(struct hw_resource *hw_res, bool on)
{
    int ret = 0;

    if (on) {// 1v8 power on
        if (!IS_ERR_OR_NULL(hw_res->vcc_1v8)) {
            TPD_INFO("Enable the Regulator1v8.\n");
            ret = regulator_enable(hw_res->vcc_1v8);
            if (ret) {
                TPD_INFO("Regulator vcc_i2c enable failed ret = %d\n", ret);
                return ret;
            }
        }

        if (hw_res->enable1v8_gpio > 0) {
            TPD_INFO("Enable the 1v8_gpio\n");
            ret = gpio_direction_output(hw_res->enable1v8_gpio, 1);
            if (ret) {
                TPD_INFO("enable the enable1v8_gpio failed.\n");
                return ret;
            }
        }
    } else {// 1v8 power off
        if (!IS_ERR_OR_NULL(hw_res->vcc_1v8)) {
            ret = regulator_disable(hw_res->vcc_1v8);
            if (ret) {
                TPD_INFO("Regulator vcc_i2c enable failed rc = %d\n", ret);
                return ret;
            }
        }

        if (hw_res->enable1v8_gpio > 0) {
            TPD_INFO("disable the 1v8_gpio\n");
            ret = gpio_direction_output(hw_res->enable1v8_gpio, 0);
            if (ret) {
                TPD_INFO("disable the enable2v8_gpio failed.\n");
                return ret;
            }
        }
    }

    return 0;
}

int tp_powercontrol_2v8(struct hw_resource *hw_res, bool on)
{
    int ret = 0;

    if (on) {// 2v8 power on
        if (!IS_ERR_OR_NULL(hw_res->vdd_2v8)) {
            TPD_INFO("Enable the Regulator2v8.\n");
            ret = regulator_enable(hw_res->vdd_2v8);
            if (ret) {
                TPD_INFO("Regulator vdd enable failed ret = %d\n", ret);
                return ret;
            }
        }
        if (hw_res->enable2v8_gpio > 0) {
            TPD_INFO("Enable the 2v8_gpio, hw_res->enable2v8_gpio is %d\n", hw_res->enable2v8_gpio);
            ret = gpio_direction_output(hw_res->enable2v8_gpio, 1);
            if (ret) {
                TPD_INFO("enable the enable2v8_gpio failed.\n");
                return ret;
            }
        }
    } else {// 2v8 power off
        if (!IS_ERR_OR_NULL(hw_res->vdd_2v8)) {
            ret = regulator_disable(hw_res->vdd_2v8);
            if (ret) {
                TPD_INFO("Regulator vdd disable failed rc = %d\n", ret);
                return ret;
            }
        }
        if (hw_res->enable2v8_gpio > 0) {
            TPD_INFO("disable the 2v8_gpio\n");
            ret = gpio_direction_output(hw_res->enable2v8_gpio, 0);
            if (ret) {
                TPD_INFO("disable the enable2v8_gpio failed.\n");
                return ret;
            }
        }
    }
    return ret;
}


static void esd_handle_func(struct work_struct *work)
{
    int ret = 0;
    struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
                                 esd_info.esd_check_work.work);

    if (ts->loading_fw) {
        TPD_INFO("FW is updating, stop esd handle!\n");
        return;
    }

    mutex_lock(&ts->esd_info.esd_lock);
    if (!ts->esd_info.esd_running_flag) {
        TPD_INFO("Esd protector has stopped!\n");
        goto ESD_END;
    }

    if (ts->is_suspended == 1) {
        TPD_INFO("Touch panel has suspended!\n");
        goto ESD_END;
    }

    if (!ts->ts_ops->esd_handle) {
        TPD_INFO("not support ts_ops->esd_handle callback\n");
        goto ESD_END;
    }

    ret = ts->ts_ops->esd_handle(ts->chip_data);

    if (ts->esd_info.esd_running_flag)
        queue_delayed_work(ts->esd_info.esd_workqueue, &ts->esd_info.esd_check_work, ts->esd_info.esd_work_time);
    else
        TPD_INFO("Esd protector suspended!");

ESD_END:
    mutex_unlock(&ts->esd_info.esd_lock);
    return;
}

/**
 * esd_handle_switch - open or close esd thread
 * @esd_info: touchpanel_data, using for common driver resource
 * @on: bool variable using for  indicating open or close esd check function.
 *     true:open;
 *     false:close;
 */
void esd_handle_switch(struct esd_information *esd_info, bool on)
{
    mutex_lock(&esd_info->esd_lock);

    if (on) {
        if (!esd_info->esd_running_flag) {
            esd_info->esd_running_flag = 1;

            TPD_INFO("Esd protector started, cycle: %d s\n", esd_info->esd_work_time / HZ);
            queue_delayed_work(esd_info->esd_workqueue, &esd_info->esd_check_work, esd_info->esd_work_time);
        }
    } else {
        if (esd_info->esd_running_flag) {
            esd_info->esd_running_flag = 0;

            TPD_INFO("Esd protector stoped!\n");
            cancel_delayed_work(&esd_info->esd_check_work);
        }
    }

    mutex_unlock(&esd_info->esd_lock);
}

int tp_register_irq_func(struct touchpanel_data *ts)
{
    int ret = 0;
    if (!(IS_ERR(ts->hw_res.irq_gpiod))) {
        TPD_DETAIL("%s, irq_gpio is %d, ts->irq is %d\n", __func__, ts->hw_res.irq_gpio, ts->irq);

        if(ts->irq_flags_cover) {
            ts->irq_flags = ts->irq_flags_cover;
            TPD_INFO("%s irq_flags is covered by 0x%x\n", __func__, ts->irq_flags_cover);
        }

        if(ts->irq <= 0) {
            ts->irq = gpiod_to_irq(ts->hw_res.irq_gpiod);
            TPD_INFO("%s, [irq info] irq_gpio is %d, ts->irq is %d\n", __func__, ts->hw_res.irq_gpio, ts->irq);

            if(ts->is_noflash_ic) {
                ts->s_client->irq = gpiod_to_irq(ts->hw_res.irq_gpiod);
                TPD_INFO("%s, [irq info] irq_gpio is %d, ts->s_client->irq is %d\n", __func__, ts->hw_res.irq_gpio, ts->s_client->irq);
            } else {
                ts->client->irq = gpiod_to_irq(ts->hw_res.irq_gpiod);
                TPD_INFO("%s, [irq info] irq_gpio is %d, ts->client->irq is %d\n", __func__, ts->hw_res.irq_gpio, ts->client->irq);
            }  
        }

        ret = irq_get_trigger_type(ts->irq);
        if (ret < 0)
        {
            return ret;
        }
        ts->irq_flags = ret;

        ret = request_threaded_irq(ts->irq, NULL,
                                   tp_irq_thread_fn,
                                   ts->irq_flags | IRQF_ONESHOT,
                                   TPD_DEVICE, ts);
        if (ret < 0) {
            TPD_INFO("%s request_threaded_irq ret is %d\n", __func__, ret);
        }
    } else {
        TPD_INFO("%s:no valid irq\n", __func__);
    }
    return ret;
}

//work schdule for reading&update delta
static void touch_read_delta(struct work_struct *work)
{
    struct touchpanel_data *ts = container_of(work, struct touchpanel_data, read_delta_work);

    mutex_lock(&ts->mutex_earsense);
    mutex_lock(&ts->mutex);
    if (!ts->is_suspended) {
        ts->earsense_ops->delta_read(ts->chip_data, ts->earsense_delta, 2 * ts->hw_res.EARSENSE_TX_NUM * ts->hw_res.EARSENSE_RX_NUM);
    }
    mutex_unlock(&ts->mutex);
    mutex_unlock(&ts->mutex_earsense);
    ts->delta_state = TYPE_DELTA_IDLE;
}

int init_touch_interfaces(struct device *dev, bool flag_register_16bit)
{
    register_is_16bit = flag_register_16bit;
    mutex_init(&i2c_mutex);

    return 0;
}

/**
 * register_common_touch_device - parse dts, get resource defined in Dts
 * @pdata: touchpanel_data, using for common driver
 *
 * entrance of common touch Driver
 * Returning zero(sucess) or negative errno(failed)
 */
int register_common_touch_device(struct touchpanel_data *pdata)
{
    struct touchpanel_data *ts = pdata;
    struct invoke_method *invoke;

    int ret = -1;

    TPD_INFO("%s  is called\n", __func__);
    //step1 : dts parse
    ret = init_parse_dts(ts->dev, ts);
    if (ret<0){
	TPD_INFO("%s: parse dts failed! :( \n", __func__);
	return -1;
	}

    //step3 : IIC interfaces init
    init_touch_interfaces(ts->dev, ts->register_is_16bit);

    //step3 : mutex init
    mutex_init(&ts->mutex);
    mutex_init(&ts->report_mutex);
    init_completion(&ts->pm_complete);
    init_completion(&ts->fw_complete);
    init_completion(&ts->resume_complete);

    INIT_WORK(&ts->async_work, tp_async_work_lock);

    if (ts->has_callback) {
        TPD_DETAIL("%s: synaptics ic need async work", __func__);
        invoke = (struct invoke_method *)pdata->chip_data;
        invoke->invoke_common = tp_work_common_callback;
        invoke->async_work = tp_async_work_callback;
    } else {
        TPD_DETAIL("%s No synaptics ic cancel async work", __func__);
        cancel_work_sync(&ts->async_work);
    }
    //step4 : Power init && setting
    preconfig_power_control(ts);
    ret = init_power_control(ts);
    if (ret) {
        TPD_INFO("%s: tp power init failed.\n", __func__);
        return -1;
    }
    ret = reconfig_power_control(ts);
    if (ret) {
        TPD_INFO("%s: reconfig power failed.\n", __func__);
        return -1;
    }
    if (!ts->ts_ops->power_control) {
        ret = -EINVAL;
        TPD_INFO("tp power_control NULL!\n");
        goto power_control_failed;
    }
    ret = ts->ts_ops->power_control(ts->chip_data, true);
    if (ret) {
        TPD_INFO("%s: tp power init failed.\n", __func__);
        goto power_control_failed;
    }

    //step5 : I2C function check
    if (!ts->is_noflash_ic) {
        if (!i2c_check_functionality(ts->client->adapter, I2C_FUNC_I2C)) {
            TPD_INFO("%s: need I2C_FUNC_I2C\n", __func__);
            ret = -ENODEV;
            goto err_check_functionality_failed;
        }
    }

    //step6 : touch input dev init
    ret = init_input_device(ts);
    if (ret < 0) {
        ret = -EINVAL;
        TPD_INFO("tp_input_init failed!\n");
        goto err_check_functionality_failed;
    }

    if (ts->int_mode == UNBANNABLE) {
        ret = tp_register_irq_func(ts);
        if (ret < 0) {
            goto free_touch_panel_input;
        }
        ts->i2c_ready = true;
    }

    //step7 : Alloc fw_name/devinfo memory space
    ts->panel_data.fw_name = kzalloc(MAX_FW_NAME_LENGTH, GFP_KERNEL);
    if (ts->panel_data.fw_name == NULL) {
        ret = -ENOMEM;
        TPD_INFO("panel_data.fw_name kzalloc error\n");
        goto free_touch_panel_input;
    }

    ts->panel_data.manufacture_info.version = kzalloc(MAX_DEVICE_VERSION_LENGTH, GFP_KERNEL);
    if (ts->panel_data.manufacture_info.version == NULL) {
        ret = -ENOMEM;
        TPD_INFO("manufacture_info.version kzalloc error\n");
        goto manu_version_alloc_err;
    }

    ts->panel_data.manufacture_info.manufacture = kzalloc(MAX_DEVICE_MANU_LENGTH, GFP_KERNEL);
    if (ts->panel_data.manufacture_info.manufacture == NULL) {
        ret = -ENOMEM;
        TPD_INFO("panel_data.fw_name kzalloc error\n");
        goto manu_info_alloc_err;
    }

    //step8 : touchpanel vendor
    if (ts->ts_ops->get_vendor) {
        ts->ts_ops->get_vendor(ts->chip_data, &ts->panel_data);
    }


    //step10:get chip info
    if (!ts->ts_ops->get_chip_info) {
        ret = -EINVAL;
        TPD_INFO("tp get_chip_info NULL!\n");
        goto err_check_functionality_failed;
    }
    ret = ts->ts_ops->get_chip_info(ts->chip_data);
    if (ret < 0) {
        ret = -EINVAL;
        TPD_INFO("tp get_chip_info failed!\n");
        goto err_check_functionality_failed;
    }

    //step11 : touchpanel Fw check
    if(!ts->is_noflash_ic) {            //noflash don't have firmware before fw update
        if (!ts->ts_ops->fw_check) {
            ret = -EINVAL;
            TPD_INFO("tp fw_check NULL!\n");
            goto manu_info_alloc_err;
        }
        ret = ts->ts_ops->fw_check(ts->chip_data, &ts->resolution_info, &ts->panel_data);
        if (ret == FW_ABNORMAL) {
            ts->force_update = 1;
            TPD_INFO("This FW need to be updated!\n");
        } else {
            ts->force_update = 0;
        }
    }


    //step12 : enable touch ic irq output ability
    if (!ts->ts_ops->mode_switch) {
        ret = -EINVAL;
        TPD_INFO("tp mode_switch NULL!\n");
        goto manu_info_alloc_err;
    }
    ret = ts->ts_ops->mode_switch(ts->chip_data, MODE_NORMAL, true);
    if (ret < 0) {
        ret = -EINVAL;
        TPD_INFO("%s:modem switch failed!\n", __func__);
        goto manu_info_alloc_err;
    }

    //step13 : irq request setting
    if (ts->int_mode == BANNABLE) {
        ret = tp_register_irq_func(ts);
        if (ret < 0) {
            goto manu_info_alloc_err;
        }
    }

    //step14 : suspend && resume fuction register
    ts->fb_notif.notifier_call = fb_notifier_callback;
    ret = fb_register_client(&ts->fb_notif);
    if (ret) {
        TPD_INFO("Unable to register fb_notifier: %d\n", ret);
    }

    //step15 : workqueue create(speedup_resume)
    ts->speedup_resume_wq = create_singlethread_workqueue("speedup_resume_wq");
    if (!ts->speedup_resume_wq) {
        ret = -ENOMEM;
        goto threaded_irq_free;
    }

    ts->lcd_trigger_load_tp_fw_wq = create_singlethread_workqueue("lcd_trigger_load_tp_fw_wq");
    if (!ts->lcd_trigger_load_tp_fw_wq) {
        ret = -ENOMEM;
        goto threaded_irq_free;
    }

    INIT_WORK(&ts->speed_up_work, speedup_resume);
    INIT_WORK(&ts->lcd_trigger_load_tp_fw_work, lcd_trigger_load_tp_fw);

    //step 16 : short edge shield
    if (ts->edge_limit_support) {
        ts->limit_enable = 1;
        ts->limit_edge = ts->limit_enable & 1;
        ts->limit_corner = 0;
        ts->limit_valid = 0;
        ts->edge_limit.limit_area = 1;
        ts->edge_limit.in_which_area = AREA_NOTOUCH;

        ts->edge_limit.left_x1  = (ts->edge_limit.limit_area * 1000) / 100;
        ts->edge_limit.right_x1 = ts->resolution_info.LCD_WIDTH - ts->edge_limit.left_x1;
        ts->edge_limit.left_x2  = 2 * ts->edge_limit.left_x1;
        ts->edge_limit.right_x2 = ts->resolution_info.LCD_WIDTH - (2 * ts->edge_limit.left_x1);
        ts->edge_limit.left_x3  = 5 * ts->edge_limit.left_x1;
        ts->edge_limit.right_x3 = ts->resolution_info.LCD_WIDTH - (5 * ts->edge_limit.left_x1);

        ts->edge_limit.left_y1  = (ts->edge_limit.limit_area * 1000) / 100;
        ts->edge_limit.right_y1 = ts->resolution_info.LCD_HEIGHT - ts->edge_limit.left_y1;
        ts->edge_limit.left_y2  = 2 * ts->edge_limit.left_y1;
        ts->edge_limit.right_y2 = ts->resolution_info.LCD_HEIGHT - (2 * ts->edge_limit.left_y1);
        ts->edge_limit.left_y3  = 5 * ts->edge_limit.left_y1;
        ts->edge_limit.right_y3 = ts->resolution_info.LCD_HEIGHT - (5 * ts->edge_limit.left_y1);
    } else if (ts->fw_edge_limit_support) {
        ts->limit_enable = 1;
        ts->limit_edge = ts->limit_enable & 1;
        ts->limit_corner = 0;
        ts->limit_valid = 0;
    }

    //step 17:esd recover support
    if (ts->esd_handle_support) {
        ts->esd_info.esd_workqueue = create_singlethread_workqueue("esd_workthread");
        INIT_DELAYED_WORK(&ts->esd_info.esd_check_work, esd_handle_func);

        mutex_init(&ts->esd_info.esd_lock);

        ts->esd_info.esd_running_flag = 0;
        ts->esd_info.esd_work_time = 2 * HZ; // HZ: clock ticks in 1 second generated by system
        TPD_DEBUG("Clock ticks for an esd cycle: %d\n", ts->esd_info.esd_work_time);

        esd_handle_switch(&ts->esd_info, true);
    }

    //frequency hopping simulate support
    if (ts->freq_hop_simulate_support) {
        ts->freq_hop_info.freq_hop_workqueue = create_singlethread_workqueue("syna_tcm_freq_hop");
        INIT_DELAYED_WORK(&ts->freq_hop_info.freq_hop_work, tp_freq_hop_work);
        ts->freq_hop_info.freq_hop_simulating = false;
        ts->freq_hop_info.freq_hop_freq = 0;
    }

    //step 18:spurious_fingerprint support
    if (ts->spurious_fp_support) {
        ts->spuri_fp_touch.thread = kthread_run(finger_protect_handler, ts, "touchpanel_fp");
        if (IS_ERR(ts->spuri_fp_touch.thread)) {
            TPD_INFO("spurious fingerprint thread create failed\n");
        }
    }

    // step 20: ear sense support
    if (ts->ear_sense_support) {
        mutex_init(&ts->mutex_earsense);    // init earsense operate mutex

        //malloc space for storing earsense delta
        ts->earsense_delta = kzalloc(2 * ts->hw_res.EARSENSE_TX_NUM * ts->hw_res.EARSENSE_RX_NUM, GFP_KERNEL);
        if (ts->earsense_delta == NULL) {
            ret = -ENOMEM;
            TPD_INFO("earsense_delta kzalloc error\n");
            goto threaded_irq_free;
        }

        //create work queue for read earsense delta
        ts->delta_read_wq = create_singlethread_workqueue("touch_delta_wq");
        if (!ts->delta_read_wq) {
            ret = -ENOMEM;
            goto earsense_alloc_free;
        }
        INIT_WORK(&ts->read_delta_work, touch_read_delta);
    }


    //initial kernel grip parameter

    // lcd_tp_refresh_support support
    if (ts->lcd_tp_refresh_support) {
        ts->tp_refresh_wq = create_singlethread_workqueue("tp_refresh_wq");
        if (!ts->tp_refresh_wq) {
            ret = -ENOMEM;
            goto threaded_irq_free;
        }

        INIT_WORK(&ts->tp_refresh_work, lcd_tp_refresh_work);
    }

    //step 21 : createproc proc files interface
    

    //step 22 : Other****
    ts->i2c_ready = true;
    ts->loading_fw = false;
    ts->is_suspended = 0;
    ts->suspend_state = TP_SPEEDUP_RESUME_COMPLETE;
    ts->gesture_enable = 1;
    ts->es_enable = 0;
    ts->fd_enable = 0;
    ts->fp_enable = 0;
    ts->fp_info.touch_state = 0;
    ts->palm_enable = 1;
    ts->touch_count = 0;
    ts->glove_enable = 0;
    ts->view_area_touched = 0;
    ts->external_touch_status = false;
    ts->tp_suspend_order = LCD_TP_SUSPEND;
    ts->tp_resume_order = TP_LCD_RESUME;
    ts->skip_suspend_operate = false;
    ts->skip_reset_in_resume = false;
    ts->irq_slot = 0;
    ts->firmware_update_type = 0;
    ts->report_point_first_enable = 0;//reporting point first ,when baseline error
    ts->resume_finished = 1;
    if(ts->is_noflash_ic) {
        ts->irq = ts->s_client->irq;
    } else {
        ts->irq = ts->client->irq;
    }
    tp_register_times++;
    g_tp = ts;
    INIT_WORK(&ts->fw_update_work, tp_fw_update_work);
    msleep(100); 
    schedule_work(&ts->fw_update_work); 
    TPD_INFO("%s : irq_gpio = %d, irq_flags = 0x%x, reset_gpio = %d\n",
        __func__, ts->hw_res.irq_gpio, ts->irq_flags, ts->hw_res.reset_gpio);
    complete(&ts->pm_complete);
    TPD_INFO("Touch panel probe : normal end\n");
    return 0;

earsense_alloc_free:
    kfree(ts->earsense_delta);

threaded_irq_free:
    free_irq(ts->irq, ts);

manu_info_alloc_err:
    kfree(ts->panel_data.manufacture_info.version);

manu_version_alloc_err:
    kfree(ts->panel_data.fw_name);

free_touch_panel_input:
    input_unregister_device(ts->input_dev);
    input_unregister_device(ts->kpd_input_dev);

err_check_functionality_failed:
    ts->ts_ops->power_control(ts->chip_data, false);

power_control_failed:

    if (!IS_ERR_OR_NULL(ts->hw_res.vdd_2v8)) {
        regulator_put(ts->hw_res.vdd_2v8);
        ts->hw_res.vdd_2v8 = NULL;
    }

    if (!IS_ERR_OR_NULL(ts->hw_res.vcc_1v8)) {
        regulator_put(ts->hw_res.vcc_1v8);
        ts->hw_res.vcc_1v8 = NULL;
    }

    if (gpio_is_valid(ts->hw_res.enable2v8_gpio))
        gpio_free(ts->hw_res.enable2v8_gpio);

    if (gpio_is_valid(ts->hw_res.enable1v8_gpio))
        gpio_free(ts->hw_res.enable1v8_gpio);

    // if (gpio_is_valid(ts->hw_res.irq_gpio)) {
    //     gpio_free(ts->hw_res.irq_gpio);
    // }

    // if (gpio_is_valid(ts->hw_res.reset_gpio)) {
    //     gpio_free(ts->hw_res.reset_gpio);
    // }

    if (gpio_is_valid(ts->hw_res.id1_gpio)) {
        gpio_free(ts->hw_res.id1_gpio);
    }

    if (gpio_is_valid(ts->hw_res.id2_gpio)) {
        gpio_free(ts->hw_res.id2_gpio);
    }

    if (gpio_is_valid(ts->hw_res.id3_gpio)) {
        gpio_free(ts->hw_res.id3_gpio);
    }

    return ret;
}

/**
 * touchpanel_ts_suspend - touchpanel suspend function
 * @dev: i2c_client->dev using to get touchpanel_data resource
 *
 * suspend function bind to LCD on/off status
 * Returning zero(sucess) or negative errno(failed)
 */
static int tp_suspend(struct device *dev)
{
    // u64 start_time = 0;
    int ret;
    struct touchpanel_data *ts = dev_get_drvdata(dev);

    TPD_INFO("%s: start.\n", __func__);

    TPD_INFO("tp_suspend ts->spuri_fp_touch.fp_trigger =%d  ts->i2c_ready =%d  ts->spuri_fp_touch.lcd_resume_ok=%d \n",
             ts->spuri_fp_touch.fp_trigger, ts->i2c_ready, ts->spuri_fp_touch.lcd_resume_ok);
    ts->spuri_fp_touch.lcd_resume_ok = false;
    //step1:detect whether we need to do suspend
    if (ts->input_dev == NULL) {
        TPD_INFO("input_dev  registration is not complete\n");
        goto NO_NEED_SUSPEND;
    }
    if (ts->loading_fw) {
        TPD_INFO("FW is updating while suspending");
        goto NO_NEED_SUSPEND;
    }

#ifndef TPD_USE_EINT
    hrtimer_cancel(&ts->timer);
#endif

    /* release all complete first */
    if (ts->ts_ops->reinit_device) {
        ts->ts_ops->reinit_device(ts->chip_data);
    }

    //step2:get mutex && start process suspend flow
    mutex_lock(&ts->mutex);
    if (!ts->is_suspended) {
        ts->monitor_data.monitor_down = 0;
        ts->monitor_data.monitor_up = 0;
        ts->is_suspended = 1;
        ts->suspend_state = TP_SUSPEND_COMPLETE;
    } else {
        TPD_INFO("%s: do not suspend twice.\n", __func__);
        goto EXIT;
    }

    //step3:Release key && touch event before suspend
    tp_btnkey_release(ts);
    tp_touch_release(ts);

    //step4:cancel esd test
    if (ts->esd_handle_support) {
        esd_handle_switch(&ts->esd_info, false);
    }

    ts->rate_ctrl_level = 0;

    if (!ts->is_incell_panel || (ts->black_gesture_support && ts->gesture_enable > 0)) {
        //step5:gamde mode support
        if (ts->game_switch_support)
            ts->ts_ops->mode_switch(ts->chip_data, MODE_GAME, false);

        if (ts->report_point_first_support)
            ts->ts_ops->set_report_point_first(ts->chip_data, false);

        if (ts->report_rate_white_list_support&&ts->ts_ops->rate_white_list_ctrl) {
            ts->ts_ops->rate_white_list_ctrl(ts->chip_data, 0);
        }

        //step5:ear sense support
        if (ts->ear_sense_support) {
            ts->ts_ops->mode_switch(ts->chip_data, MODE_EARSENSE, false);
        }
        if (ts->face_detect_support && ts->fd_enable) {
            ts->ts_ops->mode_switch(ts->chip_data, MODE_FACE_DETECT, false);
        }
    }

    //step7:gesture mode status process
    if (ts->black_gesture_support) {
        if ((ts->gesture_enable & 0x01) == 1) {
            if (ts->single_tap_support && ts->ts_ops->enable_single_tap) {
                if (ts->gesture_enable == 3) {
                    ts->ts_ops->enable_single_tap(ts->chip_data, true);
                } else {
                    ts->ts_ops->enable_single_tap(ts->chip_data, false);
                }
            }
            ts->ts_ops->mode_switch(ts->chip_data, MODE_GESTURE, true);
            goto EXIT;
        }
    }

    //step for suspend_gesture_cfg when ps is near ts->gesture_enable == 2
    if (ts->suspend_gesture_cfg && ts->black_gesture_support && ts->gesture_enable == 2) {
        ts->ts_ops->mode_switch(ts->chip_data, MODE_GESTURE, true);
        // operate_mode_switch(ts);
        goto EXIT;
    }

    //step8:skip suspend operate only when gesture_enable is 0
    if (ts->skip_suspend_operate && (!ts->gesture_enable)) {
        goto EXIT;
    }

    //step9:switch mode to sleep
    ret = ts->ts_ops->mode_switch(ts->chip_data, MODE_SLEEP, true);
    if (ret < 0) {
        TPD_INFO("%s, Touchpanel operate mode switch failed\n", __func__);
    }

EXIT:
    TPD_INFO("%s: end.\n", __func__);
    mutex_unlock(&ts->mutex);

NO_NEED_SUSPEND:
    complete(&ts->pm_complete);

    return 0;
}

/**
 * touchpanel_ts_suspend - touchpanel resume function
 * @dev: i2c_client->dev using to get touchpanel_data resource
 *
 * resume function bind to LCD on/off status, this fuction start thread to speedup screen on flow.
 * Do not care the result: Return void type
 */
static void tp_resume(struct device *dev)
{
    struct touchpanel_data *ts = dev_get_drvdata(dev);

    TPD_INFO("%s start.\n", __func__);

    if (!ts->is_suspended) {
        TPD_INFO("%s: do not resume twice.\n", __func__);
        goto NO_NEED_RESUME;
    }
    ts->monitor_data.monitor_down = 0;
    ts->monitor_data.monitor_up = 0;
    ts->is_suspended = 0;
    ts->suspend_state = TP_RESUME_COMPLETE;
    ts->disable_gesture_ctrl = false;
    if (ts->loading_fw)
        goto NO_NEED_RESUME;

    //free irq at first
    if(!ts->irq_trigger_hdl_support) {
        if (ts->int_mode == UNBANNABLE) {
            mutex_lock(&ts->mutex);
        }
        free_irq(ts->irq, ts);
        if (ts->int_mode == UNBANNABLE) {
            mutex_unlock(&ts->mutex);
        }
    }

    if (ts->ts_ops->reinit_device) {
        ts->ts_ops->reinit_device(ts->chip_data);
    }
    if(ts->ts_ops->resume_prepare) {
        mutex_lock(&ts->mutex);
        ts->ts_ops->resume_prepare(ts->chip_data);
        mutex_unlock(&ts->mutex);
    }

    if (ts->lcd_wait_tp_resume_finished_support) {
        ts->resume_finished = 0;
    }

    queue_work(ts->speedup_resume_wq, &ts->speed_up_work);
    return;

NO_NEED_RESUME:
    ts->suspend_state = TP_SPEEDUP_RESUME_COMPLETE;
    complete(&ts->pm_complete);
}

void lcd_trigger_tp_irq_reset(void)
{
    if (!g_tp)
        return;
    if (g_tp->irq_trigger_hdl_support) {
        TPD_INFO("%s\n", __func__);
        free_irq(g_tp->irq, g_tp);
        tp_register_irq_func(g_tp);
    }
}
EXPORT_SYMBOL(lcd_trigger_tp_irq_reset);

void lcd_queue_load_tp_fw(void)
{
    if (!g_tp)
        return;
    if (g_tp->lcd_trigger_load_tp_fw_support) {
        TPD_INFO("%s\n", __func__);
        g_tp->disable_gesture_ctrl = true;
        if (g_tp->ts_ops) {
            if (g_tp->ts_ops->tp_queue_work_prepare) {
                mutex_lock(&g_tp->mutex);
                g_tp->ts_ops->tp_queue_work_prepare();
                mutex_unlock(&g_tp->mutex);
            }
        }
        queue_work(g_tp->lcd_trigger_load_tp_fw_wq, &(g_tp->lcd_trigger_load_tp_fw_work));
    }
}

static void lcd_trigger_load_tp_fw(struct work_struct *work)
{
    struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
                                 lcd_trigger_load_tp_fw_work);
    static bool is_running = false;
    // u64 start_time = 0;

    if (ts->lcd_trigger_load_tp_fw_support) {
        if (is_running) {
            TPD_INFO("%s is running, can not repeat\n", __func__);
        } else {
            TPD_INFO("%s start\n", __func__);
            is_running = true;
            mutex_lock(&ts->mutex);
            ts->ts_ops->reset(ts->chip_data);
            mutex_unlock(&ts->mutex);
            is_running = false;

        }
    }
}

void lcd_wait_tp_resume_finished(void)
{
    int retry_cnt = 0;
    if (!g_tp)
        return;
    if (g_tp->lcd_wait_tp_resume_finished_support) {
        TPD_INFO("%s\n", __func__);

        do {
            if(retry_cnt) {
                msleep(100);
            }
            retry_cnt++;
            TPD_DETAIL("Wait hdl finished retry %d times...  \n", retry_cnt);
        } while(!g_tp->resume_finished && retry_cnt < 20);
    }
}
EXPORT_SYMBOL(lcd_wait_tp_resume_finished);

static void lcd_tp_refresh_work(struct work_struct *work)
{
    struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
                                 tp_refresh_work);

    mutex_lock(&ts->mutex);
    ts->ts_ops->tp_refresh_switch(ts->chip_data,
            ts->lcd_fps);
    mutex_unlock(&ts->mutex);

}

void lcd_tp_refresh_switch(int fps)
{

    if (!g_tp)
        return;
    if (g_tp->lcd_tp_refresh_support) {
        TPD_INFO("%s:fps:%d\n", __func__, fps);
        g_tp->lcd_fps = fps;
        if (g_tp->ts_ops) {
            if (g_tp->ts_ops->tp_refresh_switch && !g_tp->is_suspended) {
                queue_work(g_tp->tp_refresh_wq, &g_tp->tp_refresh_work);
            }
        }
    }

}
EXPORT_SYMBOL(lcd_tp_refresh_switch);

/**
 * speedup_resume - speedup resume thread process
 * @work: work struct using for this thread
 *
 * do actully resume function
 * Do not care the result: Return void type
 */
static void speedup_resume(struct work_struct *work)
{
    int timed_out = 0;
    // u64 start_time = 0;
    struct touchpanel_data *ts = container_of(work, struct touchpanel_data,
                                 speed_up_work);

    TPD_INFO("%s is called\n", __func__);

    //step1: get mutex for locking i2c acess flow
    mutex_lock(&ts->mutex);

    //step2:before Resume clear All of touch/key event Reset some flag to default satus
    if (ts->edge_limit_support)
        ts->edge_limit.in_which_area = AREA_NOTOUCH;
    tp_btnkey_release(ts);
    tp_touch_release(ts);

    if (!ts->irq_trigger_hdl_support) {
        if (ts->int_mode == UNBANNABLE) {
            tp_register_irq_func(ts);
        }
    }

    if (ts->use_resume_notify && (!ts->fp_info.touch_state || !ts->report_flow_unlock_support)) {
        reinit_completion(&ts->resume_complete);
    }

    //step3:Reset IC && switch work mode, ft8006 is reset by lcd, no more reset needed
    if (!ts->skip_reset_in_resume && !ts->fp_info.touch_state) {
        if (!ts->lcd_trigger_load_tp_fw_support) {
            ts->ts_ops->reset(ts->chip_data);
        }
    }

    //step4:If use resume notify, exit wait first
    if (ts->use_resume_notify && (!ts->fp_info.touch_state || !ts->report_flow_unlock_support)) {
        timed_out = wait_for_completion_timeout(&ts->resume_complete, 1 * HZ); //wait resume over for 1s
        if ((0 == timed_out) || (ts->resume_complete.done)) {
            TPD_INFO("resume state, timed_out:%d, done:%d\n", timed_out, ts->resume_complete.done);
            if (!timed_out && ts->ts_ops->resume_timedout_operate) {
                ts->ts_ops->resume_timedout_operate(ts->chip_data);
                ts->suspend_state = TP_SPEEDUP_RESUME_COMPLETE;
                TPD_INFO("%s: end!\n", __func__);
                mutex_unlock(&ts->mutex);
                complete(&ts->pm_complete);
                if (ts->lcd_wait_tp_resume_finished_support) {
                    ts->resume_finished = 1;
                }
                return;
            }
        }
    }

    if (ts->ts_ops->specific_resume_operate) {
        ts->ts_ops->specific_resume_operate(ts->chip_data);
    }

    //step5: set default ps status to far
    if (ts->ts_ops->write_ps_status) {
        ts->ts_ops->write_ps_status(ts->chip_data, 0);
    }

    // operate_mode_switch(ts);

    if (ts->esd_handle_support) {
        esd_handle_switch(&ts->esd_info, true);
    }

    //step6:Request irq again
    if (!ts->irq_trigger_hdl_support) {
        if (ts->int_mode == BANNABLE) {
            tp_register_irq_func(ts);
        }
    }

    ts->suspend_state = TP_SPEEDUP_RESUME_COMPLETE;

    if (ts->lcd_wait_tp_resume_finished_support) {
        ts->resume_finished = 1;
    }

    //step7:Unlock  && exit
    TPD_INFO("%s: end!\n", __func__);
    mutex_unlock(&ts->mutex);
    complete(&ts->pm_complete);
}


static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
    int *blank;
    int timed_out = -1;
    struct fb_event *evdata = data;
    struct touchpanel_data *ts = container_of(self, struct touchpanel_data, fb_notif);

    //to aviod some kernel bug (at fbmem.c some local veriable are not initialized)
    if(event != PM_EVENT_SUSPEND && event != PM_EVENT_RESUME && event != FB_EVENT_BLANK)
        return 0;

    if (evdata && evdata->data && ts && ts->chip_data) {
        blank = evdata->data;
        TPD_INFO("%s: event = %ld, blank = %d\n", __func__, event, *blank);
        if (*blank == FB_BLANK_POWERDOWN) { //suspend
            if (event == PM_EVENT_SUSPEND) {    //early event
                timed_out = wait_for_completion_timeout(&ts->pm_complete, 0.5 * HZ); //wait resume over for 0.5s
                if ((0 == timed_out) || (ts->pm_complete.done)) {
                    TPD_INFO("completion state, timed_out:%d, done:%d\n", timed_out, ts->pm_complete.done);
                }

                ts->suspend_state = TP_SUSPEND_EARLY_EVENT;      //set suspend_resume_state
                if (ts->esd_handle_support && ts->is_incell_panel && (ts->tp_suspend_order == LCD_TP_SUSPEND)) {
                    esd_handle_switch(&ts->esd_info, false);     //incell panel need cancel esd early
                }

                if (ts->tp_suspend_order == TP_LCD_SUSPEND) {
                    tp_suspend(ts->dev);
                } else if (ts->tp_suspend_order == LCD_TP_SUSPEND) {
                    if (!ts->gesture_enable && ts->is_incell_panel) {
                        disable_irq_nosync(ts->irq);
                    }
                }

            } else if (event == FB_EVENT_BLANK) {   //event

                if (ts->tp_suspend_order == TP_LCD_SUSPEND) {

                } else if (ts->tp_suspend_order == LCD_TP_SUSPEND) {
                    tp_suspend(ts->dev);
                }
            }
        } else if (*blank == FB_BLANK_UNBLANK ) {//resume
            if (event == PM_EVENT_RESUME) {    //early event
                timed_out = wait_for_completion_timeout(&ts->pm_complete, 0.5 * HZ); //wait suspend over for 0.5s
                if ((0 == timed_out) || (ts->pm_complete.done)) {
                    TPD_INFO("completion state, timed_out:%d, done:%d\n", timed_out, ts->pm_complete.done);
                }

                ts->suspend_state = TP_RESUME_EARLY_EVENT;      //set suspend_resume_state

                if (ts->tp_resume_order == TP_LCD_RESUME) {
                    tp_resume(ts->dev);
                } else if (ts->tp_resume_order == LCD_TP_RESUME) {
                    if (!ts->irq_trigger_hdl_support) {
                        disable_irq_nosync(ts->irq);
                    }
                }
            } else if (event == FB_EVENT_BLANK) {   //event

                if (ts->tp_resume_order == TP_LCD_RESUME) {

                } else if (ts->tp_resume_order == LCD_TP_RESUME) {
                    tp_resume(ts->dev);
                    if (!ts->irq_trigger_hdl_support) {
                        enable_irq(ts->irq);
                    }
                }
            }
        }
    }

    return 0;
}

struct touchpanel_data *common_touch_data_alloc(void)
{
    if (g_tp) {
        TPD_INFO("%s:common panel struct has alloc already!\n", __func__);
        return NULL;
    }
    return kzalloc(sizeof(struct touchpanel_data), GFP_KERNEL);
}

int common_touch_data_free(struct touchpanel_data *pdata)
{
    if (pdata) {
        kfree(pdata);
    }

    g_tp = NULL;
    return 0;
}

void tp_ftm_extra(void)
{
    if (g_tp == NULL) {
        return ;
    }
    if (g_tp->ts_ops) {
        if (g_tp->ts_ops->ftm_process_extra) {
            g_tp->ts_ops->ftm_process_extra();
        }
    }
    return ;
}

EXPORT_SYMBOL(tp_ftm_extra);



/**
 * input_report_key_oppo - Using for report virtual key
 * @work: work struct using for this thread
 *
 * before report virtual key, detect whether touch_area has been touched
 * Do not care the result: Return void type
 */
void input_report_key_oppo(struct input_dev *dev, unsigned int code, int value)
{
    if (value) {//report Key[down]
        if (g_tp) {
            if (g_tp->view_area_touched == 0) {
                input_report_key(dev, code, value);
            } else
                TPD_INFO("Sorry,tp is touch down,can not report touch key\n");
        }
    } else {
        input_report_key(dev, code, value);
    }
}

void clear_view_touchdown_flag(void)
{
    if (g_tp) {
        g_tp->view_area_touched = 0;
    }
}

MODULE_DESCRIPTION("Himax common spi touch driver");
MODULE_LICENSE("GPL v2");