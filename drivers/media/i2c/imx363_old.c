// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 Intel Corporation

#include <asm/unaligned.h>
#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>

#define IMX363_REG_MODE_SELECT 		0x0100
#define IMX363_MODE_STANDBY		    0x00
#define IMX363_MODE_STREAMING		0x01

/* Chip ID */
#define IMX363_REG_CHIP_ID		0x0016
#define IMX363_CHIP_ID			0x0363

/* V_TIMING internal */
#define IMX363_REG_FLL_H		0x0340
#define IMX363_REG_FLL_L		0x0341
#define IMX363_FLL_MAX			0xffff

/* Exposure control */
#define IMX363_REG_EXPOSURE_H		0x0202
#define IMX363_REG_EXPOSURE_L		0x0203
#define IMX363_EXPOSURE_MIN		4
#define IMX363_EXPOSURE_STEP	1
#define IMX363_EXPOSURE_DEFAULT	0x0640

/*
 *  the digital control register for all color control looks like:
 *  +-----------------+------------------+
 *  |      [7:0]      |       [15:8]     |
 *  +-----------------+------------------+
 *  |	  0x020f      |       0x020e     |
 *  --------------------------------------
 *  it is used to calculate the digital gain times value(integral + fractional)
 *  the [15:8] bits is the fractional part and [7:0] bits is the integral
 *  calculation equation is:
 *      gain value (unit: times) = REG[15:8] + REG[7:0]/0x100
 *  Only value in 0x0100 ~ 0x0FFF range is allowed.
 *  Analog gain use 10 bits in the registers and allowed range is 0 ~ 960
 */
/* Analog gain control */
#define IMX363_REG_ANALOG_GAIN_H	0x0204
#define IMX363_REG_ANALOG_GAIN_L	0x0205
#define IMX363_ANA_GAIN_MIN		0
#define IMX363_ANA_GAIN_MAX		448
#define IMX363_ANA_GAIN_STEP	1
#define IMX363_ANA_GAIN_DEFAULT	0

/* Digital gain control */
#define IMX363_REG_DPGA_USE_GLOBAL_GAIN	0x3ff9
#define IMX363_REG_DIG_GAIN_GLOBAL_H	0x020e
#define IMX363_REG_DIG_GAIN_GLOBAL_L	0x020f
#define IMX363_DGTL_GAIN_MIN		256
#define IMX363_DGTL_GAIN_MAX		4095
#define IMX363_DGTL_GAIN_STEP		1
#define IMX363_DGTL_GAIN_DEFAULT	256

/* Test Pattern Control */
#define IMX363_REG_TEST_PATTERN		0x0600
#define IMX363_TEST_PATTERN_DISABLED		0
#define IMX363_TEST_PATTERN_SOLID_COLOR		1
#define IMX363_TEST_PATTERN_COLOR_BARS		2
#define IMX363_TEST_PATTERN_GRAY_COLOR_BARS	3
#define IMX363_TEST_PATTERN_PN9			    4

/* Flip Control */
#define IMX363_REG_ORIENTATION		0x0101

/* default link frequency and external clock */
#define IMX363_LINK_FREQ_DEFAULT	600000000 // ((llp * fll * refresh_rate * bits) / lanes) / 2
#define IMX363_EXT_CLK			    24000000

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

/* regulator supplies */
static const char * const imx363_supply_name[] = {
	/* Supplies can be enabled in any order */
	"vdddo",  /* VDD supply */
	"ldo",    /* LDO supply */
	"avdd",   /* Analog supply */
	"dvdd",   /* Digital supply */
};

#define IMX363_NUM_SUPPLIES ARRAY_SIZE(imx363_supply_name)

/*
 * Initialisation delay between XCLR low->high and the moment when the sensor
 * can start capture (i.e. can leave software stanby) must be not less than:
 *   t4 + max(t5, t6 + <time to initialize the sensor register over I2C>)
 * where
 *   t4 is fixed, and is max 200uS,
 *   t5 is fixed, and is 6000uS,
 *   t6 depends on the sensor external clock, and is max 32000 clock periods.
 * As per sensor datasheet, the external clock must be from 6MHz to 27MHz.
 * So for any acceptable external clock t6 is always within the range of
 * 1185 to 5333 uS, and is always less than t5.
 * For this reason this is always safe to wait (t4 + t5) = 6200 uS, then
 * initialize the sensor over I2C, and then exit the software standby.
 *
 * This start-up time can be optimized a bit more, if we start the writes
 * over I2C after (t4+t6), but before (t4+t5) expires. But then sensor
 * initialization over I2C may complete before (t4+t5) expires, and we must
 * ensure that capture is not started before (t4+t5).
 *
 * This delay doesn't account for the power supply startup time. If needed,
 * this should be taken care of via the regulator framework. E.g. in the
 * case of DT for regulator-fixed one should define the startup-delay-us
 * property.
 */
#define IMX363_XCLR_MIN_DELAY_US	6200
#define IMX363_XCLR_DELAY_RANGE_US	1000

struct imx363_reg {
	u16 address;
	u8 val;
};

struct imx363_reg_list {
	u32 num_of_regs;
	const struct imx363_reg *regs;
};

/* Mode : resolution and related config&values */
struct imx363_mode {
	/* Frame width */
	u32 width;
	/* Frame height */
	u32 height;

	/* V-timing */
	u32 fll_def;
	u32 fll_min;

	/* H-timing */
	u32 llp;

	/* Default register values */
	struct imx363_reg_list reg_list;
};

struct imx363 {
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct clk *xclk; /* system clock to IMX363 */
	u32 xclk_freq;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[IMX363_NUM_SUPPLIES];

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;

	/* Current mode */
	const struct imx363_mode *cur_mode;

	s64 link_def_freq;	/* CSI-2 link default frequency */

	/*
	 * Mutex for serialized access:
	 * Protect sensor set pad format and start/stop streaming safely.
	 * Protect access to sensor v4l2 controls.
	 */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;
};

// static const struct imx363_reg imx363_global_regs[] = {
// 	// Magical IMX319 crap, do we need them? doesnt seem to appear in downstream camss logs
// 	// { 0x3c7e, 0x05 },
// 	// { 0x3c7f, 0x07 },
// 	// { 0x4d39, 0x0b },
// 	// { 0x4d41, 0x33 },
// 	// { 0x4d43, 0x0c },
// 	// { 0x4d49, 0x89 },
// 	// { 0x4e05, 0x0b },
// 	// { 0x4e0d, 0x33 },
// 	// { 0x4e0f, 0x0c },
// 	// { 0x4e15, 0x89 },
// 	// { 0x4e49, 0x2a },
// 	// { 0x4e51, 0x33 },
// 	// { 0x4e53, 0x0c },
// 	// { 0x4e59, 0x89 },
// 	// { 0x5601, 0x4f },
// 	// { 0x560b, 0x45 },
// 	// { 0x562f, 0x0a },
// 	// { 0x5643, 0x0a },
// 	// { 0x5645, 0x0c },
// 	// { 0x56ef, 0x51 },
// 	// { 0x586f, 0x33 },
// 	// { 0x5873, 0x89 },
// 	// { 0x5905, 0x33 },
// 	// { 0x5907, 0x89 },
// 	// { 0x590d, 0x33 },
// 	// { 0x590f, 0x89 },
// 	// { 0x5915, 0x33 },
// 	// { 0x5917, 0x89 },
// 	// { 0x5969, 0x1c },
// 	// { 0x596b, 0x72 },
// 	// { 0x5971, 0x33 },
// 	// { 0x5973, 0x89 },
// 	// { 0x5975, 0x33 },
// 	// { 0x5977, 0x89 },
// 	// { 0x5979, 0x1c },
// 	// { 0x597b, 0x72 },
// 	// { 0x5985, 0x33 },
// 	// { 0x5987, 0x89 },
// 	// { 0x5999, 0x1c },
// 	// { 0x599b, 0x72 },
// 	// { 0x59a5, 0x33 },
// 	// { 0x59a7, 0x89 },
// 	// { 0x7485, 0x08 },
// 	// { 0x7487, 0x0c },
// 	// { 0x7489, 0xc7 },
// 	// { 0x748b, 0x8b },
// 	// { 0x9004, 0x09 },
// 	// { 0x9200, 0x6a },
// 	// { 0x9201, 0x22 },
// 	// { 0x9202, 0x6a },
// 	// { 0x9203, 0x23 },
// 	// { 0x9204, 0x5f },
// 	// { 0x9205, 0x23 },
// 	// { 0x9206, 0x5f },
// 	// { 0x9207, 0x24 },
// 	// { 0x9208, 0x5f },
// 	// { 0x9209, 0x26 },
// 	// { 0x920a, 0x5f },
// 	// { 0x920b, 0x27 },
// 	// { 0x920c, 0x5f },
// 	// { 0x920d, 0x29 },
// 	// { 0x920e, 0x5f },
// 	// { 0x920f, 0x2a },
// 	// { 0x9210, 0x5f },
// 	// { 0x9211, 0x2c },
// 	// { 0xbc22, 0x1a },
// 	// { 0xf01f, 0x04 },
// 	// { 0xf021, 0x03 },
// 	// { 0xf023, 0x02 },
// 	// { 0xf03d, 0x05 },
// 	// { 0xf03f, 0x03 },
// 	// { 0xf041, 0x02 },
// 	// { 0xf0af, 0x04 },
// 	// { 0xf0b1, 0x03 },
// 	// { 0xf0b3, 0x02 },
// 	// { 0xf0cd, 0x05 },
// 	// { 0xf0cf, 0x03 },
// 	// { 0xf0d1, 0x02 },
// 	// { 0xf13f, 0x04 },
// 	// { 0xf141, 0x03 },
// 	// { 0xf143, 0x02 },
// 	// { 0xf15d, 0x05 },
// 	// { 0xf15f, 0x03 },
// 	// { 0xf161, 0x02 },
// 	// { 0xf1cf, 0x04 },
// 	// { 0xf1d1, 0x03 },
// 	// { 0xf1d3, 0x02 },
// 	// { 0xf1ed, 0x05 },
// 	// { 0xf1ef, 0x03 },
// 	// { 0xf1f1, 0x02 },
// 	// { 0xf287, 0x04 },
// 	// { 0xf289, 0x03 },
// 	// { 0xf28b, 0x02 },
// 	// { 0xf2a5, 0x05 },
// 	// { 0xf2a7, 0x03 },
// 	// { 0xf2a9, 0x02 },
// 	// { 0xf2b7, 0x04 },
// 	// { 0xf2b9, 0x03 },
// 	// { 0xf2bb, 0x02 },
// 	// { 0xf2d5, 0x05 },
// 	// { 0xf2d7, 0x03 },
// 	// { 0xf2d9, 0x02 },
// };

// static const struct imx363_reg_list imx363_global_setting = {
// 	.num_of_regs = ARRAY_SIZE(imx363_global_regs),
// 	.regs = imx363_global_regs,
// };

static const struct imx363_reg mode_4032x3024_regs[] = {
	{ IMX363_REG_EXT_CLK_FREQ_H, 0x18 }, // Integer Part (24.00 Mhz)
	{ IMX363_REG_EXT_CLK_FREQ_L, 0x00 }, // Fractional Part
	//Magical IMX363 Regs & Values - Found in downstream
	{0x31a3, 0x00},
	{0x64d4, 0x01},
	{0x64d5, 0xaa},
	{0x64d6, 0x01},
	{0x64d7, 0xa9},
	{0x64d8, 0x01},
	{0x64d9, 0xa5},
	{0x64da, 0x01},
	{0x64db, 0xa1},
	{0x720a, 0x24},
	{0x720b, 0x89},
	{0x720c, 0x85},
	{0x720d, 0xa1},
	{0x720e, 0x6e},
	{0x729c, 0x59},
	{0x817c, 0xff},
	{0x817d, 0x80},
	{0x9348, 0x96},
	{0x934b, 0x8c},
	{0x934c, 0x82},
	{0x9353, 0xaa},
	{0x9354, 0xaa},
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
	{ 0x0902, 0x0a }, //Not present in downstream camss logs. but seems important.
	//Magical IMX363 Regs & Values - Found in downstream
	{0x30F4, 0x02},
	{0x30F5, 0x80},
	{0x31A5, 0x00},
	{0x31A6, 0x00},
	{0x560F, 0xBE},
	{0x5856, 0x08},
	{0x58D0, 0x10},
	{0x734A, 0x01},
	{0x734F, 0x2B},
	{0x7441, 0x55},
	{0x7914, 0x03},
	{0x7928, 0x04},
	{0x7929, 0x04},
	{0x793F, 0x03},
	{ IMX363_REG_FLL_H, 0x0c }, // fll - 3140
	{ IMX363_REG_FLL_L, 0x44 },
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
	//Magical IMX319 crap again i guess. not present in downstream
	// { 0x3140, 0x02 },
	// { 0x3141, 0x00 },
	// { 0x3f0d, 0x0a },
	// { 0x3f14, 0x01 },
	// { 0x3f3c, 0x01 },
	// { 0x3f4d, 0x01 },
	// { 0x3f4c, 0x01 },
	// { 0x4254, 0x7f },
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
	//Magical IMX319 crap again i guess. not present in downstream
	// { 0x3261, 0x00 },
	// { 0x3264, 0x00 },
	// { 0x3265, 0x10 },
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
	//Magical IMX319 crap again i guess. not present in downstream
	// { 0x0820, 0x0f },
	// { 0x0821, 0x13 },
	// { 0x0822, 0x33 },
	// { 0x0823, 0x33 },
	// { 0x3e20, 0x01 },
	// { 0x3e37, 0x00 },
	// { 0x3e3b, 0x01 },
	// { 0x38a3, 0x01 },
	// { 0x38a8, 0x00 },
	// { 0x38a9, 0x00 },
	// { 0x38aa, 0x00 },
	// { 0x38ab, 0x00 },
	// { 0x3234, 0x00 },
	// { 0x3fc1, 0x00 },
	// { 0x3235, 0x00 },
	// { 0x3802, 0x00 },
	// { 0x3143, 0x04 },
	// { 0x360a, 0x00 },
	// { 0x0b00, 0x00 },
	// { 0x0106, 0x00 },
	// { 0x0b05, 0x01 },
	// { 0x0b06, 0x01 },
	// { 0x3230, 0x00 },
	// { 0x3602, 0x01 },
	// { 0x3607, 0x01 },
	// { 0x3c00, 0x00 },
	// { 0x3c01, 0x48 },
	// { 0x3c02, 0xc8 },
	// { 0x3c03, 0xaa },
	// { 0x3c04, 0x91 },
	// { 0x3c05, 0x54 },
	// { 0x3c06, 0x26 },
	// { 0x3c07, 0x20 },
	// { 0x3c08, 0x51 },
	// { 0x3d80, 0x00 },
	// { 0x3f50, 0x00 },
	// { 0x3f56, 0x00 },
	// { 0x3f57, 0x30 },
	// { 0x3f78, 0x01 },
	// { 0x3f79, 0x18 },
	// { 0x3f7c, 0x00 },
	// { 0x3f7d, 0x00 },
	// { 0x3fba, 0x00 },
	// { 0x3fbb, 0x00 },
	// { 0xa081, 0x00 },
	// { 0xe014, 0x00 },
	{ IMX363_REG_EXPOSURE_H, 0x06 }, // Default exposure = 0x0640
	{ IMX363_REG_EXPOSURE_L, 0x40 },
	{ IMX363_REG_ST_COARSE_INTEG_TIME_H, 0x01 },
	{ IMX363_REG_ST_COARSE_INTEG_TIME_L, 0xf4 },
	{ IMX363_REG_ANALOG_GAIN_H, 0x00 }, // Default analog gain = 0
	{ IMX363_REG_ANALOG_GAIN_L, 0x00 },
	{ IMX363_REG_ST_ANA_GAIN_GLOBAL_H, 0x00 },
	{ IMX363_REG_ST_ANA_GAIN_GLOBAL_L, 0x00 },
	{ IMX363_REG_DIG_GAIN_GLOBAL_H, 0x01 }, // Default digital gain = 256
	{ IMX363_REG_DIG_GAIN_GLOBAL_L, 0x00 },
	{ IMX363_REG_ST_DIG_GAIN_GLOBAL_H, 0x01},
	{ IMX363_REG_ST_DIG_GAIN_GLOBAL_L, 0x00},
	//Magical IMX319 crap again i guess. not present in downstream
	{ 0x0210, 0x01 }, // but the 0x02xx seems important
	{ 0x0211, 0x00 },
	{ 0x0212, 0x01 },
	{ 0x0213, 0x00 },
	{ 0x0214, 0x01 },
	{ 0x0215, 0x00 },
	{ 0x0218, 0x01 },
	{ 0x0219, 0x00 },
	// { 0x3614, 0x00 },
	// { 0x3616, 0x0d },
	// { 0x3617, 0x56 },
	// { 0xb612, 0x20 },
	// { 0xb613, 0x20 },
	// { 0xb614, 0x20 },
	// { 0xb615, 0x20 },
	// { 0xb616, 0x0a },
	// { 0xb617, 0x0a },
	// { 0xb618, 0x20 },
	// { 0xb619, 0x20 },
	// { 0xb61a, 0x20 },
	// { 0xb61b, 0x20 },
	// { 0xb61c, 0x0a },
	// { 0xb61d, 0x0a },
	// { 0xb666, 0x30 },
	// { 0xb667, 0x30 },
	// { 0xb668, 0x30 },
	// { 0xb669, 0x30 },
	// { 0xb66a, 0x14 },
	// { 0xb66b, 0x14 },
	// { 0xb66c, 0x20 },
	// { 0xb66d, 0x20 },
	// { 0xb66e, 0x20 },
	// { 0xb66f, 0x20 },
	// { 0xb670, 0x10 },
	// { 0xb671, 0x10 },
	// { 0x3237, 0x00 },
	// { 0x3900, 0x00 },
	// { 0x3901, 0x00 },
	// { 0x3902, 0x00 },
	// { 0x3904, 0x00 },
	// { 0x3905, 0x00 },
	// { 0x3906, 0x00 },
	// { 0x3907, 0x00 },
	// { 0x3908, 0x00 },
	// { 0x3909, 0x00 },
	// { 0x3912, 0x00 },
	// { 0x3930, 0x00 },
	// { 0x3931, 0x00 },
	// { 0x3933, 0x00 },
	// { 0x3934, 0x00 },
	// { 0x3935, 0x00 },
	// { 0x3936, 0x00 },
	// { 0x3937, 0x00 },
	// { 0x30ac, 0x00 },
};

static const char * const imx363_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Eight Vertical Colour Bars",
	"Colour Bars With Fade to Grey",
	"Pseudorandom Sequence (PN9)",
};

/* supported link frequencies */
static const s64 link_freq_menu_items[] = {
	IMX363_LINK_FREQ_DEFAULT,
};

/* Mode configs */
static const struct imx363_mode supported_modes[] = {
	{
		.width = 4032,
		.height = 3024,
		.fll_def = 3140,
		.fll_min = 3140,
		.llp = 8832, //half of what i read from the link freq regs, bcoz i got pixel clock too high if i used that value for calculation
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4032x3024_regs),
			.regs = mode_4032x3024_regs,
		},
	}
};

static inline struct imx363 *to_imx363(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx363, sd);
}

/* Get bayer order based on flip setting. */
static u32 imx363_get_format_code(struct imx363 *imx363)
{
	/*
	 * Only one bayer order is supported.
	 * It depends on the flip settings.
	 */
	u32 code;
	static const u32 codes[2][2] = {
		{ MEDIA_BUS_FMT_SRGGB10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10, },
		{ MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SBGGR10_1X10, },
	};

	lockdep_assert_held(&imx363->mutex);
	code = MEDIA_BUS_FMT_SRGGB10_1X10;

	return code;
}

/* Read registers up to 4 at a time */
static int imx363_read_reg(struct imx363 *imx363, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	struct i2c_msg msgs[2];
	u8 addr_buf[2];
	u8 data_buf[4] = { 0 };
	int ret;

	if (len > 4)
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

/* Write registers up to 4 at a time */
static int imx363_write_reg(struct imx363 *imx363, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	u8 buf[6];

	if (len > 4)
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_be32(val << (8 * (4 - len)), buf + 2);
	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/* Write a list of registers */
static int imx363_write_regs(struct imx363 *imx363,
			     const struct imx363_reg *regs, u32 len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	int ret;
	u32 i;

	for (i = 0; i < len; i++) {
		ret = imx363_write_reg(imx363, regs[i].address, 1, regs[i].val);
		if (ret) {
			dev_err_ratelimited(&client->dev,
					    "write reg 0x%4.4x return err %d",
					    regs[i].address, ret);
			return ret;
		}
	}

	return 0;
}

/* Open sub-device */
static int imx363_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx363 *imx363 = to_imx363(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_get_try_format(sd, fh->state, 0);

	mutex_lock(&imx363->mutex);

	/* Initialize try_fmt */
	try_fmt->width = imx363->cur_mode->width;
	try_fmt->height = imx363->cur_mode->height;
	try_fmt->code = imx363_get_format_code(imx363);
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&imx363->mutex);

	return 0;
}

static int imx363_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx363 *imx363 = container_of(ctrl->handler,
					     struct imx363, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	s64 max;
	int ret;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = imx363->cur_mode->height + ctrl->val - 18;
		__v4l2_ctrl_modify_range(imx363->exposure,
					 imx363->exposure->minimum,
					 max, imx363->exposure->step, max);
		break;
	}

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		/* Analog gain = 1024/(1024 - ctrl->val) times */
		ret = imx363_write_reg(imx363, IMX363_REG_ANALOG_GAIN_H, 2,
				       ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = imx363_write_reg(imx363, IMX363_REG_DIG_GAIN_GLOBAL_H, 2,
				       ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		ret = imx363_write_reg(imx363, IMX363_REG_EXPOSURE_H, 2,
				       ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		/* Update FLL that meets expected vertical blanking */
		ret = imx363_write_reg(imx363, IMX363_REG_FLL_H, 2,
				       imx363->cur_mode->height + ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx363_write_reg(imx363, IMX363_REG_TEST_PATTERN,
				       2, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = imx363_write_reg(imx363, IMX363_REG_ORIENTATION, 1,
				       imx363->hflip->val |
				       imx363->vflip->val << 1);
		break;
	default:
		ret = -EINVAL;
		dev_info(&client->dev, "ctrl(id:0x%x,val:0x%x) is not handled",
			 ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx363_ctrl_ops = {
	.s_ctrl = imx363_set_ctrl,
};

static int imx363_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx363 *imx363 = to_imx363(sd);

	if (code->index > 0)
		return -EINVAL;

	mutex_lock(&imx363->mutex);
	code->code = imx363_get_format_code(imx363);
	mutex_unlock(&imx363->mutex);

	return 0;
}

static int imx363_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx363 *imx363 = to_imx363(sd);

	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	mutex_lock(&imx363->mutex);
	if (fse->code != imx363_get_format_code(imx363)) {
		mutex_unlock(&imx363->mutex);
		return -EINVAL;
	}
	mutex_unlock(&imx363->mutex);

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static void imx363_update_pad_format(struct imx363 *imx363,
				     const struct imx363_mode *mode,
				     struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = imx363_get_format_code(imx363);
	fmt->format.field = V4L2_FIELD_NONE;
}

static int imx363_do_get_pad_format(struct imx363 *imx363,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt;
	struct v4l2_subdev *sd = &imx363->sd;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
		fmt->format = *framefmt;
	} else {
		imx363_update_pad_format(imx363, imx363->cur_mode, fmt);
	}

	return 0;
}

static int imx363_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	mutex_lock(&imx363->mutex);
	ret = imx363_do_get_pad_format(imx363, sd_state, fmt);
	mutex_unlock(&imx363->mutex);

	return ret;
}

static int
imx363_set_pad_format(struct v4l2_subdev *sd,
		      struct v4l2_subdev_state *sd_state,
		      struct v4l2_subdev_format *fmt)
{
	struct imx363 *imx363 = to_imx363(sd);
	const struct imx363_mode *mode;
	struct v4l2_mbus_framefmt *framefmt;
	s32 vblank_def;
	s32 vblank_min;
	s64 h_blank;
	u64 pixel_rate;
	u32 height;

	mutex_lock(&imx363->mutex);

	/*
	 * Only one bayer order is supported.
	 * It depends on the flip settings.
	 */
	fmt->format.code = imx363_get_format_code(imx363);

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);
	imx363_update_pad_format(imx363, mode, fmt);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_get_try_format(sd, sd_state, fmt->pad);
		*framefmt = fmt->format;
	} else {
		imx363->cur_mode = mode;
		pixel_rate = imx363->link_def_freq * 2 * 4;
		do_div(pixel_rate, 10);
		__v4l2_ctrl_s_ctrl_int64(imx363->pixel_rate, pixel_rate);
		/* Update limits and set FPS to default */
		height = imx363->cur_mode->height;
		vblank_def = imx363->cur_mode->fll_def - height;
		vblank_min = imx363->cur_mode->fll_min - height;
		height = IMX363_FLL_MAX - height;
		__v4l2_ctrl_modify_range(imx363->vblank, vblank_min, height, 1,
					 vblank_def);
		__v4l2_ctrl_s_ctrl(imx363->vblank, vblank_def);
		h_blank = mode->llp - imx363->cur_mode->width;
		/*
		 * Currently hblank is not changeable.
		 * So FPS control is done only by vblank.
		 */
		__v4l2_ctrl_modify_range(imx363->hblank, h_blank,
					 h_blank, 1, h_blank);
	}

	mutex_unlock(&imx363->mutex);

	return 0;
}

static int imx363_start_streaming(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	const struct imx363_reg_list *reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret < 0)
		return ret;

	/* Global Setting */
	// reg_list = &imx363_global_setting;
	// ret = imx363_write_regs(imx363, reg_list->regs, reg_list->num_of_regs);
	// if (ret) {
	// 	dev_err(&client->dev, "failed to set global settings");
	// 	return ret;
	// }

	/* Apply default values of current mode */
	reg_list = &imx363->cur_mode->reg_list;
	ret = imx363_write_regs(imx363, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "failed to set mode");
		return ret;
	}

	/* set digital gain control to all color mode */
	ret = imx363_write_reg(imx363, IMX363_REG_DPGA_USE_GLOBAL_GAIN, 1, 1);
	if (ret)
		return ret;

	/* Apply customized values from user */
	ret =  __v4l2_ctrl_handler_setup(imx363->sd.ctrl_handler);
	if (ret)
		goto err_rpm_put;

	/* set stream on register */
	ret = imx363_write_reg(imx363, IMX363_REG_MODE_SELECT,
			       1, IMX363_MODE_STREAMING);
	if (ret)
		goto err_rpm_put;

	/* vflip and hflip cannot change during streaming */
	__v4l2_ctrl_grab(imx363->vflip, true);
	__v4l2_ctrl_grab(imx363->hflip, true);

	return 0;

err_rpm_put:
	pm_runtime_put(&client->dev);
	return ret;
}

static void imx363_stop_streaming(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	int ret;

	/* set stream off register */
	ret = imx363_write_reg(imx363, IMX363_REG_MODE_SELECT,
			       1, IMX363_MODE_STANDBY);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);

	__v4l2_ctrl_grab(imx363->vflip, false);
	__v4l2_ctrl_grab(imx363->hflip, false);

	pm_runtime_put(&client->dev);
}

static int imx363_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx363 *imx363 = to_imx363(sd);
	int ret = 0;

	mutex_lock(&imx363->mutex);
	if (imx363->streaming == enable) {
		mutex_unlock(&imx363->mutex);
		return 0;
	}

	if (enable) {
		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = imx363_start_streaming(imx363);
		if (ret)
			goto err_unlock;
	} else {
		imx363_stop_streaming(imx363);
	}

	imx363->streaming = enable;

	mutex_unlock(&imx363->mutex);

	return ret;

err_unlock:
	mutex_unlock(&imx363->mutex);

	return ret;
}

/* Power/clock management functions */
static int imx363_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	ret = regulator_bulk_enable(IMX363_NUM_SUPPLIES,
				    imx363->supplies);
	if (ret) {
		dev_err(dev, "%s: failed to enable regulators\n",
			__func__);
		return ret;
	}

	ret = clk_prepare_enable(imx363->xclk);
	if (ret) {
		dev_err(dev, "%s: failed to enable clock\n",
			__func__);
		goto reg_off;
	}

	gpiod_set_value_cansleep(imx363->reset_gpio, 1);
	usleep_range(IMX363_XCLR_MIN_DELAY_US,
		     IMX363_XCLR_MIN_DELAY_US + IMX363_XCLR_DELAY_RANGE_US);

	return 0;

reg_off:
	// regulator_bulk_disable(IMX363_NUM_SUPPLIES, imx363->supplies);

	return ret;
}

static int imx363_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx363 *imx363 = to_imx363(sd);

	gpiod_set_value_cansleep(imx363->reset_gpio, 0);
	// regulator_bulk_disable(IMX363_NUM_SUPPLIES, imx363->supplies);
	clk_disable_unprepare(imx363->xclk);

	return 0;
}

static int __maybe_unused imx363_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx363 *imx363 = to_imx363(sd);

	if (imx363->streaming)
		imx363_stop_streaming(imx363);

	return 0;
}

static int __maybe_unused imx363_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	if (imx363->streaming) {
		ret = imx363_start_streaming(imx363);
		if (ret)
			goto error;
	}

	return 0;

error:
	imx363_stop_streaming(imx363);
	imx363->streaming = false;

	return ret;
}

static int imx363_get_regulators(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	unsigned int i;

	for (i = 0; i < IMX363_NUM_SUPPLIES; i++)
		imx363->supplies[i].supply = imx363_supply_name[i];

	return devm_regulator_bulk_get(&client->dev,
				       IMX363_NUM_SUPPLIES,
				       imx363->supplies);
}

/* Verify chip ID */
static int imx363_identify_module(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	int ret;
	u32 val;

	ret = imx363_read_reg(imx363, IMX363_REG_CHIP_ID, 2, &val);
	if (ret) {
		dev_err(&client->dev, "failed to read chip id %x\n",
			IMX363_CHIP_ID);
		return ret;
	}

	if (val != IMX363_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%x",
			IMX363_CHIP_ID, val);
		return -EIO;
	}

	dev_info(&client->dev, "chip id freaking matched! : %x!=%x",
			IMX363_CHIP_ID, val);

	return 0;
}

static const struct v4l2_subdev_core_ops imx363_subdev_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops imx363_video_ops = {
	.s_stream = imx363_set_stream,
};

static const struct v4l2_subdev_pad_ops imx363_pad_ops = {
	.enum_mbus_code = imx363_enum_mbus_code,
	.get_fmt = imx363_get_pad_format,
	.set_fmt = imx363_set_pad_format,
	.enum_frame_size = imx363_enum_frame_size,
};

static const struct v4l2_subdev_ops imx363_subdev_ops = {
	.core = &imx363_subdev_core_ops,
	.video = &imx363_video_ops,
	.pad = &imx363_pad_ops,
};

static const struct media_entity_operations imx363_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops imx363_internal_ops = {
	.open = imx363_open,
};

/* Initialize control handlers */
static int imx363_init_controls(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	struct v4l2_ctrl_handler *ctrl_hdlr;
	s64 exposure_max;
	s64 vblank_def;
	s64 vblank_min;
	s64 hblank;
	u64 pixel_rate;
	const struct imx363_mode *mode;
	u32 max;
	int ret;

	ctrl_hdlr = &imx363->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 10);
	if (ret)
		return ret;

	ctrl_hdlr->lock = &imx363->mutex;
	max = ARRAY_SIZE(link_freq_menu_items) - 1;
	imx363->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx363_ctrl_ops,
						   V4L2_CID_LINK_FREQ, max, 0,
						   link_freq_menu_items);
	if (imx363->link_freq)
		imx363->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* pixel_rate = link_freq * 2 * nr_of_lanes / bits_per_sample */
	pixel_rate = imx363->link_def_freq * 2 * 4;
	do_div(pixel_rate, 10);
	/* By default, PIXEL_RATE is read only */
	imx363->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					       V4L2_CID_PIXEL_RATE, pixel_rate,
					       pixel_rate, 1, pixel_rate);

	/* Initial vblank/hblank/exposure parameters based on current mode */
	mode = imx363->cur_mode;
	vblank_def = mode->fll_def - mode->height;
	vblank_min = mode->fll_min - mode->height;
	imx363->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_min,
					   IMX363_FLL_MAX - mode->height,
					   1, vblank_def);

	hblank = mode->llp - mode->width;
	imx363->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank,
					   1, hblank);
	if (imx363->hblank)
		imx363->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* fll >= exposure time + adjust parameter (default value is 18) */
	exposure_max = mode->fll_def - 18;
	imx363->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX363_EXPOSURE_MIN, exposure_max,
					     IMX363_EXPOSURE_STEP,
					     IMX363_EXPOSURE_DEFAULT);

	imx363->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	imx363->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX363_ANA_GAIN_MIN, IMX363_ANA_GAIN_MAX,
			  IMX363_ANA_GAIN_STEP, IMX363_ANA_GAIN_DEFAULT);

	/* Digital gain */
	v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX363_DGTL_GAIN_MIN, IMX363_DGTL_GAIN_MAX,
			  IMX363_DGTL_GAIN_STEP, IMX363_DGTL_GAIN_DEFAULT);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx363_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx363_test_pattern_menu) - 1,
				     0, 0, imx363_test_pattern_menu);
	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "control init failed: %d", ret);
		goto error;
	}

	imx363->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static void imx363_free_controls(struct imx363 *imx363)
{
	v4l2_ctrl_handler_free(imx363->sd.ctrl_handler);
	mutex_destroy(&imx363->mutex);
}

static int imx363_check_hwcfg(struct device *dev)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = -EINVAL;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err(dev, "could not parse endpoint\n");
		goto error_out;
	}

	/* Check the number of MIPI CSI2 data lanes */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 4) {
		dev_err(dev, "only 2 data lanes are currently supported\n");
		goto error_out;
	}

	/* Check the link frequency set in device tree */
	if (!ep_cfg.nr_of_link_frequencies) {
		dev_err(dev, "link-frequency property not found in DT\n");
		goto error_out;
	}

	if (ep_cfg.nr_of_link_frequencies != 1 ||
	    ep_cfg.link_frequencies[0] != IMX363_LINK_FREQ_DEFAULT) {
		dev_err(dev, "Link frequency not supported: %lld\n",
			ep_cfg.link_frequencies[0]);
		goto error_out;
	}

	ret = 0;

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int imx363_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx363 *imx363;
	int ret;

	imx363 = devm_kzalloc(&client->dev, sizeof(*imx363), GFP_KERNEL);
	if (!imx363)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&imx363->sd, client, &imx363_subdev_ops);

	/* Check the hardware configuration in device tree */
	if (imx363_check_hwcfg(dev))
		return -EINVAL;

	/* Get system clock (xclk) */
	imx363->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(imx363->xclk)) {
		dev_err(dev, "failed to get xclk\n");
		return PTR_ERR(imx363->xclk);
	}

	ret = fwnode_property_read_u32(dev_fwnode(dev), "clock-frequency", &imx363->xclk_freq);
	if (ret) {
		dev_err(dev, "could not get xclk frequency\n");
		goto error_power_off;
	}

	/* this driver currently expects 24MHz; allow 1% tolerance */
	if (imx363->xclk_freq < 23760000 || imx363->xclk_freq > 24240000) {
		dev_err(dev, "xclk frequency not supported: %d Hz\n",
			imx363->xclk_freq);
		ret = -EINVAL;
		goto error_power_off;
	}

	ret = clk_set_rate(imx363->xclk, imx363->xclk_freq);
	if (ret) {
		dev_err(dev, "could not set xclk frequency\n");
		goto error_power_off;
	}

	imx363->link_def_freq = link_freq_menu_items[0];
	ret = imx363_get_regulators(imx363);
	if (ret) {
		dev_err(dev, "failed to get regulators\n");
		return ret;
	}

	/* Request optional enable pin */
	imx363->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);

	/*
	 * The sensor must be powered for imx363_identify_module()
	 * to be able to read the CHIP_ID register
	 */
	ret = imx363_power_on(dev);
	if (ret)
		return ret;

	ret = imx363_identify_module(imx363);
	if (ret)
		goto error_power_off;

	dev_info(dev, "probed successfully\n");

	/* Set default mode to max resolution */
	imx363->cur_mode = &supported_modes[0];

	/* sensor doesn't enter LP-11 state upon power up until and unless
	 * streaming is started, so upon power up switch the modes to:
	 * streaming -> standby
	 */
	ret = imx363_write_reg(imx363, IMX363_REG_MODE_SELECT, 1, IMX363_MODE_STREAMING);
	if (ret < 0)
		goto error_power_off;
	usleep_range(100, 110);

	/* put sensor back to standby mode */
	ret = imx363_write_reg(imx363, IMX363_REG_MODE_SELECT, 1, IMX363_MODE_STANDBY);
	if (ret < 0)
		goto error_power_off;
	usleep_range(100, 110);

	ret = imx363_init_controls(imx363);
	if (ret)
		goto error_power_off;

	/* Initialize subdev */
	imx363->sd.internal_ops = &imx363_internal_ops;
	imx363->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	imx363->sd.entity.ops = &imx363_subdev_entity_ops;
	imx363->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx363->pad.flags = MEDIA_PAD_FL_SOURCE;

	/* Initialize default format */
	// imx363_set_default_format(imx363);

	ret = media_entity_pads_init(&imx363->sd.entity, 1, &imx363->pad);
	if (ret) {
		dev_err(dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&imx363->sd);
	if (ret < 0) {
		dev_err(dev, "failed to register sensor sub-device: %d\n", ret);
		goto error_media_entity;
	}

	/* Enable runtime PM and turn off the device */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

error_media_entity:
	media_entity_cleanup(&imx363->sd.entity);

error_handler_free:
	imx363_free_controls(imx363);

error_power_off:
	imx363_power_off(dev);

	return ret;
}

static int imx363_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx363 *imx363 = to_imx363(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	imx363_free_controls(imx363);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx363_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

static const struct of_device_id imx363_dt_ids[] = {
	{ .compatible = "sony,imx363" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx363_dt_ids);

static const struct dev_pm_ops imx363_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(imx363_suspend, imx363_resume)
	SET_RUNTIME_PM_OPS(imx363_power_off, imx363_power_on, NULL)
};

static struct i2c_driver imx363_i2c_driver = {
	.driver = {
		.name = "imx363",
		.of_match_table	= imx363_dt_ids,
		.pm = &imx363_pm_ops,
	},
	.probe_new = imx363_probe,
	.remove = imx363_remove,
};

module_i2c_driver(imx363_i2c_driver);

MODULE_AUTHOR("Mis012");
MODULE_AUTHOR("Joel Selvaraj <jo@jsfamily.in>");
MODULE_DESCRIPTION("Sony IMX363 sensor driver");
MODULE_LICENSE("GPL v2");
