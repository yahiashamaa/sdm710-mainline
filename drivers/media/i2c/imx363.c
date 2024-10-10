// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 Intel Corporation

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <asm/unaligned.h>

#define IMX363_REG_MODE_SELECT	CCI_REG8(0x0100)
#define IMX363_MODE_STANDBY		0x00
#define IMX363_MODE_STREAMING	0x01

#define IMX363_REG_RESET		CCI_REG8(0x0103)

/* Chip ID */
#define IMX363_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX363_CHIP_ID			0x0363

/* V_TIMING internal */
// basically frame length lines? from downstream
#define IMX363_VTS_30FPS		3136 //0c40
// #define IMX363_VTS_30FPS_2K		0x0638
#define IMX363_VTS_30FPS_HD		1296
#define IMX363_VTS_MAX			65525

/* HBLANK control - read only */
#define IMX363_PPL_DEFAULT		8832

/* Exposure control */
#define IMX363_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX363_EXPOSURE_OFFSET	10
#define IMX363_EXPOSURE_MIN		4
#define IMX363_EXPOSURE_STEP	1
#define IMX363_EXPOSURE_DEFAULT	0x640
#define IMX363_EXPOSURE_MAX		(IMX363_VTS_MAX - IMX363_EXPOSURE_OFFSET)

/* Analog gain control */
#define IMX363_REG_ANALOG_GAIN	CCI_REG16(0x0204)
#define IMX363_ANA_GAIN_MIN		0
#define IMX363_ANA_GAIN_MAX		480
#define IMX363_ANA_GAIN_STEP	1
#define IMX363_ANA_GAIN_DEFAULT	0x0

/* Digital gain control */
#define IMX363_REG_GR_DIGITAL_GAIN	CCI_REG16(0x020e)
#define IMX363_REG_R_DIGITAL_GAIN	CCI_REG16(0x0210)
#define IMX363_REG_B_DIGITAL_GAIN	CCI_REG16(0x0212)
#define IMX363_REG_GB_DIGITAL_GAIN	CCI_REG16(0x0214)
#define IMX363_DGTL_GAIN_MIN		0
#define IMX363_DGTL_GAIN_MAX		4096	/* Max = 0xFFF */
#define IMX363_DGTL_GAIN_DEFAULT	1024
#define IMX363_DGTL_GAIN_STEP		1

/* HDR control */
#define IMX363_REG_HDR				CCI_REG8(0x0220)
#define IMX363_HDR_ON				BIT(0)
#define IMX363_REG_HDR_RATIO		CCI_REG8(0x0222)
#define IMX363_HDR_RATIO_MIN		0
#define IMX363_HDR_RATIO_MAX		5
#define IMX363_HDR_RATIO_STEP		1
#define IMX363_HDR_RATIO_DEFAULT	0x0

/* Test Pattern Control */
#define IMX363_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define IMX363_CLK_BLANK_STOP		CCI_REG8(0x4040)

/* Orientation */
#define REG_MIRROR_FLIP_CONTROL		CCI_REG8(0x0101)
#define REG_CONFIG_MIRROR_HFLIP		0x01
#define REG_CONFIG_MIRROR_VFLIP		0x02

/* IMX363 native and active pixel array size. */
#define IMX363_NATIVE_WIDTH			4048U
#define IMX363_NATIVE_HEIGHT		3168U
#define IMX363_PIXEL_ARRAY_LEFT		8U
#define IMX363_PIXEL_ARRAY_TOP		24U
#define IMX363_PIXEL_ARRAY_WIDTH	4032U
#define IMX363_PIXEL_ARRAY_HEIGHT	3024U

/* regs */
#define IMX363_REG_PLL_MULT_DRIV                  CCI_REG8(0x0310)
#define IMX363_REG_IVTPXCK_DIV                    CCI_REG8(0x0301)
#define IMX363_REG_IVTSYCK_DIV                    CCI_REG8(0x0303)
#define IMX363_REG_PREPLLCK_VT_DIV                CCI_REG8(0x0305)
#define IMX363_REG_IOPPXCK_DIV                    CCI_REG8(0x0309)
#define IMX363_REG_IOPSYCK_DIV                    CCI_REG8(0x030b)
#define IMX363_REG_PREPLLCK_OP_DIV                CCI_REG8(0x030d)
#define IMX363_REG_PHASE_PIX_OUTEN                CCI_REG8(0x3030)
#define IMX363_REG_PDPIX_DATA_RATE                CCI_REG8(0x3032)
#define IMX363_REG_SCALE_MODE                     CCI_REG8(0x0401)
#define IMX363_REG_SCALE_MODE_EXT                 CCI_REG8(0x3038)
#define IMX363_REG_AF_WINDOW_MODE                 CCI_REG8(0x7bcd)
#define IMX363_REG_FRM_LENGTH_CTL                 CCI_REG8(0x0350)
#define IMX363_REG_CSI_LANE_MODE                  CCI_REG8(0x0114)
#define IMX363_REG_X_EVN_INC                      CCI_REG8(0x0381)
#define IMX363_REG_X_ODD_INC                      CCI_REG8(0x0383)
#define IMX363_REG_Y_EVN_INC                      CCI_REG8(0x0385)
#define IMX363_REG_Y_ODD_INC                      CCI_REG8(0x0387)
#define IMX363_REG_BINNING_MODE                   CCI_REG8(0x0900)
#define IMX363_REG_BINNING_TYPE_V                 CCI_REG8(0x0901)
#define IMX363_REG_FORCE_FD_SUM                   CCI_REG8(0x300d)
#define IMX363_REG_DIG_CROP_X_OFFSET              CCI_REG16(0x0408)
#define IMX363_REG_DIG_CROP_Y_OFFSET              CCI_REG16(0x040a)
#define IMX363_REG_DIG_CROP_IMAGE_WIDTH           CCI_REG16(0x040c)
#define IMX363_REG_DIG_CROP_IMAGE_HEIGHT          CCI_REG16(0x040e)
#define IMX363_REG_SCALE_M                        CCI_REG16(0x0404)
#define IMX363_REG_X_OUT_SIZE                     CCI_REG16(0x034c)
#define IMX363_REG_Y_OUT_SIZE                     CCI_REG16(0x034e)
#define IMX363_REG_X_ADD_STA                      CCI_REG16(0x0344)
#define IMX363_REG_Y_ADD_STA                      CCI_REG16(0x0346)
#define IMX363_REG_X_ADD_END                      CCI_REG16(0x0348)
#define IMX363_REG_Y_ADD_END                      CCI_REG16(0x034a)
#define IMX363_REG_EXCK_FREQ                      CCI_REG16(0x0136)
#define IMX363_REG_CSI_DT_FMT                     CCI_REG16(0x0112)
#define IMX363_REG_LINE_LENGTH_PCK                CCI_REG16(0x0342)
#define IMX363_REG_SCALE_M_EXT                    CCI_REG16(0x303a)
#define IMX363_REG_FRM_LENGTH_LINES               CCI_REG16(0x0340)
#define IMX363_REG_FINE_INTEG_TIME                CCI_REG8(0x0200)
#define IMX363_REG_PLL_IVT_MPY                    CCI_REG16(0x0306)
#define IMX363_REG_PLL_IOP_MPY                    CCI_REG16(0x030e)
#define IMX363_REG_REQ_LINK_BIT_RATE_MBPS_H       CCI_REG16(0x0820)
#define IMX363_REG_REQ_LINK_BIT_RATE_MBPS_L       CCI_REG16(0x0822)

struct imx363_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

struct imx363_link_cfg {
	unsigned int lf_to_pix_rate_factor;
	struct imx363_reg_list reg_list;
};

enum {
	IMX363_2_LANE_MODE,
	IMX363_4_LANE_MODE,
	IMX363_LANE_CONFIGS,
};

/* Link frequency config */
struct imx363_link_freq_config {
	u32 pixels_per_line;

	/* Configuration for this link frequency / num lanes selection */
	struct imx363_link_cfg link_cfg[IMX363_LANE_CONFIGS];
};

/* Mode : resolution and related config&values */
struct imx363_mode {
	/* Frame width */
	u32 width;
	/* Frame height */
	u32 height;

	/* V-timing */
	u32 vts_def;
	u32 vts_min;

	/* Index of Link frequency config to be used */
	u32 link_freq_index;
	/* Default register values */
	struct imx363_reg_list reg_list;

	/* Analog crop rectangle */
	struct v4l2_rect crop;
};

static const struct cci_reg_sequence mipi_2300mbps_24mhz_2l[] = {
	{ IMX363_REG_EXCK_FREQ, 0x1800 },
	{ IMX363_REG_IVTPXCK_DIV, 10 },
	{ IMX363_REG_IVTSYCK_DIV, 2 },
	{ IMX363_REG_PREPLLCK_VT_DIV, 4 },
	{ IMX363_REG_PLL_IVT_MPY, 212 },
	{ IMX363_REG_IOPPXCK_DIV, 10 },
	{ IMX363_REG_IOPSYCK_DIV, 1 },
	{ IMX363_REG_PREPLLCK_OP_DIV, 2 },
	{ IMX363_REG_PLL_IOP_MPY, 216 },
	{ IMX363_REG_PLL_MULT_DRIV, 0 },

	{ IMX363_REG_CSI_LANE_MODE, 1 },
};

static const struct cci_reg_sequence mipi_2300mbps_24mhz_4l[] = {
	{ IMX363_REG_EXCK_FREQ, 0x1800 },
	{ IMX363_REG_IVTPXCK_DIV, 3 },
	{ IMX363_REG_IVTSYCK_DIV, 2 },
	{ IMX363_REG_PREPLLCK_VT_DIV, 4 },
	{ IMX363_REG_PLL_IVT_MPY, 208 },
	{ IMX363_REG_IOPPXCK_DIV, 10 },
	{ IMX363_REG_IOPSYCK_DIV, 1 },
	{ IMX363_REG_PREPLLCK_OP_DIV, 4 },
	{ IMX363_REG_PLL_IOP_MPY, 230 },
	{ IMX363_REG_PLL_MULT_DRIV, 1 },

	{ IMX363_REG_CSI_LANE_MODE, 3 },
	// { IMX363_REG_REQ_LINK_BIT_RATE_MBPS_H, 2300 * 4 },
	// { IMX363_REG_REQ_LINK_BIT_RATE_MBPS_L, 0 },
};

static const struct cci_reg_sequence mipi_642mbps_24mhz_2l[] = {
	{ IMX363_REG_EXCK_FREQ, 0x1800 },
	{ IMX363_REG_IVTPXCK_DIV, 5 },
	{ IMX363_REG_IVTSYCK_DIV, 2 },
	{ IMX363_REG_PREPLLCK_VT_DIV, 4 },
	{ IMX363_REG_PLL_IVT_MPY, 107 },
	{ IMX363_REG_IOPPXCK_DIV, 10 },
	{ IMX363_REG_IOPSYCK_DIV, 1 },
	{ IMX363_REG_PREPLLCK_OP_DIV, 2 },
	{ IMX363_REG_PLL_IOP_MPY, 216 },
	{ IMX363_REG_PLL_MULT_DRIV, 0 },

	{ IMX363_REG_CSI_LANE_MODE, 1 },
	{ IMX363_REG_REQ_LINK_BIT_RATE_MBPS_H, 642 * 2 },
	{ IMX363_REG_REQ_LINK_BIT_RATE_MBPS_L, 0 },
};

static const struct cci_reg_sequence mipi_642mbps_24mhz_4l[] = {
	{ IMX363_REG_EXCK_FREQ, 0x1800 },
	{ IMX363_REG_IVTPXCK_DIV, 5 },
	{ IMX363_REG_IVTSYCK_DIV, 2 },
	{ IMX363_REG_PREPLLCK_VT_DIV, 4 },
	{ IMX363_REG_PLL_IVT_MPY, 107 },
	{ IMX363_REG_IOPPXCK_DIV, 10 },
	{ IMX363_REG_IOPSYCK_DIV, 1 },
	{ IMX363_REG_PREPLLCK_OP_DIV, 2 },
	{ IMX363_REG_PLL_IOP_MPY, 216 },
	{ IMX363_REG_PLL_MULT_DRIV, 0 },

	{ IMX363_REG_CSI_LANE_MODE, 3 },
	{ IMX363_REG_REQ_LINK_BIT_RATE_MBPS_H, 642 * 4 },
	{ IMX363_REG_REQ_LINK_BIT_RATE_MBPS_L, 0 },
};

static const struct cci_reg_sequence mode_common_regs[] = {
	//Magical IMX363 Regs & Values - Found in downstream. Doesnt affect output except for the few noted. So disable.
	// { CCI_REG8(0x31a3), 0x00 },
	// { CCI_REG8(0x64d4), 0x01 },
	// { CCI_REG8(0x64d5), 0xaa },
	// { CCI_REG8(0x64d6), 0x01 },
	// { CCI_REG8(0x64d7), 0xa9 },
	// { CCI_REG8(0x64d8), 0x01 },
	// { CCI_REG8(0x64d9), 0xa5 },
	// { CCI_REG8(0x64da), 0x01 },
	// { CCI_REG8(0x64db), 0xa1 },
	// { CCI_REG8(0x720a), 0x24 },
	// { CCI_REG8(0x720b), 0x89 },
	// { CCI_REG8(0x720c), 0x85 },
	// { CCI_REG8(0x720d), 0xa1 },
	// { CCI_REG8(0x720e), 0x6e },
	// { CCI_REG8(0x729c), 0x59 },
	// { CCI_REG8(0x817c), 0xff },
	// { CCI_REG8(0x817d), 0x80 },
	// { CCI_REG8(0x9348), 0x96 },
	// { CCI_REG8(0x934b), 0x8c },
	// { CCI_REG8(0x934c), 0x82 },
	// { CCI_REG8(0x9353), 0xaa },
	// { CCI_REG8(0x9354), 0xaa },
	// { CCI_REG8(0x30F4), 0x02 },
	// { CCI_REG8(0x30F5), 0x80 },
	// { CCI_REG8(0x31A5), 0x00 }, //causes white output
	// { CCI_REG8(0x31A6), 0x00 }, //causes white output
	// { CCI_REG8(0x560F), 0xBE }, //causes cropped corrupted nonsensical output
	// { CCI_REG8(0x5856), 0x08 },
	// { CCI_REG8(0x58D0), 0x10 },
	// { CCI_REG8(0x734A), 0x01 },
	// { CCI_REG8(0x734F), 0x2B },
	// { CCI_REG8(0x7441), 0x55 },
	// { CCI_REG8(0x7914), 0x03 },
	// { CCI_REG8(0x7928), 0x04 },
	// { CCI_REG8(0x7929), 0x04 },
	// { CCI_REG8(0x793F), 0x03 },
	
	// present in imx258. not present in android downstream logs. doesnt seem to affect output.
	// {IMX363_REG_SCALE_MODE_EXT, 0}, 
	// {IMX363_REG_SCALE_M_EXT, 16},
	// {IMX363_REG_FORCE_FD_SUM, 1},
	// {IMX363_REG_FRM_LENGTH_CTL, 0},
	// {IMX363_REG_ANALOG_GAIN, 0},
	// {IMX363_REG_GR_DIGITAL_GAIN, 256},
	// {IMX363_REG_R_DIGITAL_GAIN, 256},
	// {IMX363_REG_B_DIGITAL_GAIN, 256},
	// {IMX363_REG_GB_DIGITAL_GAIN, 256},
	// {IMX363_REG_AF_WINDOW_MODE, 0},
	// {IMX363_REG_PHASE_PIX_OUTEN, 0},
	// {IMX363_REG_PDPIX_DATA_RATE, 0},
	// {IMX363_REG_HDR, 0},
	
	// Seems important. Probably will work even without specifying these. But let's just set it anyway.
	// {IMX363_REG_CSI_DT_FMT, 0x0a0a},
	// {IMX363_REG_LINE_LENGTH_PCK, IMX363_PPL_DEFAULT},
	{CCI_REG8(0x0136), 0x18},
	{CCI_REG8(0x0137), 0x00},
	{CCI_REG8(0x31A3), 0x00},
	{CCI_REG8(0x64D4), 0x01},
	{CCI_REG8(0x64D5), 0xAA},
	{CCI_REG8(0x64D6), 0x01},
	{CCI_REG8(0x64D7), 0xA9},
	{CCI_REG8(0x64D8), 0x01},
	{CCI_REG8(0x64D9), 0xA5},
	{CCI_REG8(0x64DA), 0x01},
	{CCI_REG8(0x64DB), 0xA1},
	{CCI_REG8(0x720A), 0x24},
	{CCI_REG8(0x720B), 0x89},
	{CCI_REG8(0x720C), 0x85},
	{CCI_REG8(0x720D), 0xA1},
	{CCI_REG8(0x720E), 0x6E},
	{CCI_REG8(0x729C), 0x59},
	{CCI_REG8(0x817C), 0xFF},
	{CCI_REG8(0x817D), 0x80},
	{CCI_REG8(0x9348), 0x96},
	{CCI_REG8(0x934B), 0x8C},
	{CCI_REG8(0x934C), 0x82},
	{CCI_REG8(0x9353), 0xAA},
	{CCI_REG8(0x9354), 0xAA},
	{CCI_REG8(0x4073), 0x30},
};

static const struct cci_reg_sequence mode_4032x3024_regs[] = {
	// {IMX363_REG_BINNING_MODE, 0},
	// {IMX363_REG_BINNING_TYPE_V, 0x11},
	// // {IMX363_REG_SCALE_MODE, 1},
	// // {IMX363_REG_SCALE_M, 64},
	// {IMX363_REG_X_ADD_STA, 0}, // analog cropping
	// {IMX363_REG_Y_ADD_STA, 0},
	// {IMX363_REG_X_ADD_END, 4031},
	// {IMX363_REG_Y_ADD_END, 3023},
	// {IMX363_REG_X_EVN_INC, 1}, //subsampling
	// {IMX363_REG_X_ODD_INC, 1},
	// {IMX363_REG_Y_EVN_INC, 1},
	// {IMX363_REG_Y_ODD_INC, 1},
	// {IMX363_REG_DIG_CROP_X_OFFSET, 0}, // digital cropping
	// {IMX363_REG_DIG_CROP_Y_OFFSET, 0},
	// {IMX363_REG_DIG_CROP_IMAGE_WIDTH, 4032},
	// {IMX363_REG_DIG_CROP_IMAGE_HEIGHT, 3024},
	// {IMX363_REG_X_OUT_SIZE, 4032},
	// {IMX363_REG_Y_OUT_SIZE, 3024},
	// {IMX363_REG_FRM_LENGTH_LINES, IMX363_VTS_30FPS},
	{CCI_REG8(0x0112), 0x0A},
	{CCI_REG8(0x0113), 0x0A},
	{CCI_REG8(0x0114), 0x03},
	{CCI_REG8(0x0220), 0x00},
	{CCI_REG8(0x0221), 0x11},
	{CCI_REG8(0x0340), 0x0C},
	{CCI_REG8(0x0341), 0x62},
	{CCI_REG8(0x0342), 0x22},
	{CCI_REG8(0x0343), 0x80},
	{CCI_REG8(0x0381), 0x01},
	{CCI_REG8(0x0383), 0x01},
	{CCI_REG8(0x0385), 0x01},
	{CCI_REG8(0x0387), 0x01},
	{CCI_REG8(0x0900), 0x00},
	{CCI_REG8(0x0901), 0x11},
	{CCI_REG8(0x30F4), 0x02},
	{CCI_REG8(0x30F5), 0x80},
	{CCI_REG8(0x30F6), 0x00},
	{CCI_REG8(0x30F7), 0xc8},
	{CCI_REG8(0x31A0), 0x00},
	{CCI_REG8(0x31A5), 0x00},
	{CCI_REG8(0x31A6), 0x00},
	{CCI_REG8(0x560F), 0xbe},
	{CCI_REG8(0x5856), 0x08},
	{CCI_REG8(0x58D0), 0x10},
	{CCI_REG8(0x734A), 0x01},
	{CCI_REG8(0x734F), 0x2b},
	{CCI_REG8(0x7441), 0x55},
	{CCI_REG8(0x7914), 0x03},
	{CCI_REG8(0x7928), 0x04},
	{CCI_REG8(0x7929), 0x04},
	{CCI_REG8(0x793F), 0x03},
	{CCI_REG8(0xBC7B), 0x18},
	{CCI_REG8(0x0344), 0x00},
	{CCI_REG8(0x0345), 0x00},
	{CCI_REG8(0x0346), 0x00},
	{CCI_REG8(0x0347), 0x00},
	{CCI_REG8(0x0348), 0x0F},
	{CCI_REG8(0x0349), 0xBF},
	{CCI_REG8(0x034A), 0x0B},
	{CCI_REG8(0x034B), 0xCF},
	{CCI_REG8(0x034C), 0x0F},
	{CCI_REG8(0x034D), 0xC0},
	{CCI_REG8(0x034E), 0x0B},
	{CCI_REG8(0x034F), 0xD0},
	{CCI_REG8(0x0408), 0x00},
	{CCI_REG8(0x0409), 0x00},
	{CCI_REG8(0x040A), 0x00},
	{CCI_REG8(0x040B), 0x00},
	{CCI_REG8(0x040C), 0x0F},
	{CCI_REG8(0x040D), 0xC0},
	{CCI_REG8(0x040E), 0x0B},
	{CCI_REG8(0x040F), 0xD0},
	{CCI_REG8(0x0301), 0x03},
	{CCI_REG8(0x0303), 0x02},
	{CCI_REG8(0x0305), 0x04},
	{CCI_REG8(0x0306), 0x00},
	{CCI_REG8(0x0307), 0xd2},
	{CCI_REG8(0x0309), 0x0A},
	{CCI_REG8(0x030B), 0x01},
	{CCI_REG8(0x030D), 0x04},
	{CCI_REG8(0x030E), 0x00},
	{CCI_REG8(0x030F), 0xdf},
	{CCI_REG8(0x0310), 0x01},
	{CCI_REG8(0x0202), 0x0C},
	{CCI_REG8(0x0203), 0x52},
	{CCI_REG8(0x0224), 0x06},
	{CCI_REG8(0x0225), 0x29},
	{CCI_REG8(0x0204), 0x00},
	{CCI_REG8(0x0205), 0x00},
	{CCI_REG8(0x0216), 0x00},
	{CCI_REG8(0x0217), 0x00},
	{CCI_REG8(0x020E), 0x01},
	{CCI_REG8(0x020F), 0x00},
	{CCI_REG8(0x0226), 0x01},
	{CCI_REG8(0x0227), 0x00},
};

static const struct cci_reg_sequence mode_1920_1080_regs[] = {
	{IMX363_REG_BINNING_MODE, 1},
	{IMX363_REG_BINNING_TYPE_V, 0x42},
	// {IMX363_REG_SCALE_MODE, 1},
	// {IMX363_REG_SCALE_M, 64},
	{IMX363_REG_X_ADD_STA, 0}, // analog cropping
	{IMX363_REG_Y_ADD_STA, 0},
	{IMX363_REG_X_ADD_END, 4031},
	{IMX363_REG_Y_ADD_END, 3023},
	{IMX363_REG_X_EVN_INC, 1}, //subsampling
	{IMX363_REG_X_ODD_INC, 1},
	{IMX363_REG_Y_EVN_INC, 1},
	{IMX363_REG_Y_ODD_INC, 1},
	{IMX363_REG_DIG_CROP_X_OFFSET, 0}, // digital cropping
	{IMX363_REG_DIG_CROP_Y_OFFSET, 0},
	{IMX363_REG_DIG_CROP_IMAGE_WIDTH, 1920},
	{IMX363_REG_DIG_CROP_IMAGE_HEIGHT, 1080},
	{IMX363_REG_X_OUT_SIZE, 1920},
	{IMX363_REG_Y_OUT_SIZE, 1080},
	{IMX363_REG_FRM_LENGTH_LINES, IMX363_VTS_30FPS_HD},
};

/*
 * The supported formats.
 * This table MUST contain 4 entries per format, to cover the various flip
 * combinations in the order
 * - no flip
 * - h flip
 * - v flip
 * - h&v flips
 */
static const u32 codes[] = {
	/* 10-bit modes. */
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10
};

static const char * const imx363_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Eight Vertical Colour Bars",
	"Colour Bars With Fade to Grey",
	"Pseudorandom Sequence (PN9)",
};

/* regulator supplies */
static const char * const imx363_supply_name[] = {
	/* Supplies can be enabled in any order */
	"vana",  /* Analog (2.8V) supply */
	"vdig",  /* Digital Core (1.2V) supply */
	"vif",  /* IF (1.8V) supply */
};

#define IMX363_NUM_SUPPLIES ARRAY_SIZE(imx363_supply_name)

enum {
	IMX363_LINK_FREQ_2300MBPS,
	IMX363_LINK_FREQ_640MBPS,
};

/*
 * Pixel rate does not necessarily relate to link frequency on this sensor as
 * there is a FIFO between the pixel array pipeline and the MIPI serializer.
 * The recommendation from Sony is that the pixel array is always run with a
 * line length of 8832 pixels, which means that there is a large amount of
 * blanking time for the 1048x780 mode. There is no need to replicate this
 * blanking on the CSI2 bus, and the configuration of register 0x0301 allows the
 * divider to be altered.
 *
 * The actual factor between link frequency and pixel rate is in the
 * imx363_link_cfg, so use this to convert between the two.
 * bits per pixel being 10, and D-PHY being DDR is assumed by this function, so
 * the value is only the combination of number of lanes and pixel clock divider.
 */
static u64 link_freq_to_pixel_rate(u64 f, const struct imx363_link_cfg *link_cfg)
{
	f *= 2 * link_cfg->lf_to_pix_rate_factor;
	do_div(f, 10);

	return f;
}

/* Menu items for LINK_FREQ V4L2 control */
/* Configurations for supported link frequencies */
// static const s64 link_freq_menu_items_19_2[] = {
// 	633600000ULL,
// 	320000000ULL,
// };

static const s64 link_freq_menu_items_24[] = {
	636000000ULL, // NOT SURE HOW TO FIND THIS VALUE
	321000000ULL,
};

#define REGS(_list) { .num_of_regs = ARRAY_SIZE(_list), .regs = _list, }

static const struct imx363_link_freq_config link_freq_configs_24[] = {
	[IMX363_LINK_FREQ_2300MBPS] = {
		.pixels_per_line = IMX363_PPL_DEFAULT,
		.link_cfg = {
			[IMX363_2_LANE_MODE] = {
				.lf_to_pix_rate_factor = 2,
				.reg_list = REGS(mipi_2300mbps_24mhz_2l),
			},
			[IMX363_4_LANE_MODE] = {
				.lf_to_pix_rate_factor = 4,
				.reg_list = REGS(mipi_2300mbps_24mhz_4l),
			},
		}
	},
	[IMX363_LINK_FREQ_640MBPS] = {
		.pixels_per_line = IMX363_PPL_DEFAULT,
		.link_cfg = {
			[IMX363_2_LANE_MODE] = {
				.lf_to_pix_rate_factor = 2 * 2,
				.reg_list = REGS(mipi_642mbps_24mhz_2l),
			},
			[IMX363_4_LANE_MODE] = {
				.lf_to_pix_rate_factor = 4,
				.reg_list = REGS(mipi_642mbps_24mhz_4l),
			},
		}
	},
};

/* Mode configs */
static const struct imx363_mode supported_modes[] = {
	{
		.width = 4032,
		.height = 3024,
		.vts_def = IMX363_VTS_30FPS,
		.vts_min = IMX363_VTS_30FPS,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4032x3024_regs),
			.regs = mode_4032x3024_regs,
		},
		.link_freq_index = IMX363_LINK_FREQ_2300MBPS,
		.crop = {
			.left = IMX363_PIXEL_ARRAY_LEFT,
			.top = IMX363_PIXEL_ARRAY_TOP,
			.width = IMX363_PIXEL_ARRAY_WIDTH,
			.height = IMX363_PIXEL_ARRAY_HEIGHT,
		},
	},
	{
		.width = 1920,
		.height = 1080,
		.vts_def = IMX363_VTS_30FPS_HD,
		.vts_min = IMX363_VTS_30FPS_HD,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1920_1080_regs),
			.regs = mode_1920_1080_regs,
		},
		.link_freq_index = IMX363_LINK_FREQ_640MBPS,
		.crop = {
			.left = IMX363_PIXEL_ARRAY_LEFT,
			.top = IMX363_PIXEL_ARRAY_TOP,
			.width = 4032,
			.height = 3024,
		},
	},
};

struct imx363 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	/* Current mode */
	const struct imx363_mode *cur_mode;

	unsigned long link_freq_bitmap;
	const struct imx363_link_freq_config *link_freq_configs;
	const s64 *link_freq_menu_items;
	unsigned int lane_mode_idx;
	unsigned int csi2_flags;

	struct gpio_desc *reset_gpio;

	/*
	 * Mutex for serialized access:
	 * Protect sensor module set pad format and start/stop streaming safely.
	 */
	struct mutex mutex;

	struct clk *clk;
	struct regulator_bulk_data supplies[IMX363_NUM_SUPPLIES];
};

static inline struct imx363 *to_imx363(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct imx363, sd);
}

/* Get bayer order based on flip setting. */
static u32 imx363_get_format_code(const struct imx363 *imx363)
{
	unsigned int i;

	lockdep_assert_held(&imx363->mutex);

	i = (imx363->vflip->val ? 2 : 0) |
	    (imx363->hflip->val ? 1 : 0);

	return codes[i];
}

/* Open sub-device */
static int imx363_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx363 *imx363 = to_imx363(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_state_get_format(fh->state, 0);
	struct v4l2_rect *try_crop;

	/* Initialize try_fmt */
	try_fmt->width = supported_modes[0].width;
	try_fmt->height = supported_modes[0].height;
	try_fmt->code = imx363_get_format_code(imx363);
	try_fmt->field = V4L2_FIELD_NONE;

	/* Initialize try_crop */
	try_crop = v4l2_subdev_state_get_crop(fh->state, 0);
	try_crop->left = IMX363_PIXEL_ARRAY_LEFT;
	try_crop->top = IMX363_PIXEL_ARRAY_TOP;
	try_crop->width = IMX363_PIXEL_ARRAY_WIDTH;
	try_crop->height = IMX363_PIXEL_ARRAY_HEIGHT;

	return 0;
}

static int imx363_update_digital_gain(struct imx363 *imx363, u32 val)
{
	int ret = 0;

	cci_write(imx363->regmap, IMX363_REG_GR_DIGITAL_GAIN, val, &ret);
	cci_write(imx363->regmap, IMX363_REG_GB_DIGITAL_GAIN, val, &ret);
	cci_write(imx363->regmap, IMX363_REG_R_DIGITAL_GAIN, val, &ret);
	cci_write(imx363->regmap, IMX363_REG_B_DIGITAL_GAIN, val, &ret);

	return ret;
}

static void imx363_adjust_exposure_range(struct imx363 *imx363)
{
	int exposure_max, exposure_def;

	/* Honour the VBLANK limits when setting exposure. */
	exposure_max = imx363->cur_mode->height + imx363->vblank->val -
		       IMX363_EXPOSURE_OFFSET;
	exposure_def = min(exposure_max, imx363->exposure->val);
	__v4l2_ctrl_modify_range(imx363->exposure, imx363->exposure->minimum,
				 exposure_max, imx363->exposure->step,
				 exposure_def);
}

static int imx363_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx363 *imx363 =
		container_of(ctrl->handler, struct imx363, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	int ret = 0;

	/*
	 * The VBLANK control may change the limits of usable exposure, so check
	 * and adjust if necessary.
	 */
	if (ctrl->id == V4L2_CID_VBLANK)
		imx363_adjust_exposure_range(imx363);

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(imx363->regmap, IMX363_REG_ANALOG_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(imx363->regmap, IMX363_REG_EXPOSURE,
				ctrl->val, NULL);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = imx363_update_digital_gain(imx363, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(imx363->regmap, IMX363_REG_TEST_PATTERN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_WIDE_DYNAMIC_RANGE:
		if (!ctrl->val) {
			ret = cci_write(imx363->regmap, IMX363_REG_HDR,
					IMX363_HDR_RATIO_MIN, NULL);
		} else {
			ret = cci_write(imx363->regmap, IMX363_REG_HDR,
					IMX363_HDR_ON, NULL);
			if (ret)
				break;
			ret = cci_write(imx363->regmap, IMX363_REG_HDR_RATIO,
					BIT(IMX363_HDR_RATIO_MAX), NULL);
		}
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(imx363->regmap, IMX363_REG_FRM_LENGTH_LINES,
				imx363->cur_mode->height + ctrl->val, NULL);
		break;
	case V4L2_CID_VFLIP:
	case V4L2_CID_HFLIP:
		ret = cci_write(imx363->regmap, REG_MIRROR_FLIP_CONTROL,
				(imx363->hflip->val ?
				 REG_CONFIG_MIRROR_HFLIP : 0) |
				(imx363->vflip->val ?
				 REG_CONFIG_MIRROR_VFLIP : 0),
				NULL);
		break;
	default:
		dev_info(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
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

	/* Only one bayer format (10 bit) is supported */
	if (code->index > 0)
		return -EINVAL;

	code->code = imx363_get_format_code(imx363);

	return 0;
}

static int imx363_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx363 *imx363 = to_imx363(sd);
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != imx363_get_format_code(imx363))
		return -EINVAL;

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

static int __imx363_get_pad_format(struct imx363 *imx363,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_format *fmt)
{
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_state_get_format(sd_state,
							    fmt->pad);
	else
		imx363_update_pad_format(imx363, imx363->cur_mode, fmt);

	return 0;
}

static int imx363_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx363 *imx363 = to_imx363(sd);
	int ret;

	mutex_lock(&imx363->mutex);
	ret = __imx363_get_pad_format(imx363, sd_state, fmt);
	mutex_unlock(&imx363->mutex);

	return ret;
}

static int imx363_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx363 *imx363 = to_imx363(sd);
	const struct imx363_link_freq_config *link_freq_cfgs;
	const struct imx363_link_cfg *link_cfg;
	struct v4l2_mbus_framefmt *framefmt;
	const struct imx363_mode *mode;
	s32 vblank_def;
	s32 vblank_min;
	s64 h_blank;
	s64 pixel_rate;
	s64 link_freq;

	mutex_lock(&imx363->mutex);

	fmt->format.code = imx363_get_format_code(imx363);

	mode = v4l2_find_nearest_size(supported_modes,
		ARRAY_SIZE(supported_modes), width, height,
		fmt->format.width, fmt->format.height);
	imx363_update_pad_format(imx363, mode, fmt);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_state_get_format(sd_state, fmt->pad);
		*framefmt = fmt->format;
	} else {
		imx363->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(imx363->link_freq, mode->link_freq_index);

		link_freq = imx363->link_freq_menu_items[mode->link_freq_index];
		link_freq_cfgs =
			&imx363->link_freq_configs[mode->link_freq_index];

		link_cfg = &link_freq_cfgs->link_cfg[imx363->lane_mode_idx];
		pixel_rate = link_freq_to_pixel_rate(link_freq, link_cfg);
		__v4l2_ctrl_modify_range(imx363->pixel_rate, pixel_rate,
					 pixel_rate, 1, pixel_rate);
		/* Update limits and set FPS to default */
		vblank_def = imx363->cur_mode->vts_def -
			     imx363->cur_mode->height;
		vblank_min = imx363->cur_mode->vts_min -
			     imx363->cur_mode->height;
		__v4l2_ctrl_modify_range(
			imx363->vblank, vblank_min,
			IMX363_VTS_MAX - imx363->cur_mode->height, 1,
			vblank_def);
		__v4l2_ctrl_s_ctrl(imx363->vblank, vblank_def);
		h_blank =
			imx363->link_freq_configs[mode->link_freq_index].pixels_per_line
			 - imx363->cur_mode->width;
		__v4l2_ctrl_modify_range(imx363->hblank, h_blank,
					 h_blank, 1, h_blank);
	}

	mutex_unlock(&imx363->mutex);

	return 0;
}

static const struct v4l2_rect *
__imx363_get_pad_crop(struct imx363 *imx363,
		      struct v4l2_subdev_state *sd_state,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_state_get_crop(sd_state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &imx363->cur_mode->crop;
	}

	return NULL;
}

static int imx363_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP: {
		struct imx363 *imx363 = to_imx363(sd);

		mutex_lock(&imx363->mutex);
		sel->r = *__imx363_get_pad_crop(imx363, sd_state, sel->pad,
						sel->which);
		mutex_unlock(&imx363->mutex);

		return 0;
	}

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX363_NATIVE_WIDTH;
		sel->r.height = IMX363_NATIVE_HEIGHT;

		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = IMX363_PIXEL_ARRAY_LEFT;
		sel->r.top = IMX363_PIXEL_ARRAY_TOP;
		sel->r.width = IMX363_PIXEL_ARRAY_WIDTH;
		sel->r.height = IMX363_PIXEL_ARRAY_HEIGHT;

		return 0;
	}

	return -EINVAL;
}

/* Start streaming */
static int imx363_start_streaming(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	const struct imx363_reg_list *reg_list;
	const struct imx363_link_freq_config *link_freq_cfg;
	int ret, link_freq_index;

	ret = cci_write(imx363->regmap, IMX363_REG_RESET, 0x01, NULL);
	if (ret) {
		dev_err(&client->dev, "%s failed to reset sensor\n", __func__);
		return ret;
	}

	/* 12ms is required from poweron to standby */
	fsleep(12000);

	/* Setup PLL */
	link_freq_index = imx363->cur_mode->link_freq_index;
	link_freq_cfg = &imx363->link_freq_configs[link_freq_index];

	reg_list = &link_freq_cfg->link_cfg[imx363->lane_mode_idx].reg_list;
	ret = cci_multi_reg_write(imx363->regmap, reg_list->regs, reg_list->num_of_regs, NULL);
	if (ret) {
		dev_err(&client->dev, "%s failed to set plls\n", __func__);
		return ret;
	}

	ret = cci_multi_reg_write(imx363->regmap, mode_common_regs,
				  ARRAY_SIZE(mode_common_regs), NULL);
	if (ret) {
		dev_err(&client->dev, "%s failed to set common regs\n", __func__);
		return ret;
	}

	ret = cci_write(imx363->regmap, IMX363_CLK_BLANK_STOP,
			!!(imx363->csi2_flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK),
			NULL);
	if (ret) {
		dev_err(&client->dev, "%s failed to set clock lane mode\n", __func__);
		return ret;
	}

	/* Apply default values of current mode */
	reg_list = &imx363->cur_mode->reg_list;
	ret = cci_multi_reg_write(imx363->regmap, reg_list->regs, reg_list->num_of_regs, NULL);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		return ret;
	}

	/* Apply customized values from user */
	ret =  __v4l2_ctrl_handler_setup(imx363->sd.ctrl_handler);
	if (ret)
		return ret;

	/* set stream on register */
	return cci_write(imx363->regmap, IMX363_REG_MODE_SELECT,
			 IMX363_MODE_STREAMING, NULL);
}

/* Stop streaming */
static int imx363_stop_streaming(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	int ret;

	/* set stream off register */
	ret = cci_write(imx363->regmap, IMX363_REG_MODE_SELECT,
			IMX363_MODE_STANDBY, NULL);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);

	/*
	 * Return success even if it was an error, as there is nothing the
	 * caller can do about it.
	 */
	return 0;
}

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

	usleep_range(400, 600);

	gpiod_set_value_cansleep(imx363->reset_gpio, 1);

	ret = clk_prepare_enable(imx363->clk);
	if (ret) {
		dev_err(dev, "failed to enable clock\n");
		regulator_bulk_disable(IMX363_NUM_SUPPLIES, imx363->supplies);
	}

	usleep_range(1000, 1200);

	return 0;
}

static int imx363_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx363 *imx363 = to_imx363(sd);

	clk_disable_unprepare(imx363->clk);

	gpiod_set_value_cansleep(imx363->reset_gpio, 0);

	regulator_bulk_disable(IMX363_NUM_SUPPLIES, imx363->supplies);

	return 0;
}

static int imx363_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx363 *imx363 = to_imx363(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&imx363->mutex);

	if (enable) {
		ret = pm_runtime_resume_and_get(&client->dev);
		if (ret < 0)
			goto err_unlock;

		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = imx363_start_streaming(imx363);
		if (ret)
			goto err_rpm_put;
	} else {
		imx363_stop_streaming(imx363);
		pm_runtime_put(&client->dev);
	}

	mutex_unlock(&imx363->mutex);

	return ret;

err_rpm_put:
	pm_runtime_put(&client->dev);
err_unlock:
	mutex_unlock(&imx363->mutex);

	return ret;
}

/* Verify chip ID */
static int imx363_identify_module(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	int ret;
	u64 val;

	ret = cci_read(imx363->regmap, IMX363_REG_CHIP_ID,
		       &val, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to read chip id %x\n",
			IMX363_CHIP_ID);
		return ret;
	}

	if (val != IMX363_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%llx\n",
			IMX363_CHIP_ID, val);
		return -EIO;
	}

	return 0;
}

static const struct v4l2_subdev_video_ops imx363_video_ops = {
	.s_stream = imx363_set_stream,
};

static const struct v4l2_subdev_pad_ops imx363_pad_ops = {
	.enum_mbus_code = imx363_enum_mbus_code,
	.get_fmt = imx363_get_pad_format,
	.set_fmt = imx363_set_pad_format,
	.enum_frame_size = imx363_enum_frame_size,
	.get_selection = imx363_get_selection,
};

static const struct v4l2_subdev_ops imx363_subdev_ops = {
	.video = &imx363_video_ops,
	.pad = &imx363_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx363_internal_ops = {
	.open = imx363_open,
};

/* Initialize control handlers */
static int imx363_init_controls(struct imx363 *imx363)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx363->sd);
	const struct imx363_link_freq_config *link_freq_cfgs;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	const struct imx363_link_cfg *link_cfg;
	s64 vblank_def;
	s64 vblank_min;
	s64 pixel_rate;
	int ret;

	ctrl_hdlr = &imx363->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 13);
	if (ret)
		return ret;

	mutex_init(&imx363->mutex);
	ctrl_hdlr->lock = &imx363->mutex;
	imx363->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr,
				&imx363_ctrl_ops,
				V4L2_CID_LINK_FREQ,
				ARRAY_SIZE(link_freq_menu_items_24) - 1,
				0,
				imx363->link_freq_menu_items);

	if (imx363->link_freq)
		imx363->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx363->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 1);
	if (imx363->hflip)
		imx363->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx363->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 1);
	if (imx363->vflip)
		imx363->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	link_freq_cfgs = &imx363->link_freq_configs[0];
	link_cfg = link_freq_cfgs[imx363->lane_mode_idx].link_cfg;
	pixel_rate = link_freq_to_pixel_rate(imx363->link_freq_menu_items[0],
					     link_cfg);
	printk(KERN_INFO "imx363: pixel_rate: %lld\n", pixel_rate);

	/* By default, PIXEL_RATE is read only */
	imx363->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops,
				V4L2_CID_PIXEL_RATE,
				pixel_rate, pixel_rate,
				1, pixel_rate);

	vblank_def = imx363->cur_mode->vts_def - imx363->cur_mode->height;
	vblank_min = imx363->cur_mode->vts_min - imx363->cur_mode->height;
	imx363->vblank = v4l2_ctrl_new_std(
				ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_VBLANK,
				vblank_min,
				IMX363_VTS_MAX - imx363->cur_mode->height, 1,
				vblank_def);

	imx363->hblank = v4l2_ctrl_new_std(
				ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_HBLANK,
				IMX363_PPL_DEFAULT - imx363->cur_mode->width,
				IMX363_PPL_DEFAULT - imx363->cur_mode->width,
				1,
				IMX363_PPL_DEFAULT - imx363->cur_mode->width);

	if (imx363->hblank)
		imx363->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx363->exposure = v4l2_ctrl_new_std(
				ctrl_hdlr, &imx363_ctrl_ops,
				V4L2_CID_EXPOSURE, IMX363_EXPOSURE_MIN,
				IMX363_EXPOSURE_MAX, IMX363_EXPOSURE_STEP,
				IMX363_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
				IMX363_ANA_GAIN_MIN, IMX363_ANA_GAIN_MAX,
				IMX363_ANA_GAIN_STEP, IMX363_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
				IMX363_DGTL_GAIN_MIN, IMX363_DGTL_GAIN_MAX,
				IMX363_DGTL_GAIN_STEP,
				IMX363_DGTL_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx363_ctrl_ops, V4L2_CID_WIDE_DYNAMIC_RANGE,
				0, 1, 1, IMX363_HDR_RATIO_DEFAULT);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx363_ctrl_ops,
				V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(imx363_test_pattern_menu) - 1,
				0, 0, imx363_test_pattern_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
				__func__, ret);
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx363_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	imx363->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	mutex_destroy(&imx363->mutex);

	return ret;
}

static void imx363_free_controls(struct imx363 *imx363)
{
	v4l2_ctrl_handler_free(imx363->sd.ctrl_handler);
	mutex_destroy(&imx363->mutex);
}

static int imx363_get_regulators(struct imx363 *imx363,
				 struct i2c_client *client)
{
	unsigned int i;

	for (i = 0; i < IMX363_NUM_SUPPLIES; i++)
		imx363->supplies[i].supply = imx363_supply_name[i];

	return devm_regulator_bulk_get(&client->dev,
				    IMX363_NUM_SUPPLIES, imx363->supplies);
}

static int imx363_probe(struct i2c_client *client)
{
	struct imx363 *imx363;
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret;
	u32 val = 0;

	imx363 = devm_kzalloc(&client->dev, sizeof(*imx363), GFP_KERNEL);
	if (!imx363)
		return -ENOMEM;

	imx363->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx363->regmap)) {
		ret = PTR_ERR(imx363->regmap);
		dev_err(&client->dev, "failed to initialize CCI: %d\n", ret);
		return ret;
	}

	ret = imx363_get_regulators(imx363, client);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to get regulators\n");

	imx363->clk = devm_clk_get_optional(&client->dev, NULL);
	if (IS_ERR(imx363->clk))
		return dev_err_probe(&client->dev, PTR_ERR(imx363->clk),
				     "error getting clock\n");
	// if (!imx363->clk) {
	// 	dev_warn(&client->dev,
	// 		"no clock provided, using clock-frequency property\n");

	device_property_read_u32(&client->dev, "clock-frequency", &val);
	// } else {
		// val = clk_get_rate(imx363->clk);
	// }
	ret = clk_set_rate(imx363->clk, val);
	if (ret)
		return dev_err_probe(&client->dev, ret,
						"failed to set clock rate\n");

	switch (val) {
	// case 19200000:
	// 	imx363->link_freq_configs = link_freq_configs_19_2;
	// 	imx363->link_freq_menu_items = link_freq_menu_items_19_2;
	// 	break;
	case 24000000:
		imx363->link_freq_configs = link_freq_configs_24;
		imx363->link_freq_menu_items = link_freq_menu_items_24;
		break;
	default:
		dev_err(&client->dev, "input clock frequency of %u not supported\n",
			val);
		return -EINVAL;
	}

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(&client->dev), NULL);
	if (!endpoint) {
		dev_err(&client->dev, "Endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(&client->dev, "Parsing endpoint node failed\n");
		return ret;
	}

	ret = v4l2_link_freq_to_bitmap(&client->dev,
				       ep.link_frequencies,
				       ep.nr_of_link_frequencies,
				       imx363->link_freq_menu_items,
				       ARRAY_SIZE(link_freq_menu_items_24),
				       &imx363->link_freq_bitmap);
	if (ret) {
		dev_err(&client->dev, "Link frequency not supported\n");
		goto error_endpoint_free;
	}

	/* Get number of data lanes */
	switch (ep.bus.mipi_csi2.num_data_lanes) {
	case 2:
		imx363->lane_mode_idx = IMX363_2_LANE_MODE;
		break;
	case 4:
		imx363->lane_mode_idx = IMX363_4_LANE_MODE;
		printk(KERN_INFO "imx363: 4 lanes\n");
		break;
	default:
		dev_err(&client->dev, "Invalid data lanes: %u\n",
			ep.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto error_endpoint_free;
	}

	imx363->csi2_flags = ep.bus.mipi_csi2.flags;

	/* request optional reset pin */
	imx363->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
						     GPIOD_OUT_LOW);
	if (IS_ERR(imx363->reset_gpio))
		return PTR_ERR(imx363->reset_gpio);

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&imx363->sd, client, &imx363_subdev_ops);

	/* Will be powered off via pm_runtime_idle */
	ret = imx363_power_on(&client->dev);
	if (ret)
		goto error_endpoint_free;

	/* Check module identity */
	ret = imx363_identify_module(imx363);
	if (ret)
		goto error_identify;

	/* Set default mode to max resolution */
	imx363->cur_mode = &supported_modes[0];

	ret = imx363_init_controls(imx363);
	if (ret)
		goto error_identify;

	/* Initialize subdev */
	imx363->sd.internal_ops = &imx363_internal_ops;
	imx363->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx363->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	imx363->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx363->sd.entity, 1, &imx363->pad);
	if (ret)
		goto error_handler_free;

	ret = v4l2_async_register_subdev_sensor(&imx363->sd);
	if (ret < 0)
		goto error_media_entity;

	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);
	v4l2_fwnode_endpoint_free(&ep);

	return 0;

error_media_entity:
	media_entity_cleanup(&imx363->sd.entity);

error_handler_free:
	imx363_free_controls(imx363);

error_identify:
	imx363_power_off(&client->dev);

error_endpoint_free:
	v4l2_fwnode_endpoint_free(&ep);

	return ret;
}

static void imx363_remove(struct i2c_client *client)
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
}

static const struct dev_pm_ops imx363_pm_ops = {
	SET_RUNTIME_PM_OPS(imx363_power_off, imx363_power_on, NULL)
};

#ifdef CONFIG_ACPI
static const struct acpi_device_id imx363_acpi_ids[] = {
	{ "SONY363A" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(acpi, imx363_acpi_ids);
#endif

static const struct of_device_id imx363_dt_ids[] = {
	{ .compatible = "sony,imx363"},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx363_dt_ids);

static struct i2c_driver imx363_i2c_driver = {
	.driver = {
		.name = "imx363",
		.pm = &imx363_pm_ops,
		.acpi_match_table = ACPI_PTR(imx363_acpi_ids),
		.of_match_table	= imx363_dt_ids,
	},
	.probe = imx363_probe,
	.remove = imx363_remove,
};

module_i2c_driver(imx363_i2c_driver);

MODULE_AUTHOR("Yeh, Andy <andy.yeh@intel.com>");
MODULE_AUTHOR("Chiang, Alan");
MODULE_AUTHOR("Chen, Jason");
MODULE_DESCRIPTION("Sony IMX363 sensor driver");
MODULE_LICENSE("GPL v2");
