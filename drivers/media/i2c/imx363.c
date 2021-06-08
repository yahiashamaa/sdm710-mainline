// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony imx363 Camera Sensor Driver
 *
 * Copyright (C) 2021 Intel Corporation
 */
#include <asm/unaligned.h>

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include <linux/regulator/consumer.h>

/* Streaming Mode */
#define IMX363_REG_MODE_SELECT	0x0100
#define IMX363_MODE_STANDBY	0x00
#define IMX363_MODE_STREAMING	0x01

/* Lines per frame */
#define IMX363_REG_LPFR		0x0340

/* Chip ID */
#define IMX363_REG_ID		0x0016
#define IMX363_ID		0x0363

/* Exposure control */
#define IMX363_REG_EXPOSURE_CIT	0x0202
#define IMX363_EXPOSURE_MIN	8
#define IMX363_EXPOSURE_OFFSET	22
#define IMX363_EXPOSURE_STEP	1
#define IMX363_EXPOSURE_DEFAULT	0x0648

/* Analog gain control */
#define IMX363_REG_AGAIN	0x0204
#define IMX363_AGAIN_MIN	0
#define IMX363_AGAIN_MAX	978
#define IMX363_AGAIN_STEP	1
#define IMX363_AGAIN_DEFAULT	0

/* Group hold register */
#define IMX363_REG_HOLD		0x0104

/* Input clock rate */
#define IMX363_INCLK_RATE	24000000

/* CSI2 HW configuration */
#define IMX363_LINK_FREQ	600000000
#define IMX363_NUM_DATA_LANES	4

#define IMX363_REG_MIN		0x00
#define IMX363_REG_MAX		0xffff

/* Register Map */
#define IMX363_REG_EXT_CLK_FREQ_H	 0x0136
#define IMX363_REG_EXT_CLK_FREQ_L	 0x0137
#define IMX363_REG_CSI_DATA_FMT_H	 0x0112
#define IMX363_REG_CSI_DATA_FMT_L	 0x0113
#define IMX363_REG_CSI_LANE_MODE     0x0114
#define IMX363_REG_LINE_LENGTH_PCK_H 0x0342
#define IMX363_REG_LINE_LENGTH_PCK_L 0x0343
#define IMX363_REG_X_ADDR_START_H	 0x0344 
#define IMX363_REG_X_ADDR_START_L	 0x0345 
#define IMX363_REG_Y_ADDR_START_H	 0x0346
#define IMX363_REG_Y_ADDR_START_L	 0x0347
#define IMX363_REG_X_ADDR_END_H		 0x0348
#define IMX363_REG_X_ADDR_END_L		 0x0349
#define IMX363_REG_Y_ADDR_END_H		 0x034a
#define IMX363_REG_Y_ADDR_END_L		 0x034b
#define IMX363_REG_HDR_MODE			 0x0220
#define IMX363_REG_HDR_RESO_REDU_V	 0x0221
#define IMX363_REG_X_EVN_INC		 0x0381
#define IMX363_REG_X_ODD_INC		 0x0383
#define IMX363_REG_Y_EVN_INC		 0x0385
#define IMX363_REG_Y_ODD_INC		 0x0387
#define IMX363_REG_BINNING_MODE		 0x0900
#define IMX363_REG_BINNING_TYPE		 0x0901
#define IMX363_REG_DIG_CROP_X_OFFSET_H	   0x0408
#define IMX363_REG_DIG_CROP_X_OFFSET_L	   0x0409
#define IMX363_REG_DIG_CROP_Y_OFFSET_H	   0x040a
#define IMX363_REG_DIG_CROP_Y_OFFSET_L	   0x040b
#define IMX363_REG_DIG_CROP_IMAGE_WIDTH_H  0x040c
#define IMX363_REG_DIG_CROP_IMAGE_WIDTH_L  0x040d
#define IMX363_REG_DIG_CROP_IMAGE_HEIGHT_H 0x040e
#define IMX363_REG_DIG_CROP_IMAGE_HEIGHT_L 0x040f
#define IMX363_REG_X_OUT_SIZE_H			   0x034c
#define IMX363_REG_X_OUT_SIZE_L			   0x034d
#define IMX363_REG_Y_OUT_SIZE_H			   0x034e
#define IMX363_REG_Y_OUT_SIZE_L			   0x034f
#define IMX363_REG_IVTSXCK_DIV			   0x0301
#define IMX363_REG_IVTSYCK_DIV			   0x0303
#define IMX363_REG_PREPLLCK_IVT_DIV		   0x0305
#define IMX363_REG_PLL_IVT_MPY_H		   0x0306
#define IMX363_REG_PLL_IVT_MPY_L		   0x0307
#define IMX363_REG_IOPPXCK_DIV			   0x0309
#define IMX363_REG_IOPSYCK_DIV			   0x030b
#define IMX363_REG_PREPLLCK_IOP_DIV		   0x030d
#define IMX363_REG_PLL_IOP_MPY_H		   0x030e
#define IMX363_REG_PLL_IOP_MPY_L		   0x030f
#define IMX363_REG_PLL_MULT_DRIV		   0x0310
#define IMX363_REG_ST_COARSE_INTEG_TIME_H  0x0224
#define IMX363_REG_ST_COARSE_INTEG_TIME_L  0x0225
#define IMX363_REG_ST_ANA_GAIN_GLOBAL_H	   0x0216
#define IMX363_REG_ST_ANA_GAIN_GLOBAL_L	   0x0217
#define IMX363_REG_ST_DIG_GAIN_GLOBAL_H	   0x0226
#define IMX363_REG_ST_DIG_GAIN_GLOBAL_L	   0x0227
#define IMX363_REG_DIG_GAIN_GLOBAL_H	0x020e
#define IMX363_REG_DIG_GAIN_GLOBAL_L	0x020f

/**
 * struct imx363_reg - imx363 sensor register
 * @address: Register address
 * @val: Register value
 */
struct imx363_reg {
	u16 address;
	u8 val;
};

/**
 * struct imx363_reg_list - imx363 sensor register list
 * @num_of_regs: Number of registers in the list
 * @regs: Pointer to register list
 */
struct imx363_reg_list {
	u32 num_of_regs;
	const struct imx363_reg *regs;
};

/**
 * struct imx363_mode - imx363 sensor mode structure
 * @width: Frame width
 * @height: Frame height
 * @code: Format code
 * @hblank: Horizontal blanking in lines
 * @vblank: Vertical blanking in lines
 * @vblank_min: Minimum vertical blanking in lines
 * @vblank_max: Maximum vertical blanking in lines
 * @pclk: Sensor pixel clock
 * @link_freq_idx: Link frequency index
 * @reg_list: Register list for sensor mode
 */
struct imx363_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 hblank;
	u32 vblank;
	u32 vblank_min;
	u32 vblank_max;
	u64 pclk;
	u32 link_freq_idx;
	struct imx363_reg_list reg_list;
};

static const char * const imx363_supply_names [] = {
	"dovdd",	/* Digital I/O power */
	"avdd",		/* Analog power */
	"dvdd",		/* Digital core power */
};

/**
 * struct imx363 - imx363 sensor device structure
 * @dev: Pointer to generic device
 * @client: Pointer to i2c client
 * @sd: V4L2 sub-device
 * @pad: Media pad. Only one pad supported
 * @reset_gpio: Sensor reset gpio
 * @inclk: Sensor input clock
 * @ctrl_handler: V4L2 control handler
 * @link_freq_ctrl: Pointer to link frequency control
 * @pclk_ctrl: Pointer to pixel clock control
 * @hblank_ctrl: Pointer to horizontal blanking control
 * @vblank_ctrl: Pointer to vertical blanking control
 * @exp_ctrl: Pointer to exposure control
 * @again_ctrl: Pointer to analog gain control
 * @vblank: Vertical blanking in lines
 * @cur_mode: Pointer to current selected sensor mode
 * @mutex: Mutex for serializing sensor controls
 * @streaming: Flag indicating streaming state
 */
struct imx363 {
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	struct clk *inclk;
    struct regulator_bulk_data supplies[ARRAY_SIZE(imx363_supply_names)];
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq_ctrl;
	struct v4l2_ctrl *pclk_ctrl;
	struct v4l2_ctrl *hblank_ctrl;
	struct v4l2_ctrl *vblank_ctrl;
	struct {
		struct v4l2_ctrl *exp_ctrl;
		struct v4l2_ctrl *again_ctrl;
	};
	u32 vblank;
	const struct imx363_mode *cur_mode;
	struct mutex mutex;
	bool streaming;
};

static const s64 link_freq[] = {
	IMX363_LINK_FREQ,
};

/* Sensor mode registers */
static const struct imx363_reg mode_4032x3024_regs[] = {
    { IMX363_REG_EXT_CLK_FREQ_H, 0x18 }, // Integer Part (24.00 Mhz)
	{ IMX363_REG_EXT_CLK_FREQ_L, 0x00 }, // Fractional Part
	//Magical IMX363 Regs & Values - Found in downstream
	{ 0x31a3, 0x00 },
	{ 0x64d4, 0x01 },
	{ 0x64d5, 0xaa },
	{ 0x64d6, 0x01 },
	{ 0x64d7, 0xa9 },
	{ 0x64d8, 0x01 },
	{ 0x64d9, 0xa5 },
	{ 0x64da, 0x01 },
	{ 0x64db, 0xa1 },
	{ 0x720a, 0x24 },
	{ 0x720b, 0x89 },
	{ 0x720c, 0x85 },
	{ 0x720d, 0xa1 },
	{ 0x720e, 0x6e },
	{ 0x729c, 0x59 },
	{ 0x817c, 0xff },
	{ 0x817d, 0x80 },
	{ 0x9348, 0x96 },
	{ 0x934b, 0x8c },
	{ 0x934c, 0x82 },
	{ 0x9353, 0xaa },
	{ 0x9354, 0xaa },
	{ IMX363_REG_CSI_DATA_FMT_H, 0x0a }, // 10bit data format
	{ IMX363_REG_CSI_DATA_FMT_L, 0x0a },
	{ IMX363_REG_CSI_LANE_MODE, 0x03 }, // 4 Lanes
	{ IMX363_REG_LINE_LENGTH_PCK_H, 0x22 }, // llp - 8832
	{ IMX363_REG_LINE_LENGTH_PCK_L, 0x80 },
	{ IMX363_REG_X_EVN_INC, 0x01 },
	{ IMX363_REG_X_ODD_INC, 0x01 },
	{ IMX363_REG_Y_EVN_INC, 0x01 },
	{ IMX363_REG_Y_ODD_INC, 0x01 },
	{ IMX363_REG_BINNING_MODE, 0x00 },
	{ IMX363_REG_BINNING_TYPE, 0x11 },
	{ 0x0902, 0x00 }, //Not present in downstream camss logs. but seems important.
	//Magical IMX363 Regs & Values - Found in downstream
	{ 0x30F4, 0x02 },
	{ 0x30F5, 0x80 },
	{ 0x31A5, 0x00 },
	{ 0x31A6, 0x00 },
	{ 0x560F, 0xBE },
	{ 0x5856, 0x08 },
	{ 0x58D0, 0x10 },
	{ 0x734A, 0x01 },
	{ 0x734F, 0x2B },
	{ 0x7441, 0x55 },
	{ 0x7914, 0x03 },
	{ 0x7928, 0x04 },
	{ 0x7929, 0x04 },
	{ 0x793F, 0x03 },
	{ 0x0340, 0x0c }, // fll - 3140
	{ 0x0341, 0x44 },
	{ IMX363_REG_X_ADDR_START_H, 0x00 }, //Start (0,0)
	{ IMX363_REG_X_ADDR_START_L, 0x00 },
	{ IMX363_REG_Y_ADDR_START_H, 0x00 },
	{ IMX363_REG_Y_ADDR_START_L, 0x00 },
	{ IMX363_REG_X_ADDR_END_H, 0x0f }, //End (4031,3023)
	{ IMX363_REG_X_ADDR_END_L, 0xbf },
	{ IMX363_REG_Y_ADDR_END_H, 0x0b },
	{ IMX363_REG_Y_ADDR_END_L, 0xcf },
	{ IMX363_REG_HDR_MODE, 0x00 },
	{ IMX363_REG_HDR_RESO_REDU_V, 0x11 },
	////Not present in downstream camss logs. but seems important.
	{ 0x0401, 0x00 },
	{ 0x0404, 0x00 },
	{ 0x0405, 0x10 },
	{ IMX363_REG_DIG_CROP_X_OFFSET_H, 0x00 },
	{ IMX363_REG_DIG_CROP_X_OFFSET_L, 0x00 },
	{ IMX363_REG_DIG_CROP_Y_OFFSET_H, 0x00 },
	{ IMX363_REG_DIG_CROP_Y_OFFSET_L, 0x00 },
	{ IMX363_REG_DIG_CROP_IMAGE_WIDTH_H, 0x0f }, //Crop Size (4032,3024) - No crop - Full image
	{ IMX363_REG_DIG_CROP_IMAGE_WIDTH_L, 0xc0 },
	{ IMX363_REG_DIG_CROP_IMAGE_HEIGHT_H, 0x0b },
	{ IMX363_REG_DIG_CROP_IMAGE_HEIGHT_L, 0xd0 },
	{ IMX363_REG_X_OUT_SIZE_H, 0x0f }, // Output Image Size (4032,3024)
	{ IMX363_REG_X_OUT_SIZE_L, 0xc0 },
	{ IMX363_REG_Y_OUT_SIZE_H, 0x0b },
	{ IMX363_REG_Y_OUT_SIZE_L, 0xd0 },
	{ IMX363_REG_IVTSXCK_DIV, 0x03 },
	{ IMX363_REG_IVTSYCK_DIV, 0x02 },
	{ IMX363_REG_PREPLLCK_IVT_DIV, 0x04 },
	{ IMX363_REG_PLL_IVT_MPY_H, 0x00 },
	{ IMX363_REG_PLL_IVT_MPY_L, 0xd0 },
	{ IMX363_REG_IOPPXCK_DIV, 0x0a },
	{ IMX363_REG_IOPSYCK_DIV, 0x01 },
	{ IMX363_REG_PREPLLCK_IOP_DIV, 0x04 },
	{ IMX363_REG_PLL_IOP_MPY_H, 0x00 },
	{ IMX363_REG_PLL_IOP_MPY_L, 0xe6 },
	{ IMX363_REG_PLL_MULT_DRIV, 0x01 },
	{ IMX363_REG_EXPOSURE_CIT, 0x06 }, // Default exposure = 0x0640
	{ 0x0203, 0x40 },
	{ IMX363_REG_ST_COARSE_INTEG_TIME_H, 0x01 },
	{ IMX363_REG_ST_COARSE_INTEG_TIME_L, 0xf4 },
	{ IMX363_REG_AGAIN, 0x00 }, // Default analog gain = 0
	{ 0x0205, 0x00 },
	{ IMX363_REG_ST_ANA_GAIN_GLOBAL_H, 0x00 },
	{ IMX363_REG_ST_ANA_GAIN_GLOBAL_L, 0x00 },
	{ IMX363_REG_DIG_GAIN_GLOBAL_H, 0x01 }, // Default digital gain = 256
	{ IMX363_REG_DIG_GAIN_GLOBAL_L, 0x00 },
	{ IMX363_REG_ST_DIG_GAIN_GLOBAL_H, 0x01},
	{ IMX363_REG_ST_DIG_GAIN_GLOBAL_L, 0x00},
};

/* Supported sensor mode configurations */
static const struct imx363_mode supported_mode = {
	.width = 4032,
	.height = 3024,
	.hblank = 4800, /* hblank = llp - width */
	.vblank = 116, /* vblank = fll - height */
	.vblank_min = 116,
	.vblank_max = 32420,
	.pclk = 686000000,
	.link_freq_idx = 0,
	.code = MEDIA_BUS_FMT_SRGGB10_1X10,
	.reg_list = {
		.num_of_regs = ARRAY_SIZE(mode_4032x3024_regs),
		.regs = mode_4032x3024_regs,
	},
};

/**
 * to_imx363() - imx363 V4L2 sub-device to imx363 device.
 * @subdev: pointer to imx363 V4L2 sub-device
 *
 * Return: pointer to imx363 device
 */
static inline struct imx363 *to_imx363(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct imx363, sd);
}

/**
 * imx363_read_reg() - Read registers.
 * @imx363: pointer to imx363 device
 * @reg: register address
 * @len: length of bytes to read. Max supported bytes is 4
 * @val: pointer to register value to be filled.
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_read_reg(struct imx363 *imx363, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	struct i2c_msg msgs[2] = {0};
	u8 addr_buf[2] = {0};
	u8 data_buf[4] = {0};
	int ret;

	if (WARN_ON(len > 4))
		return -EINVAL;

	put_unaligned_be16(reg, addr_buf);

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_buf[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = get_unaligned_be32(data_buf);

	return 0;
}

/**
 * imx363_write_reg() - Write register
 * @imx363: pointer to imx363 device
 * @reg: register address
 * @len: length of bytes. Max supported bytes is 4
 * @val: register value
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_write_reg(struct imx363 *imx363, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	u8 buf[6] = {0};

	if (WARN_ON(len > 4))
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_be32(val << (8 * (4 - len)), buf + 2);
	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/**
 * imx363_write_regs() - Write a list of registers
 * @imx363: pointer to imx363 device
 * @regs: list of registers to be written
 * @len: length of registers array
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_write_regs(struct imx363 *imx363,
			     const struct imx363_reg *regs, u32 len)
{
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = imx363_write_reg(imx363, regs[i].address, 1, regs[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * imx363_update_controls() - Update control ranges based on streaming mode
 * @imx363: pointer to imx363 device
 * @mode: pointer to imx363_mode sensor mode
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_update_controls(struct imx363 *imx363,
				  const struct imx363_mode *mode)
{
	int ret;

	ret = __v4l2_ctrl_s_ctrl(imx363->link_freq_ctrl, mode->link_freq_idx);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(imx363->hblank_ctrl, mode->hblank);
	if (ret)
		return ret;

	return __v4l2_ctrl_modify_range(imx363->vblank_ctrl, mode->vblank_min,
					mode->vblank_max, 1, mode->vblank);
}

/**
 * imx363_update_exp_gain() - Set updated exposure and gain
 * @imx363: pointer to imx363 device
 * @exposure: updated exposure value
 * @gain: updated analog gain value
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_update_exp_gain(struct imx363 *imx363, u32 exposure, u32 gain)
{
	u32 lpfr, shutter;
	int ret;

	lpfr = imx363->vblank + imx363->cur_mode->height;
	shutter = lpfr - exposure;

	dev_dbg(imx363->dev, "Set exp %u, analog gain %u, shutter %u, lpfr %u",
		exposure, gain, shutter, lpfr);

	ret = imx363_write_reg(imx363, IMX363_REG_HOLD, 1, 1);
	if (ret)
		return ret;

	ret = imx363_write_reg(imx363, IMX363_REG_LPFR, 2, lpfr);
	if (ret)
		goto error_release_group_hold;

	ret = imx363_write_reg(imx363, IMX363_REG_EXPOSURE_CIT, 2, shutter);
	if (ret)
		goto error_release_group_hold;

	ret = imx363_write_reg(imx363, IMX363_REG_AGAIN, 2, gain);

error_release_group_hold:
	imx363_write_reg(imx363, IMX363_REG_HOLD, 1, 0);

	return ret;
}

/**
 * imx363_set_ctrl() - Set subdevice control
 * @ctrl: pointer to v4l2_ctrl structure
 *
 * Supported controls:
 * - V4L2_CID_VBLANK
 * - cluster controls:
 *   - V4L2_CID_ANALOGUE_GAIN
 *   - V4L2_CID_EXPOSURE
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx363 *imx363 =
		container_of(ctrl->handler, struct imx363, ctrl_handler);
	u32 analog_gain;
	u32 exposure;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		imx363->vblank = imx363->vblank_ctrl->val;

		dev_dbg(imx363->dev, "Received vblank %u, new lpfr %u",
			imx363->vblank,
			imx363->vblank + imx363->cur_mode->height);

		ret = __v4l2_ctrl_modify_range(imx363->exp_ctrl,
					       IMX363_EXPOSURE_MIN,
					       imx363->vblank +
					       imx363->cur_mode->height -
					       IMX363_EXPOSURE_OFFSET,
					       1, IMX363_EXPOSURE_DEFAULT);
		break;
	case V4L2_CID_EXPOSURE:
		/* Set controls only if sensor is in power on state */
		if (!pm_runtime_get_if_in_use(imx363->dev))
			return 0;

		exposure = ctrl->val;
		analog_gain = imx363->again_ctrl->val;

		dev_dbg(imx363->dev, "Received exp %u, analog gain %u",
			exposure, analog_gain);

		ret = imx363_update_exp_gain(imx363, exposure, analog_gain);

		pm_runtime_put(imx363->dev);

		break;
	default:
		dev_err(imx363->dev, "Invalid control %d", ctrl->id);
		ret = -EINVAL;
	}

	return ret;
}

/* V4l2 subdevice control ops*/
static const struct v4l2_ctrl_ops imx363_ctrl_ops = {
	.s_ctrl = imx363_set_ctrl,
};

/**
 * imx363_enum_mbus_code() - Enumerate V4L2 sub-device mbus codes
 * @sd: pointer to imx363 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @code: V4L2 sub-device code enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = supported_mode.code;

	return 0;
}

/**
 * imx363_enum_frame_size() - Enumerate V4L2 sub-device frame sizes
 * @sd: pointer to imx363 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fsize: V4L2 sub-device size enumeration need to be filled
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fsize)
{
	if (fsize->index > 0)
		return -EINVAL;

	if (fsize->code != supported_mode.code)
		return -EINVAL;

	fsize->min_width = supported_mode.width;
	fsize->max_width = fsize->min_width;
	fsize->min_height = supported_mode.height;
	fsize->max_height = fsize->min_height;

	return 0;
}

/**
 * imx363_fill_pad_format() - Fill subdevice pad format
 *                            from selected sensor mode
 * @imx363: pointer to imx363 device
 * @mode: pointer to imx363_mode sensor mode
 * @fmt: V4L2 sub-device format need to be filled
 */
static void imx363_fill_pad_format(struct imx363 *imx363,
				   const struct imx363_mode *mode,
				   struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = mode->code;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;
}

/**
 * imx363_get_pad_format() - Get subdevice pad format
 * @sd: pointer to imx363 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx363 *imx363 = to_imx363(sd);

	mutex_lock(&imx363->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
		fmt->format = *framefmt;
	} else {
		imx363_fill_pad_format(imx363, imx363->cur_mode, fmt);
	}

	mutex_unlock(&imx363->mutex);

	return 0;
}

/**
 * imx363_set_pad_format() - Set subdevice pad format
 * @sd: pointer to imx363 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx363 *imx363 = to_imx363(sd);
	const struct imx363_mode *mode;
	int ret = 0;

	mutex_lock(&imx363->mutex);

	mode = &supported_mode;
	imx363_fill_pad_format(imx363, mode, fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
		*framefmt = fmt->format;
	} else {
		ret = imx363_update_controls(imx363, mode);
		if (!ret)
			imx363->cur_mode = mode;
	}

	mutex_unlock(&imx363->mutex);

	return ret;
}

/**
 * imx363_init_pad_cfg() - Initialize sub-device pad configuration
 * @sd: pointer to imx363 V4L2 sub-device structure
 * @sd_state: V4L2 sub-device configuration
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_init_pad_cfg(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state)
{
	struct imx363 *imx363 = to_imx363(sd);
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	imx363_fill_pad_format(imx363, &supported_mode, &fmt);

	return imx363_set_pad_format(sd, sd_state, &fmt);
}

/**
 * imx363_start_streaming() - Start sensor stream
 * @imx363: pointer to imx363 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_start_streaming(struct imx363 *imx363)
{
	const struct imx363_reg_list *reg_list;
	int ret;

	/* Write sensor mode registers */
	reg_list = &imx363->cur_mode->reg_list;
	ret = imx363_write_regs(imx363, reg_list->regs,
				reg_list->num_of_regs);
	if (ret) {
		dev_err(imx363->dev, "fail to write initial registers");
		return ret;
	}

	/* Setup handler will write actual exposure and gain */
	ret =  __v4l2_ctrl_handler_setup(imx363->sd.ctrl_handler);
	if (ret) {
		dev_err(imx363->dev, "fail to setup handler");
		return ret;
	}

	/* Delay is required before streaming*/
	usleep_range(7400, 8000);

	/* Start streaming */
	ret = imx363_write_reg(imx363, IMX363_REG_MODE_SELECT,
			       1, IMX363_MODE_STREAMING);
	if (ret) {
		dev_err(imx363->dev, "fail to start streaming");
		return ret;
	}

	return 0;
}

/**
 * imx363_stop_streaming() - Stop sensor stream
 * @imx363: pointer to imx363 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_stop_streaming(struct imx363 *imx363)
{
	return imx363_write_reg(imx363, IMX363_REG_MODE_SELECT,
				1, IMX363_MODE_STANDBY);
}

/**
 * imx363_set_stream() - Enable sensor streaming
 * @sd: pointer to imx363 subdevice
 * @enable: set to enable sensor streaming
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	mutex_lock(&imx363->mutex);

	if (imx363->streaming == enable) {
		mutex_unlock(&imx363->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_resume_and_get(imx363->dev);
		if (ret)
			goto error_unlock;

		ret = imx363_start_streaming(imx363);
		if (ret)
			goto error_power_off;
	} else {
		imx363_stop_streaming(imx363);
		pm_runtime_put(imx363->dev);
	}

	imx363->streaming = enable;

	mutex_unlock(&imx363->mutex);

	return 0;

error_power_off:
	pm_runtime_put(imx363->dev);
error_unlock:
	mutex_unlock(&imx363->mutex);

	return ret;
}

/**
 * imx363_detect() - Detect imx363 sensor
 * @imx363: pointer to imx363 device
 *
 * Return: 0 if successful, -EIO if sensor id does not match
 */
static int imx363_detect(struct imx363 *imx363)
{
	int ret;
	u32 val;

	ret = imx363_read_reg(imx363, IMX363_REG_ID, 2, &val);
	if (ret)
		return ret;

	if (val != IMX363_ID) {
		dev_err(imx363->dev, "chip id mismatch: %x!=%x",
			IMX363_ID, val);
		return -ENXIO;
	}

	return 0;
}

/**
 * imx363_parse_hw_config() - Parse HW configuration and check if supported
 * @imx363: pointer to imx363 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_parse_hw_config(struct imx363 *imx363)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx363->dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep;
	unsigned long rate;
	unsigned int i;
    u32 inclk_rate;
	int ret;

	if (!fwnode)
		return -ENXIO;

	/* Request optional reset pin */
	imx363->reset_gpio = devm_gpiod_get_optional(imx363->dev, "reset",
						     GPIOD_OUT_LOW);
	if (IS_ERR(imx363->reset_gpio)) {
		dev_err(imx363->dev, "failed to get reset gpio %ld",
			PTR_ERR(imx363->reset_gpio));
		return PTR_ERR(imx363->reset_gpio);
	}

	/* Get sensor input clock */
	imx363->inclk = devm_clk_get(imx363->dev, NULL);
	if (IS_ERR(imx363->inclk)) {
		dev_err(imx363->dev, "could not get inclk");
		return PTR_ERR(imx363->inclk);
	}

    if (!is_acpi_node(fwnode)) {
		ret = fwnode_property_read_u32(fwnode, "clock-frequency",
					       &inclk_rate);
		if (ret)
			return ret;

		clk_set_rate(imx363->inclk, inclk_rate);
	}

	rate = clk_get_rate(imx363->inclk);
	if (rate != IMX363_INCLK_RATE) {
		dev_err(imx363->dev, "inclk frequency mismatch");
		return -EINVAL;
	}

    if (!is_acpi_node(fwnode)) {
		/* Get DT defined regulators */
		for (i = 0; i < ARRAY_SIZE(imx363_supply_names); i++)
			imx363->supplies[i].supply = imx363_supply_names[i];

		ret = devm_regulator_bulk_get(imx363->dev,
					      ARRAY_SIZE(imx363_supply_names),
					      imx363->supplies);
		if (ret)
			return ret;
	}

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX363_NUM_DATA_LANES) {
		dev_err(imx363->dev,
			"number of CSI2 data lanes %d is not supported",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(imx363->dev, "no link frequencies defined");
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++)
		if (bus_cfg.link_frequencies[i] == IMX363_LINK_FREQ)
			goto done_endpoint_free;

	ret = -EINVAL;

done_endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

/* V4l2 subdevice ops */
static const struct v4l2_subdev_video_ops imx363_video_ops = {
	.s_stream = imx363_set_stream,
};

static const struct v4l2_subdev_pad_ops imx363_pad_ops = {
	.init_cfg = imx363_init_pad_cfg,
	.enum_mbus_code = imx363_enum_mbus_code,
	.enum_frame_size = imx363_enum_frame_size,
	.get_fmt = imx363_get_pad_format,
	.set_fmt = imx363_set_pad_format,
};

static const struct v4l2_subdev_ops imx363_subdev_ops = {
	.video = &imx363_video_ops,
	.pad = &imx363_pad_ops,
};

/**
 * imx363_power_on() - Sensor power on sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

    ret = regulator_bulk_enable(ARRAY_SIZE(imx363_supply_names),
				    imx363->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable regulators\n");
		goto error_reset;
	}

	usleep_range(400, 600);

	gpiod_set_value_cansleep(imx363->reset_gpio, 1);

	ret = clk_prepare_enable(imx363->inclk);
	if (ret) {
		dev_err(imx363->dev, "fail to enable inclk");
		goto error_reset;
	}

	usleep_range(1000, 1200);

	return 0;

error_reset:
	gpiod_set_value_cansleep(imx363->reset_gpio, 0);

	return ret;
}

/**
 * imx363_power_off() - Sensor power off sequence
 * @dev: pointer to i2c device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx363 *imx363 = to_imx363(sd);

	gpiod_set_value_cansleep(imx363->reset_gpio, 0);

	clk_disable_unprepare(imx363->inclk);

    regulator_bulk_disable(ARRAY_SIZE(imx363_supply_names),
			       imx363->supplies);

	return 0;
}

/**
 * imx363_init_controls() - Initialize sensor subdevice controls
 * @imx363: pointer to imx363 device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_init_controls(struct imx363 *imx363)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx363->ctrl_handler;
	const struct imx363_mode *mode = imx363->cur_mode;
	u32 lpfr;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 6);
	if (ret)
		return ret;

	/* Serialize controls with sensor device */
	ctrl_hdlr->lock = &imx363->mutex;

	/* Initialize exposure and gain */
	lpfr = mode->vblank + mode->height;
	imx363->exp_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					     &imx363_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX363_EXPOSURE_MIN,
					     lpfr - IMX363_EXPOSURE_OFFSET,
					     IMX363_EXPOSURE_STEP,
					     IMX363_EXPOSURE_DEFAULT);

	imx363->again_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					       &imx363_ctrl_ops,
					       V4L2_CID_ANALOGUE_GAIN,
					       IMX363_AGAIN_MIN,
					       IMX363_AGAIN_MAX,
					       IMX363_AGAIN_STEP,
					       IMX363_AGAIN_DEFAULT);

	v4l2_ctrl_cluster(2, &imx363->exp_ctrl);

	imx363->vblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx363_ctrl_ops,
						V4L2_CID_VBLANK,
						mode->vblank_min,
						mode->vblank_max,
						1, mode->vblank);

	/* Read only controls */
	imx363->pclk_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					      &imx363_ctrl_ops,
					      V4L2_CID_PIXEL_RATE,
					      mode->pclk, mode->pclk,
					      1, mode->pclk);

	imx363->link_freq_ctrl = v4l2_ctrl_new_int_menu(ctrl_hdlr,
							&imx363_ctrl_ops,
							V4L2_CID_LINK_FREQ,
							ARRAY_SIZE(link_freq) -
							1,
							mode->link_freq_idx,
							link_freq);
	if (imx363->link_freq_ctrl)
		imx363->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx363->hblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx363_ctrl_ops,
						V4L2_CID_HBLANK,
						IMX363_REG_MIN,
						IMX363_REG_MAX,
						1, mode->hblank);
	if (imx363->hblank_ctrl)
		imx363->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (ctrl_hdlr->error) {
		dev_err(imx363->dev, "control init failed: %d",
			ctrl_hdlr->error);
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ctrl_hdlr->error;
	}

	imx363->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

/**
 * imx363_probe() - I2C client device binding
 * @client: pointer to i2c client device
 *
 * Return: 0 if successful, error code otherwise.
 */
static int imx363_probe(struct i2c_client *client)
{
	struct imx363 *imx363;
	int ret;

	imx363 = devm_kzalloc(&client->dev, sizeof(*imx363), GFP_KERNEL);
	if (!imx363)
		return -ENOMEM;

	imx363->dev = &client->dev;

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&imx363->sd, client, &imx363_subdev_ops);

	ret = imx363_parse_hw_config(imx363);
	if (ret) {
		dev_err(imx363->dev, "HW configuration is not supported");
		return ret;
	}

	mutex_init(&imx363->mutex);

	ret = imx363_power_on(imx363->dev);
	if (ret) {
		dev_err(imx363->dev, "failed to power-on the sensor");
		goto error_mutex_destroy;
	}

	/* Check module identity */
	ret = imx363_detect(imx363);
	if (ret) {
		dev_err(imx363->dev, "failed to find sensor: %d", ret);
		goto error_power_off;
	}

	/* Set default mode to max resolution */
	imx363->cur_mode = &supported_mode;
	imx363->vblank = imx363->cur_mode->vblank;

	ret = imx363_init_controls(imx363);
	if (ret) {
		dev_err(imx363->dev, "failed to init controls: %d", ret);
		goto error_power_off;
	}

	/* Initialize subdev */
	imx363->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx363->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx363->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx363->sd.entity, 1, &imx363->pad);
	if (ret) {
		dev_err(imx363->dev, "failed to init entity pads: %d", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&imx363->sd);
	if (ret < 0) {
		dev_err(imx363->dev,
			"failed to register async subdev: %d", ret);
		goto error_media_entity;
	}

	pm_runtime_set_active(imx363->dev);
	pm_runtime_enable(imx363->dev);
	pm_runtime_idle(imx363->dev);

	dev_info(imx363->dev, "probed successfully");

	return 0;

error_media_entity:
	media_entity_cleanup(&imx363->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(imx363->sd.ctrl_handler);
error_power_off:
	imx363_power_off(imx363->dev);
error_mutex_destroy:
	mutex_destroy(&imx363->mutex);

	return ret;
}

/**
 * imx363_remove() - I2C client device unbinding
 * @client: pointer to I2C client device
 *
 */
static void imx363_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx363 *imx363 = to_imx363(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx363_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	mutex_destroy(&imx363->mutex);
}

static const struct dev_pm_ops imx363_pm_ops = {
	SET_RUNTIME_PM_OPS(imx363_power_off, imx363_power_on, NULL)
};

static const struct of_device_id imx363_of_match[] = {
	{ .compatible = "sony,imx363" },
	{ }
};

MODULE_DEVICE_TABLE(of, imx363_of_match);

static struct i2c_driver imx363_driver = {
	.probe_new = imx363_probe,
	.remove = imx363_remove,
	.driver = {
		.name = "imx363",
		.pm = &imx363_pm_ops,
		.of_match_table = imx363_of_match,
	},
};

module_i2c_driver(imx363_driver);

MODULE_DESCRIPTION("Sony imx363 sensor driver");
MODULE_LICENSE("GPL");
