// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Samsung S6E3FA7 panel.
 *
 * Copyright (c) 2022, Richard Acayan. All rights reserved.
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
 *   Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct s6e3fa7_sdc {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	bool prepared;
};

static inline struct s6e3fa7_sdc *to_s6e3fa7_sdc(struct drm_panel *panel)
{
	return container_of(panel, struct s6e3fa7_sdc, panel);
}

#define dsi_dcs_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static void s6e3fa7_sdc_reset(struct s6e3fa7_sdc *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int s6e3fa7_sdc_on(struct s6e3fa7_sdc *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(120);

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear on: %d\n", ret);
		return ret;
	}

	dsi_dcs_write_seq(dsi, 0xf0, 0x5a, 0x5a);
	dsi_dcs_write_seq(dsi, 0xf4,
			  0xbb, 0x23, 0x19, 0x3a, 0x9f, 0x0f, 0x09, 0xc0, 0x00,
			  0xb4, 0x37, 0x70, 0x79, 0x69);
	dsi_dcs_write_seq(dsi, 0xf0, 0xa5, 0xa5);
	dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}

	return 0;
}

static int s6e3fa7_sdc_off(struct s6e3fa7_sdc *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(120);

	return 0;
}

static int s6e3fa7_sdc_prepare(struct drm_panel *panel)
{
	struct s6e3fa7_sdc *ctx = to_s6e3fa7_sdc(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	s6e3fa7_sdc_reset(ctx);

	ret = s6e3fa7_sdc_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int s6e3fa7_sdc_unprepare(struct drm_panel *panel)
{
	struct s6e3fa7_sdc *ctx = to_s6e3fa7_sdc(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = s6e3fa7_sdc_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	ctx->prepared = false;
	return 0;
}

static const struct drm_display_mode s6e3fa7_sdc_mode = {
	.clock = (1080 + 32 + 32 + 78) * (2220 + 32 + 4 + 78) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 32,
	.hsync_end = 1080 + 32 + 32,
	.htotal = 1080 + 32 + 32 + 78,
	.vdisplay = 2220,
	.vsync_start = 2220 + 32,
	.vsync_end = 2220 + 32 + 4,
	.vtotal = 2220 + 32 + 4 + 78,
	.width_mm = 62,
	.height_mm = 127,
};

static int s6e3fa7_sdc_get_modes(struct drm_panel *panel,
				 struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &s6e3fa7_sdc_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs s6e3fa7_sdc_panel_funcs = {
	.prepare = s6e3fa7_sdc_prepare,
	.unprepare = s6e3fa7_sdc_unprepare,
	.get_modes = s6e3fa7_sdc_get_modes,
};

static int s6e3fa7_sdc_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	u16 mapped_brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	/*
	 * The brightness controls for this panel are weird:
	 *
	 * The 3 least significant bits affect brightness the most.
	 * Unless the 3 LSBs are all zero, the 3 most significant bits affect
	 * brightness the least and the middle bits can stay put.
	 */
	if ((brightness & 0x380) == 0)
		mapped_brightness = brightness << 3;
	else
		mapped_brightness = ((brightness & 0x380) >> 7) | (brightness & 0x78) | ((brightness & 0x7) << 7);

	ret = mipi_dsi_dcs_set_display_brightness(dsi, mapped_brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static const struct backlight_ops s6e3fa7_sdc_bl_ops = {
	.update_status = s6e3fa7_sdc_bl_update_status,
};

static struct backlight_device *
s6e3fa7_sdc_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1023,
		.max_brightness = 1023,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &s6e3fa7_sdc_bl_ops, &props);
}

static int s6e3fa7_sdc_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct s6e3fa7_sdc *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &s6e3fa7_sdc_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ctx->panel.backlight = s6e3fa7_sdc_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static int s6e3fa7_sdc_remove(struct mipi_dsi_device *dsi)
{
	struct s6e3fa7_sdc *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);
		return ret;
	}

	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id s6e3fa7_sdc_of_match[] = {
	{ .compatible = "samsung,s6e3fa7" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s6e3fa7_sdc_of_match);

static struct mipi_dsi_driver s6e3fa7_sdc_driver = {
	.probe = s6e3fa7_sdc_probe,
	.remove = s6e3fa7_sdc_remove,
	.driver = {
		.name = "panel-samsung-s6e3fa7",
		.of_match_table = s6e3fa7_sdc_of_match,
	},
};
module_mipi_dsi_driver(s6e3fa7_sdc_driver);

MODULE_AUTHOR("linux-mdss-dsi-panel-driver-generator <fix@me>"); // FIXME
MODULE_DESCRIPTION("DRM driver for Samsung s6e3fa7 cmd mode dsi sdc panel");
MODULE_LICENSE("GPL");
